#include "neo/query/QueryEngine.h"

#include "neo/dsa/BinaryHeap.h"
#include "neo/dsa/Sort.h"
#include "neo/model/JulianDate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace neo {

const char* toString(ExecMode mode) {
    switch (mode) {
    case ExecMode::Naive:      return "naive";
    case ExecMode::FixedOrder: return "fixed-order";
    case ExecMode::Planned:    return "planned";
    }
    return "?";
}

const char* toString(Access access) {
    switch (access) {
    case Access::ScanObjects:    return "scan objects";
    case Access::ScanApproaches: return "scan approaches";
    case Access::HashLookup:     return "hash lookup";
    case Access::NamePrefix:     return "name index";
    case Access::ObjectView:     return "sorted view";
    case Access::SizeBuckets:    return "size buckets";
    case Access::DateTree:       return "AVL tree";
    case Access::YearBuckets:    return "year buckets";
    case Access::ApproachView:   return "sorted view (approach)";
    }
    return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kInf = std::numeric_limits<double>::infinity();

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The predicates, in canonical order. The order matters: the object predicates
// come first, then the approach predicates, and FixedOrder applies residuals in
// exactly this order.
enum class Pred : std::uint8_t {
    Designation,
    NamePrefix,
    Kind,
    Neo,
    Pha,
    OrbitClass,
    Diameter,
    H,
    Moid,
    SemiMajor,
    Ecc,
    Inc,
    // approach predicates (all evaluated on one approach row)
    Date,
    Dist,
    VRel,
    Grazing,
    Count,
};
constexpr std::size_t kPreds = static_cast<std::size_t>(Pred::Count);
constexpr std::size_t idx(Pred p) { return static_cast<std::size_t>(p); }
constexpr bool isApproachPred(Pred p) { return p >= Pred::Date && p < Pred::Count; }

const char* predLabel(Pred p) {
    switch (p) {
    case Pred::Designation: return "designation (exact)";
    case Pred::NamePrefix:  return "name or designation prefix";
    case Pred::Kind:        return "kind";
    case Pred::Neo:         return "neo flag";
    case Pred::Pha:         return "pha flag";
    case Pred::OrbitClass:  return "orbit class";
    case Pred::Diameter:    return "diameter";
    case Pred::H:           return "H";
    case Pred::Moid:        return "MOID";
    case Pred::SemiMajor:   return "semi-major axis";
    case Pred::Ecc:         return "eccentricity";
    case Pred::Inc:         return "inclination";
    case Pred::Date:        return "approach date";
    case Pred::Dist:        return "approach distance";
    case Pred::VRel:        return "approach v_rel";
    case Pred::Grazing:     return "grazing flag";
    case Pred::Count:       break;
    }
    return "?";
}

struct Bounds {
    double lo = -kInf;
    double hi = kInf;
    // NaN fails both comparisons, so an unknown (NaN) value never lies in a range.
    bool contains(double v) const { return v >= lo && v <= hi; }
};

Bounds boundsOf(const Range& r) {
    Bounds b;
    if (r.lo) {
        b.lo = *r.lo;
    }
    if (r.hi) {
        b.hi = *r.hi;
    }
    return b;
}

std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::string boundsText(const Bounds& b) {
    return "[" + (std::isinf(b.lo) ? std::string("-") : g(b.lo)) + ", " + (std::isinf(b.hi) ? std::string("-") : g(b.hi)) +
           "]";
}

std::string dateBoundsText(const Bounds& b) {
    return "[" + (std::isinf(b.lo) ? std::string("-") : formatJulianDay(b.lo)) + ", " +
           (std::isinf(b.hi) ? std::string("-") : formatJulianDay(b.hi)) + "]";
}

std::string commas(std::uint64_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    const std::size_t len = digits.size();
    for (std::size_t i = 0; i < len; ++i) {
        out += digits[i];
        const std::size_t remaining = len - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) {
            out += ',';
        }
    }
    return out;
}

// --- a query, resolved against the indexes ----------------------------------

struct Compiled {
    bool active[kPreds] = {};
    std::string designation;
    std::string prefixLower;
    std::uint8_t kind = 0;
    std::uint8_t neoWant = 0; // 1 yes, 2 no
    std::uint8_t phaWant = 0;
    std::vector<std::uint16_t> classIds; // may be empty while OrbitClass is active: matches nothing
    Bounds diam, h, moid, a, e, i, date, dist, vel;
    DiameterMode diamMode = DiameterMode::MeasuredOrEstimated;
    bool grazingWant = false;
    bool anyObject = false;
    bool anyApproach = false;
    SortField sortBy = SortField::None;
    SortDirection dir = SortDirection::Ascending;
    std::size_t topK = 0;
};

Compiled compile(const Query& q, const IndexSet& ix) {
    Compiled c;
    c.designation = q.designation;
    c.prefixLower = toLowerAscii(q.namePrefix);
    c.diamMode = q.diameterMode;
    c.diam = boundsOf(q.diameterKm);
    c.h = boundsOf(q.absoluteMagnitude);
    c.moid = boundsOf(q.moidAU);
    c.a = boundsOf(q.semiMajorAxisAU);
    c.e = boundsOf(q.eccentricity);
    c.i = boundsOf(q.inclinationDeg);
    c.date = boundsOf(q.dateJd);
    c.dist = boundsOf(q.distanceAU);
    c.vel = boundsOf(q.velocityKms);
    c.grazingWant = q.grazing == TriState::Yes;
    c.sortBy = q.sortBy;
    c.dir = q.direction;
    c.topK = q.topK;

    c.active[idx(Pred::Designation)] = !q.designation.empty();
    c.active[idx(Pred::NamePrefix)] = !q.namePrefix.empty();
    c.active[idx(Pred::Kind)] = q.kind.has_value();
    if (q.kind) {
        c.kind = static_cast<std::uint8_t>(*q.kind);
    }
    c.active[idx(Pred::Neo)] = q.neo != TriState::Any;
    c.neoWant = q.neo == TriState::Yes ? 1 : 2;
    c.active[idx(Pred::Pha)] = q.pha != TriState::Any;
    c.phaWant = q.pha == TriState::Yes ? 1 : 2;
    c.active[idx(Pred::OrbitClass)] = !q.orbitClasses.empty();
    for (const std::string& code : q.orbitClasses) {
        const std::uint16_t id = ix.classIdOf(code);
        if (id != kNoClass) {
            c.classIds.push_back(id);
        }
    }
    c.active[idx(Pred::Diameter)] = q.diameterKm.active();
    c.active[idx(Pred::H)] = q.absoluteMagnitude.active();
    c.active[idx(Pred::Moid)] = q.moidAU.active();
    c.active[idx(Pred::SemiMajor)] = q.semiMajorAxisAU.active();
    c.active[idx(Pred::Ecc)] = q.eccentricity.active();
    c.active[idx(Pred::Inc)] = q.inclinationDeg.active();
    c.active[idx(Pred::Date)] = q.dateJd.active();
    c.active[idx(Pred::Dist)] = q.distanceAU.active();
    c.active[idx(Pred::VRel)] = q.velocityKms.active();
    c.active[idx(Pred::Grazing)] = q.grazing != TriState::Any;

    for (std::size_t p = 0; p < kPreds; ++p) {
        if (c.active[p]) {
            (isApproachPred(static_cast<Pred>(p)) ? c.anyApproach : c.anyObject) = true;
        }
    }
    return c;
}

// --- the result, assembled the same way by every path ------------------------

struct Match {
    std::uint32_t obj = 0;
    std::uint32_t begin = 0; // into the pool
    std::uint32_t count = 0;
};

struct Collected {
    std::vector<Match> matches;
    std::vector<std::uint32_t> pool; // approach indices
    std::uint64_t candidates = 0;
};

// Copies the chosen rows' approaches into a compact pool, so a top-10 result does
// not carry the approach lists of the thousands of matches that were discarded.
void assemble(const Collected& col, const std::vector<std::uint32_t>& order, const std::vector<double>& key,
              const std::vector<std::uint8_t>& known, QueryResult& res) {
    res.rows.clear();
    res.approachPool.clear();
    res.rows.reserve(order.size());
    for (const std::uint32_t position : order) {
        const Match& m = col.matches[position];
        ResultRow row;
        row.object = m.obj;
        row.approachBegin = static_cast<std::uint32_t>(res.approachPool.size());
        row.approachCount = m.count;
        row.sortKeyKnown = known[position] != 0;
        row.sortKey = row.sortKeyKnown ? key[position] : 0.0;
        for (std::uint32_t k = 0; k < m.count; ++k) {
            res.approachPool.push_back(col.pool[m.begin + k]);
        }
        res.rows.push_back(row);
    }
}

std::size_t totalApproachCount(const Collected& col) {
    std::size_t total = 0;
    for (const Match& m : col.matches) {
        total += m.count;
    }
    return total;
}

// ===========================================================================
// NAIVE: the oracle. Reads the records directly, shares no index, no column and
// no sorting code with the indexed paths.
// ===========================================================================

bool inRange(double v, const Range& r) { return (!r.lo || v >= *r.lo) && (!r.hi || v <= *r.hi); }

class NaiveRunner {
public:
    NaiveRunner(const Dataset& ds, const Query& q) : ds_(ds), q_(q) {}

    void run(QueryResult& res) {
        Collected col;
        const std::size_t n = ds_.records().size();
        const bool approachFilter = hasApproachFilter(q_);
        for (std::uint32_t o = 0; o < n; ++o) {
            ++col.candidates;
            if (!objectMatches(ds_.records()[o].object)) {
                continue;
            }
            const std::uint32_t first = ds_.records()[o].firstApproach;
            const std::uint32_t count = ds_.records()[o].approachCount;
            Match m;
            m.obj = o;
            m.begin = static_cast<std::uint32_t>(col.pool.size());
            for (std::uint32_t k = 0; k < count; ++k) {
                if (!approachFilter || approachMatches(ds_.approaches()[first + k])) {
                    col.pool.push_back(first + k);
                }
            }
            m.count = static_cast<std::uint32_t>(col.pool.size()) - m.begin;
            if (approachFilter && m.count == 0) {
                continue; // no single approach satisfied every approach condition
            }
            col.matches.push_back(m);
        }
        finish(col, res);
        res.stats.actualCandidates = col.candidates;
    }

    std::uint64_t evaluated[kPreds] = {};
    std::uint64_t passed[kPreds] = {};

private:
    bool count(Pred p, bool ok) {
        ++evaluated[idx(p)];
        if (ok) {
            ++passed[idx(p)];
        }
        return ok;
    }

    bool objectMatches(const Asteroid& a) {
        if (!q_.designation.empty() && !count(Pred::Designation, a.pdes == q_.designation)) {
            return false;
        }
        if (!q_.namePrefix.empty()) {
            const std::string lower = toLowerAscii(q_.namePrefix);
            const bool ok = startsWithIgnoreCase(a.pdes, lower) || startsWithIgnoreCase(a.name, lower);
            if (!count(Pred::NamePrefix, ok)) {
                return false;
            }
        }
        if (q_.kind && !count(Pred::Kind, a.classification.kind == *q_.kind)) {
            return false;
        }
        if (q_.neo != TriState::Any) {
            const bool ok = a.classification.isNEO.has_value() && *a.classification.isNEO == (q_.neo == TriState::Yes);
            if (!count(Pred::Neo, ok)) {
                return false;
            }
        }
        if (q_.pha != TriState::Any) {
            const bool ok = a.classification.isPHA.has_value() && *a.classification.isPHA == (q_.pha == TriState::Yes);
            if (!count(Pred::Pha, ok)) {
                return false;
            }
        }
        if (!q_.orbitClasses.empty()) {
            bool ok = false;
            for (const std::string& code : q_.orbitClasses) {
                ok = ok || (!a.classification.orbitClass.empty() && a.classification.orbitClass == code);
            }
            if (!count(Pred::OrbitClass, ok)) {
                return false;
            }
        }
        if (q_.diameterKm.active()) {
            const std::optional<double> d = diameterOf(a);
            const bool ok = d ? inRange(*d, q_.diameterKm) : q_.diameterMode == DiameterMode::IncludeUnknown;
            if (!count(Pred::Diameter, ok)) {
                return false;
            }
        }
        if (q_.absoluteMagnitude.active() &&
            !count(Pred::H, a.physical.absoluteMagnitudeH && inRange(*a.physical.absoluteMagnitudeH, q_.absoluteMagnitude))) {
            return false;
        }
        if (q_.moidAU.active() && !count(Pred::Moid, a.orbital.moidAU && inRange(*a.orbital.moidAU, q_.moidAU))) {
            return false;
        }
        if (q_.semiMajorAxisAU.active() && !count(Pred::SemiMajor, inRange(a.orbital.semiMajorAxisAU, q_.semiMajorAxisAU))) {
            return false;
        }
        if (q_.eccentricity.active() && !count(Pred::Ecc, inRange(a.orbital.eccentricity, q_.eccentricity))) {
            return false;
        }
        if (q_.inclinationDeg.active() && !count(Pred::Inc, inRange(a.orbital.inclinationDeg, q_.inclinationDeg))) {
            return false;
        }
        return true;
    }

    // One approach row must satisfy every approach condition at once.
    bool approachMatches(const CloseApproach& c) {
        if (q_.dateJd.active() && !count(Pred::Date, inRange(c.jdTdb, q_.dateJd))) {
            return false;
        }
        if (q_.distanceAU.active() && !count(Pred::Dist, inRange(c.distanceAU, q_.distanceAU))) {
            return false;
        }
        if (q_.velocityKms.active() && !count(Pred::VRel, inRange(c.relVelocityKms, q_.velocityKms))) {
            return false;
        }
        if (q_.grazing != TriState::Any && !count(Pred::Grazing, c.grazingOrImpact() == (q_.grazing == TriState::Yes))) {
            return false;
        }
        return true;
    }

    std::optional<double> diameterOf(const Asteroid& a) const {
        return q_.diameterMode == DiameterMode::MeasuredOnly ? a.physical.diameterKm : a.physical.bestDiameterKm();
    }

    // Sorting done with the standard library, so the oracle also checks my merge
    // sort and my heap on every query.
    void finish(Collected& col, QueryResult& res) {
        const std::size_t m = col.matches.size();
        std::vector<double> key(m, 0.0);
        std::vector<std::uint8_t> known(m, 0);
        for (std::size_t i = 0; i < m; ++i) {
            const Asteroid& a = ds_.records()[col.matches[i].obj].object;
            std::optional<double> k;
            switch (q_.sortBy) {
            case SortField::Diameter:          k = diameterOf(a); break;
            case SortField::AbsoluteMagnitude: k = a.physical.absoluteMagnitudeH; break;
            case SortField::Moid:              k = a.orbital.moidAU; break;
            case SortField::SemiMajorAxis:     k = a.orbital.semiMajorAxisAU; break;
            case SortField::Eccentricity:      k = a.orbital.eccentricity; break;
            case SortField::Inclination:       k = a.orbital.inclinationDeg; break;
            case SortField::Date:
            case SortField::Distance:
            case SortField::Velocity:
                for (std::uint32_t j = 0; j < col.matches[i].count; ++j) {
                    const CloseApproach& c = ds_.approaches()[col.pool[col.matches[i].begin + j]];
                    const double v = q_.sortBy == SortField::Date       ? c.jdTdb
                                     : q_.sortBy == SortField::Distance ? c.distanceAU
                                                                        : c.relVelocityKms;
                    if (!k) {
                        k = v;
                    } else if (q_.direction == SortDirection::Ascending) {
                        k = std::min(*k, v);
                    } else {
                        k = std::max(*k, v);
                    }
                }
                break;
            case SortField::None:
            case SortField::Designation:
                break;
            }
            if (k) {
                key[i] = *k;
                known[i] = 1;
            }
        }

        const bool asc = q_.direction == SortDirection::Ascending;
        const auto better = [&](std::uint32_t x, std::uint32_t y) {
            const std::uint32_t ox = col.matches[x].obj;
            const std::uint32_t oy = col.matches[y].obj;
            if (q_.sortBy == SortField::None) {
                return ox < oy;
            }
            if (q_.sortBy == SortField::Designation) {
                const int c = ds_.records()[ox].object.pdes.compare(ds_.records()[oy].object.pdes);
                if (c != 0) {
                    return asc ? c < 0 : c > 0;
                }
                return ox < oy;
            }
            if (known[x] != known[y]) {
                return known[x] != 0; // unknown keys always sort last
            }
            if (known[x] != 0 && key[x] != key[y]) {
                return asc ? key[x] < key[y] : key[x] > key[y];
            }
            return ox < oy;
        };

        std::vector<std::uint32_t> order(m);
        for (std::size_t i = 0; i < m; ++i) {
            order[i] = static_cast<std::uint32_t>(i);
        }
        std::stable_sort(order.begin(), order.end(), better);
        if (q_.topK > 0 && order.size() > q_.topK) {
            order.resize(q_.topK);
        }
        res.totalObjects = m;
        res.totalApproaches = totalApproachCount(col);
        assemble(col, order, key, known, res);
    }

    const Dataset& ds_;
    const Query& q_;
};

// ===========================================================================
// INDEXED: fixed-order and planned execution.
// ===========================================================================

// Relative per-row costs. Nothing here is measured in seconds: only the RATIOS
// matter, because the planner compares the total work of alternative plans.
//   a flag / numeric range check on a dense column          1.0
//   a class-code or designation check (small loop, compare)  1.5
//   a name-prefix check (two case-folded string compares)    4.0
constexpr double unitCostOf(Pred p) {
    switch (p) {
    case Pred::NamePrefix: return 4.0;
    case Pred::Designation:
    case Pred::OrbitClass:
    case Pred::Diameter:   return 1.5;
    default:               return 1.0;
    }
}

// Cost of pulling one candidate out of each access path.
//   sequential scan of a dense array     0.5
//   slice of a bucket / sorted view      0.8 / 1.0   (contiguous indices, random rows)
//   hash lookup                          1.0
//   name index range                     1.5         (read + de-duplicate)
//   AVL in-order walk                    2.5         (pointer chasing through a stack)
constexpr double kScanCost = 0.5;
constexpr double kBucketCost = 0.8;
constexpr double kViewCost = 1.0;
constexpr double kHashCost = 1.0;
constexpr double kNameCost = 1.5;
constexpr double kTreeCost = 2.5;
// Fetching an approach's parent object when driving from the approach side.
constexpr double kParentFetchCost = 0.5;

struct DriverOption {
    Access access = Access::ScanObjects;
    Pred pred = Pred::Count; // the predicate this access path serves (Count for a scan)
    bool approachUniverse = false;
    bool exact = true;       // false: a superset, so the predicate is still re-checked
    double est = 0.0;
    double accessCost = 1.0;
    Bounds bounds;
    std::string text;
    double work = 0.0;
};

struct Plan {
    DriverOption driver;
    std::vector<Pred> objChain;
    std::vector<Pred> appChain;
    std::vector<ConsideredPlan> considered;
};

struct Chain {
    std::vector<Pred> order;
    double cost = 0.0; // expected work per input row
    double pass = 1.0; // fraction of input rows that survive the whole chain
};

class Indexed {
public:
    Indexed(const IndexSet& ix, const Query& q, const Compiled& c)
        : ix_(ix), ds_(*ix.dataset), q_(q), c_(c),
          nObj_(static_cast<double>(std::max<std::size_t>(1, ix.dataset->records().size()))),
          nApp_(static_cast<double>(std::max<std::size_t>(1, ix.dataset->approaches().size()))) {
        for (std::size_t p = 0; p < kPreds; ++p) {
            if (c_.active[p]) {
                sel_[p] = selectivity(static_cast<Pred>(p));
            }
        }
    }

    // ---- planning ---------------------------------------------------------------

    Plan planCosted(const std::optional<Access>& force) const {
        std::vector<DriverOption> options = enumerateOptions();
        for (DriverOption& o : options) {
            o.work = estimateWork(o);
        }
        Plan plan;
        const DriverOption* best = nullptr;
        for (const DriverOption& o : options) {
            if (force && o.access != *force) {
                continue;
            }
            if (best == nullptr || betterOption(o, *best)) {
                best = &o;
            }
        }
        if (best == nullptr) { // forced access not applicable: the planner's own choice stands
            for (const DriverOption& o : options) {
                if (best == nullptr || betterOption(o, *best)) {
                    best = &o;
                }
            }
        }
        plan.driver = *best;
        for (const DriverOption& o : options) {
            ConsideredPlan cp;
            cp.text = o.text;
            cp.estCandidates = o.est;
            cp.estWork = o.work;
            cp.chosen = o.access == plan.driver.access && o.pred == plan.driver.pred;
            plan.considered.push_back(cp);
        }
        std::stable_sort(plan.considered.begin(), plan.considered.end(),
                         [](const ConsideredPlan& a, const ConsideredPlan& b) { return a.estWork < b.estWork; });
        buildChains(plan, /*byRank=*/true);
        return plan;
    }

    // Always the same driver: the first present predicate in a fixed priority
    // order, whatever the data looks like. Residuals in declaration order.
    Plan planFixed(const std::optional<Access>& force) const {
        std::vector<DriverOption> options = enumerateOptions();
        for (DriverOption& o : options) {
            o.work = estimateWork(o);
        }
        static const Pred priority[] = {Pred::Designation, Pred::NamePrefix, Pred::Date, Pred::Dist, Pred::VRel,
                                        Pred::Diameter,    Pred::H,          Pred::Moid, Pred::SemiMajor,
                                        Pred::Ecc,         Pred::Inc,        Pred::Grazing};
        const DriverOption* chosen = nullptr;
        if (force) {
            for (const DriverOption& o : options) {
                if (o.access == *force && (chosen == nullptr || betterOption(o, *chosen))) {
                    chosen = &o;
                }
            }
        }
        for (std::size_t k = 0; chosen == nullptr && k < sizeof priority / sizeof priority[0]; ++k) {
            if (!c_.active[idx(priority[k])]) {
                continue;
            }
            // The predicate's primary access path: an exact one, never the superset buckets.
            for (const DriverOption& o : options) {
                if (o.pred == priority[k] && o.exact) {
                    chosen = &o;
                    break;
                }
            }
            if (chosen == nullptr) { // IncludeUnknown diameters have only the bucket path
                for (const DriverOption& o : options) {
                    if (o.pred == priority[k]) {
                        chosen = &o;
                        break;
                    }
                }
            }
        }
        if (chosen == nullptr) {
            chosen = &options.front(); // the object scan
        }
        Plan plan;
        plan.driver = *chosen;
        buildChains(plan, /*byRank=*/false);
        return plan;
    }

    // ---- execution ---------------------------------------------------------------

    void execute(const Plan& plan, Collected& out) {
        const DriverOption& d = plan.driver;
        if (!d.approachUniverse) {
            forEachCandidate(d, [&](std::uint32_t o) {
                ++out.candidates;
                if (!passObjects(plan.objChain, o)) {
                    return;
                }
                appendObject(o, plan.appChain, out);
            });
            return;
        }
        std::vector<std::uint32_t> hits;
        forEachCandidate(d, [&](std::uint32_t a) {
            ++out.candidates;
            if (!passApproaches(plan.appChain, a)) {
                return;
            }
            const std::uint32_t o = ds_.approaches()[a].objectIndex;
            if (!passObjects(plan.objChain, o)) {
                return;
            }
            hits.push_back(a);
        });
        // The flat approach vector is ordered by (object, date), so ascending
        // approach index is ascending object index: one sort makes every
        // object's matches contiguous and in date order.
        dsa::mergeSort(hits, [](std::uint32_t x, std::uint32_t y) { return x < y; });
        std::size_t i = 0;
        while (i < hits.size()) {
            const std::uint32_t o = ds_.approaches()[hits[i]].objectIndex;
            Match m;
            m.obj = o;
            m.begin = static_cast<std::uint32_t>(out.pool.size());
            while (i < hits.size() && ds_.approaches()[hits[i]].objectIndex == o) {
                out.pool.push_back(hits[i]);
                ++i;
            }
            m.count = static_cast<std::uint32_t>(out.pool.size()) - m.begin;
            out.matches.push_back(m);
        }
    }

    // Sort, top-K and assemble, with the project's own merge sort and heap.
    void finish(Collected& col, QueryResult& res) const {
        const std::size_t m = col.matches.size();
        std::vector<double> key(m, 0.0);
        std::vector<std::uint8_t> known(m, 0);
        computeKeys(col, key, known);

        const bool asc = c_.dir == SortDirection::Ascending;
        const auto better = [&](std::uint32_t x, std::uint32_t y) {
            const std::uint32_t ox = col.matches[x].obj;
            const std::uint32_t oy = col.matches[y].obj;
            if (c_.sortBy == SortField::None) {
                return ox < oy;
            }
            if (c_.sortBy == SortField::Designation) {
                const int cmp = ds_.records()[ox].object.pdes.compare(ds_.records()[oy].object.pdes);
                if (cmp != 0) {
                    return asc ? cmp < 0 : cmp > 0;
                }
                return ox < oy;
            }
            if (known[x] != known[y]) {
                return known[x] != 0;
            }
            if (known[x] != 0 && key[x] != key[y]) {
                return asc ? key[x] < key[y] : key[x] > key[y];
            }
            return ox < oy;
        };

        std::vector<std::uint32_t> order(m);
        for (std::size_t i = 0; i < m; ++i) {
            order[i] = static_cast<std::uint32_t>(i);
        }
        if (c_.topK > 0 && c_.topK < m) {
            order = dsa::topK(order, c_.topK, better); // O(n log k), O(k) space
        } else {
            dsa::mergeSort(order, better);
            if (c_.topK > 0 && order.size() > c_.topK) {
                order.resize(c_.topK);
            }
        }
        res.totalObjects = m;
        res.totalApproaches = totalApproachCount(col);
        assemble(col, order, key, known, res);
    }

    // ---- reporting ----------------------------------------------------------------

    void fillStats(const Plan& plan, QueryStats& stats) const {
        const DriverOption& d = plan.driver;
        stats.driverAccess = d.access;
        stats.driverText = d.text;
        stats.driverApproachUniverse = d.approachUniverse;
        stats.driverExact = d.exact;
        stats.estCandidates = d.est;
        stats.estWork = d.work;
        stats.considered = plan.considered;

        const std::vector<Pred>& first = d.approachUniverse ? plan.appChain : plan.objChain;
        const std::vector<Pred>& second = d.approachUniverse ? plan.objChain : plan.appChain;
        for (const std::vector<Pred>* chain : {&first, &second}) {
            for (const Pred p : *chain) {
                StepStat s;
                s.text = stepText(p);
                s.approachUniverse = isApproachPred(p);
                s.estSelectivity = sel_[idx(p)];
                s.unitCost = unitCostOf(p);
                s.evaluated = evaluated_[idx(p)];
                s.passed = passed_[idx(p)];
                stats.steps.push_back(s);
                stats.predicateEvaluations += s.evaluated;
            }
        }
    }

private:
    // ---- selectivity: the estimated fraction of a predicate's universe that passes

    double selectivity(Pred p) const {
        switch (p) {
        case Pred::Designation:
            return ix_.byDesignation.contains(c_.designation) ? 1.0 / nObj_ : 0.0;
        case Pred::NamePrefix:
            return std::min(1.0, static_cast<double>(ix_.names.countPrefix(c_.prefixLower)) / nObj_);
        case Pred::Kind:
            return static_cast<double>(ix_.stats.kind[c_.kind < 3 ? c_.kind : 2]) / nObj_;
        case Pred::Neo:
            return static_cast<double>(ix_.stats.neo[c_.neoWant]) / nObj_;
        case Pred::Pha:
            return static_cast<double>(ix_.stats.pha[c_.phaWant]) / nObj_;
        case Pred::OrbitClass: {
            double total = 0.0;
            for (const std::uint16_t id : c_.classIds) {
                total += static_cast<double>(ix_.stats.classCounts[id]);
            }
            return std::min(1.0, total / nObj_);
        }
        case Pred::Diameter:
            if (c_.diamMode == DiameterMode::MeasuredOnly) {
                return ix_.histDiameterMeasured.selectivity(c_.diam.lo, c_.diam.hi);
            }
            if (c_.diamMode == DiameterMode::MeasuredOrEstimated) {
                return ix_.histDiameterBest.selectivity(c_.diam.lo, c_.diam.hi);
            }
            // Unknowns pass too: the known share in range, plus every unknown.
            return std::min(1.0, ix_.histDiameterBest.selectivity(c_.diam.lo, c_.diam.hi) +
                                     (1.0 - ix_.histDiameterBest.knownFraction()));
        case Pred::H:         return ix_.histH.selectivity(c_.h.lo, c_.h.hi);
        case Pred::Moid:      return ix_.histMoid.selectivity(c_.moid.lo, c_.moid.hi);
        case Pred::SemiMajor: return ix_.histA.selectivity(c_.a.lo, c_.a.hi);
        case Pred::Ecc:       return ix_.histE.selectivity(c_.e.lo, c_.e.hi);
        case Pred::Inc:       return ix_.histI.selectivity(c_.i.lo, c_.i.hi);
        case Pred::Date:      return ix_.histDate.selectivity(c_.date.lo, c_.date.hi);
        case Pred::Dist:      return ix_.histDist.selectivity(c_.dist.lo, c_.dist.hi);
        case Pred::VRel:      return ix_.histVrel.selectivity(c_.vel.lo, c_.vel.hi);
        case Pred::Grazing: {
            const double share = static_cast<double>(ix_.stats.grazing) / nApp_;
            return c_.grazingWant ? share : 1.0 - share;
        }
        case Pred::Count: break;
        }
        return 1.0;
    }

    // The bounds a range-style access path reads for a predicate.
    Bounds boundsFor(Pred p) const {
        switch (p) {
        case Pred::Diameter:  return c_.diam;
        case Pred::H:         return c_.h;
        case Pred::Moid:      return c_.moid;
        case Pred::SemiMajor: return c_.a;
        case Pred::Ecc:       return c_.e;
        case Pred::Inc:       return c_.i;
        case Pred::Date:      return c_.date;
        case Pred::Dist:      return c_.dist;
        case Pred::VRel:      return c_.vel;
        case Pred::Grazing: {
            // "nominal distance < one Earth radius" is exactly a distance range,
            // so the distance view answers it directly.
            Bounds b;
            if (c_.grazingWant) {
                b.hi = std::nextafter(kEarthRadiusAU, -kInf);
            } else {
                b.lo = kEarthRadiusAU;
            }
            return b;
        }
        default: break;
        }
        return Bounds();
    }

    const dsa::SortedView& viewFor(Pred p) const {
        switch (p) {
        case Pred::Diameter:
            return c_.diamMode == DiameterMode::MeasuredOnly ? ix_.diameterMeasured : ix_.diameterBest;
        case Pred::H:         return ix_.h;
        case Pred::Moid:      return ix_.moid;
        case Pred::SemiMajor: return ix_.a;
        case Pred::Ecc:       return ix_.e;
        case Pred::Inc:       return ix_.i;
        case Pred::VRel:      return ix_.vrel;
        default:              return ix_.dist; // Dist and Grazing
        }
    }

    // ---- options ----------------------------------------------------------------

    static int clampedYear(double jd, int fallback) {
        if (std::isinf(jd)) {
            return fallback;
        }
        return dsa::yearOfJulianDate(std::clamp(jd, 0.0, 6.0e6));
    }

    std::size_t sizeBucketCandidates() const {
        const dsa::SizeBucketIndex& s = c_.diamMode == DiameterMode::MeasuredOnly ? ix_.sizeMeasured : ix_.sizeBest;
        const int lo = static_cast<int>(dsa::sizeBucketOf(c_.diam.lo));
        const int hi = static_cast<int>(dsa::sizeBucketOf(c_.diam.hi));
        std::size_t total = 0;
        for (int b = lo; b <= hi; ++b) {
            total += s.count(static_cast<dsa::SizeBucket>(b));
        }
        if (c_.diamMode == DiameterMode::IncludeUnknown) {
            total += s.count(dsa::SizeBucket::Unknown);
        }
        return total;
    }

    std::size_t yearBucketCandidates() const {
        if (ix_.years.size() == 0) {
            return 0;
        }
        const int y0 = clampedYear(c_.date.lo, ix_.years.firstYear());
        const int y1 = clampedYear(c_.date.hi, ix_.years.lastYear());
        std::size_t total = 0;
        for (int y = std::max(y0, ix_.years.firstYear()); y <= std::min(y1, ix_.years.lastYear()); ++y) {
            total += ix_.years.count(y);
        }
        return total;
    }

    std::vector<DriverOption> enumerateOptions() const {
        std::vector<DriverOption> out;
        auto add = [&out](Access access, Pred pred, bool approachUniverse, bool exact, double est, double cost,
                          const Bounds& bounds, std::string text) {
            DriverOption o;
            o.access = access;
            o.pred = pred;
            o.approachUniverse = approachUniverse;
            o.exact = exact;
            o.est = est;
            o.accessCost = cost;
            o.bounds = bounds;
            o.text = std::move(text);
            out.push_back(std::move(o));
        };

        add(Access::ScanObjects, Pred::Count, false, true, nObj_, kScanCost, Bounds(), "full scan of all objects");
        if (c_.anyApproach) {
            add(Access::ScanApproaches, Pred::Count, true, true, nApp_, kScanCost, Bounds(),
                "full scan of all approaches");
        }
        if (c_.active[idx(Pred::Designation)]) {
            const double found = ix_.byDesignation.contains(c_.designation) ? 1.0 : 0.0;
            add(Access::HashLookup, Pred::Designation, false, true, found, kHashCost, Bounds(),
                "hash lookup: designation = '" + c_.designation + "'");
        }
        if (c_.active[idx(Pred::NamePrefix)]) {
            add(Access::NamePrefix, Pred::NamePrefix, false, true,
                static_cast<double>(ix_.names.countPrefix(c_.prefixLower)), kNameCost, Bounds(),
                "name index: prefix '" + c_.prefixLower + "'");
        }
        if (c_.active[idx(Pred::Diameter)]) {
            if (c_.diamMode != DiameterMode::IncludeUnknown) {
                add(Access::ObjectView, Pred::Diameter, false, true, sel_[idx(Pred::Diameter)] * nObj_, kViewCost,
                    c_.diam,
                    std::string("sorted view: diameter ") + boundsText(c_.diam) +
                        (c_.diamMode == DiameterMode::MeasuredOnly ? " (measured)" : " (measured or estimate)"));
            }
            add(Access::SizeBuckets, Pred::Diameter, false, false, static_cast<double>(sizeBucketCandidates()),
                kBucketCost, c_.diam, "size buckets overlapping diameter " + boundsText(c_.diam));
        }
        const struct {
            Pred p;
            const char* name;
        } numeric[] = {{Pred::H, "H"}, {Pred::Moid, "MOID"}, {Pred::SemiMajor, "a"}, {Pred::Ecc, "e"}, {Pred::Inc, "i"}};
        for (const auto& n : numeric) {
            if (c_.active[idx(n.p)]) {
                add(Access::ObjectView, n.p, false, true, sel_[idx(n.p)] * nObj_, kViewCost, boundsFor(n.p),
                    std::string("sorted view: ") + n.name + " " + boundsText(boundsFor(n.p)));
            }
        }
        if (c_.active[idx(Pred::Date)]) {
            add(Access::DateTree, Pred::Date, true, true, sel_[idx(Pred::Date)] * nApp_, kTreeCost, c_.date,
                "AVL tree: date " + dateBoundsText(c_.date));
            add(Access::YearBuckets, Pred::Date, true, false, static_cast<double>(yearBucketCandidates()), kBucketCost,
                c_.date, "year buckets overlapping " + dateBoundsText(c_.date));
        }
        if (c_.active[idx(Pred::Dist)]) {
            add(Access::ApproachView, Pred::Dist, true, true, sel_[idx(Pred::Dist)] * nApp_, kViewCost, c_.dist,
                "sorted view: approach distance " + boundsText(c_.dist) + " au");
        }
        if (c_.active[idx(Pred::VRel)]) {
            add(Access::ApproachView, Pred::VRel, true, true, sel_[idx(Pred::VRel)] * nApp_, kViewCost, c_.vel,
                "sorted view: approach v_rel " + boundsText(c_.vel) + " km/s");
        }
        if (c_.active[idx(Pred::Grazing)]) {
            add(Access::ApproachView, Pred::Grazing, true, true, sel_[idx(Pred::Grazing)] * nApp_, kViewCost,
                boundsFor(Pred::Grazing),
                std::string("sorted view: approach distance, grazing = ") + (c_.grazingWant ? "yes" : "no"));
        }
        return out;
    }

    // ---- cost model ---------------------------------------------------------------

    double rank(Pred p) const {
        // Rejection per unit cost: the share of rows this predicate removes,
        // divided by what it costs to test. Highest first.
        return (1.0 - sel_[idx(p)]) / unitCostOf(p);
    }

    Chain makeChain(std::vector<Pred> preds, bool byRank) const {
        if (byRank) {
            // preds arrive in canonical order, and a stable sort keeps ties there.
            std::stable_sort(preds.begin(), preds.end(), [this](Pred a, Pred b) { return rank(a) > rank(b); });
        }
        Chain chain;
        for (const Pred p : preds) {
            chain.cost += unitCostOf(p) * chain.pass;
            chain.pass *= sel_[idx(p)];
        }
        chain.order = std::move(preds);
        return chain;
    }

    void splitPreds(const DriverOption& driver, std::vector<Pred>& objPreds, std::vector<Pred>& appPreds) const {
        for (std::size_t p = 0; p < kPreds; ++p) {
            if (!c_.active[p]) {
                continue;
            }
            const Pred pred = static_cast<Pred>(p);
            if (driver.exact && driver.pred == pred) {
                continue; // the access path already guarantees this predicate
            }
            (isApproachPred(pred) ? appPreds : objPreds).push_back(pred);
        }
    }

    // Expected total work of a plan: candidates pulled from the driver, then the
    // residual chains applied to the survivors.
    double estimateWork(const DriverOption& d) const {
        std::vector<Pred> objPreds;
        std::vector<Pred> appPreds;
        splitPreds(d, objPreds, appPreds);
        const Chain oc = makeChain(objPreds, true);
        const Chain ac = makeChain(appPreds, true);
        const double avgApproaches = nApp_ / nObj_;
        double work = d.est * d.accessCost;
        if (!d.approachUniverse) {
            work += d.est * oc.cost;
            if (c_.anyApproach) {
                work += d.est * oc.pass * avgApproaches * ac.cost;
            }
        } else {
            work += d.est * ac.cost;
            work += d.est * ac.pass * (kParentFetchCost + oc.cost);
        }
        return work;
    }

    static bool betterOption(const DriverOption& a, const DriverOption& b) {
        if (a.work != b.work) {
            return a.work < b.work;
        }
        if (a.est != b.est) {
            return a.est < b.est;
        }
        return static_cast<int>(a.access) < static_cast<int>(b.access);
    }

    void buildChains(Plan& plan, bool byRank) const {
        std::vector<Pred> objPreds;
        std::vector<Pred> appPreds;
        splitPreds(plan.driver, objPreds, appPreds);
        plan.objChain = makeChain(objPreds, byRank).order;
        plan.appChain = makeChain(appPreds, byRank).order;
    }

    // ---- candidate streams ----------------------------------------------------------

    template <class Fn>
    void forEachCandidate(const DriverOption& d, Fn&& fn) const {
        switch (d.access) {
        case Access::ScanObjects: {
            const std::uint32_t n = static_cast<std::uint32_t>(ds_.records().size());
            for (std::uint32_t o = 0; o < n; ++o) {
                fn(o);
            }
            break;
        }
        case Access::ScanApproaches: {
            const std::uint32_t n = static_cast<std::uint32_t>(ds_.approaches().size());
            for (std::uint32_t a = 0; a < n; ++a) {
                fn(a);
            }
            break;
        }
        case Access::HashLookup: {
            if (const std::uint32_t* found = ix_.byDesignation.find(c_.designation)) {
                fn(*found);
            }
            break;
        }
        case Access::NamePrefix: {
            for (const std::uint32_t o : ix_.names.matches(c_.prefixLower)) {
                fn(o);
            }
            break;
        }
        case Access::ObjectView:
        case Access::ApproachView: {
            const dsa::SortedView& view = viewFor(d.pred);
            const std::pair<std::size_t, std::size_t> range = view.range(d.bounds.lo, d.bounds.hi);
            for (std::size_t k = range.first; k < range.second; ++k) {
                fn(view.order[k]);
            }
            break;
        }
        case Access::SizeBuckets: {
            const dsa::SizeBucketIndex& s = c_.diamMode == DiameterMode::MeasuredOnly ? ix_.sizeMeasured : ix_.sizeBest;
            const int lo = static_cast<int>(dsa::sizeBucketOf(c_.diam.lo));
            const int hi = static_cast<int>(dsa::sizeBucketOf(c_.diam.hi));
            for (int b = lo; b <= hi; ++b) {
                const dsa::SizeBucket bucket = static_cast<dsa::SizeBucket>(b);
                for (const std::uint32_t* it = s.begin(bucket); it != s.end(bucket); ++it) {
                    fn(*it);
                }
            }
            if (c_.diamMode == DiameterMode::IncludeUnknown) {
                for (const std::uint32_t* it = s.begin(dsa::SizeBucket::Unknown); it != s.end(dsa::SizeBucket::Unknown);
                     ++it) {
                    fn(*it);
                }
            }
            break;
        }
        case Access::DateTree:
            ix_.dateTree.range(d.bounds.lo, d.bounds.hi, [&fn](double, std::uint32_t a) { fn(a); });
            break;
        case Access::YearBuckets: {
            if (ix_.years.size() == 0) {
                break;
            }
            const int y0 = clampedYear(c_.date.lo, ix_.years.firstYear());
            const int y1 = clampedYear(c_.date.hi, ix_.years.lastYear());
            ix_.years.forEachInYears(y0, y1, fn);
            break;
        }
        }
    }

    // ---- predicate evaluation on the dense columns --------------------------------------

    bool evalObject(Pred p, std::uint32_t o) {
        bool ok = false;
        switch (p) {
        case Pred::Designation: ok = ds_.records()[o].object.pdes == c_.designation; break;
        case Pred::NamePrefix: {
            const Asteroid& a = ds_.records()[o].object;
            ok = startsWithIgnoreCase(a.pdes, c_.prefixLower) || startsWithIgnoreCase(a.name, c_.prefixLower);
            break;
        }
        case Pred::Kind:  ok = ix_.obj.kind[o] == c_.kind; break;
        case Pred::Neo:   ok = ix_.obj.neo[o] == c_.neoWant; break;
        case Pred::Pha:   ok = ix_.obj.pha[o] == c_.phaWant; break;
        case Pred::OrbitClass: {
            const std::uint16_t id = ix_.obj.classId[o];
            for (const std::uint16_t want : c_.classIds) {
                ok = ok || (id != kNoClass && id == want);
            }
            break;
        }
        case Pred::Diameter: {
            const double v = c_.diamMode == DiameterMode::MeasuredOnly ? ix_.obj.diameterMeasured[o] : ix_.obj.diameterBest[o];
            ok = std::isnan(v) ? c_.diamMode == DiameterMode::IncludeUnknown : c_.diam.contains(v);
            break;
        }
        case Pred::H:         ok = c_.h.contains(ix_.obj.h[o]); break;
        case Pred::Moid:      ok = c_.moid.contains(ix_.obj.moid[o]); break;
        case Pred::SemiMajor: ok = c_.a.contains(ix_.obj.a[o]); break;
        case Pred::Ecc:       ok = c_.e.contains(ix_.obj.e[o]); break;
        case Pred::Inc:       ok = c_.i.contains(ix_.obj.i[o]); break;
        default: break;
        }
        ++evaluated_[idx(p)];
        if (ok) {
            ++passed_[idx(p)];
        }
        return ok;
    }

    bool evalApproach(Pred p, std::uint32_t a) {
        bool ok = false;
        switch (p) {
        case Pred::Date:    ok = c_.date.contains(ix_.app.jd[a]); break;
        case Pred::Dist:    ok = c_.dist.contains(ix_.app.dist[a]); break;
        case Pred::VRel:    ok = c_.vel.contains(ix_.app.vrel[a]); break;
        case Pred::Grazing: ok = (ix_.app.dist[a] < kEarthRadiusAU) == c_.grazingWant; break;
        default: break;
        }
        ++evaluated_[idx(p)];
        if (ok) {
            ++passed_[idx(p)];
        }
        return ok;
    }

    bool passObjects(const std::vector<Pred>& chain, std::uint32_t o) {
        for (const Pred p : chain) {
            if (!evalObject(p, o)) {
                return false;
            }
        }
        return true;
    }

    bool passApproaches(const std::vector<Pred>& chain, std::uint32_t a) {
        for (const Pred p : chain) {
            if (!evalApproach(p, a)) {
                return false;
            }
        }
        return true;
    }

    // An object driven from the object side: its own approaches are tested here,
    // each one against every approach condition (same-row semantics).
    void appendObject(std::uint32_t o, const std::vector<Pred>& appChain, Collected& out) {
        const std::uint32_t first = ds_.records()[o].firstApproach;
        const std::uint32_t count = ds_.records()[o].approachCount;
        Match m;
        m.obj = o;
        m.begin = static_cast<std::uint32_t>(out.pool.size());
        for (std::uint32_t k = 0; k < count; ++k) {
            if (!c_.anyApproach || passApproaches(appChain, first + k)) {
                out.pool.push_back(first + k);
            }
        }
        m.count = static_cast<std::uint32_t>(out.pool.size()) - m.begin;
        if (c_.anyApproach && m.count == 0) {
            return;
        }
        out.matches.push_back(m);
    }

    // ---- sort keys -------------------------------------------------------------------

    void computeKeys(const Collected& col, std::vector<double>& key, std::vector<std::uint8_t>& known) const {
        for (std::size_t i = 0; i < col.matches.size(); ++i) {
            const std::uint32_t o = col.matches[i].obj;
            double v = kUnknownValue;
            switch (c_.sortBy) {
            case SortField::Diameter:
                v = c_.diamMode == DiameterMode::MeasuredOnly ? ix_.obj.diameterMeasured[o] : ix_.obj.diameterBest[o];
                break;
            case SortField::AbsoluteMagnitude: v = ix_.obj.h[o]; break;
            case SortField::Moid:              v = ix_.obj.moid[o]; break;
            case SortField::SemiMajorAxis:     v = ix_.obj.a[o]; break;
            case SortField::Eccentricity:      v = ix_.obj.e[o]; break;
            case SortField::Inclination:       v = ix_.obj.i[o]; break;
            case SortField::Date:
            case SortField::Distance:
            case SortField::Velocity: {
                const std::vector<double>& column = c_.sortBy == SortField::Date       ? ix_.app.jd
                                                    : c_.sortBy == SortField::Distance ? ix_.app.dist
                                                                                       : ix_.app.vrel;
                for (std::uint32_t k = 0; k < col.matches[i].count; ++k) {
                    const double x = column[col.pool[col.matches[i].begin + k]];
                    if (std::isnan(v)) {
                        v = x;
                    } else if (c_.dir == SortDirection::Ascending) {
                        v = std::min(v, x);
                    } else {
                        v = std::max(v, x);
                    }
                }
                break;
            }
            case SortField::None:
            case SortField::Designation:
                break;
            }
            if (!std::isnan(v)) {
                key[i] = v;
                known[i] = 1;
            }
        }
    }

    std::string stepText(Pred p) const {
        switch (p) {
        case Pred::Designation: return "designation = '" + c_.designation + "'";
        case Pred::NamePrefix:  return "name or designation starts with '" + c_.prefixLower + "'";
        case Pred::Kind:        return std::string("kind = ") + toString(static_cast<ObjectKind>(c_.kind));
        case Pred::Neo:         return std::string("neo = ") + (c_.neoWant == 1 ? "yes" : "no");
        case Pred::Pha:         return std::string("pha = ") + (c_.phaWant == 1 ? "yes" : "no");
        case Pred::OrbitClass: {
            std::string list;
            for (const std::string& code : q_.orbitClasses) {
                list += (list.empty() ? "" : ",") + code;
            }
            return "class in {" + list + "}";
        }
        case Pred::Diameter:
            return "diameter " + boundsText(c_.diam) + " km (" + toString(c_.diamMode) + ")";
        case Pred::H:         return "H " + boundsText(c_.h);
        case Pred::Moid:      return "moid " + boundsText(c_.moid) + " au";
        case Pred::SemiMajor: return "a " + boundsText(c_.a) + " au";
        case Pred::Ecc:       return "e " + boundsText(c_.e);
        case Pred::Inc:       return "i " + boundsText(c_.i) + " deg";
        case Pred::Date:      return "date " + dateBoundsText(c_.date);
        case Pred::Dist:      return "dist " + boundsText(c_.dist) + " au";
        case Pred::VRel:      return "v_rel " + boundsText(c_.vel) + " km/s";
        case Pred::Grazing:   return std::string("grazing = ") + (c_.grazingWant ? "yes" : "no");
        case Pred::Count:     break;
        }
        return "?";
    }

    const IndexSet& ix_;
    const Dataset&  ds_;
    const Query&    q_;
    const Compiled& c_;
    double nObj_;
    double nApp_;
    double sel_[kPreds] = {};
    std::uint64_t evaluated_[kPreds] = {};
    std::uint64_t passed_[kPreds] = {};
};

} // namespace

// ---------------------------------------------------------------------------
// QueryEngine
// ---------------------------------------------------------------------------

void QueryEngine::build() {
    ix_.build(*dataset_);
    built_ = true;
}

std::uint32_t QueryEngine::findDesignation(const std::string& pdes) const {
    const std::uint32_t* found = ix_.byDesignation.find(pdes);
    return found == nullptr ? kInvalidRecord : *found;
}

std::uint32_t QueryEngine::findSpkId(const std::string& spkid) const {
    const std::uint32_t* found = ix_.bySpkId.find(spkid);
    return found == nullptr ? kInvalidRecord : *found;
}

std::vector<std::uint32_t> QueryEngine::searchNames(const std::string& prefix, std::size_t limit) const {
    std::vector<std::uint32_t> out = ix_.names.matches(toLowerAscii(prefix));
    if (limit > 0 && out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

QueryResult QueryEngine::run(const Query& q, ExecMode mode, std::optional<Access> force) const {
    const Clock::time_point start = Clock::now();
    QueryResult res;
    res.stats.mode = mode;
    res.stats.queryText = describe(q);

    res.errors = validate(q);
    if (!res.errors.empty()) {
        res.ok = false;
        return res;
    }
    if (mode != ExecMode::Naive && !built_) {
        res.ok = false;
        res.errors.push_back("the indexes are not built: call QueryEngine::build() before run()");
        return res;
    }

    if (mode == ExecMode::Naive) {
        NaiveRunner naive(*dataset_, q);
        const Clock::time_point execStart = Clock::now();
        naive.run(res);
        res.stats.execMs = msSince(execStart);

        QueryStats& s = res.stats;
        s.driverAccess = Access::ScanObjects;
        s.driverText = "full scan of all objects (no index)";
        s.estCandidates = static_cast<double>(dataset_->records().size());
        for (std::size_t p = 0; p < kPreds; ++p) {
            if (naive.evaluated[p] == 0) {
                continue;
            }
            StepStat step;
            step.text = predLabel(static_cast<Pred>(p));
            step.estSelectivity = -1.0; // the oracle does no estimation
            step.evaluated = naive.evaluated[p];
            step.passed = naive.passed[p];
            step.approachUniverse = isApproachPred(static_cast<Pred>(p));
            s.steps.push_back(step);
            s.predicateEvaluations += step.evaluated;
        }
    } else {
        const Compiled compiled = compile(q, ix_);
        Indexed indexed(ix_, q, compiled);

        const Clock::time_point planStart = Clock::now();
        const Plan plan = mode == ExecMode::Planned ? indexed.planCosted(force) : indexed.planFixed(force);
        res.stats.planMs = msSince(planStart);

        const Clock::time_point execStart = Clock::now();
        Collected col;
        indexed.execute(plan, col);
        res.stats.actualCandidates = col.candidates;
        indexed.finish(col, res);
        res.stats.execMs = msSince(execStart);
        indexed.fillStats(plan, res.stats);
    }

    res.stats.matchedObjects = res.totalObjects;
    res.stats.matchedApproaches = res.totalApproaches;
    res.stats.returnedObjects = res.rows.size();
    res.stats.totalMs = msSince(start);
    return res;
}

// ---------------------------------------------------------------------------
// EXPLAIN
// ---------------------------------------------------------------------------

std::string QueryStats::summaryLine() const {
    char buf[320];
    if (mode == ExecMode::Naive) {
        std::snprintf(buf, sizeof buf, "naive scan | %s objects matched | %.2f ms", commas(matchedObjects).c_str(), totalMs);
    } else {
        std::snprintf(buf, sizeof buf, "driver: %s | candidates est %.0f / actual %s | %s objects | %.2f ms",
                      toString(driverAccess), estCandidates, commas(actualCandidates).c_str(),
                      commas(matchedObjects).c_str(), totalMs);
    }
    return buf;
}

std::string QueryStats::explain() const {
    std::string out;
    char buf[512];
    auto line = [&out, &buf](const char* label, const std::string& text) {
        std::snprintf(buf, sizeof buf, "  %-11s %s\n", label, text.c_str());
        out += buf;
    };
    auto cont = [&out, &buf](const std::string& text) {
        std::snprintf(buf, sizeof buf, "              %s\n", text.c_str());
        out += buf;
    };

    out += std::string("EXPLAIN (") + toString(mode) + ")\n";
    line("query", queryText);

    line("driver", driverText);
    {
        std::string est;
        if (mode == ExecMode::Naive) {
            est = "every object is read: " + commas(actualCandidates) + " candidates";
        } else {
            std::snprintf(buf, sizeof buf, "%s access, %s; estimated %.0f candidates, actual %s", toString(driverAccess),
                          driverExact ? "exact" : "superset (the predicate is re-checked)", estCandidates,
                          commas(actualCandidates).c_str());
            est = buf;
            // A scan reads everything by definition, so its "estimate" is not one.
            const bool scan = driverAccess == Access::ScanObjects || driverAccess == Access::ScanApproaches;
            if (estCandidates >= 1.0 && driverExact && !scan) {
                const double error = (static_cast<double>(actualCandidates) - estCandidates) / estCandidates * 100.0;
                std::snprintf(buf, sizeof buf, " (%+.1f%% estimate error)", error);
                est += buf;
            }
        }
        cont(est);
    }

    if (steps.empty()) {
        line("steps", "(no residual predicates)");
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const StepStat& s = steps[i];
        std::string text = std::to_string(i + 1) + ". " + s.text + (s.approachUniverse ? "   [approach row]" : "   [object]");
        if (i == 0) {
            line("steps", text);
        } else {
            cont(text);
        }
        if (s.estSelectivity >= 0.0) {
            std::snprintf(buf, sizeof buf, "     est selectivity %.4f, unit cost %.1f | evaluated %s, passed %s (actual %.4f)",
                          s.estSelectivity, s.unitCost, commas(s.evaluated).c_str(), commas(s.passed).c_str(),
                          s.evaluated > 0 ? static_cast<double>(s.passed) / static_cast<double>(s.evaluated) : 0.0);
        } else {
            std::snprintf(buf, sizeof buf, "     evaluated %s, passed %s", commas(s.evaluated).c_str(),
                          commas(s.passed).c_str());
        }
        cont(buf);
    }

    if (!considered.empty()) {
        for (std::size_t i = 0; i < considered.size(); ++i) {
            const ConsideredPlan& c = considered[i];
            std::snprintf(buf, sizeof buf, "%s est work %10.0f, est %9.0f candidates  %s", c.chosen ? "*" : " ", c.estWork,
                          c.estCandidates, c.text.c_str());
            if (i == 0) {
                line("considered", buf);
            } else {
                cont(buf);
            }
        }
    }

    std::snprintf(buf, sizeof buf, "candidates examined %s, predicate evaluations %s", commas(actualCandidates).c_str(),
                  commas(predicateEvaluations).c_str());
    line("work", buf);
    std::snprintf(buf, sizeof buf, "%s objects (%s approaches) matched; %s returned", commas(matchedObjects).c_str(),
                  commas(matchedApproaches).c_str(), commas(returnedObjects).c_str());
    line("result", buf);
    if (mode == ExecMode::Naive) {
        std::snprintf(buf, sizeof buf, "%.3f ms", totalMs);
    } else {
        std::snprintf(buf, sizeof buf, "plan %.3f ms + execute %.3f ms = %.3f ms", planMs, execMs, totalMs);
    }
    line("time", buf);
    return out;
}

} // namespace neo
