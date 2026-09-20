// Earth view tests: query -> flyby mapping, the filter form, and the async service.
//
//   radial scale   monotonic, continuous, invertible; ring order preserved
//   directions     deterministic from the designation; unit, perpendicular, spread
//   flyby mapping  the closest point is at the real distance (log-scaled) and is
//                  reached AT the real CAD time; speed is proportional to v_rel;
//                  paths never pass nearer than their closest point
//   scene          built from a real QueryResult: one flyby per MATCHING approach,
//                  marker size and hollow/solid from the diameter, cap reported
//   filter form    units, "0 = no limit", inclusive end date, error messages
//   service        async load and query on a worker thread, latest-wins, failure
//
// No network, no window. The real-dataset part skips when data/neo.db is absent.

#include "neo/model/JulianDate.h"
#include "neo/query/FilterState.h"
#include "neo/query/NeoService.h"
#include "neo/sim/EarthFlybys.h"
#include "neo/storage/Database.h"
#include "sim/Constants.h"

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

double jd(const char* iso) {
    double out = 0.0;
    neo::julianDateFromIsoDate(iso, out);
    return out;
}

double angleDeg(const sim::Vec3d& a, const sim::Vec3d& b) {
    const double c = std::clamp(sim::dot(a, b) / (sim::length(a) * sim::length(b)), -1.0, 1.0);
    return std::acos(c) * 180.0 / sim::kPi;
}

// --- a small dataset with known values ------------------------------------------

struct Obj {
    std::string pdes;
    std::optional<double> diameter;
    std::optional<double> h;
    std::optional<bool> pha;
};
struct App {
    std::size_t object;
    double jd;
    double dist;
    double vrel;
};

neo::Dataset buildDataset(const std::vector<Obj>& objects, const std::vector<App>& approaches) {
    std::vector<neo::Asteroid> list;
    for (const Obj& o : objects) {
        neo::Asteroid a;
        a.pdes = o.pdes;
        a.spkid = "spk" + o.pdes;
        a.physical.diameterKm = o.diameter;
        a.physical.absoluteMagnitudeH = o.h;
        a.classification.isPHA = o.pha;
        a.classification.isNEO = true;
        a.orbital.eccentricity = 0.3;
        a.orbital.semiMajorAxisAU = 1.2;
        list.push_back(std::move(a));
    }
    neo::Dataset ds;
    ds.setObjects(std::move(list));
    std::vector<neo::CloseApproach> rows;
    for (const App& p : approaches) {
        neo::CloseApproach c;
        c.objectIndex = static_cast<std::uint32_t>(p.object);
        c.jdTdb = p.jd;
        c.distanceAU = p.dist;
        c.distanceMinAU = p.dist * 0.99;
        c.distanceMaxAU = p.dist * 1.01;
        c.relVelocityKms = p.vrel;
        rows.push_back(c);
    }
    ds.setApproaches(std::move(rows));
    return ds;
}

// --- radial scale -------------------------------------------------------------------

void testRadialScale() {
    std::printf("[scale] the log radial scale\n");
    const neo::EarthViewScale s;
    check(near(neo::radialFromKm(s.earthRadiusKm, s), 1.0, 1e-12), "the Earth's surface is at radius 1");
    check(neo::radialFromKm(0.0, s) == 0.0, "the centre is at 0");
    check(near(neo::radialFromKm(s.earthRadiusKm * 0.5, s), 0.5, 1e-12), "inside the planet the scale is linear");

    // Continuity at the surface: no jump between the two branches.
    const double below = neo::radialFromKm(s.earthRadiusKm * (1.0 - 1e-9), s);
    const double above = neo::radialFromKm(s.earthRadiusKm * (1.0 + 1e-9), s);
    check(near(below, above, 1e-6), "continuous across the surface", num(below) + " vs " + num(above));

    // Strictly increasing over ten decades of distance.
    bool monotone = true;
    double previous = -1.0;
    for (double km = 1.0; km < 1e10; km *= 1.05) {
        const double r = neo::radialFromKm(km, s);
        monotone = monotone && r > previous;
        previous = r;
    }
    check(monotone, "radius strictly increases with distance from 1 km to 10 billion km");

    // Round trip.
    bool roundTrip = true;
    for (double km = 10.0; km < 1e9; km *= 1.37) {
        roundTrip = roundTrip && near(neo::kmFromRadial(neo::radialFromKm(km, s), s) / km, 1.0, 1e-9);
    }
    check(roundTrip, "kmFromRadial inverts radialFromKm");
    check(near(neo::radialFromAu(1.0 / sim::kAU_km, s), neo::radialFromKm(1.0, s), 1e-15), "AU and km agree");

    // A decade of distance is always logSlope render units.
    check(near(neo::radialFromKm(1e6, s) - neo::radialFromKm(1e5, s), s.logSlope, 1e-9),
          "one decade of distance is logSlope render units");

    // The reference rings keep their real-world order, GEO inside 1 LD inside 5 LD inside 0.05 AU.
    const std::vector<neo::ReferenceRing>& rings = neo::referenceRings();
    check(rings.size() == 4, "four reference rings");
    bool ordered = true;
    for (std::size_t i = 1; i < rings.size(); ++i) {
        ordered = ordered && neo::radialFromKm(rings[i].km, s) > neo::radialFromKm(rings[i - 1].km, s);
    }
    check(ordered, "the rings are nested in order on the log scale");
    check(near(rings[0].km, 42164.0, 1e-9) && near(rings[1].km, 384400.0, 1e-9) &&
              near(rings[2].km, 5.0 * 384400.0, 1e-9) && near(rings[3].km, 0.05 * sim::kAU_km, 1e-6),
          "GEO, 1 LD, 5 LD and 0.05 AU have their real radii");
    check(neo::radialFromKm(rings[3].km, s) < s.pathHalfLength, "the outermost ring fits inside a path's length");
    std::printf("  ring radii (render units): GEO %.2f, 1 LD %.2f, 5 LD %.2f, 0.05 AU %.2f\n",
                neo::radialFromKm(rings[0].km, s), neo::radialFromKm(rings[1].km, s),
                neo::radialFromKm(rings[2].km, s), neo::radialFromKm(rings[3].km, s));
}

// --- directions -----------------------------------------------------------------------

void testDirections() {
    std::printf("[flyby] schematic directions come from the designation hash\n");
    sim::Vec3d p, t;
    neo::flybyDirections(neo::flybyHash("2020 AB", 0), p, t);
    check(near(sim::length(p), 1.0, 1e-12) && near(sim::length(t), 1.0, 1e-12), "both directions are unit vectors");
    check(near(sim::dot(p, t), 0.0, 1e-12), "the path direction is perpendicular to the closest point");

    sim::Vec3d p2, t2;
    neo::flybyDirections(neo::flybyHash("2020 AB", 0), p2, t2);
    check(p.x == p2.x && p.y == p2.y && p.z == p2.z && t.x == t2.x && t.y == t2.y && t.z == t2.z,
          "the same designation always gives the same picture");
    check(neo::flybyHash("2020 AB", 0) != neo::flybyHash("2020 AB", 1), "a second approach of the same object differs");
    check(neo::flybyHash("2020 AB", 0) != neo::flybyHash("2020 AC", 0), "different designations differ");

    // 1,000 designations: spread over the sphere, no two on top of each other.
    std::vector<sim::Vec3d> points;
    sim::Vec3d mean;
    int quartile[4] = {0, 0, 0, 0};
    bool allUnit = true;
    bool allPerpendicular = true;
    for (int i = 0; i < 1000; ++i) {
        sim::Vec3d a, b;
        neo::flybyDirections(neo::flybyHash("2024 XX" + std::to_string(i), 0), a, b);
        points.push_back(a);
        mean += a;
        ++quartile[std::min(3, static_cast<int>((a.y + 1.0) * 2.0))];
        allUnit = allUnit && near(sim::length(a), 1.0, 1e-9) && near(sim::length(b), 1.0, 1e-9);
        allPerpendicular = allPerpendicular && near(sim::dot(a, b), 0.0, 1e-9);
    }
    check(allUnit && allPerpendicular, "1,000 designations all give valid perpendicular unit pairs");
    check(sim::length(mean / 1000.0) < 0.1, "the directions are not biased to one side",
          num(sim::length(mean / 1000.0)));
    bool evenBands = true;
    for (const int q : quartile) {
        evenBands = evenBands && q > 200 && q < 300; // uniform on a sphere: each latitude quarter holds 25%
    }
    check(evenBands, "the latitude quarters each hold about a quarter", num(quartile[0]) + "/" + num(quartile[3]));
    double minAngle = 180.0;
    double nearestSum = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        double nearest = 180.0;
        for (std::size_t j = 0; j < points.size(); ++j) {
            if (i != j) {
                nearest = std::min(nearest, angleDeg(points[i], points[j]));
            }
        }
        nearestSum += nearest;
        minAngle = std::min(minAngle, nearest);
    }
    std::printf("  1,000 directions: mean nearest-neighbour angle %.2f deg, closest pair %.3f deg\n",
                nearestSum / 1000.0, minAngle);
    check(nearestSum / 1000.0 > 1.5, "typical neighbours are a couple of degrees apart");
    check(minAngle > 0.001, "no two flybys share a direction");
}

// --- the mapping ----------------------------------------------------------------------------

neo::Asteroid asteroid(const char* pdes, std::optional<double> d, std::optional<double> h, std::optional<bool> pha) {
    neo::Asteroid a;
    a.pdes = pdes;
    a.physical.diameterKm = d;
    a.physical.absoluteMagnitudeH = h;
    a.classification.isPHA = pha;
    return a;
}

void testFlybyMapping() {
    std::printf("[flyby] closest approach: right time, right distance, right speed\n");
    const neo::EarthViewScale s;
    const neo::Asteroid a = asteroid("99942", 0.34, 19.09, true);
    neo::CloseApproach c;
    c.jdTdb = 2462240.407091969; // the 2029 Apophis encounter
    c.distanceAU = 0.000254090910419299;
    c.relVelocityKms = 7.42253895678452;

    const neo::Flyby f = neo::makeFlyby(a, c, 0, s);
    check(f.tcaJd == c.jdTdb, "the flyby's closest-approach time IS the CAD time, unchanged");
    check(near(f.radius, neo::radialFromAu(c.distanceAU, s), 1e-12), "its closest distance is the log-scaled real distance");
    check(near(sim::length(f.closest), f.radius, 1e-9), "and the closest point sits exactly there");
    check(near(sim::length(f.direction), 1.0, 1e-12) && near(sim::dot(f.direction, f.closest), 0.0, 1e-9),
          "the path is perpendicular to the closest point");

    // Position at the CAD time is the closest point; no other time is nearer.
    const sim::Vec3d atTca = neo::flybyPosition(f, f.tcaJd);
    check(near(sim::length(atTca - f.closest), 0.0, 1e-12), "at the CAD time the object is at its closest point");
    check(neo::alongTrack(f, f.tcaJd) == 0.0, "the along-track distance is zero at the CAD time");
    check(neo::alongTrack(f, f.tcaJd - 1.0) < 0.0 && neo::alongTrack(f, f.tcaJd + 1.0) > 0.0,
          "negative before, positive after");

    double bestRadius = 1e300;
    double bestT = 0.0;
    for (int k = -4000; k <= 4000; ++k) {
        const double t = f.tcaJd + k * 0.0025; // 10 days either side, in 3.6 minute steps
        const double r = sim::length(neo::flybyPosition(f, t));
        if (r < bestRadius) {
            bestRadius = r;
            bestT = t;
        }
    }
    check(near(bestT, f.tcaJd, 0.0026), "the nearest point of the whole path is reached at the CAD time",
          num(bestT - f.tcaJd) + " d");
    check(near(bestRadius, f.radius, 1e-6), "and it is at the closest distance", num(bestRadius) + " vs " + num(f.radius));
    check(near(sim::length(neo::flybyPosition(f, f.tcaJd + 2.0)), sim::length(neo::flybyPosition(f, f.tcaJd - 2.0)), 1e-9),
          "the path is symmetric about closest approach");

    // Speed is proportional to the real relative velocity.
    neo::CloseApproach fast = c;
    fast.relVelocityKms = 2.0 * c.relVelocityKms;
    const neo::Flyby g = neo::makeFlyby(a, fast, 0, s);
    check(near(g.speed / f.speed, 2.0, 1e-12), "double the v_rel, double the along-track speed");
    check(near(f.speed, s.unitsPerDayPerKms * c.relVelocityKms, 1e-12), "speed = declared units-per-day-per-km/s x v_rel");
    check(near(neo::alongTrack(f, f.tcaJd + 1.0), f.speed, 1e-12), "one day along the path moves `speed` render units");

    // Visibility window, and its entry/exit times.
    const double entry = neo::flybyEntryJd(f, s);
    const double exitJd = neo::flybyExitJd(f, s);
    check(near(entry, f.tcaJd - s.pathHalfLength / f.speed, 1e-9) && near(exitJd, f.tcaJd + s.pathHalfLength / f.speed, 1e-9),
          "entry and exit are half a path length from closest approach");
    check(neo::flybyVisible(f, f.tcaJd, s) && neo::flybyVisible(f, entry + 1e-4, s) &&
              !neo::flybyVisible(f, entry - 1e-3, s) && !neo::flybyVisible(f, exitJd + 1e-3, s),
          "the object is drawn exactly while it is inside its path");
    check(near(std::fabs(neo::alongTrack(f, entry)), s.pathHalfLength, 1e-9), "the path ends are half-length out");

    // A pass inside the planet is still mapped (linear inside), and monotonic with the outside.
    neo::CloseApproach graze = c;
    graze.distanceAU = neo::kEarthRadiusAU * 0.5;
    // kEarthRadiusAU (4.2635e-5, rounded) and 6378.137 km differ in the 5th digit, hence 1e-4.
    check(near(neo::makeFlyby(a, graze, 0, s).radius, 0.5, 1e-4), "a pass at half an Earth radius maps to render radius 0.5");
    // Nearer real distance always means a smaller drawn radius.
    bool ordered = true;
    double previous = -1.0;
    for (double au = 1e-6; au < 0.3; au *= 1.15) {
        neo::CloseApproach x = c;
        x.distanceAU = au;
        const double r = neo::makeFlyby(a, x, 0, s).radius;
        ordered = ordered && r > previous;
        previous = r;
    }
    check(ordered, "flyby radius is strictly increasing in the real distance");
}

void testMarkers() {
    std::printf("[flyby] marker size and solid/hollow from the diameter\n");
    check(neo::markerPixelsFromDiameter(0.010) < neo::markerPixelsFromDiameter(0.140) &&
              neo::markerPixelsFromDiameter(0.140) < neo::markerPixelsFromDiameter(1.0) &&
              neo::markerPixelsFromDiameter(1.0) < neo::markerPixelsFromDiameter(10.0),
          "marker radius grows with diameter");
    bool monotone = true;
    float previous = 0.0f;
    for (double d = 0.001; d < 50.0; d *= 1.2) {
        const float px = neo::markerPixelsFromDiameter(d);
        monotone = monotone && px >= previous;
        previous = px;
    }
    check(monotone, "marker size never decreases as diameter grows");
    check(neo::markerPixelsFromDiameter(1e-9) >= 3.0f && neo::markerPixelsFromDiameter(1e9) <= 12.0f,
          "marker size is clamped to 3..12 px");
    check(neo::markerPixelsFromDiameter(0.0) == 3.5f && neo::markerPixelsFromDiameter(-1.0) == 3.5f,
          "an unknown diameter gets the small default");

    const neo::EarthViewScale s;
    neo::CloseApproach c;
    c.distanceAU = 0.01;
    c.relVelocityKms = 10.0;
    const neo::Flyby measured = neo::makeFlyby(asteroid("A", 0.5, 20.0, false), c, 0, s);
    const neo::Flyby estimated = neo::makeFlyby(asteroid("B", std::nullopt, 20.0, false), c, 0, s);
    const neo::Flyby unknown = neo::makeFlyby(asteroid("C", std::nullopt, std::nullopt, std::nullopt), c, 0, s);
    check(!measured.hollow, "a measured diameter is a solid marker");
    check(estimated.hollow, "a diameter estimated from H is hollow");
    check(unknown.hollow && unknown.markerPx == 3.5f, "no diameter at all is a small hollow marker");
    check(estimated.markerPx > 3.0f, "an estimate still sizes the marker");
    check(neo::makeFlyby(asteroid("D", 0.2, 21.0, true), c, 0, s).pha && !measured.pha && !unknown.pha,
          "PHA is carried through; an unknown flag is not PHA");
}

// --- the scene from a real query result -----------------------------------------------------

void testScene() {
    std::printf("[scene] built from a QueryResult: one flyby per matching approach\n");
    std::vector<Obj> objects;
    std::vector<App> approaches;
    for (int i = 0; i < 40; ++i) {
        Obj o;
        o.pdes = "2030 T" + std::to_string(i);
        o.diameter = i % 3 == 0 ? std::optional<double>(0.05 + 0.01 * i) : std::nullopt;
        o.h = 20.0 + 0.1 * i;
        o.pha = i % 4 == 0 ? std::optional<bool>(true) : std::optional<bool>(false);
        objects.push_back(o);
        const int n = 1 + i % 3; // one to three approaches each
        for (int k = 0; k < n; ++k) {
            approaches.push_back({static_cast<std::size_t>(i), jd("2030-01-01") + 30.0 * i + 400.0 * k,
                                  0.002 + 0.001 * i + 0.0005 * k, 5.0 + i * 0.5});
        }
    }
    const neo::Dataset ds = buildDataset(objects, approaches);
    neo::QueryEngine engine(ds);
    engine.build();

    neo::Query q;
    q.sortBy = neo::SortField::Distance;
    const neo::QueryResult r = engine.run(q);
    check(r.ok && r.rows.size() == 40, "the query returns every object");

    const neo::FlybyScene scene = neo::buildFlybyScene(ds, r);
    check(scene.flybys.size() == ds.approachCount() && scene.available == ds.approachCount(),
          "one flyby per matching approach", num(static_cast<double>(scene.flybys.size())));
    check(scene.truncated() == 0, "nothing was cut");

    bool consistent = true;
    bool sizesOk = true;
    for (const neo::Flyby& f : scene.flybys) {
        const neo::CloseApproach& a = ds.approaches()[f.approach];
        consistent = consistent && f.tcaJd == a.jdTdb && f.distanceAU == a.distanceAU && f.vRelKms == a.relVelocityKms &&
                     ds.approaches()[f.approach].objectIndex == f.object &&
                     near(f.radius, neo::radialFromAu(a.distanceAU, scene.scale), 1e-12) &&
                     near(sim::length(f.closest), f.radius, 1e-9) &&
                     near(f.speed, scene.scale.unitsPerDayPerKms * a.relVelocityKms, 1e-12);
        sizesOk = sizesOk && f.hollow == !ds.records()[f.object].object.physical.diameterKm.has_value() &&
                  f.pha == ds.records()[f.object].object.classification.isPHA.value_or(false);
    }
    check(consistent, "every flyby carries its approach's real time, distance and speed");
    check(sizesOk, "hollow = no measured diameter, PHA carried through");

    // Result order: best (nearest) first, so the smallest radius comes first.
    check(scene.flybys.front().distanceAU <= scene.flybys[scene.flybys.size() / 2].distanceAU,
          "flybys keep the result's best-first order");
    check(scene.firstJd <= scene.lastJd && scene.firstJd == jd("2030-01-01"), "the scene's time span is recorded");

    // The cap: extra approaches are counted, not silently lost.
    const neo::FlybyScene capped = neo::buildFlybyScene(ds, r, neo::EarthViewScale(), 25);
    check(capped.flybys.size() == 25 && capped.available == ds.approachCount() &&
              capped.truncated() == ds.approachCount() - 25,
          "a cap keeps the first N and reports how many were left out");

    // Only MATCHING approaches become flybys.
    neo::Query windowed;
    windowed.dateJd = neo::Range::between(jd("2030-01-01"), jd("2030-12-31"));
    windowed.sortBy = neo::SortField::Date;
    const neo::QueryResult wr = engine.run(windowed);
    const neo::FlybyScene ws = neo::buildFlybyScene(ds, wr);
    bool inside = !ws.flybys.empty();
    for (const neo::Flyby& f : ws.flybys) {
        inside = inside && f.tcaJd >= jd("2030-01-01") && f.tcaJd <= jd("2030-12-31") + 1.0;
    }
    check(inside && ws.flybys.size() == wr.totalApproaches, "a date window yields flybys only inside the window");

    // The same object's approaches get different directions but stable ones.
    const neo::FlybyScene again = neo::buildFlybyScene(ds, r);
    bool stable = again.flybys.size() == scene.flybys.size();
    for (std::size_t i = 0; stable && i < again.flybys.size(); ++i) {
        stable = again.flybys[i].closest.x == scene.flybys[i].closest.x && again.flybys[i].hash == scene.flybys[i].hash;
    }
    check(stable, "rebuilding the scene reproduces every direction exactly");

    // Next / previous approach in time.
    const double t0 = jd("2030-06-01");
    const int next = neo::nextFlyby(scene, t0);
    const int prev = neo::prevFlyby(scene, t0);
    check(next >= 0 && scene.flybys[static_cast<std::size_t>(next)].tcaJd > t0, "next is later than now");
    check(prev >= 0 && scene.flybys[static_cast<std::size_t>(prev)].tcaJd < t0, "prev is earlier than now");
    bool nextIsEarliest = true;
    bool prevIsLatest = true;
    for (const neo::Flyby& f : scene.flybys) {
        nextIsEarliest = nextIsEarliest && !(f.tcaJd > t0 && f.tcaJd < scene.flybys[static_cast<std::size_t>(next)].tcaJd);
        prevIsLatest = prevIsLatest && !(f.tcaJd < t0 && f.tcaJd > scene.flybys[static_cast<std::size_t>(prev)].tcaJd);
    }
    check(nextIsEarliest && prevIsLatest, "they are the nearest in time on each side");
    const double landed = scene.flybys[static_cast<std::size_t>(next)].tcaJd;
    check(neo::nextFlyby(scene, landed) != next, "standing exactly on an approach, next moves on to another");
    check(neo::prevFlyby(scene, scene.firstJd - 1.0) == -1 && neo::nextFlyby(scene, scene.lastJd + 1.0) == -1,
          "there is no previous before the first or next after the last");
    neo::FlybyScene empty;
    check(neo::nextFlyby(empty, 0.0) == -1 && neo::prevFlyby(empty, 0.0) == -1, "an empty scene has neither");
}

// --- the filter form ---------------------------------------------------------------------------

void testFilterState() {
    std::printf("[filter] the NEO FILTER form -> Query\n");
    neo::NeoFilterState f; // the shipped defaults
    neo::FilterParse p = neo::toQuery(f);
    check(p.errors.empty(), "the default form is valid", p.errors.empty() ? "" : p.errors[0]);
    check(p.query.distanceAU.hi && near(*p.query.distanceAU.hi, 10.0 * neo::kLunarDistanceAU, 1e-12),
          "10 LD becomes 10 x 0.002570 au");
    check(!p.query.dateJd.active() && !p.query.diameterKm.active() && !p.query.velocityKms.active(),
          "0 and empty mean no limit");
    check(p.query.pha == neo::TriState::Any && p.query.grazing == neo::TriState::Any, "flags default to any");
    check(p.query.sortBy == neo::SortField::Distance && p.query.topK == 50, "sorted by distance, top 50 by default");
    check(p.query.diameterMode == neo::DiameterMode::MeasuredOrEstimated, "default diameter mode");

    f.distanceUnit = neo::DistanceUnit::Au;
    f.maxDistance = 0.05f;
    p = neo::toQuery(f);
    check(p.query.distanceAU.hi && near(*p.query.distanceAU.hi, 0.05f, 1e-9), "an AU value is used as it is");
    check(neo::distanceToAu(1.0, neo::DistanceUnit::LunarDistance) == neo::kLunarDistanceAU &&
              neo::distanceToAu(1.0, neo::DistanceUnit::Au) == 1.0,
          "the two unit conversions");
    f.maxDistance = 0.0f;
    check(!neo::toQuery(f).query.distanceAU.active(), "a maximum distance of 0 removes the limit");

    // Switching the unit keeps the physical distance: 10 LD <-> 0.0257 AU, and back.
    neo::NeoFilterState u;
    u.maxDistance = 10.0f;
    neo::changeDistanceUnit(u, neo::DistanceUnit::Au);
    check(u.distanceUnit == neo::DistanceUnit::Au && near(u.maxDistance, 10.0 * neo::kLunarDistanceAU, 1e-6),
          "LD -> AU converts the value");
    neo::changeDistanceUnit(u, neo::DistanceUnit::Au);
    check(near(u.maxDistance, 10.0 * neo::kLunarDistanceAU, 1e-6), "choosing the unit already in use changes nothing");
    neo::changeDistanceUnit(u, neo::DistanceUnit::LunarDistance);
    check(u.distanceUnit == neo::DistanceUnit::LunarDistance && near(u.maxDistance, 10.0, 1e-5), "AU -> LD converts back");
    u.maxDistance = 0.0f;
    neo::changeDistanceUnit(u, neo::DistanceUnit::Au);
    check(u.maxDistance == 0.0f, "no limit stays no limit across a unit change");

    std::snprintf(f.dateFrom, sizeof f.dateFrom, "2030-01-01");
    std::snprintf(f.dateTo, sizeof f.dateTo, "2030-01-01");
    p = neo::toQuery(f);
    check(p.errors.empty() && p.query.dateJd.lo && p.query.dateJd.hi, "a one-day window is valid");
    check(near(*p.query.dateJd.lo, jd("2030-01-01"), 1e-12), "the window starts at 00:00 of the first day");
    check(*p.query.dateJd.hi > jd("2030-01-01") + 0.999 && *p.query.dateJd.hi < jd("2030-01-02"),
          "and 'to' includes the whole last day");

    std::snprintf(f.dateFrom, sizeof f.dateFrom, "next tuesday");
    p = neo::toQuery(f);
    check(p.errors.size() == 1 && p.errors[0].find("date from") != std::string::npos &&
              p.errors[0].find("YYYY-MM-DD") != std::string::npos,
          "text that is not a date is reported with the expected format", p.errors.empty() ? "" : p.errors[0]);
    std::snprintf(f.dateFrom, sizeof f.dateFrom, "2031-01-01");
    std::snprintf(f.dateTo, sizeof f.dateTo, "2030-01-01");
    p = neo::toQuery(f);
    check(!p.errors.empty() && p.errors[0].find("greater than") != std::string::npos,
          "a window that ends before it starts is reported", p.errors.empty() ? "" : p.errors[0]);
    std::snprintf(f.dateFrom, sizeof f.dateFrom, "%s", "  2030-06-01 ");
    std::snprintf(f.dateTo, sizeof f.dateTo, "%s", "");
    check(neo::toQuery(f).errors.empty(), "surrounding spaces are ignored and an empty end is open");

    f = neo::NeoFilterState();
    f.minDiameterM = 140.0f;
    f.maxDiameterM = 1000.0f;
    f.minVelocity = 5.0f;
    f.maxVelocity = 20.0f;
    f.phaOnly = true;
    f.grazingOnly = true;
    f.diameterMode = neo::DiameterMode::MeasuredOnly;
    f.sortBy = neo::SortField::Velocity;
    f.direction = neo::SortDirection::Descending;
    f.topK = 5000; // above the cap
    p = neo::toQuery(f);
    check(p.errors.empty(), "a full form is valid");
    check(near(*p.query.diameterKm.lo, 0.14, 1e-12) && near(*p.query.diameterKm.hi, 1.0, 1e-12), "diameters go in as metres, out as km");
    check(*p.query.velocityKms.lo == 5.0 && *p.query.velocityKms.hi == 20.0, "velocity range");
    check(p.query.pha == neo::TriState::Yes && p.query.grazing == neo::TriState::Yes, "PHA-only and grazing-only");
    check(p.query.diameterMode == neo::DiameterMode::MeasuredOnly, "diameter mode carried through");
    check(p.query.topK == static_cast<std::size_t>(neo::kMaxTopK), "top-K is capped at 1,000 (the frame budget)");
    f.topK = -3;
    check(neo::toQuery(f).query.topK == 1, "and never below 1");

    f.minDiameterM = 500.0f;
    f.maxDiameterM = 100.0f;
    p = neo::toQuery(f);
    check(!p.errors.empty() && p.errors[0].find("diameter") != std::string::npos, "min diameter above max is an error");
    f.minDiameterM = -1.0f;
    check(!neo::toQuery(f).errors.empty(), "a negative diameter is an error");
    f.minDiameterM = 0.0f;
    f.maxDiameterM = 0.0f;
    f.maxVelocity = std::nanf("");
    check(!neo::toQuery(f).errors.empty(), "a NaN velocity is an error");
}

// --- the service -------------------------------------------------------------------------------

template <class Pred>
bool waitFor(Pred done, int timeoutMs = 20000) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >
            timeoutMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void testService() {
    std::printf("[service] load and query on a worker thread\n");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "neo_earthview_tests";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "neo.db").string();

    std::vector<Obj> objects;
    std::vector<App> approaches;
    for (int i = 0; i < 300; ++i) {
        objects.push_back({"2031 S" + std::to_string(i), i % 2 ? std::optional<double>(0.1 + i * 0.001) : std::nullopt,
                           20.0 + 0.01 * i, i % 5 == 0 ? std::optional<bool>(true) : std::optional<bool>(false)});
        approaches.push_back({static_cast<std::size_t>(i), jd("2031-01-01") + 1.0 * i, 0.001 + 0.00015 * i, 6.0 + 0.05 * i});
    }
    const neo::Dataset source = buildDataset(objects, approaches);
    neo::DatabaseMeta meta;
    check(neo::saveDatabase(source, meta, path).ok, "write a test database");

    {
        neo::NeoService service;
        check(service.state() == neo::NeoService::State::Idle, "a new service is idle");
        check(service.dataset() == nullptr && service.engine() == nullptr, "and exposes no data yet");

        service.startLoad(path);
        check(waitFor([&] { return service.state() == neo::NeoService::State::Ready; }), "the database loads on the worker");
        check(service.objectCount() == 300 && service.approachCount() == 300, "with the right counts");
        check(service.message().find("300 objects") != std::string::npos && service.message().find("approaches") != std::string::npos,
              "and a status message for the strip", service.message());
        check(service.dataset() != nullptr && service.engine() != nullptr, "the data is exposed once ready");
        check(service.loadMs() >= 0.0 && service.indexMs() >= 0.0, "load and index times are recorded");

        // A query.
        neo::NeoOutcome out;
        neo::Query q;
        q.pha = neo::TriState::Yes;
        q.sortBy = neo::SortField::Distance;
        q.topK = 10;
        const std::uint64_t serial = service.submit(q, neo::EarthViewScale());
        check(waitFor([&] { return service.poll(out); }), "a query result arrives");
        check(out.serial == serial && out.ok && out.result.rows.size() == 10, "with the right serial and rows");
        check(out.scene.flybys.size() == 10, "and the flyby scene is built on the worker too");
        check(!out.summary.empty() && out.summary.find("driver") != std::string::npos, "with a one-line EXPLAIN", out.summary);
        check(out.explain.find("EXPLAIN") == 0, "and the full one");
        check(!service.poll(out), "a result is delivered once");

        // The result agrees with running the engine directly.
        neo::QueryEngine direct(*service.dataset());
        direct.build();
        const neo::QueryResult expected = direct.run(q);
        bool same = expected.rows.size() == out.result.rows.size();
        for (std::size_t i = 0; same && i < expected.rows.size(); ++i) {
            same = expected.rows[i].object == out.result.rows[i].object;
        }
        check(same, "the service returns exactly what QueryEngine::run returns");

        // Latest wins: a burst of queries never leaves a stale answer as the last one.
        std::uint64_t last = 0;
        for (int i = 1; i <= 60; ++i) {
            neo::Query b;
            b.topK = static_cast<std::size_t>(i);
            last = service.submit(b, neo::EarthViewScale());
        }
        neo::NeoOutcome final;
        check(waitFor([&] {
                  neo::NeoOutcome o;
                  while (service.poll(o)) {
                      final = std::move(o);
                  }
                  return final.serial == last;
              }),
              "after a burst of 60 queries the newest one is what arrives");
        check(final.result.rows.size() == 60, "and it is the newest query's answer", num(static_cast<double>(final.result.rows.size())));

        // A validation failure comes back as an outcome, not a crash.
        neo::Query bad;
        bad.diameterKm = neo::Range::between(5.0, 1.0);
        service.submit(bad, neo::EarthViewScale());
        neo::NeoOutcome badOut;
        check(waitFor([&] { return service.poll(badOut); }), "an invalid query still gets a reply");
        check(!badOut.ok && !badOut.errors.empty() && badOut.scene.flybys.empty(), "which says why and carries no flybys");
    } // the destructor must join the worker without hanging

    // A missing database: Failed, with the reason, and queries get a clear error.
    {
        neo::NeoService service;
        service.startLoad((dir / "does_not_exist.db").string());
        check(waitFor([&] { return service.state() == neo::NeoService::State::Failed; }), "a missing database fails");
        check(service.message().find("neo_ingest") != std::string::npos, "and says how to fix it", service.message());
        service.submit(neo::Query(), neo::EarthViewScale());
        neo::NeoOutcome out;
        check(waitFor([&] { return service.poll(out); }) && !out.ok && !out.errors.empty(),
              "a query against it returns an error instead of waiting forever");
    }
    std::filesystem::remove_all(dir, ec);
}

// --- NEO RESULTS checkboxes: what is drawn ------------------------------------------------------

std::string sceneFingerprint(const neo::FlybyScene& scene) {
    std::string f = std::to_string(scene.flybys.size()) + "/" + std::to_string(scene.available);
    for (const neo::Flyby& fl : scene.flybys) {
        f += "|" + std::to_string(fl.object) + ":" + std::to_string(fl.approach) + ":" + num(fl.tcaJd);
    }
    return f;
}

void testChecks() {
    std::printf("[checks] the results checkboxes only change the drawn subset\n");

    // The model on its own.
    neo::FlybyChecks c;
    check(c.size() == 0 && c.drawnList(-1).empty() && c.allChecked(), "no result: nothing to draw");
    c.reset(10);
    check(c.size() == 10 && c.checkedCount() == 10 && c.allChecked(), "a new result starts with every flyby checked");
    std::vector<int> everything;
    for (int i = 0; i < 10; ++i) everything.push_back(i);
    check(c.drawnList(-1) == everything && c.drawnCount(-1) == 10, "so everything is drawn (the previous behaviour)");

    c.toggle(3);
    check(!c.checked(3) && c.checkedCount() == 9 && !c.allChecked(), "unchecking one row removes exactly that one");
    std::vector<int> without3 = everything;
    without3.erase(without3.begin() + 3);
    check(c.drawnList(-1) == without3, "and only it leaves the drawn subset");
    c.toggle(3);
    check(c.drawnList(-1) == everything && c.checkedCount() == 10, "checking it again brings it back");
    c.set(99, false);
    c.set(3, true); // already checked: no change
    check(c.checkedCount() == 10 && !c.checked(99), "out-of-range and no-op changes do nothing");

    // SELECT NONE / SELECT ALL touch every row.
    c.setAll(false);
    bool none = c.checkedCount() == 0 && c.drawnList(-1).empty();
    for (std::size_t i = 0; i < c.size(); ++i) none = none && !c.checked(i);
    check(none, "SELECT NONE unchecks every row and empties the drawn subset");
    c.setAll(true);
    bool all = c.checkedCount() == 10 && c.drawnList(-1) == everything && c.allChecked();
    for (std::size_t i = 0; i < c.size(); ++i) all = all && c.checked(i);
    check(all, "SELECT ALL checks every row and restores the whole drawn subset");

    // Shift-click ranges, either direction, clamped.
    c.setRange(2, 5, false);
    check(c.checkedCount() == 6 && !c.checked(2) && !c.checked(5) && c.checked(1) && c.checked(6), "a range unchecks rows 2..5 inclusive");
    c.setRange(5, 2, true);
    check(c.allChecked(), "a range given backwards works the same way");
    c.setRange(8, 500, false);
    check(c.checkedCount() == 8 && !c.checked(9) && c.checked(7), "a range past the end is clamped");

    // The row-selected flyby is always drawn, whatever its checkbox says.
    c.setAll(false);
    check(c.drawnList(4) == std::vector<int>{4} && c.drawnCount(4) == 1, "a selected but unchecked flyby is still drawn");
    c.set(4, true);
    check(c.drawnList(4) == std::vector<int>{4} && c.drawnCount(4) == 1, "and is not counted twice when it is also checked");
    c.set(7, true);
    check(c.drawnList(4) == (std::vector<int>{4, 7}) && c.drawnCount(4) == 2, "it is drawn together with the checked ones");
    check(c.drawnList(-1) == (std::vector<int>{4, 7}), "with no selection only the checked ones are");

    // Unchecking the selected flyby's own box clears the selection; other changes do not.
    neo::FlybyChecks r;
    r.reset(6);
    r.toggle(4);
    check(neo::selectionAfterCheckChange(4, true, r) == -1, "unchecking the selected row clears the selection");
    r.reset(6);
    r.toggle(1);
    check(neo::selectionAfterCheckChange(4, true, r) == 4, "unchecking a different row keeps it");
    r.setAll(false);
    check(neo::selectionAfterCheckChange(4, true, r) == -1, "SELECT NONE clears it too (its box was checked)");
    check(neo::selectionAfterCheckChange(4, false, r) == 4, "a row selected while unchecked stays selected through later changes");
    check(neo::selectionAfterCheckChange(-1, true, r) == -1, "no selection stays none");

    // Against a real result from the service: none of this may touch the result or the service.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "neo_earthview_checks_tests";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "neo.db").string();
    std::vector<Obj> objects;
    std::vector<App> approaches;
    for (int i = 0; i < 40; ++i) {
        objects.push_back({"2032 H" + std::to_string(i), std::nullopt, 21.0 + 0.05 * i, std::optional<bool>(i % 4 == 0)});
        approaches.push_back({static_cast<std::size_t>(i), jd("2032-03-01") + 2.0 * i, 0.002 + 0.0002 * i, 7.0 + 0.1 * i});
    }
    const neo::Dataset source = buildDataset(objects, approaches);
    neo::DatabaseMeta meta;
    check(neo::saveDatabase(source, meta, path).ok, "write a test database");

    neo::NeoService service;
    service.startLoad(path);
    check(waitFor([&] { return service.state() == neo::NeoService::State::Ready; }), "the database loads");
    neo::Query q;
    q.sortBy = neo::SortField::Distance;
    q.topK = 12;
    service.submit(q, neo::EarthViewScale());
    neo::NeoOutcome out;
    check(waitFor([&] { return service.poll(out); }) && out.ok && out.scene.flybys.size() == 12, "a 12-flyby result arrives");

    neo::FlybyChecks checks;
    checks.reset(out.scene.flybys.size()); // what the application does when a result is adopted
    const std::uint64_t serial = service.latestSerial();
    const std::string before = sceneFingerprint(out.scene);
    const std::size_t rowsBefore = out.result.rows.size();
    check(checks.drawnList(0).size() == 12, "a new result draws all 12 flybys");

    checks.toggle(2);
    checks.toggle(9);
    check(checks.drawnList(0).size() == 10 && !checks.checked(2) && !checks.checked(9), "unchecking two rows draws the other ten");
    checks.setRange(4, 7, false);
    check(checks.drawnList(0) == (std::vector<int>{0, 1, 3, 8, 10, 11}), "a range removes those rows too");
    checks.setAll(false);
    check(checks.drawnList(-1).empty() && checks.drawnList(6) == std::vector<int>{6}, "SELECT NONE draws nothing but the selected row");
    checks.setAll(true);
    check(checks.drawnList(-1).size() == 12, "SELECT ALL draws everything again");

    check(sceneFingerprint(out.scene) == before && out.result.rows.size() == rowsBefore, "the result set and its flybys were never touched");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    neo::NeoOutcome extra;
    check(service.latestSerial() == serial && !service.busy() && !service.poll(extra),
          "no NeoService query was submitted, run or delivered", "serial " + std::to_string(service.latestSerial()) + " vs " + std::to_string(serial));

    // A second query is a new result: the application resets the checkboxes for it.
    service.submit(q, neo::EarthViewScale());
    check(waitFor([&] { return service.poll(extra); }) && service.latestSerial() == serial + 1, "RUN is the only thing that queries");
    checks.setAll(false);
    checks.reset(extra.scene.flybys.size());
    check(checks.allChecked() && checks.size() == extra.scene.flybys.size(), "and a new result comes back fully checked");
    std::filesystem::remove_all(dir, ec);
}

// --- the DEFAULT (startup) result: the same checkbox state as a RUN result -----------------------

// What the application does with a finished query, minus the GL and the log: the state that
// must be fresh for every new result.
struct AdoptedResult {
    neo::FlybyChecks checks;
    int selected = 7; // deliberately stale, as if left over from an earlier result
    int hovered = 3;
    std::size_t flybys = 0;
};

AdoptedResult adopt(const neo::NeoOutcome& out, AdoptedResult previous) {
    // Dirty state from "the previous result": some boxes unchecked, a selection, a hover.
    previous.checks.reset(12);
    previous.checks.setAll(false); // the earlier result's boxes were all unchecked
    neo::resetForNewResult(previous.checks, previous.selected, previous.hovered, out.ok ? out.scene.flybys.size() : 0);
    previous.flybys = out.scene.flybys.size();
    return previous;
}

// The Earth view runs its default query by itself on the first visit (no RUN pressed): the
// NEO FILTER form as it is on startup, through the same toQuery / NeoService::submit / poll
// the RUN button uses. Its result must come with the same state as any other: everything
// checked, "N of M shown" = M of M, and SELECT NONE / SELECT ALL working on it.
void checkDefaultResult(neo::NeoService& service, const char* what) {
    const neo::NeoFilterState startupForm; // untouched: what the panel holds before anyone edits it
    const neo::FilterParse startup = neo::toQuery(startupForm);
    check(startup.errors.empty(), std::string(what) + ": the untouched form is a valid query");

    service.submit(startup.query, neo::EarthViewScale()); // the automatic first query
    neo::NeoOutcome first;
    check(waitFor([&] { return service.poll(first); }) && first.ok && !first.scene.flybys.empty(), std::string(what) + ": the default query returns flybys");
    const std::size_t m = first.scene.flybys.size();

    const AdoptedResult a = adopt(first, AdoptedResult());
    check(a.checks.size() == m && a.checks.checkedCount() == m && a.checks.allChecked(), std::string(what) + ": every row of the default result is checked");
    check(a.selected == -1 && a.hovered == -1, std::string(what) + ": with no stale selection or hover");
    check(a.checks.drawnCount(a.selected) == m && a.checks.drawnList(a.selected).size() == m,
          std::string(what) + ": 'N of M shown' is M of M", std::to_string(a.checks.drawnCount(a.selected)) + " of " + std::to_string(m));

    // SELECT NONE, then SELECT ALL, on the default result.
    neo::FlybyChecks c = a.checks;
    c.setAll(false);
    bool none = c.drawnCount(-1) == 0;
    for (std::size_t i = 0; i < m; ++i) none = none && !c.checked(i);
    check(none, std::string(what) + ": SELECT NONE unchecks every row of the default result");
    c.setAll(true);
    bool all = c.drawnCount(-1) == m;
    for (std::size_t i = 0; i < m; ++i) all = all && c.checked(i);
    check(all, std::string(what) + ": SELECT ALL checks them all again");
    c.toggle(m > 1 ? 1 : 0);
    check(c.drawnCount(-1) == m - 1, std::string(what) + ": one checkbox changes the count by exactly one");

    // A RUN of the same form is the same query: the same result, and the same fresh state.
    service.submit(startup.query, neo::EarthViewScale());
    neo::NeoOutcome rerun;
    check(waitFor([&] { return service.poll(rerun); }) && rerun.ok, std::string(what) + ": RUN returns a result");
    check(sceneFingerprint(rerun.scene) == sceneFingerprint(first.scene), std::string(what) + ": identical to the default result (same query path)");
    const AdoptedResult b = adopt(rerun, AdoptedResult());
    check(b.checks.drawnList(b.selected) == a.checks.drawnList(a.selected) && b.selected == a.selected,
          std::string(what) + ": and it starts in the same checkbox state as the default one");
}

void testDefaultResult() {
    std::printf("[default] the automatic first result has the same checkboxes as a RUN result\n");

    // A synthetic database (always available) with results well inside the default form's 10 LD limit.
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "neo_earthview_default_tests";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::string path = (dir / "neo.db").string();
        std::vector<Obj> objects;
        std::vector<App> approaches;
        for (int i = 0; i < 60; ++i) {
            objects.push_back({"2033 D" + std::to_string(i), std::nullopt, 22.0 + 0.04 * i, std::optional<bool>(i % 3 == 0)});
            approaches.push_back({static_cast<std::size_t>(i), jd("2033-05-01") + 3.0 * i, 0.001 + 0.00012 * i, 6.0 + 0.1 * i});
            if (i % 5 == 0) { // a second approach for some objects: more rows than objects, like the real result
                approaches.push_back({static_cast<std::size_t>(i), jd("2035-05-01") + 3.0 * i, 0.002 + 0.00012 * i, 8.0});
            }
        }
        const neo::Dataset source = buildDataset(objects, approaches);
        neo::DatabaseMeta meta;
        check(neo::saveDatabase(source, meta, path).ok, "write a test database");
        neo::NeoService service;
        service.startLoad(path);
        check(waitFor([&] { return service.state() == neo::NeoService::State::Ready; }), "the database loads");
        checkDefaultResult(service, "synthetic");
        std::filesystem::remove_all(dir, ec);
    }

    // And the real one, where the default result is the ~116 flybys the Earth view opens with.
    const char* candidates[] = {"data/neo.db", "../data/neo.db", "../../data/neo.db"};
    std::string path;
    for (const char* cand : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(cand, ec)) {
            path = cand;
            break;
        }
    }
    if (path.empty()) {
        std::printf("  real dataset skipped: no neo.db\n");
        return;
    }
    neo::NeoService real;
    real.startLoad(path);
    if (!waitFor([&] { return real.state() != neo::NeoService::State::Loading; }, 60000) || real.state() != neo::NeoService::State::Ready) {
        std::printf("  real dataset skipped: %s\n", real.message().c_str());
        return;
    }
    checkDefaultResult(real, "real");
}

// --- the real dataset -----------------------------------------------------------------------------

void testReal() {
    std::printf("[real] the Earth-view pipeline over data/neo.db, if it exists\n");
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
    std::printf("  %s (load %.0f ms, indexes %.0f ms)\n", service.message().c_str(), service.loadMs(), service.indexMs());

    neo::NeoFilterState f;
    std::snprintf(f.dateFrom, sizeof f.dateFrom, "2030-01-01");
    std::snprintf(f.dateTo, sizeof f.dateTo, "2040-12-31");
    f.maxDistance = 5.0f;
    f.topK = 1000;
    const neo::FilterParse p = neo::toQuery(f);
    check(p.errors.empty(), "the filter form for the demo window is valid");
    service.submit(p.query, neo::EarthViewScale());
    neo::NeoOutcome out;
    check(waitFor([&] { return service.poll(out); }), "a real query completes");
    check(out.ok && !out.scene.flybys.empty(), "and returns flybys");
    std::printf("  2030-2040, <= 5 LD, top 1000: %zu flybys (%zu available), %s, %.2f ms incl. scene\n", out.scene.flybys.size(),
                out.scene.available, out.summary.c_str(), out.totalMs);
    check(out.scene.flybys.size() <= neo::kMaxFlybys, "never more than the frame-budget cap");

    const neo::Dataset& ds = *service.dataset();
    bool valid = true;
    bool inWindow = true;
    for (const neo::Flyby& fl : out.scene.flybys) {
        const neo::CloseApproach& a = ds.approaches()[fl.approach];
        valid = valid && fl.tcaJd == a.jdTdb && near(sim::length(fl.closest), neo::radialFromAu(a.distanceAU), 1e-9) &&
                near(sim::length(neo::flybyPosition(fl, fl.tcaJd) - fl.closest), 0.0, 1e-12) &&
                fl.distanceAU <= 5.0 * neo::kLunarDistanceAU + 1e-12;
        inWindow = inWindow && fl.tcaJd >= jd("2030-01-01") && fl.tcaJd <= jd("2040-12-31") + 1.0;
    }
    check(valid, "every real flyby reaches its real distance at its real CAD time");
    check(inWindow, "and lies inside the requested window");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the progress lines
    testRadialScale();
    testDirections();
    testFlybyMapping();
    testMarkers();
    testScene();
    testFilterState();
    testService();
    testChecks();
    testDefaultResult();
    testReal();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
