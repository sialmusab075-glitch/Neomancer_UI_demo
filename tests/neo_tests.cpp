// NEO data-layer tests: JSON parsing against saved JPL fixtures, missing-field
// handling, the designation join, and the date helpers. No network: every
// fixture lives in tests/fixtures/ with a .meta.json recording its query URL,
// fetch date and API signature version.

#include "neo/ingest/CadParser.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/model/Asteroid.h"
#include "neo/model/Dataset.h"
#include "neo/model/JulianDate.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_fixtureDir;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string fmt(const char* f, double a, double b = 0.0) {
    char buf[192];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

std::string readFixture(const char* name) {
    const std::string path = g_fixtureDir + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("  FAIL  cannot open fixture %s\n", path.c_str());
        ++g_failures;
        return std::string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const neo::Asteroid* findObject(const std::vector<neo::Asteroid>& v, const char* pdes) {
    for (const neo::Asteroid& a : v) {
        if (a.pdes == pdes) {
            return &a;
        }
    }
    return nullptr;
}

// --- SBDB ------------------------------------------------------------------

std::vector<neo::Asteroid> g_objects;

void testSbdbParsing() {
    std::printf("[sbdb] parse the saved SBDB page\n");
    neo::ValidationReport report;
    const neo::ParseStatus status = neo::parseSbdbObjects(readFixture("sbdb_neo_page.json"), g_objects, report);
    check(status.ok, "SBDB page parses", status.error);
    check(report.signatureVersion == neo::kSbdbApiVersion, "signature version recorded", report.signatureVersion);
    check(g_objects.size() == 23, "23 objects", fmt("got %.0f", static_cast<double>(g_objects.size())));
    check(report.rowsAccepted == 23 && report.rowsRejected == 0, "every row accepted");
    check(report.nullCount("diameter") == 15, "null diameter count",
          fmt("got %.0f", static_cast<double>(report.nullCount("diameter"))));

    // A numbered asteroid with a measured diameter and full-precision elements.
    const neo::Asteroid* eros = findObject(g_objects, "433");
    check(eros != nullptr, "433 Eros present");
    if (eros != nullptr) {
        check(eros->name == "Eros" && eros->spkid == "20000433", "name and spkid", eros->spkid);
        check(eros->classification.kind == neo::ObjectKind::Asteroid && eros->classification.numbered,
              "kind 'an' -> numbered asteroid");
        check(eros->classification.isNEO.value_or(false) && eros->classification.isPHA.has_value() &&
                  !*eros->classification.isPHA,
              "NEO yes, PHA no");
        check(eros->classification.orbitClass == "AMO", "orbit class AMO");
        check(near(eros->orbital.eccentricity, 0.2228779627700761, 1e-15), "full-precision eccentricity");
        check(near(eros->orbital.epochJdTdb, 2461200.5, 1e-9), "epoch JD");
        check(near(eros->orbital.meanMotionDegPerDay, 0.5597046347038453, 1e-15), "mean motion from SBDB 'n'");
        check(eros->physical.diameterKm.has_value() && near(*eros->physical.diameterKm, 16.84, 1e-9),
              "measured diameter 16.84 km");
        check(eros->orbital.propagationSupported(), "elliptical orbit is propagatable");
    }

    // Unknown is not zero: no diameter, no albedo, but H is there.
    const neo::Asteroid* noDiameter = findObject(g_objects, "136795");
    check(noDiameter != nullptr, "136795 present");
    if (noDiameter != nullptr) {
        check(!noDiameter->physical.diameterKm.has_value(), "null diameter stays empty");
        check(!noDiameter->physical.albedo.has_value(), "null albedo stays empty");
        check(noDiameter->physical.absoluteMagnitudeH.has_value(), "H present");
        const std::optional<double> est = noDiameter->physical.estimatedDiameterKm();
        // 1329 / sqrt(0.14) * 10^(-18.11/5)
        const double expected = 1329.0 / std::sqrt(0.14) * std::pow(10.0, -18.11 / 5.0);
        check(est.has_value() && near(*est, expected, 1e-9), "H-based diameter estimate",
              fmt("got %.6f want %.6f", est.value_or(-1.0), expected));
        check(noDiameter->physical.bestDiameterKm().has_value(), "best diameter falls back to the estimate");
    }

    // A comet: no H at all, and no PHA flag.
    const neo::Asteroid* encke = findObject(g_objects, "2P");
    check(encke != nullptr, "2P/Encke present");
    if (encke != nullptr) {
        check(encke->classification.kind == neo::ObjectKind::Comet, "kind 'cn' -> comet");
        check(!encke->physical.absoluteMagnitudeH.has_value(), "comet has no H");
        check(!encke->physical.estimatedDiameterKm().has_value(), "no H -> no estimate");
        check(encke->physical.diameterKm.has_value() && near(*encke->physical.diameterKm, 4.8, 1e-9),
              "comet still has a measured diameter");
        check(!encke->classification.isPHA.has_value(), "null PHA flag is not 'false'");
        check(encke->classification.isNEO.value_or(false), "comet is flagged NEO");
        check(encke->orbital.conditionCode.has_value() && *encke->orbital.conditionCode == 1, "condition code 1");
    }

    // Hyperbolic: stored, searchable, but never propagated.
    const neo::Asteroid* oumuamua = findObject(g_objects, "2017 U1");
    check(oumuamua != nullptr, "1I/'Oumuamua present");
    if (oumuamua != nullptr) {
        check(oumuamua->orbital.eccentricity > 1.0, "e > 1");
        check(!oumuamua->orbital.propagationSupported(), "hyperbolic orbit is unsupported for propagation");
        check(oumuamua->orbital.semiMajorAxisAU < 0.0, "negative semi-major axis kept as published");
        check(!oumuamua->orbital.periodDays.has_value(), "no orbital period");
        check(!oumuamua->classification.isNEO.has_value(), "null NEO flag is not 'false'");
        check(!oumuamua->orbital.conditionCode.has_value(), "null condition code stays empty");
    }
}

void testSbdbRejections() {
    std::printf("[sbdb] signature check and malformed payloads\n");

    // Requirement: an unexpected signature version must be refused, loudly.
    {
        std::vector<neo::Asteroid> out;
        neo::ValidationReport report;
        const neo::ParseStatus status = neo::parseSbdbObjects(readFixture("sbdb_bad_signature.json"), out, report);
        check(!status.ok, "wrong signature version is rejected");
        check(out.empty(), "no objects are produced from a rejected payload");
        const bool explains = status.error.find("9.9") != std::string::npos &&
                              status.error.find(neo::kSbdbApiVersion) != std::string::npos &&
                              status.error.find("sbdb_query") != std::string::npos;
        check(explains, "error names both versions and the API doc", status.error);
        check(report.signatureVersion == "9.9", "the offending version is recorded for the log");
    }
    // The same payload parses once the parser is told to expect that version,
    // which proves the rejection came from the check and not from bad data.
    {
        std::vector<neo::Asteroid> out;
        neo::ValidationReport report;
        neo::SbdbParseOptions options;
        options.expectedVersion = "9.9";
        const neo::ParseStatus status =
            neo::parseSbdbObjects(readFixture("sbdb_bad_signature.json"), out, report, options);
        check(status.ok && out.size() == 2, "same payload parses when the version is expected", status.error);
    }
    {
        std::vector<neo::Asteroid> out;
        neo::ValidationReport report;
        const neo::ParseStatus status = neo::parseSbdbObjects("{ this is not json", out, report);
        check(!status.ok, "malformed JSON is rejected");
    }
    {
        std::vector<neo::Asteroid> out;
        neo::ValidationReport report;
        const neo::ParseStatus status =
            neo::parseSbdbObjects(R"({"code":"400","message":"bad character(s) in sb-cdata"})", out, report);
        check(!status.ok && status.error.find("400") != std::string::npos, "API error document is reported",
              status.error);
    }
    {
        // Signature present, but no data: a count-only response is valid.
        std::vector<neo::Asteroid> out;
        neo::ValidationReport report;
        const neo::ParseStatus status = neo::parseSbdbObjects(
            R"({"signature":{"version":"1.0","source":"x"},"count":0})", out, report);
        check(status.ok && out.empty(), "count-only response is accepted with no rows", status.error);
    }
    {
        // Rows that cannot be keyed or propagated are rejected, not guessed at.
        const std::string json =
            R"({"signature":{"version":"1.0","source":"x"},"fields":["pdes","epoch","e","a","q","i","om","w","ma","n"],)"
            R"("data":[[null,"2461200.5",".1","1.0",".9","1","2","3","4",".5"],)"
            R"(["good","2461200.5",".1","1.0",".9","1","2","3","4",".5"],)"
            R"(["noorbit",null,".1","1.0",".9","1","2","3","4",".5"]],"count":3})";
        std::vector<neo::Asteroid> out;
        neo::ValidationReport report;
        const neo::ParseStatus status = neo::parseSbdbObjects(json, out, report);
        check(status.ok, "payload with bad rows still parses", status.error);
        check(out.size() == 1 && out[0].pdes == "good", "only the usable row is kept");
        check(report.rowsRejected == 2, "both bad rows are counted");
        check(report.rejectedSamples.size() == 2 &&
                  report.rejectedSamples[1].reason.find("incomplete orbit") != std::string::npos,
              "rejection reasons are recorded", report.rejectedSamples.empty() ? "" : report.rejectedSamples[1].reason);
    }
}

// --- CAD -------------------------------------------------------------------

std::vector<neo::ParsedApproach> g_approaches;

void testCadParsing() {
    std::printf("[cad] parse the saved close-approach pages\n");
    neo::ValidationReport window;
    neo::ParseStatus status = neo::parseCadApproaches(readFixture("cad_pha_window.json"), g_approaches, window);
    check(status.ok, "CAD window page parses", status.error);
    check(window.signatureVersion == neo::kCadApiVersion, "CAD signature version", window.signatureVersion);
    check(g_approaches.size() == 22, "22 rows in the window page",
          fmt("got %.0f", static_cast<double>(g_approaches.size())));
    check(window.nullCount("diameter") == 17, "null diameters counted in the window page");

    neo::ValidationReport apophis;
    status = neo::parseCadApproaches(readFixture("cad_apophis.json"), g_approaches, apophis);
    check(status.ok, "CAD single-object page parses", status.error);
    check(apophis.rowsAccepted == 28, "28 Apophis approaches",
          fmt("got %.0f", static_cast<double>(apophis.rowsAccepted)));
    check(g_approaches.size() == 50, "rows from both pages accumulate");

    // The 2029 encounter, checked against the published values.
    const neo::ParsedApproach* closest = nullptr;
    for (const neo::ParsedApproach& p : g_approaches) {
        if (p.designation == "99942" && (closest == nullptr || p.approach.distanceAU < closest->approach.distanceAU)) {
            closest = &p;
        }
    }
    check(closest != nullptr, "Apophis rows are present");
    if (closest != nullptr) {
        check(near(closest->approach.distanceAU, 0.000254090910419299, 1e-18), "2029 nominal distance");
        check(near(closest->approach.jdTdb, 2462240.407091969, 1e-9), "2029 approach time (JD TDB)");
        check(neo::formatJulianDate(closest->approach.jdTdb).substr(0, 10) == "2029-04-13", "2029-04-13",
              neo::formatJulianDate(closest->approach.jdTdb));
        check(near(closest->approach.relVelocityKms, 7.42253895678452, 1e-12), "relative velocity");
        check(closest->approach.vInfinityKms.has_value() &&
                  *closest->approach.vInfinityKms < closest->approach.relVelocityKms,
              "v_inf is present and below v_rel");
        check(!closest->approach.distRangeDerived, "real CAD rows carry their own 3-sigma bounds");
        check(closest->approach.diameterKm.has_value() && near(*closest->approach.diameterKm, 0.34, 1e-9),
              "CAD diameter column read");
        check(closest->approach.distanceMinAU <= closest->approach.distanceAU &&
                  closest->approach.distanceAU <= closest->approach.distanceMaxAU,
              "nominal distance lies inside the 3-sigma bounds");
    }

    // Missing optional columns must not become zeros.
    {
        const std::string json =
            R"({"signature":{"version":"1.5","source":"x"},"fields":["des","jd","dist","v_rel"],)"
            R"("data":[["2020 AA","2458849.5","0.01","12.5"]],"count":1})";
        std::vector<neo::ParsedApproach> out;
        neo::ValidationReport report;
        const neo::ParseStatus s = neo::parseCadApproaches(json, out, report);
        check(s.ok && out.size() == 1, "CAD page without optional columns parses", s.error);
        if (out.size() == 1) {
            check(!out[0].approach.diameterKm.has_value(), "absent diameter column stays empty");
            check(!out[0].approach.absoluteMagnitudeH.has_value(), "absent H column stays empty");
            check(near(out[0].approach.distanceMinAU, 0.01, 1e-12), "dist_min falls back to the nominal distance");
            check(near(out[0].approach.distanceMaxAU, 0.01, 1e-12), "dist_max falls back to the nominal distance");
            check(out[0].approach.distRangeDerived, "the derived distance range is flagged on the row");
            check(report.derivedDistanceRanges == 1, "and counted in the validation report");
            check(!out[0].approach.vInfinityKms.has_value(),
                  "absent v_inf stays empty: it is a different quantity from v_rel");
            check(report.toString().find("derived dist_min/dist_max") != std::string::npos,
                  "the report text names the derived rows");
        }
    }
    {
        std::vector<neo::ParsedApproach> out;
        neo::ValidationReport report;
        const neo::ParseStatus s = neo::parseCadApproaches(readFixture("cad_apophis.json"), out, report,
                                                           [] {
                                                               neo::CadParseOptions o;
                                                               o.expectedVersion = "0.9";
                                                               return o;
                                                           }());
        check(!s.ok, "CAD signature version is checked too");
    }
}

// --- join ------------------------------------------------------------------

void testJoin() {
    std::printf("[join] CAD designations onto SBDB objects (pdes)\n");
    neo::Dataset dataset;
    std::vector<neo::Asteroid> objects = g_objects; // keep the parsed copy for later checks
    dataset.setObjects(std::move(objects));
    check(dataset.objectCount() == 23, "23 objects in the dataset");
    check(dataset.duplicatesDropped() == 0, "no duplicate designations");

    std::vector<neo::ParsedApproach> rows = g_approaches;
    const neo::JoinReport report = dataset.joinApproaches(std::move(rows));
    check(report.rowsSeen == 50, "50 rows offered to the join");
    check(report.rowsUnmatched == 3, "3 rows have no parent object",
          fmt("got %.0f", static_cast<double>(report.rowsUnmatched)));
    check(report.distinctUnmatched == 3, "from 3 distinct designations");
    check(report.rowsMatched == 47, "47 rows matched");
    check(dataset.approachCount() == 47, "only matched rows are stored");

    bool sawExpected = false;
    for (const auto& s : report.unmatchedSamples) {
        sawExpected = sawExpected || s.first == "2020 FM6";
    }
    check(sawExpected, "the unmatched designations are named in the report");
    check(report.toString().find("unmatched") != std::string::npos, "report text mentions unmatched rows");

    // One object with many approaches: the range must be contiguous and sorted.
    const std::uint32_t apophis = dataset.find("99942");
    check(apophis != neo::kInvalidRecord, "Apophis found by designation");
    const neo::ApproachSpan span = dataset.approachesOf(apophis);
    check(span.count == 28, "28 approaches for one object", fmt("got %.0f", static_cast<double>(span.count)));
    bool ordered = true;
    for (std::size_t i = 1; i < span.count; ++i) {
        ordered = ordered && span[i - 1].jdTdb <= span[i].jdTdb;
        ordered = ordered && span[i].objectIndex == apophis;
    }
    check(ordered, "approaches are in date order and all belong to the object");

    // Every stored approach points back at the record that owns it.
    bool consistent = true;
    for (std::uint32_t i = 0; i < dataset.records().size(); ++i) {
        const neo::ApproachSpan s = dataset.approachesOf(i);
        for (std::size_t k = 0; k < s.count; ++k) {
            consistent = consistent && s[k].objectIndex == i;
        }
    }
    check(consistent, "every approach range is owned by its record");

    const std::uint32_t bySpk = dataset.findBySpkId("20000433");
    check(bySpk != neo::kInvalidRecord && dataset.records()[bySpk].object.pdes == "433",
          "secondary SPK-ID index resolves to the same record");
    check(dataset.find("1998 QE2") == neo::kInvalidRecord, "an absent designation returns kInvalidRecord");
    check(dataset.approachesOf(dataset.find("2P")).empty(), "an object with no approaches has an empty span");
}

// --- dates -----------------------------------------------------------------

void testJulianDates() {
    std::printf("[time] Julian Date helpers\n");
    check(near(neo::daysSinceJ2000(neo::kJ2000Jd), 0.0, 1e-12), "J2000 is day zero");
    check(near(neo::julianDateFromDaysSinceJ2000(0.0), 2451545.0, 1e-12), "J2000 JD");
    check(near(neo::daysSinceJ2000(2462240.407091969), 10695.407091969, 1e-9), "2029 encounter in sim days");

    // The CLI date options and their published Julian Dates. Each call is made
    // before check() so the failure message cannot print a stale value
    // (argument evaluation order is unspecified).
    struct IsoCase {
        const char* iso;
        double      jd;
    };
    const IsoCase isoCases[] = {
        {"2000-01-01", 2451544.5}, // J2000 epoch is noon on this day
        {"1950-01-01", 2433282.5}, // default CAD window start
        {"2150-01-01", 2506331.5}, // default CAD window end
        {"1900-01-01", 2415020.5}, // widest window start
        {"2200-01-01", 2524593.5}, // widest window end
    };
    for (const IsoCase& c : isoCases) {
        double value = 0.0;
        const bool ok = neo::julianDateFromIsoDate(c.iso, value) && near(value, c.jd, 1e-9);
        check(ok, c.iso, fmt("got %.4f want %.4f", value, c.jd));
    }
    double jd = 0.0;
    check(neo::formatJulianDay(2451545.0) == "2000-01-01", "format J2000", neo::formatJulianDay(2451545.0));
    check(!neo::julianDateFromIsoDate("2020-02-30", jd), "invalid calendar date refused");
    check(!neo::julianDateFromIsoDate("2020-2-1", jd), "wrong format refused");
    check(neo::julianDateFromIsoDate("2020-02-29", jd), "leap day accepted");
}

// --- grazing / impact ------------------------------------------------------

void testGrazing() {
    std::printf("[graze] approaches inside one Earth radius are flagged\n");
    check(near(neo::kEarthRadiusAU, 6378.137 / 149597870.7, 1e-9), "the threshold is the 6378.1 km equatorial radius in AU",
          fmt("%.6e", neo::kEarthRadiusAU));

    neo::CloseApproach a;
    a.distanceAU = 4.0e-5;
    check(a.grazingOrImpact(), "4.0e-5 au (~5,984 km from the centre) is inside the Earth");
    a.distanceAU = 4.2635e-5 - 1e-12;
    check(a.grazingOrImpact(), "just under one Earth radius is flagged");
    a.distanceAU = 4.2635e-5;
    check(!a.grazingOrImpact(), "exactly one Earth radius is not (the bound is strict)");
    // The real 2025 UC11 pass: 0.0000441 au = 1.034 Earth radii, ~220 km above
    // the surface. A genuine very close pass, but not below the threshold.
    a.distanceAU = 0.0000441;
    check(!a.grazingOrImpact(), "2025 UC11 (1.034 R_Earth) is a close pass, not a graze");
    a.distanceAU = 0.05;
    check(!a.grazingOrImpact(), "an ordinary 0.05 au pass is not flagged");

    // The fixture data contains no such row, and the flag must not invent one.
    std::size_t flagged = 0;
    for (const neo::ParsedApproach& p : g_approaches) {
        flagged += p.approach.grazingOrImpact() ? 1u : 0u;
    }
    check(flagged == 0, "no fixture approach is flagged", std::to_string(flagged));
}

} // namespace

// The flat approach vector is the memory-dominant structure once real CAD data
// is loaded, so its size is pinned here: the stage 7 memory numbers quote it,
// and a stray std::string in the struct would multiply it.
static_assert(sizeof(neo::CloseApproach) <= 120, "CloseApproach grew: update the memory figures in NEO_PLAN.md");
static_assert(std::is_trivially_copyable<neo::CloseApproach>::value, "CloseApproach must stay trivially copyable");

int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : NEO_FIXTURES_DIR;
    std::printf("fixtures: %s\n", g_fixtureDir.c_str());
    std::printf("sizeof: CloseApproach=%zu Asteroid=%zu AsteroidRecord=%zu\n", sizeof(neo::CloseApproach),
                sizeof(neo::Asteroid), sizeof(neo::AsteroidRecord));

    testSbdbParsing();
    testSbdbRejections();
    testCadParsing();
    testJoin();
    testJulianDates();
    testGrazing();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
