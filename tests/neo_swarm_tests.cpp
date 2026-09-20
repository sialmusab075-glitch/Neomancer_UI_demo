// NEOS layer tests: selection presets, propagation, the node-grid field, the legend.
//
//   kernel      propagateSwarm == sim::propagate (position 1e-12 AU, velocity 1e-10 relative),
//               and the position AT an object's own epoch is the published mean anomaly
//   selection   nested numbered presets in RANK order (diameter, largest first),
//               deterministic, PHAs, e >= 1 skipped, "current result" filtered
//   field       DIRECT mode matches sim::propagate; NODES mode (worker thread + Hermite
//               interpolation) matches it at and between nodes; the grid step follows
//               the clock rate; replacing the objects while work is queued is safe
//   legend      diameter / PHA attributes, days to the next approach
//
// No network, no window. The real-dataset part skips when data/neo.db is absent.

#include "neo/model/JulianDate.h"
#include "neo/query/NeoService.h"
#include "neo/sim/SwarmField.h"
#include "neo/sim/SwarmLegend.h"
#include "neo/sim/SwarmSelection.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what.c_str(), detail.c_str());
    }
}

std::string num(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// Deterministic pseudo-random numbers (no <random>: the sequence must not depend on the library).
struct Lcg {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
};

neo::Asteroid randomAsteroid(Lcg& rng, int id) {
    neo::Asteroid a;
    a.pdes = "T" + std::to_string(id);
    a.spkid = "S" + std::to_string(id);
    const double q = rng.range(0.15, 1.6);
    a.orbital.eccentricity = rng.range(0.0, 0.92);
    a.orbital.semiMajorAxisAU = q / (1.0 - a.orbital.eccentricity);
    a.orbital.inclinationDeg = rng.range(0.0, 60.0);
    a.orbital.ascendingNodeDeg = rng.range(0.0, 360.0);
    a.orbital.argPerihelionDeg = rng.range(0.0, 360.0);
    a.orbital.meanAnomalyDeg = rng.range(0.0, 360.0);
    a.orbital.epochJdTdb = 2451545.0 + rng.range(-9000.0, 9000.0);
    return a;
}

sim::OrbitState reference(const neo::Asteroid& a, double tDays) {
    return sim::propagate(neo::toSimElements(a), sim::kMuSun_m3s2, tDays);
}

double distance(const double p[3], const sim::Vec3d& q) {
    return std::sqrt((p[0] - q.x) * (p[0] - q.x) + (p[1] - q.y) * (p[1] - q.y) + (p[2] - q.z) * (p[2] - q.z));
}

template <class Pred>
bool waitFor(Pred pred, int timeoutMs = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// --- kernel -----------------------------------------------------------------------------

void testKernel() {
    std::printf("[kernel] propagateSwarm against sim::propagate\n");
    Lcg rng;

    // At the object's own epoch the mean anomaly is the published one.
    {
        neo::Asteroid a = randomAsteroid(rng, 0);
        const sim::OrbitState s = reference(a, a.orbital.epochJdTdb - neo::kJ2000Jd);
        const double published = sim::wrapPi(a.orbital.meanAnomalyDeg * sim::kDegToRad);
        check(std::fabs(sim::wrapPi(s.M - published)) < 1e-9, "the mean anomaly at the epoch is the published one",
              num(s.M) + " vs " + num(published));
    }

    double maxPos = 0.0, maxVel = 0.0;
    int compared = 0;
    for (int i = 0; i < 400; ++i) {
        const neo::Asteroid a = randomAsteroid(rng, i);
        const neo::SwarmElements el = neo::makeSwarmElements(a);
        for (int k = 0; k < 6; ++k) {
            const double t = rng.range(-60000.0, 60000.0);
            const sim::OrbitState ref = reference(a, t);
            double p[3], v[3];
            neo::propagateSwarm(el, t, p, v);
            maxPos = std::max(maxPos, distance(p, ref.pos_AU));
            const double toAuPerDay = 86400.0 / sim::kAU_km;
            const sim::Vec3d rv{ref.vel_kms.x * toAuPerDay, ref.vel_kms.y * toAuPerDay, ref.vel_kms.z * toAuPerDay};
            maxVel = std::max(maxVel, distance(v, rv) / std::max(sim::length(rv), 1e-12));
            ++compared;
        }
    }
    std::printf("  %d (object, time) pairs: max position difference %.3g AU, max relative velocity difference %.3g\n",
                compared, maxPos, maxVel);
    check(maxPos < 1e-12, "positions match sim::propagate to 1e-12 AU", num(maxPos));
    check(maxVel < 1e-10, "velocities match to 1e-10 relative", num(maxVel));

    // Hyperbolic and parabolic orbits are never propagated: the catalogue drops them.
    neo::Asteroid h = randomAsteroid(rng, 999);
    h.orbital.eccentricity = 1.2;
    check(!h.orbital.propagationSupported(), "e >= 1 is flagged unsupported");
}

// --- selection --------------------------------------------------------------------------

struct Obj {
    std::string pdes;
    std::optional<double> diameter;
    std::optional<double> h;
    std::optional<bool> pha;
    double e = 0.3;
};

neo::Dataset buildDataset(const std::vector<Obj>& objects) {
    std::vector<neo::Asteroid> list;
    Lcg rng;
    int id = 0;
    for (const Obj& o : objects) {
        neo::Asteroid a = randomAsteroid(rng, id++);
        a.pdes = o.pdes;
        a.spkid = "spk" + o.pdes;
        a.physical.diameterKm = o.diameter;
        a.physical.absoluteMagnitudeH = o.h;
        a.classification.isPHA = o.pha;
        a.classification.isNEO = true;
        a.orbital.eccentricity = o.e;
        list.push_back(std::move(a));
    }
    neo::Dataset ds;
    ds.setObjects(std::move(list));
    return ds;
}

std::string names(const neo::Dataset& ds, const std::vector<std::uint32_t>& records) {
    std::string s;
    for (const std::uint32_t r : records) {
        s += ds.records()[r].object.pdes + " ";
    }
    return s;
}

void testSelection() {
    std::printf("[selection] presets are nested, deterministic and ranked by size\n");
    // 0 measured 1.0 km | 1 estimated (H=18 -> ~0.9 km) | 2 measured 5 km, PHA | 3 unknown | 4 measured 0.01 km, PHA
    // 5 hyperbolic (skipped) | 6 measured 2.0 km, PHA=null (unknown flag) | 7 measured 5 km (tie with 2), not PHA
    neo::Dataset ds = buildDataset({
        {"a", 1.0, 20.0, false},
        {"b", std::nullopt, 18.0, false},
        {"c", 5.0, 15.0, true},
        {"d", std::nullopt, std::nullopt, false},
        {"e", 0.01, 25.0, true},
        {"f", 9.0, 12.0, false, 1.3},
        {"g", 2.0, 17.0, std::nullopt},
        {"h", 5.0, 15.5, false},
    });
    const neo::SwarmCatalog cat(ds);
    check(cat.objectCount() == 8 && cat.propagatable() == 7, "seven of eight objects can be propagated",
          std::to_string(cat.propagatable()));
    const std::vector<std::uint32_t> all = cat.select(neo::SwarmPreset::All);
    check(names(ds, all) == "c h g a b e d ", "RANK: measured or estimated diameter, largest first, unknown last, ties by index",
          names(ds, all));
    check(names(ds, cat.select(neo::SwarmPreset::All)) == names(ds, all), "the order is deterministic");

    const std::vector<std::uint32_t> pha = cat.select(neo::SwarmPreset::PhaOnly);
    check(names(ds, pha) == "c e " && cat.phaCount() == 2, "PHAs only: those flagged yes, in RANK order (unknown flag is not a PHA)",
          names(ds, pha));

    // Numbered presets are prefixes of RANK (nested), clamped to what exists.
    const std::vector<std::uint32_t> n1000 = cat.select(neo::SwarmPreset::N1000);
    check(n1000 == all, "a preset larger than the dataset gives everything");
    check(neo::presetCount(neo::SwarmPreset::N5000) == 5000 && neo::presetCount(neo::SwarmPreset::All) == 0, "preset sizes");

    const std::vector<std::uint32_t> current = {3, 5, 0, 99};
    const std::vector<std::uint32_t> cur = cat.select(neo::SwarmPreset::CurrentResult, &current);
    check(names(ds, cur) == "d a ", "the current result keeps its order, drops unpropagatable and unknown records", names(ds, cur));
    check(cat.select(neo::SwarmPreset::CurrentResult, nullptr).empty(), "no result yet: nothing");

    // A bigger dataset for the nesting property.
    std::vector<Obj> many;
    Lcg rng;
    for (int i = 0; i < 3000; ++i) {
        Obj o;
        o.pdes = "m" + std::to_string(i);
        if (rng.next() < 0.8) o.diameter = std::pow(10.0, rng.range(-2.0, 1.0));
        if (rng.next() < 0.9) o.h = rng.range(14.0, 28.0);
        if (rng.next() < 0.1) o.pha = true; else if (rng.next() < 0.8) o.pha = false;
        many.push_back(o);
    }
    neo::Dataset big = buildDataset(many);
    const neo::SwarmCatalog bc(big);
    const std::vector<std::uint32_t> a1 = bc.select(neo::SwarmPreset::N1000);
    const std::vector<std::uint32_t> a2 = bc.select(neo::SwarmPreset::N5000);
    check(a1.size() == 1000 && a2.size() == 3000, "counts: 1,000 and everything for a 3,000-object set");
    check(std::equal(a1.begin(), a1.end(), a2.begin()), "1,000 is a prefix of 5,000: raising the count only adds points");
    std::set<std::uint32_t> unique(a2.begin(), a2.end());
    check(unique.size() == a2.size(), "no object twice");
    bool sorted = true;
    double prev = 1e300;
    for (const std::uint32_t r : a2) {
        const std::optional<double> d = big.records()[r].object.physical.bestDiameterKm();
        const double v = d ? *d : -1.0;
        sorted = sorted && v <= prev;
        prev = v;
    }
    check(sorted, "non-increasing diameter along the ranking");
}

// --- field --------------------------------------------------------------------------------

std::vector<neo::Asteroid> randomSet(std::uint64_t seed, int n) {
    Lcg rng;
    rng.s ^= seed * 0x2545F4914F6CDD1Dull;
    std::vector<neo::Asteroid> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(randomAsteroid(rng, i));
    }
    return out;
}

std::vector<neo::SwarmElements> elementsOf(const std::vector<neo::Asteroid>& set) {
    std::vector<neo::SwarmElements> out;
    for (const neo::Asteroid& a : set) {
        out.push_back(neo::makeSwarmElements(a));
    }
    return out;
}

double maxError(const std::vector<neo::Asteroid>& set, const std::vector<float>& got, double t) {
    double worst = 0.0;
    for (std::size_t i = 0; i < set.size(); ++i) {
        const sim::Vec3d ref = reference(set[i], t).pos_AU;
        const double p[3] = {got[3 * i], got[3 * i + 1], got[3 * i + 2]};
        worst = std::max(worst, distance(p, ref));
    }
    return worst;
}

// The same, relative to max(1 AU, the object's distance from the Sun): a float has
// 24 bits, so an object at 20 AU cannot be placed better than about 2e-6 AU.
double maxRelError(const std::vector<neo::Asteroid>& set, const std::vector<float>& got, double t) {
    double worst = 0.0;
    for (std::size_t i = 0; i < set.size(); ++i) {
        const sim::Vec3d ref = reference(set[i], t).pos_AU;
        const double p[3] = {got[3 * i], got[3 * i + 1], got[3 * i + 2]};
        worst = std::max(worst, distance(p, ref) / std::max(1.0, sim::length(ref)));
    }
    return worst;
}

bool settle(neo::SwarmField& f, double t, double rate, std::vector<float>& out) {
    return waitFor([&] { return f.positionsAt(t, rate, out); }, 10000);
}

void testField() {
    std::printf("[field] direct and node-grid modes against sim::propagate\n");
    const std::vector<neo::Asteroid> set = randomSet(1, 300);

    // DIRECT: the rendered (float) positions are sim::propagate's, to float precision.
    {
        neo::SwarmField f;
        f.setObjects(elementsOf(set));
        check(f.direct() && f.size() == 300, "300 objects are served directly");
        std::vector<float> out;
        double worst = 0.0;
        for (const double t : {0.0, 123.456, -4000.25, 9000.0, 20000.75}) {
            check(f.positionsAt(t, 10.0, out) && out.size() == 900, "a direct frame is always ready");
            worst = std::max(worst, maxRelError(set, out, t));
        }
        std::printf("  direct: worst rendered position error %.3g (relative to max(1 AU, r))\n", worst);
        check(worst < 2e-7, "direct positions match sim::propagate to float precision (2e-7 relative)", num(worst));
    }

    // NODES: worker thread + Hermite interpolation.
    {
        neo::SwarmField f;
        f.setObjects(elementsOf(set));
        f.setDirectLimit(0);
        check(!f.direct(), "with the direct limit at 0 the node grid is used");
        std::vector<float> out;

        check(settle(f, 9000.0, 1.0, out) && out.size() == 900, "the first frame becomes ready once the worker has computed the nodes");
        const double atNode = maxRelError(set, out, 9000.0);
        check(f.stepDays() == 1.0 && atNode < 2e-7, "at a node the position is exact to float precision", num(atNode));

        check(settle(f, 9000.5, 1.0, out), "a mid-node frame becomes ready");
        const double mid = maxError(set, out, 9000.5);
        check(settle(f, 9000.13, 1.0, out), "so does another");
        const double odd = maxError(set, out, 9000.13);
        std::printf("  nodes, step 1 d: error at a node %.3g, mid-node %.3g, at u=0.13 %.3g AU\n", atNode, mid, odd);
        check(mid < 5e-5 && odd < 5e-5, "between nodes the Hermite interpolation is within 5e-5 AU (7,500 km)", num(std::max(mid, odd)));

        // Running the clock forwards: consecutive frames, no gaps once the pipeline is primed.
        double t = 9000.5;
        int notReady = 0;
        for (int i = 0; i < 200; ++i) {
            t += 0.4;
            if (!f.positionsAt(t, 24.0, out)) {
                ++notReady;
                settle(f, t, 24.0, out);
            }
        }
        check(maxError(set, out, t) < 5e-5, "after playing forward 80 days the positions are still right", num(maxError(set, out, t)));
        std::printf("  playing forward: %d of 200 frames had to wait for the worker\n", notReady);

        // Backwards: the prefetch goes the other way.
        for (int i = 0; i < 100; ++i) {
            t -= 0.4;
            if (!f.positionsAt(t, -24.0, out)) {
                settle(f, t, -24.0, out);
            }
        }
        check(maxError(set, out, t) < 5e-5, "and backwards", num(maxError(set, out, t)));

        // A jump of centuries is served after the worker has caught up, and is correct.
        check(settle(f, 90000.3, 1.0, out), "a jump of two centuries is served");
        check(maxError(set, out, 90000.3) < 5e-5, "correct after the jump", num(maxError(set, out, 90000.3)));
        const neo::SwarmField::Stats st = f.stats();
        check(st.nodesComputed > 0 && st.lastNodeMs >= 0.0, "the worker reports what it did");

        // The clock rate sets the grid step: coarser when fast, finer again when slow.
        settle(f, 90001.0, 200.0, out);
        check(f.stepDays() == 8.0, "200 d/s asks for an 8-day grid (30 nodes/s at most)", num(f.stepDays()));
        settle(f, 90002.0, 5.0, out);
        check(f.stepDays() == 1.0, "and 5 d/s returns to 1 day", num(f.stepDays()));
        settle(f, 90003.0, 0.0, out);
        check(f.stepDays() == 1.0, "a pause keeps the grid", num(f.stepDays()));
        settle(f, 90000.0, 2000.0, out);
        check(f.stepDays() == 32.0, "the grid is capped at 32 days", num(f.stepDays()));
        const double coarse = maxError(set, out, 90000.0);
        std::printf("  32-day grid: error %.3g AU mid-node, the worst object (information only; asserted at 1 day)\n", coarse);
    }

    // Replacing the objects while work is queued or running never mixes the sets.
    {
        neo::SwarmField f;
        f.setDirectLimit(0);
        std::vector<float> out;
        const std::vector<neo::Asteroid> a = randomSet(2, 500);
        const std::vector<neo::Asteroid> b = randomSet(3, 700);
        for (int round = 0; round < 6; ++round) {
            f.setObjects(elementsOf(a));
            f.positionsAt(100.0 + round, 1.0, out); // queues work for `a`, does not wait
            f.setObjects(elementsOf(b));
            check(settle(f, 200.5, 1.0, out) && out.size() == 700 * 3, "after replacing the objects only the new set is served");
            check(maxError(b, out, 200.5) < 5e-5, "and its positions are its own", num(maxError(b, out, 200.5)));
        }
        f.setObjects({});
        check(f.positionsAt(1.0, 1.0, out) && out.empty(), "an empty set is served as nothing");
    }
}

// --- legend -------------------------------------------------------------------------------

void testLegend() {
    std::printf("[legend] per-object attributes and time to the next approach\n");
    neo::Asteroid a;
    a.physical.diameterKm = 2.0;
    a.classification.isPHA = true;
    neo::SwarmAttr at = neo::makeSwarmAttr(a);
    check(std::fabs(at.logDiameterKm - std::log10(2.0f)) < 1e-6 && at.pha == 1.0f, "measured diameter and PHA flag");
    neo::Asteroid b;
    b.physical.absoluteMagnitudeH = 22.0;
    at = neo::makeSwarmAttr(b);
    check(at.logDiameterKm > -2.0f && at.logDiameterKm < 0.0f && at.pha == 0.0f, "estimated from H when not measured");
    neo::Asteroid c;
    at = neo::makeSwarmAttr(c);
    check(at.logDiameterKm == neo::kUnknownLogDiameter && at.pha == 0.0f, "no diameter and no H: the unknown sentinel");
    check(at.daysToApproach == neo::kNoApproach, "and no approach until the caller fills it in");

    neo::Dataset ds;
    {
        std::vector<neo::Asteroid> list(2);
        list[0].pdes = "x";
        list[0].spkid = "1";
        list[1].pdes = "y";
        list[1].spkid = "2";
        ds.setObjects(std::move(list));
        std::vector<neo::CloseApproach> rows;
        for (const double jd : {2460000.0, 2460100.0, 2460300.0}) {
            neo::CloseApproach r;
            r.objectIndex = 0;
            r.jdTdb = jd;
            rows.push_back(r);
        }
        ds.setApproaches(std::move(rows));
    }
    check(neo::daysToNextApproach(ds, 0, 2459990.0) == 10.0f, "the next approach is the earliest one after now");
    check(neo::daysToNextApproach(ds, 0, 2460000.0) == 100.0f, "an approach exactly now is 'now', so the next one is the answer");
    check(neo::daysToNextApproach(ds, 0, 2460150.5) == 149.5f, "in the middle of the list");
    check(neo::daysToNextApproach(ds, 0, 2460300.0) == neo::kNoApproach, "after the last one there is none");
    check(neo::daysToNextApproach(ds, 1, 2450000.0) == neo::kNoApproach, "an object without approaches has none");
    check(neo::daysToNextApproach(ds, 77, 2450000.0) == neo::kNoApproach, "an unknown record has none");
    for (int i = 0; i < neo::kSwarmLegendCount; ++i) {
        const auto l = static_cast<neo::SwarmLegend>(i);
        check(std::string(neo::toString(l)).size() > 0 && std::string(neo::legendNearText(l)).size() > 0 &&
                  std::string(neo::legendFarText(l)).size() > 0,
              "every legend has a name and both ends");
    }
}

// --- the real dataset ----------------------------------------------------------------------

void testReal() {
    std::printf("[real] the NEOS pipeline over data/neo.db, if it exists\n");
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
        std::printf("  skipped: no neo.db\n");
        return;
    }
    neo::NeoService service;
    service.startLoad(path);
    if (!waitFor([&] { return service.state() != neo::NeoService::State::Loading; }, 60000) ||
        service.state() != neo::NeoService::State::Ready) {
        std::printf("  skipped: %s\n", service.message().c_str());
        return;
    }
    const neo::Dataset& ds = *service.dataset();
    const neo::SwarmCatalog cat(ds);
    std::printf("  %zu objects, %zu propagatable, %zu PHAs\n", cat.objectCount(), cat.propagatable(), cat.phaCount());
    check(cat.propagatable() > 0 && cat.propagatable() <= cat.objectCount(), "the catalogue is built");
    const std::vector<std::uint32_t> all = cat.select(neo::SwarmPreset::All);
    const std::vector<std::uint32_t> pha = cat.select(neo::SwarmPreset::PhaOnly);
    const std::vector<std::uint32_t> k1 = cat.select(neo::SwarmPreset::N1000);
    const std::vector<std::uint32_t> k5 = cat.select(neo::SwarmPreset::N5000);
    const std::vector<std::uint32_t> k20 = cat.select(neo::SwarmPreset::N20000);
    check(k1.size() == 1000 && k5.size() == 5000 && k20.size() == 20000, "the numbered presets have their sizes");
    check(std::equal(k1.begin(), k1.end(), k5.begin()) && std::equal(k5.begin(), k5.end(), k20.begin()) &&
              std::equal(k20.begin(), k20.end(), all.begin()),
          "1,000 < 5,000 < 20,000 < All, each a prefix of the next");
    bool allPha = true;
    for (const std::uint32_t r : pha) {
        const neo::Flag f = ds.records()[r].object.classification.isPHA;
        allPha = allPha && f && *f;
    }
    check(allPha && !pha.empty(), "every object in the PHA preset is a PHA");
    std::printf("  largest object: %s (%.1f km); 1,000th: %s\n", ds.records()[all[0]].object.label().c_str(),
                *ds.records()[all[0]].object.physical.bestDiameterKm(), ds.records()[k1.back()].object.label().c_str());
    check(*ds.records()[all[0]].object.physical.bestDiameterKm() >= *ds.records()[all[1]].object.physical.bestDiameterKm(),
          "the ranking starts with the largest");

    // Propagation of the whole catalogue: sane radii, and what a frame of it costs.
    std::vector<neo::SwarmElements> els = neo::makeSwarmElements(ds, all);
    std::vector<float> out;
    neo::SwarmField field;
    field.setObjects(els);
    field.setDirectLimit(1000000);
    double jd = 0.0;
    neo::julianDateFromIsoDate("2026-09-20", jd);
    const double tNow = neo::daysSinceJ2000(jd);
    const auto start = std::chrono::steady_clock::now();
    field.positionsAt(tNow, 1.0, out);
    const std::chrono::duration<double, std::milli> ms = std::chrono::steady_clock::now() - start;
    std::printf("  one direct frame of all %zu objects: %.2f ms\n", all.size(), ms.count());
    bool inBounds = out.size() == all.size() * 3;
    std::size_t checked = 0;
    for (std::size_t i = 0; i < all.size() && inBounds; i += 37) {
        const neo::Asteroid& a = ds.records()[all[i]].object;
        const double r = std::sqrt(static_cast<double>(out[3 * i]) * out[3 * i] + static_cast<double>(out[3 * i + 1]) * out[3 * i + 1] +
                                   static_cast<double>(out[3 * i + 2]) * out[3 * i + 2]);
        const double q = a.orbital.semiMajorAxisAU * (1.0 - a.orbital.eccentricity);
        const double apo = a.orbital.semiMajorAxisAU * (1.0 + a.orbital.eccentricity);
        inBounds = inBounds && r >= q - 1e-5 && r <= apo + 1e-5;
        ++checked;
    }
    check(inBounds, "every sampled object lies between its perihelion and aphelion", std::to_string(checked) + " sampled");

    // A real object compared with sim::propagate directly.
    const std::uint32_t rec = all[123];
    const neo::Asteroid& a = ds.records()[rec].object;
    const sim::Vec3d ref = reference(a, tNow).pos_AU;
    const double p[3] = {out[3 * 123], out[3 * 123 + 1], out[3 * 123 + 2]};
    check(distance(p, ref) < 1e-6, "a real object's drawn position is sim::propagate's", a.label() + " " + num(distance(p, ref)));

    // One node on the worker: the cost of the threaded mode.
    neo::SwarmField nodes;
    nodes.setObjects(els);
    nodes.setDirectLimit(0);
    std::vector<float> o2;
    check(settle(nodes, tNow, 1.0, o2), "the worker serves all objects through the node grid");
    const neo::SwarmField::Stats st = nodes.stats();
    std::printf("  worker: %.2f ms per node of %zu objects (%llu nodes so far)\n", st.lastNodeMs, all.size(),
                static_cast<unsigned long long>(st.nodesComputed));
    double worst = 0.0;
    for (std::size_t i = 0; i < all.size(); i += 53) {
        const sim::Vec3d rp = reference(ds.records()[all[i]].object, tNow).pos_AU;
        const double q[3] = {o2[3 * i], o2[3 * i + 1], o2[3 * i + 2]};
        worst = std::max(worst, distance(q, rp));
    }
    check(worst < 5e-5, "and its interpolated positions are sim::propagate's, over the real population", num(worst));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testKernel();
    testSelection();
    testField();
    testLegend();
    testReal();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
