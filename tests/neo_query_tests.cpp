// Query engine tests.
//
//   validation        lo > hi and NaN are reported with clear messages
//   semantics         hand-built datasets with known answers: unknown-diameter
//                     policies, tri-state flags, boundary-equal values, top-K with
//                     ties, and SAME-ROW approach semantics
//   histogram / names the planner's estimator and the prefix index
//   planner           which driver it picks, that EXPLAIN reports estimated and
//                     actual candidates, and that estimates are close
//   oracle            >= 10,000 random queries: planned == fixed-order == naive,
//                     and every access path (forced) == naive, on fixtures, on
//                     synthetic data with heavy ties, and on the real neo.db
//   threads           concurrent queries on one engine agree with sequential ones
//
// No network. The real-dataset part skips cleanly when data/neo.db is absent.

#include "neo/ingest/CadParser.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/model/JulianDate.h"
#include "neo/query/Histogram.h"
#include "neo/query/NameIndex.h"
#include "neo/query/QueryEngine.h"
#include "neo/storage/Database.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_fixtureDir;

void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what.c_str(), detail.c_str());
    }
}

std::string num(std::size_t v) { return std::to_string(v); }

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n); }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

private:
    std::uint64_t state_;
};

double jd(const char* iso) {
    double out = 0.0;
    if (!neo::julianDateFromIsoDate(iso, out)) {
        std::printf("  FAIL  bad test date %s\n", iso);
        ++g_failures;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hand-built datasets
// ---------------------------------------------------------------------------

struct ObjSpec {
    std::string pdes;
    std::string name;
    std::optional<double> diameter;
    std::optional<double> h;
    std::optional<double> moid;
    double a = 1.2;
    double e = 0.3;
    double i = 10.0;
    std::optional<bool> neo;
    std::optional<bool> pha;
    std::string orbitClass;
    neo::ObjectKind kind = neo::ObjectKind::Asteroid;
};

// A default ObjSpec with just a designation (a partial aggregate initialiser would
// trip -Wmissing-field-initializers).
ObjSpec spec(const char* pdes) {
    ObjSpec s;
    s.pdes = pdes;
    return s;
}

struct AppSpec {
    std::size_t object = 0;
    double jd = 0.0;
    double dist = 0.01;
    double vrel = 10.0;
};

neo::Dataset buildDataset(const std::vector<ObjSpec>& objects, const std::vector<AppSpec>& approaches) {
    std::vector<neo::Asteroid> list;
    for (const ObjSpec& s : objects) {
        neo::Asteroid a;
        a.pdes = s.pdes;
        a.name = s.name;
        a.physical.diameterKm = s.diameter;
        a.physical.absoluteMagnitudeH = s.h;
        a.orbital.moidAU = s.moid;
        a.orbital.semiMajorAxisAU = s.a;
        a.orbital.eccentricity = s.e;
        a.orbital.inclinationDeg = s.i;
        a.classification.isNEO = s.neo;
        a.classification.isPHA = s.pha;
        a.classification.orbitClass = s.orbitClass;
        a.classification.kind = s.kind;
        list.push_back(std::move(a));
    }
    neo::Dataset dataset;
    dataset.setObjects(std::move(list));

    std::vector<neo::CloseApproach> rows;
    for (const AppSpec& s : approaches) {
        neo::CloseApproach c;
        c.objectIndex = static_cast<std::uint32_t>(s.object);
        c.jdTdb = s.jd;
        c.distanceAU = s.dist;
        c.distanceMinAU = s.dist;
        c.distanceMaxAU = s.dist;
        c.relVelocityKms = s.vrel;
        rows.push_back(c);
    }
    dataset.setApproaches(std::move(rows));
    return dataset;
}

std::vector<std::string> pdesOf(const neo::QueryEngine& engine, const neo::QueryResult& r) {
    std::vector<std::string> out;
    for (const neo::ResultRow& row : r.rows) {
        out.push_back(engine.dataset().records()[row.object].object.pdes);
    }
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string out = "{";
    for (std::size_t i = 0; i < v.size(); ++i) {
        out += (i == 0 ? "" : ", ") + v[i];
    }
    return out + "}";
}

const neo::Access kAllAccess[] = {neo::Access::ScanObjects, neo::Access::ScanApproaches, neo::Access::HashLookup,
                                  neo::Access::NamePrefix,  neo::Access::ObjectView,     neo::Access::SizeBuckets,
                                  neo::Access::DateTree,    neo::Access::YearBuckets,    neo::Access::ApproachView};

// Runs the query on every path (naive, fixed, planned, and each forced access)
// and checks each returns exactly `expected`, in order.
void expectPdes(const neo::QueryEngine& engine, const char* name, const neo::Query& q,
                const std::vector<std::string>& expected) {
    const neo::QueryResult naive = engine.run(q, neo::ExecMode::Naive);
    check(naive.ok, std::string(name) + " (naive runs)", join(naive.errors));
    const std::vector<std::string> got = pdesOf(engine, naive);
    check(got == expected, std::string(name) + " (naive)", "expected " + join(expected) + " got " + join(got));

    const std::vector<std::string> fixed = pdesOf(engine, engine.run(q, neo::ExecMode::FixedOrder));
    const std::vector<std::string> planned = pdesOf(engine, engine.run(q, neo::ExecMode::Planned));
    check(fixed == expected, std::string(name) + " (fixed-order)", "got " + join(fixed));
    check(planned == expected, std::string(name) + " (planned)", "got " + join(planned));
    bool forcedOk = true;
    for (const neo::Access access : kAllAccess) {
        const std::vector<std::string> forced = pdesOf(engine, engine.run(q, neo::ExecMode::Planned, access));
        forcedOk = forcedOk && forced == expected;
    }
    check(forcedOk, std::string(name) + " (every forced access path)");
}

// --- validation ---------------------------------------------------------------

void testValidation() {
    std::printf("[query] validation\n");
    neo::Query q;
    check(neo::validate(q).empty(), "an empty query is valid");
    q.diameterKm = neo::Range::between(1.0, 1.0);
    check(neo::validate(q).empty(), "lo == hi is valid (bounds are inclusive)");

    q.diameterKm = neo::Range::between(5.0, 1.0);
    std::vector<std::string> errors = neo::validate(q);
    check(errors.size() == 1, "lo > hi is one error", num(errors.size()));
    check(!errors.empty() && errors[0].find("diameter") != std::string::npos &&
              errors[0].find("5") != std::string::npos && errors[0].find("greater than") != std::string::npos,
          "the message names the field and both bounds", errors.empty() ? "" : errors[0]);

    q.dateJd = neo::Range::between(jd("2040-01-01"), jd("2030-01-01"));
    q.moidAU = neo::Range::between(0.5, 0.1);
    check(neo::validate(q).size() == 3, "every invalid range is reported, not just the first");

    neo::Query nan;
    nan.absoluteMagnitude.lo = std::numeric_limits<double>::quiet_NaN();
    errors = neo::validate(nan);
    check(errors.size() == 1 && errors[0].find("not a number") != std::string::npos, "a NaN bound is rejected",
          errors.empty() ? "" : errors[0]);

    // run() refuses an invalid query rather than guessing.
    neo::Dataset dataset = buildDataset({spec("1")}, {});
    neo::QueryEngine engine(dataset);
    engine.build();
    const neo::QueryResult bad = engine.run(q);
    check(!bad.ok && !bad.errors.empty() && bad.rows.empty(), "run() reports validation errors and returns no rows");
    neo::QueryEngine unbuilt(dataset);
    const neo::QueryResult early = unbuilt.run(neo::Query());
    check(!early.ok && early.errors[0].find("build()") != std::string::npos,
          "querying before build() is an error that says what to call");
}

// --- semantics on a hand-built dataset -----------------------------------------

neo::Dataset semanticsDataset() {
    std::vector<ObjSpec> o(6);
    o[0] = {"100", "Alpha", 0.30, 19.0, 0.01, 1.2, 0.3, 10.0, true, true, "APO", neo::ObjectKind::Asteroid};
    o[1] = {"200", "Beta", std::nullopt, 22.0, std::nullopt, 1.0, 0.5, 10.0, true, false, "ATE",
            neo::ObjectKind::Asteroid};
    o[2] = {"300", "", std::nullopt, std::nullopt, std::nullopt, 3.0, 0.9, 30.0, std::nullopt, std::nullopt, "",
            neo::ObjectKind::Comet};
    o[3] = {"400", "Delta", 0.14, 21.0, std::nullopt, 1.5, 0.2, 10.0, true, true, "APO", neo::ObjectKind::Asteroid};
    o[4] = {"2020 AB", "", 1.0, 18.0, std::nullopt, 2.0, 0.1, 5.0, false, false, "AMO", neo::ObjectKind::Asteroid};
    o[5] = {"2020 AC", "", std::nullopt, 25.0, std::nullopt, 1.1, 0.4, 5.0, true, std::nullopt, "APO",
            neo::ObjectKind::Asteroid};
    std::vector<AppSpec> a;
    a.push_back({0, jd("2030-06-01"), 0.010, 20.0});
    a.push_back({0, jd("2035-01-01"), 0.040, 5.0});
    a.push_back({1, jd("2031-03-01"), 0.020, 10.0});
    a.push_back({3, jd("2030-01-01"), 0.0499, 30.0});
    a.push_back({4, jd("2040-01-01"), 0.030, 8.0});
    a.push_back({5, jd("2030-06-01"), 0.00002, 15.0}); // inside one Earth radius
    return buildDataset(o, a);
}

void testSemantics() {
    std::printf("[query] semantics on a hand-built dataset\n");
    const neo::Dataset dataset = semanticsDataset();
    neo::QueryEngine engine(dataset);
    engine.build();

    neo::Query q;
    // Diameter policies. Beta has no measured diameter but H = 22, which estimates
    // to 0.1414 km; 300 has neither; 2020 AC estimates to 0.0355 km.
    q.diameterKm = neo::Range::atLeast(0.14);
    q.diameterMode = neo::DiameterMode::MeasuredOnly;
    expectPdes(engine, "diameter >= 0.14, measured only", q, {"100", "400", "2020 AB"});
    q.diameterMode = neo::DiameterMode::MeasuredOrEstimated;
    expectPdes(engine, "diameter >= 0.14, measured or estimate", q, {"100", "200", "400", "2020 AB"});
    q.diameterMode = neo::DiameterMode::IncludeUnknown;
    expectPdes(engine, "diameter >= 0.14, unknown included", q, {"100", "200", "300", "400", "2020 AB"});
    q.diameterMode = neo::DiameterMode::MeasuredOnly;
    q.diameterKm = neo::Range::between(0.14, 0.14);
    expectPdes(engine, "diameter == 0.14 exactly (boundary equal)", q, {"400"});
    q.diameterMode = neo::DiameterMode::MeasuredOnly;
    q.diameterKm = neo::Range::atMost(0.05);
    expectPdes(engine, "small diameter, measured only: an estimate is not a measurement", q, {});
    q.diameterMode = neo::DiameterMode::MeasuredOrEstimated;
    expectPdes(engine, "small diameter, with estimates", q, {"2020 AC"});

    // Flags: unknown matches neither yes nor no.
    q = neo::Query();
    q.pha = neo::TriState::Yes;
    expectPdes(engine, "pha = yes", q, {"100", "400"});
    q.pha = neo::TriState::No;
    expectPdes(engine, "pha = no (null flags excluded)", q, {"200", "2020 AB"});
    q = neo::Query();
    q.neo = neo::TriState::Yes;
    expectPdes(engine, "neo = yes", q, {"100", "200", "400", "2020 AC"});
    q.neo = neo::TriState::No;
    expectPdes(engine, "neo = no", q, {"2020 AB"});

    // Numeric ranges: unknown never matches; inclusive at both ends.
    q = neo::Query();
    q.absoluteMagnitude = neo::Range::between(20.0, 22.0);
    expectPdes(engine, "H in [20, 22] includes both endpoints, excludes no-H", q, {"200", "400"});
    q = neo::Query();
    q.moidAU = neo::Range::atLeast(0.0);
    expectPdes(engine, "a MOID range excludes every object whose MOID is unknown", q, {"100"});

    q = neo::Query();
    q.kind = neo::ObjectKind::Comet;
    expectPdes(engine, "kind = comet", q, {"300"});
    q = neo::Query();
    q.orbitClasses = {"APO"};
    expectPdes(engine, "class APO", q, {"100", "400", "2020 AC"});
    q.orbitClasses = {"APO", "ATE"};
    expectPdes(engine, "class APO or ATE", q, {"100", "200", "400", "2020 AC"});
    q.orbitClasses = {"apo"};
    expectPdes(engine, "class codes are case-sensitive", q, {});
    q.orbitClasses = {"XYZ"};
    expectPdes(engine, "a class that never occurs matches nothing", q, {});

    // Lookups.
    q = neo::Query();
    q.designation = "400";
    expectPdes(engine, "exact designation", q, {"400"});
    q.designation = "999";
    expectPdes(engine, "absent designation", q, {});
    q = neo::Query();
    q.namePrefix = "al";
    expectPdes(engine, "name prefix, lower case", q, {"100"});
    q.namePrefix = "AL";
    expectPdes(engine, "name prefix, upper case (case-insensitive)", q, {"100"});
    q.namePrefix = "2020 a";
    expectPdes(engine, "designation prefix", q, {"2020 AB", "2020 AC"});
    q.namePrefix = "zzz";
    expectPdes(engine, "a prefix nothing has", q, {});

    // Approach filters.
    q = neo::Query();
    q.grazing = neo::TriState::Yes;
    expectPdes(engine, "grazing = yes", q, {"2020 AC"});
    q.grazing = neo::TriState::No;
    expectPdes(engine, "grazing = no", q, {"100", "200", "400", "2020 AB"});
    q = neo::Query();
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2030-12-31"));
    expectPdes(engine, "date window (2030-01-01 boundary is inclusive)", q, {"100", "400", "2020 AC"});
    q.dateJd = neo::Range::between(jd("2040-01-01"), jd("2040-01-01"));
    expectPdes(engine, "a one-instant window on an approach's exact date", q, {"2020 AB"});

    // The matching approaches of an object are only those that matched.
    q = neo::Query();
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2030-12-31"));
    const neo::QueryResult windowed = engine.run(q);
    const std::uint32_t alpha = engine.findDesignation("100");
    for (const neo::ResultRow& row : windowed.rows) {
        if (row.object == alpha) {
            check(row.approachCount == 1, "Alpha lists only its 2030 approach, not the 2035 one", num(row.approachCount));
        }
    }

    // No filter: everything, with every approach.
    const neo::QueryResult all = engine.run(neo::Query());
    check(all.totalObjects == 6 && all.totalApproaches == 6, "an empty query returns every object and approach",
          num(all.totalObjects) + "/" + num(all.totalApproaches));
    check(all.rows.size() == 6, "and all rows");

    // Combined object + approach filters.
    q = neo::Query();
    q.pha = neo::TriState::Yes;
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2035-12-31"));
    q.distanceAU = neo::Range::atMost(0.02);
    expectPdes(engine, "pha + date + distance", q, {"100"});
}

void testSorting() {
    std::printf("[query] sorting, ties, unknown keys and top-K\n");
    const neo::Dataset dataset = semanticsDataset();
    neo::QueryEngine engine(dataset);
    engine.build();

    neo::Query q;
    q.sortBy = neo::SortField::Distance;
    expectPdes(engine, "sort by distance ascending: the best approach per object", q,
               {"2020 AC", "100", "200", "2020 AB", "400", "300"});
    q.direction = neo::SortDirection::Descending;
    expectPdes(engine, "sort by distance descending: the worst approach per object, unknown last", q,
               {"400", "100", "2020 AB", "200", "2020 AC", "300"});

    q = neo::Query();
    q.sortBy = neo::SortField::Inclination;
    expectPdes(engine, "sort by inclination: ties broken by index", q, {"2020 AB", "2020 AC", "100", "200", "400", "300"});
    q.topK = 3;
    expectPdes(engine, "top 3 by inclination", q, {"2020 AB", "2020 AC", "100"});
    q.direction = neo::SortDirection::Descending;
    q.topK = 4;
    expectPdes(engine, "top 4 by inclination descending", q, {"300", "100", "200", "400"});

    q = neo::Query();
    q.sortBy = neo::SortField::Diameter;
    expectPdes(engine, "sort by diameter (measured or estimate), unknown last", q,
               {"2020 AC", "400", "200", "100", "2020 AB", "300"});
    q.direction = neo::SortDirection::Descending;
    expectPdes(engine, "diameter descending, unknown still last", q, {"2020 AB", "100", "200", "400", "2020 AC", "300"});
    q.diameterMode = neo::DiameterMode::MeasuredOnly;
    expectPdes(engine, "measured-only sort: an estimate is unknown, so it sorts last", q,
               {"2020 AB", "100", "400", "200", "300", "2020 AC"});

    q = neo::Query();
    q.sortBy = neo::SortField::Designation;
    expectPdes(engine, "sort by designation", q, {"100", "200", "2020 AB", "2020 AC", "300", "400"});

    q = neo::Query();
    q.sortBy = neo::SortField::Date;
    q.topK = 100;
    check(engine.run(q).rows.size() == 6, "top-K larger than the result returns everything");
    q.topK = 1;
    check(engine.run(q).rows.size() == 1 && engine.run(q).totalObjects == 6,
          "top 1 returns one row but reports all 6 matches");

    // Approach-sort key comes from the MATCHING approaches only.
    q = neo::Query();
    q.dateJd = neo::Range::between(jd("2034-01-01"), jd("2036-01-01"));
    q.sortBy = neo::SortField::Distance;
    const neo::QueryResult r = engine.run(q);
    check(r.rows.size() == 1 && r.rows[0].sortKeyKnown && r.rows[0].sortKey == 0.040,
          "the sort key is the matching approach's distance, not the object's best overall",
          r.rows.empty() ? "" : std::to_string(r.rows[0].sortKey));
}

void testSameRowSemantics() {
    std::printf("[query] approach conditions apply to the SAME approach row\n");
    // X has two approaches: one close but fast, one distant but slow. Each meets
    // ONE of "dist <= 0.02" and "v_rel <= 10", and neither meets both.
    std::vector<ObjSpec> o = {spec("X"), spec("Y"), spec("Z")};
    std::vector<AppSpec> a;
    a.push_back({0, jd("2030-01-01"), 0.010, 30.0}); // X: close, fast
    a.push_back({0, jd("2031-01-01"), 0.040, 5.0});  // X: far, slow
    a.push_back({1, jd("2030-01-01"), 0.010, 5.0});  // Y: close AND slow
    a.push_back({2, jd("2030-01-01"), 0.050, 5.0});  // Z: far, slow
    const neo::Dataset dataset = buildDataset(o, a);
    neo::QueryEngine engine(dataset);
    engine.build();

    neo::Query q;
    q.distanceAU = neo::Range::atMost(0.02);
    q.velocityKms = neo::Range::atMost(10.0);
    expectPdes(engine, "dist <= 0.02 AND v_rel <= 10: only Y, never X across two rows", q, {"Y"});

    q = neo::Query();
    q.distanceAU = neo::Range::atMost(0.02);
    expectPdes(engine, "dist alone matches X and Y", q, {"X", "Y"});
    const neo::QueryResult onlyDist = engine.run(q);
    check(onlyDist.rows.size() == 2 && onlyDist.rows[0].approachCount == 1,
          "X lists just the one approach that met the distance limit");

    q = neo::Query();
    q.velocityKms = neo::Range::atMost(10.0);
    expectPdes(engine, "v_rel alone matches X, Y and Z", q, {"X", "Y", "Z"});

    q = neo::Query();
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2030-12-31"));
    q.velocityKms = neo::Range::atMost(10.0);
    expectPdes(engine, "2030 AND v_rel <= 10: X's 2030 approach is fast, its slow one is in 2031", q, {"Y", "Z"});

    q = neo::Query();
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2030-12-31"));
    q.distanceAU = neo::Range::atMost(0.02);
    q.velocityKms = neo::Range::atMost(10.0);
    expectPdes(engine, "three approach conditions on one row", q, {"Y"});

    q = neo::Query();
    q.distanceAU = neo::Range::atMost(0.02);
    q.velocityKms = neo::Range::atMost(10.0);
    q.grazing = neo::TriState::No;
    expectPdes(engine, "adding a fourth condition keeps the same-row rule", q, {"Y"});
}

// --- histogram and names ----------------------------------------------------------

void testHistogram() {
    std::printf("[histogram] equi-depth estimates\n");
    neo::EquiDepthHistogram empty;
    empty.build(nullptr, 0, 100);
    check(empty.selectivity(0.0, 1.0) == 0.0, "an empty histogram estimates 0");

    std::vector<double> uniform(10000);
    for (std::size_t i = 0; i < uniform.size(); ++i) {
        uniform[i] = static_cast<double>(i);
    }
    neo::EquiDepthHistogram h;
    h.build(uniform.data(), uniform.size(), uniform.size());
    check(h.bins() == 64, "64 bins by default", num(h.bins()));
    check(std::fabs(h.selectivity(2500.0, 7499.0) - 0.5) < 0.02, "the middle half of uniform data is ~0.5",
          std::to_string(h.selectivity(2500.0, 7499.0)));
    check(h.selectivity(-1e9, 1e9) > 0.999, "everything is ~1");
    check(h.selectivity(-1e9, -1.0) == 0.0, "a range below all data is 0");
    check(h.selectivity(1e5, 1e6) == 0.0, "a range above all data is 0");
    check(h.selectivity(5.0, 1.0) == 0.0, "an inverted range is 0");
    check(std::fabs(h.selectivity(0.0, std::numeric_limits<double>::infinity()) - 1.0) < 0.01,
          "an unbounded upper end works");
    bool monotone = true;
    for (std::size_t i = 1; i < h.edges().size(); ++i) {
        monotone = monotone && h.edges()[i] >= h.edges()[i - 1];
    }
    check(monotone, "the bin edges never decrease");

    // Unknown values dilute every estimate: only known rows can match a range.
    neo::EquiDepthHistogram half;
    half.build(uniform.data(), 5000, 10000);
    check(std::fabs(half.selectivity(-1e9, 1e9) - 0.5) < 0.01, "5,000 known of 10,000 caps the estimate at 0.5",
          std::to_string(half.selectivity(-1e9, 1e9)));

    // A value so common it fills whole bins is estimated as such.
    std::vector<double> spike;
    for (int i = 0; i < 2000; ++i) spike.push_back(1.0);
    for (int i = 0; i < 6000; ++i) spike.push_back(5.0);
    for (int i = 0; i < 2000; ++i) spike.push_back(9.0);
    neo::EquiDepthHistogram s;
    s.build(spike.data(), spike.size(), spike.size());
    check(std::fabs(s.selectivity(5.0, 5.0) - 0.6) < 0.03, "a point mass holding 60% of the rows is estimated at ~0.6",
          std::to_string(s.selectivity(5.0, 5.0)));

    const double one = 3.0;
    neo::EquiDepthHistogram single;
    single.build(&one, 1, 1);
    check(single.selectivity(3.0, 3.0) >= 0.0 && single.selectivity(0.0, 10.0) <= 1.0, "a single value is safe");
}

void testNameIndex() {
    std::printf("[names] prefix search\n");
    std::vector<ObjSpec> o(5);
    o[0].pdes = "433";
    o[0].name = "Eros";
    o[1].pdes = "99942";
    o[1].name = "Apophis";
    o[2].pdes = "2004 MN4";
    o[3].pdes = "2020 AB";
    o[4].pdes = "eros-twin"; // its designation starts like Eros's name
    o[4].name = "Eros II";
    const neo::Dataset dataset = buildDataset(o, {});
    neo::QueryEngine engine(dataset);
    engine.build();

    check(engine.findDesignation("433") == 0 && engine.findDesignation("nope") == neo::kInvalidRecord,
          "exact designation lookup");
    check(engine.searchNames("ero") == std::vector<std::uint32_t>({0, 4}), "'ero' finds both Eros objects, once each");
    check(engine.searchNames("EROS") == std::vector<std::uint32_t>({0, 4}), "case-insensitive");
    check(engine.searchNames("eros i") == std::vector<std::uint32_t>({4}), "a longer prefix narrows it");
    check(engine.searchNames("20") == std::vector<std::uint32_t>({2, 3}), "designation prefixes match too");
    check(engine.searchNames("apo") == std::vector<std::uint32_t>({1}), "a name prefix");
    check(engine.searchNames("zzz").empty(), "no match");
    check(engine.searchNames("20", 1).size() == 1, "the limit is honoured");
    std::string error;
    check(engine.indexes().names.checkInvariants(error), "the name index is sorted", error);
    check(engine.indexes().names.countPrefix("ero") >= 2, "the prefix count is available without reading matches");
}

// ---------------------------------------------------------------------------
// Synthetic data with heavy ties, unknowns and boundary values
// ---------------------------------------------------------------------------

neo::Dataset makeSynthetic(std::uint64_t seed, std::size_t n) {
    Rng r(seed);
    static const char* const words[] = {"Nemo", "Vesta", "Cerberus", "Icarus", "Toro", "Eros", "Midas", "Adonis"};
    static const char* const classes[] = {"APO", "APO", "APO", "ATE", "AMO", "AMO", "IEO", "", "JFc"};
    static const double boundaryDiameters[] = {0.01, 0.05, 0.14, 1.0};

    std::vector<ObjSpec> objects(n);
    for (std::size_t i = 0; i < n; ++i) {
        ObjSpec& s = objects[i];
        char buf[48];
        std::snprintf(buf, sizeof buf, "%04u %c%c%zu", 1990u + static_cast<unsigned>(i % 36),
                      static_cast<char>('A' + (i % 26)), static_cast<char>('A' + ((i / 26) % 26)), i);
        s.pdes = buf;
        if (r.below(100) < 8) {
            s.name = std::string(words[r.below(8)]) + std::to_string(r.below(40));
        }
        s.kind = r.below(100) < 10 ? neo::ObjectKind::Comet : neo::ObjectKind::Asteroid;
        if (r.below(100) < 25) {
            s.diameter = r.below(100) < 6 ? boundaryDiameters[r.below(4)]
                                          : 0.005 * (1 + std::floor(std::pow(r.unit(), 3.0) * 400.0));
        }
        if (r.below(100) < 95) {
            s.h = 14.0 + 0.5 * static_cast<double>(r.below(32));
        }
        if (r.below(100) < 90) {
            s.moid = 0.001 * static_cast<double>(r.below(500));
        }
        s.a = 0.5 + 0.05 * static_cast<double>(r.below(80));
        s.e = 0.01 * static_cast<double>(r.below(95));
        s.i = std::floor(std::pow(r.unit(), 1.6) * 61.0);
        const std::uint32_t nr = r.below(100);
        s.neo = nr < 85 ? std::optional<bool>(true) : (nr < 95 ? std::optional<bool>(false) : std::nullopt);
        const std::uint32_t pr = r.below(100);
        s.pha = pr < 20 ? std::optional<bool>(true) : (pr < 90 ? std::optional<bool>(false) : std::nullopt);
        s.orbitClass = classes[r.below(9)];
    }

    std::vector<AppSpec> approaches;
    const double first = jd("1950-01-01");
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t count = 0;
        if (r.below(100) >= 45) {
            count = 1;
            while (count < 6 && r.below(100) < 50) {
                ++count;
            }
        }
        double previous = first;
        for (std::size_t k = 0; k < count; ++k) {
            AppSpec a;
            a.object = i;
            a.jd = first + r.unit() * 73050.0;
            if (r.below(100) < 10) {
                char iso[16];
                std::snprintf(iso, sizeof iso, "%04u-01-01", 1950u + r.below(200));
                a.jd = jd(iso); // exactly on a year boundary
            } else if (k > 0 && r.below(100) < 10) {
                a.jd = previous; // the same instant twice for one object
            }
            previous = a.jd;
            a.dist = r.below(100) < 2 ? r.unit() * neo::kEarthRadiusAU : 1e-4 * static_cast<double>(r.below(501));
            a.vrel = 1.0 + 0.1 * static_cast<double>(r.below(391));
            approaches.push_back(a);
        }
    }
    return buildDataset(objects, approaches);
}

// ---------------------------------------------------------------------------
// Random queries
// ---------------------------------------------------------------------------

struct Samples {
    std::vector<double> diamMeasured, diamBest, h, moid, a, e, i, jd, dist, vel;
    std::vector<std::string> pdes, names, classes;
};

Samples collectSamples(const neo::Dataset& ds) {
    Samples s;
    for (const neo::AsteroidRecord& r : ds.records()) {
        const neo::Asteroid& o = r.object;
        if (o.physical.diameterKm) s.diamMeasured.push_back(*o.physical.diameterKm);
        if (const std::optional<double> b = o.physical.bestDiameterKm()) s.diamBest.push_back(*b);
        if (o.physical.absoluteMagnitudeH) s.h.push_back(*o.physical.absoluteMagnitudeH);
        if (o.orbital.moidAU) s.moid.push_back(*o.orbital.moidAU);
        s.a.push_back(o.orbital.semiMajorAxisAU);
        s.e.push_back(o.orbital.eccentricity);
        s.i.push_back(o.orbital.inclinationDeg);
        s.pdes.push_back(o.pdes);
        if (!o.name.empty()) s.names.push_back(o.name);
        if (!o.classification.orbitClass.empty()) s.classes.push_back(o.classification.orbitClass);
    }
    for (const neo::CloseApproach& c : ds.approaches()) {
        s.jd.push_back(c.jdTdb);
        s.dist.push_back(c.distanceAU);
        s.vel.push_back(c.relVelocityKms);
    }
    return s;
}

neo::Range pickRange(Rng& r, const std::vector<double>& s) {
    if (s.empty()) {
        return neo::Range::between(0.0, 1.0);
    }
    const double x = s[r.below(static_cast<std::uint32_t>(s.size()))];
    const double y = s[r.below(static_cast<std::uint32_t>(s.size()))];
    const double lo = std::min(x, y);
    const double hi = std::max(x, y);
    switch (r.below(12)) {
    case 0:  return neo::Range::atLeast(lo);
    case 1:  return neo::Range::atMost(hi);
    case 2:  return neo::Range::between(x, x);           // a point on an existing value
    case 3:  return neo::Range::between(hi + 1.0, hi + 2.0); // beyond all data: empty
    case 4:  return neo::Range::atMost(-1e30);           // below all data: empty
    default: return neo::Range::between(lo, hi);
    }
}

std::string mixedCase(Rng& r, std::string s) {
    for (char& c : s) {
        if (r.below(2) == 0) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return s;
}

neo::Query randomQuery(Rng& r, const Samples& s) {
    neo::Query q;
    if (r.below(100) < 3 && !s.pdes.empty()) {
        q.designation = r.below(4) == 0 ? "no such designation" : s.pdes[r.below(static_cast<std::uint32_t>(s.pdes.size()))];
    }
    if (r.below(100) < 7) {
        const std::vector<std::string>& pool = (r.below(2) == 0 && !s.names.empty()) ? s.names : s.pdes;
        if (!pool.empty() && r.below(6) != 0) {
            std::string base = pool[r.below(static_cast<std::uint32_t>(pool.size()))];
            base = base.substr(0, 1 + r.below(5));
            q.namePrefix = mixedCase(r, base);
        } else {
            q.namePrefix = "zz";
        }
    }
    if (r.below(100) < 8) q.kind = r.below(2) == 0 ? neo::ObjectKind::Asteroid : neo::ObjectKind::Comet;
    if (r.below(100) < 12) q.neo = r.below(2) == 0 ? neo::TriState::Yes : neo::TriState::No;
    if (r.below(100) < 12) q.pha = r.below(2) == 0 ? neo::TriState::Yes : neo::TriState::No;
    if (r.below(100) < 10 && !s.classes.empty()) {
        const std::uint32_t k = 1 + r.below(3);
        for (std::uint32_t j = 0; j < k; ++j) {
            q.orbitClasses.push_back(r.below(8) == 0 ? "NOPE" : s.classes[r.below(static_cast<std::uint32_t>(s.classes.size()))]);
        }
    }
    if (r.below(100) < 22) {
        const std::uint32_t m = r.below(3);
        q.diameterMode = m == 0 ? neo::DiameterMode::MeasuredOnly
                         : m == 1 ? neo::DiameterMode::MeasuredOrEstimated
                                  : neo::DiameterMode::IncludeUnknown;
        q.diameterKm = pickRange(r, q.diameterMode == neo::DiameterMode::MeasuredOnly ? s.diamMeasured : s.diamBest);
    } else {
        q.diameterMode = static_cast<neo::DiameterMode>(r.below(3)); // matters for sorting even without a filter
    }
    if (r.below(100) < 12) q.absoluteMagnitude = pickRange(r, s.h);
    if (r.below(100) < 8) q.moidAU = pickRange(r, s.moid);
    if (r.below(100) < 6) q.semiMajorAxisAU = pickRange(r, s.a);
    if (r.below(100) < 6) q.eccentricity = pickRange(r, s.e);
    if (r.below(100) < 6) q.inclinationDeg = pickRange(r, s.i);

    if (r.below(100) < 25) {
        if (r.below(100) < 40) {
            // A window aligned to whole years, which is where the year buckets are exact.
            char a[16];
            char b[16];
            const unsigned y0 = 1950u + r.below(200);
            const unsigned y1 = y0 + 1 + r.below(12);
            std::snprintf(a, sizeof a, "%04u-01-01", y0);
            std::snprintf(b, sizeof b, "%04u-01-01", y1);
            q.dateJd = r.below(2) == 0 ? neo::Range::between(jd(a), jd(b)) : neo::Range::between(jd(a), jd(b) - 1e-9);
        } else {
            q.dateJd = pickRange(r, s.jd);
        }
    }
    if (r.below(100) < 25) q.distanceAU = pickRange(r, s.dist);
    if (r.below(100) < 15) q.velocityKms = pickRange(r, s.vel);
    if (r.below(100) < 5) q.grazing = r.below(2) == 0 ? neo::TriState::Yes : neo::TriState::No;

    if (r.below(100) < 55) {
        q.sortBy = static_cast<neo::SortField>(1 + r.below(10));
        q.direction = r.below(2) == 0 ? neo::SortDirection::Ascending : neo::SortDirection::Descending;
    }
    if (r.below(100) < 40) {
        q.topK = 1 + r.below(40);
    }
    return q;
}

std::string diffResults(const neo::QueryResult& a, const neo::QueryResult& b) {
    if (a.ok != b.ok) return "the ok flags differ";
    if (a.totalObjects != b.totalObjects) return "totalObjects " + num(a.totalObjects) + " vs " + num(b.totalObjects);
    if (a.totalApproaches != b.totalApproaches) {
        return "totalApproaches " + num(a.totalApproaches) + " vs " + num(b.totalApproaches);
    }
    if (a.rows.size() != b.rows.size()) return "row count " + num(a.rows.size()) + " vs " + num(b.rows.size());
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        const neo::ResultRow& x = a.rows[i];
        const neo::ResultRow& y = b.rows[i];
        if (x.object != y.object) return "row " + num(i) + ": object " + num(x.object) + " vs " + num(y.object);
        if (x.sortKeyKnown != y.sortKeyKnown || (x.sortKeyKnown && x.sortKey != y.sortKey)) {
            return "row " + num(i) + ": sort key differs";
        }
        if (x.approachCount != y.approachCount) return "row " + num(i) + ": approach count differs";
        for (std::uint32_t k = 0; k < x.approachCount; ++k) {
            if (a.approachPool[x.approachBegin + k] != b.approachPool[y.approachBegin + k]) {
                return "row " + num(i) + ": approach list differs";
            }
        }
    }
    return std::string();
}

struct OracleStats {
    std::size_t queries = 0;
    std::size_t mismatches = 0;
    std::size_t nonEmpty = 0;
    std::size_t empty = 0;
    std::size_t truncated = 0;      // top-K cut the result
    std::size_t withApproach = 0;
    std::size_t sorted = 0;
    std::size_t forcedHit[9] = {};  // times each access path really drove a query
    std::size_t plannedPick[9] = {};
    double msNaive = 0.0;
    double msFixed = 0.0;
    double msPlanned = 0.0;
};

OracleStats runOracle(const char* label, const neo::Dataset& ds, std::size_t count, std::uint64_t seed) {
    OracleStats st;
    neo::QueryEngine engine(ds);
    engine.build();
    const Samples samples = collectSamples(ds);
    Rng rng(seed);
    std::size_t reported = 0;

    for (std::size_t n = 0; n < count; ++n) {
        const neo::Query q = randomQuery(rng, samples);
        ++st.queries;

        const neo::QueryResult naive = engine.run(q, neo::ExecMode::Naive);
        const neo::QueryResult fixed = engine.run(q, neo::ExecMode::FixedOrder);
        const neo::QueryResult planned = engine.run(q, neo::ExecMode::Planned);
        st.msNaive += naive.stats.totalMs;
        st.msFixed += fixed.stats.totalMs;
        st.msPlanned += planned.stats.totalMs;
        st.plannedPick[static_cast<int>(planned.stats.driverAccess)]++;

        auto verify = [&](const neo::QueryResult& got, const char* what) {
            const std::string why = diffResults(naive, got);
            if (!why.empty()) {
                ++st.mismatches;
                if (reported < 5) {
                    ++reported;
                    std::printf("  MISMATCH [%s] %s: %s\n    query: %s\n", label, what, why.c_str(),
                                neo::describe(q).c_str());
                }
            }
        };
        if (!naive.ok) {
            ++st.mismatches;
            if (reported < 5) {
                ++reported;
                std::printf("  INVALID QUERY [%s]: %s\n    query: %s\n", label,
                            naive.errors.empty() ? "?" : naive.errors[0].c_str(), neo::describe(q).c_str());
            }
            continue;
        }
        verify(fixed, "fixed-order vs naive");
        verify(planned, "planned vs naive");

        // Every access path, forced, must also agree.
        for (const neo::Access access : kAllAccess) {
            const neo::QueryResult forced = engine.run(q, neo::ExecMode::Planned, access);
            if (forced.stats.driverAccess == access) {
                ++st.forcedHit[static_cast<int>(access)];
            }
            verify(forced, neo::toString(access));
        }

        (naive.totalObjects > 0 ? st.nonEmpty : st.empty)++;
        st.truncated += naive.rows.size() < naive.totalObjects ? 1u : 0u;
        st.withApproach += neo::hasApproachFilter(q) ? 1u : 0u;
        st.sorted += q.sortBy != neo::SortField::None ? 1u : 0u;
    }
    return st;
}

void reportOracle(const char* label, const OracleStats& st, bool requireCoverage) {
    std::printf("  %s: %zu queries, %zu mismatches; %zu non-empty, %zu empty, %zu top-K cut, %zu with approach "
                "conditions, %zu sorted\n",
                label, st.queries, st.mismatches, st.nonEmpty, st.empty, st.truncated, st.withApproach, st.sorted);
    std::printf("    paths that drove a forced query:");
    for (const neo::Access a : kAllAccess) {
        std::printf(" %s=%zu", neo::toString(a), st.forcedHit[static_cast<int>(a)]);
    }
    std::printf("\n    planner's own choices:");
    for (const neo::Access a : kAllAccess) {
        std::printf(" %s=%zu", neo::toString(a), st.plannedPick[static_cast<int>(a)]);
    }
    std::printf("\n    total time: naive %.1f ms, fixed-order %.1f ms, planned %.1f ms\n", st.msNaive, st.msFixed,
                st.msPlanned);

    check(st.mismatches == 0, std::string(label) + ": planned == fixed-order == naive on every query, on every path",
          num(st.mismatches) + " mismatches");
    if (requireCoverage) {
        check(st.nonEmpty > st.queries / 10, std::string(label) + ": a healthy share of queries return rows");
        check(st.empty > st.queries / 20, std::string(label) + ": and a healthy share return nothing");
        check(st.truncated > 0, std::string(label) + ": top-K truncation is exercised");
        for (const neo::Access a : kAllAccess) {
            check(st.forcedHit[static_cast<int>(a)] > 0,
                  std::string(label) + ": the '" + neo::toString(a) + "' path was really exercised");
        }
    }
}

void testOracleFixtures() {
    std::printf("[oracle] fixtures\n");
    std::vector<neo::Asteroid> objects;
    neo::ValidationReport report;
    auto read = [](const char* name) {
        std::ifstream in(g_fixtureDir + "/" + name, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    neo::parseSbdbObjects(read("sbdb_neo_page.json"), objects, report);
    neo::Dataset dataset;
    dataset.setObjects(std::move(objects));
    std::vector<neo::ParsedApproach> rows;
    neo::parseCadApproaches(read("cad_pha_window.json"), rows, report);
    neo::parseCadApproaches(read("cad_apophis.json"), rows, report);
    dataset.joinApproaches(std::move(rows));
    check(dataset.objectCount() == 23 && dataset.approachCount() == 47, "the fixture dataset loads");

    const OracleStats st = runOracle("fixtures", dataset, 1500, 0xF1F7u);
    reportOracle("fixtures", st, false);
}

void testOracleSynthetic() {
    std::printf("[oracle] synthetic data with ties, unknowns and boundary values\n");
    const neo::Dataset dataset = makeSynthetic(0x5EED1234u, 3000);
    std::printf("  %zu objects, %zu approaches\n", dataset.objectCount(), dataset.approachCount());
    const OracleStats st = runOracle("synthetic", dataset, 10000, 0xA11CE5u);
    reportOracle("synthetic", st, true);
}

// ---------------------------------------------------------------------------
// The planner
// ---------------------------------------------------------------------------

void testPlanner() {
    std::printf("[planner] driver choice, EXPLAIN and estimate quality\n");
    const neo::Dataset dataset = makeSynthetic(0xC0DEu, 6000);
    neo::QueryEngine engine(dataset);
    engine.build();

    auto driverOf = [&engine](const neo::Query& q) { return engine.run(q, neo::ExecMode::Planned).stats.driverAccess; };

    neo::Query q;
    q.designation = dataset.records()[123].object.pdes;
    check(driverOf(q) == neo::Access::HashLookup, "an exact designation drives from the hash map");

    q = neo::Query();
    q.namePrefix = "zzzz";
    check(driverOf(q) == neo::Access::NamePrefix, "a prefix that matches nothing drives from the name index (exactly 0)");

    q = neo::Query();
    q.pha = neo::TriState::Yes;
    check(driverOf(q) == neo::Access::ScanObjects, "a flag alone has no index: it scans the objects");

    // A narrow window and a wide one: the planner follows the selective one.
    q = neo::Query();
    q.distanceAU = neo::Range::atMost(2e-4); // a tiny slice of approach distances
    q.dateJd = neo::Range::between(jd("1950-01-01"), jd("2149-01-01")); // nearly everything
    check(driverOf(q) == neo::Access::ApproachView, "with a rare distance and a wide date window it drives from distance");
    q = neo::Query();
    q.distanceAU = neo::Range::atMost(0.0499); // nearly everything
    q.dateJd = neo::Range::between(jd("2030-03-01"), jd("2030-03-15"));
    const neo::Access narrowDate = driverOf(q);
    check(narrowDate == neo::Access::DateTree || narrowDate == neo::Access::YearBuckets,
          "with a wide distance and a two-week window it drives from the date", neo::toString(narrowDate));

    // A whole-year-aligned window is the case the year buckets answer exactly.
    q = neo::Query();
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2040-01-01"));
    check(driverOf(q) == neo::Access::YearBuckets, "a year-aligned window is served by the year buckets",
          neo::toString(driverOf(q)));
    // A window inside one year is not: the AVL tree is exact where buckets over-read.
    q.dateJd = neo::Range::between(jd("2030-03-01"), jd("2030-03-10"));
    check(driverOf(q) == neo::Access::DateTree, "a window inside one year is served by the AVL tree",
          neo::toString(driverOf(q)));

    q = neo::Query();
    q.diameterKm = neo::Range::atLeast(1.0);
    q.diameterMode = neo::DiameterMode::IncludeUnknown;
    check(driverOf(q) == neo::Access::SizeBuckets, "unknown-included diameters can only use the size buckets");

    // EXPLAIN carries the estimate, the actual, the order and the alternatives.
    q = neo::Query();
    q.pha = neo::TriState::Yes;
    q.dateJd = neo::Range::between(jd("2030-01-01"), jd("2040-01-01"));
    q.distanceAU = neo::Range::atMost(0.02);
    q.sortBy = neo::SortField::Distance;
    q.topK = 10;
    const neo::QueryResult r = engine.run(q);
    const std::string text = r.stats.explain();
    check(text.find("EXPLAIN (planned)") == 0, "EXPLAIN opens with the mode");
    for (const char* needle : {"driver", "estimated", "actual", "steps", "considered", "work", "result", "time"}) {
        check(text.find(needle) != std::string::npos, std::string("EXPLAIN mentions '") + needle + "'");
    }
    check(r.stats.actualCandidates > 0 && r.stats.estCandidates > 0.0, "the estimate and the actual count are both recorded");
    check(r.stats.considered.size() >= 3 && r.stats.considered[0].chosen, "the alternatives are listed, cheapest first, with the chosen one marked");
    check(!r.stats.steps.empty(), "the residual steps are recorded");
    const std::string naiveText = engine.run(q, neo::ExecMode::Naive).stats.explain();
    check(naiveText.find("full scan") != std::string::npos, "the naive EXPLAIN says it is a full scan");
    check(engine.run(q, neo::ExecMode::FixedOrder).stats.explain().find("fixed-order") != std::string::npos,
          "the fixed-order EXPLAIN is labelled");

    // The estimate is close to the truth: measure it on many random ranges.
    const Samples s = collectSamples(dataset);
    Rng rng(0xE57u);
    double worst = 0.0;
    double total = 0.0;
    std::size_t trials = 0;
    struct Field {
        const char* name;
        const std::vector<double>* samples;
        const neo::EquiDepthHistogram* hist;
        bool approach;
        int kind; // which query field
    };
    const neo::IndexSet& ix = engine.indexes();
    const Field fields[] = {{"H", &s.h, &ix.histH, false, 0},         {"MOID", &s.moid, &ix.histMoid, false, 1},
                            {"a", &s.a, &ix.histA, false, 2},         {"e", &s.e, &ix.histE, false, 3},
                            {"i", &s.i, &ix.histI, false, 4},         {"date", &s.jd, &ix.histDate, true, 5},
                            {"dist", &s.dist, &ix.histDist, true, 6}, {"v_rel", &s.vel, &ix.histVrel, true, 7}};
    for (const Field& f : fields) {
        for (int t = 0; t < 150; ++t) {
            const neo::Range range = pickRange(rng, *f.samples);
            const double lo = range.lo ? *range.lo : -std::numeric_limits<double>::infinity();
            const double hi = range.hi ? *range.hi : std::numeric_limits<double>::infinity();
            std::size_t actual = 0;
            for (const double v : *f.samples) {
                actual += (v >= lo && v <= hi) ? 1u : 0u;
            }
            const double universe = static_cast<double>(f.approach ? dataset.approachCount() : dataset.objectCount());
            const double est = f.hist->selectivity(lo, hi);
            const double err = std::fabs(est - static_cast<double>(actual) / universe);
            worst = std::max(worst, err);
            total += err;
            ++trials;
        }
    }
    const double mean = total / static_cast<double>(trials);
    std::printf("  histogram estimate vs actual over %zu random ranges: mean error %.4f, worst %.4f of the universe\n",
                trials, mean, worst);
    check(mean < 0.012, "the mean estimate error is about one percent of the rows", std::to_string(mean));
    check(worst < 0.05, "and never more than a few bins", std::to_string(worst));

    // The index report: every index has a time and a size.
    check(!engine.buildReport().empty(), "the build report lists the indexes");
    bool sized = true;
    bool named = true;
    for (const neo::IndexBuildInfo& b : engine.buildReport()) {
        sized = sized && b.bytes > 0;
        named = named && !b.name.empty();
    }
    check(sized && named, "every index reports a name and its memory");
    check(engine.totalBuildMs() > 0.0 && engine.totalIndexBytes() > 0, "the totals are recorded");
    std::printf("  indexes for %zu objects / %zu approaches: %.1f ms, %.2f MB\n", dataset.objectCount(),
                dataset.approachCount(), engine.totalBuildMs(),
                static_cast<double>(engine.totalIndexBytes()) / (1024.0 * 1024.0));
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

void testThreads() {
    std::printf("[threads] concurrent queries on one engine\n");
    const neo::Dataset dataset = makeSynthetic(0x7EAD5u, 3000);
    neo::QueryEngine engine(dataset);
    engine.build();
    const Samples s = collectSamples(dataset);

    Rng rng(0xBEEFu);
    std::vector<neo::Query> queries;
    std::vector<neo::QueryResult> expected;
    for (int i = 0; i < 300; ++i) {
        queries.push_back(randomQuery(rng, s));
        expected.push_back(engine.run(queries.back(), neo::ExecMode::Planned));
    }

    std::atomic<int> wrong{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            for (int round = 0; round < 3; ++round) {
                for (std::size_t i = static_cast<std::size_t>(t); i < queries.size(); i += 1) {
                    const neo::QueryResult got = engine.run(queries[i], neo::ExecMode::Planned);
                    if (!diffResults(expected[i], got).empty()) {
                        ++wrong;
                    }
                }
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    check(wrong.load() == 0, "4 threads x 900 queries return exactly the sequential answers", num(static_cast<std::size_t>(wrong.load())));
}

// ---------------------------------------------------------------------------
// The real dataset
// ---------------------------------------------------------------------------

void testReal() {
    std::printf("[real] the query engine over data/neo.db, if it exists\n");
    const char* candidates[] = {"data/neo.db", "../data/neo.db", "../../data/neo.db"};
    std::string path;
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) {
            path = c;
            break;
        }
    }
    if (path.empty()) {
        std::printf("  skipped: no neo.db (run neo_ingest to create one)\n");
        return;
    }
    neo::Dataset dataset;
    neo::DatabaseMeta meta;
    const neo::DbStatus status = neo::loadDatabase(path, dataset, meta);
    if (!status) {
        std::printf("  skipped: %s\n", status.error.c_str());
        return;
    }
    std::printf("  %zu objects, %zu approaches\n", dataset.objectCount(), dataset.approachCount());

    {
        neo::QueryEngine engine(dataset);
        engine.build();
        std::printf("\n  index build: %.1f ms, %.2f MB total\n", engine.totalBuildMs(),
                    static_cast<double>(engine.totalIndexBytes()) / (1024.0 * 1024.0));
        for (const neo::IndexBuildInfo& b : engine.buildReport()) {
            std::printf("    %-46s %8zu entries %8.3f MB %8.2f ms\n", b.name.c_str(), b.entries,
                        static_cast<double>(b.bytes) / (1024.0 * 1024.0), b.buildMs);
        }
        check(engine.findDesignation("433") != neo::kInvalidRecord, "433 Eros is found by designation");
        check(!engine.searchNames("apoph").empty(), "Apophis is found by name prefix");
        std::printf("\n");
    }

    const OracleStats st = runOracle("real neo.db", dataset, 700, 0x2EA1u);
    reportOracle("real neo.db", st, false);
}

} // namespace

int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : NEO_FIXTURES_DIR;

    testValidation();
    testSemantics();
    testSorting();
    testSameRowSemantics();
    testHistogram();
    testNameIndex();
    testPlanner();
    testOracleFixtures();
    testOracleSynthetic();
    testThreads();
    testReal();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
