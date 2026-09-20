#pragma once

#include "neo/model/Dataset.h"
#include "neo/query/QueryEngine.h"
#include "sim/Vec3.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neo {

// Earth view: turning query results into a schematic flyby scene.
//
// This is section 9 of docs/NEO_DSA_Context.md made concrete. It is pure C++ (no
// OpenGL, no ImGui) so the mapping can be tested to the last digit; the renderer
// and the HUD only read what is built here.
//
// WHAT IS REAL AND WHAT IS SCHEMATIC
//   REAL, taken straight from the data
//     * the time of closest approach: the flyby reaches its closest point at the
//       approach's Julian Date (TDB), exactly, on the simulation clock
//     * the closest distance, as a monotonic function of the nominal distance
//     * how fast it moves: along-track speed is proportional to the real v_rel
//     * the marker size, from the diameter (hollow when it is only estimated)
//   SCHEMATIC, chosen for legibility and stated as such on screen
//     * the DIRECTION: CAD does not give a flyby's geometry in a useful form here,
//       so the approach direction and the path's orientation come from a hash of
//       the designation. Same asteroid, same picture; but it is not where the
//       object really passed.
//     * the RADIAL SCALE is logarithmic: 0.05 au and 26,000 km cannot share a
//       linear screen, so distance is compressed (below).
//     * the path is a straight line, not the true hyperbola.
//     * the TIME SCALE: along-track motion is kUnitsPerDayPerKms render units
//       per day per km/s, one declared constant for every object.

struct EarthViewScale {
    double earthRadiusKm = 6378.137;    // equatorial radius; drawn as 1.0 render unit
    double logSlope = 3.2;              // render units per decade of distance outside the surface
    double pathHalfLength = 14.0;       // along-track extent of a path, either side of closest approach
    double unitsPerDayPerKms = 0.20;    // along-track speed = this * v_rel   (render units per day)
};

// Radial position of a distance from the Earth's CENTRE, in render units.
//
//   d <= R :  r = d / R                      (inside the planet: linear, continuous at the surface)
//   d >  R :  r = 1 + logSlope * log10(d / R)
//
// Strictly increasing and continuous, so ordering by distance is ordering by
// radius, which is what the tests pin down.
double radialFromKm(double km, const EarthViewScale& scale = EarthViewScale());
double radialFromAu(double au, const EarthViewScale& scale = EarthViewScale());
double kmFromRadial(double radius, const EarthViewScale& scale = EarthViewScale()); // inverse

// A reference ring on the log scale.
struct ReferenceRing {
    const char* label;   // "GEO", "1 LD", "5 LD", "0.05 AU"
    double      km;
    bool        equatorial; // GEO lies in the Earth's equatorial plane; the rest are drawn in the ecliptic
};
const std::vector<ReferenceRing>& referenceRings();

struct Flyby {
    std::uint32_t object = 0;    // Dataset::records() index
    std::uint32_t approach = 0;  // Dataset::approaches() index
    std::uint32_t ordinal = 0;   // which of the object's approaches this is (0-based, by date)
    std::uint32_t hash = 0;      // designation hash that fixed the direction

    // Real values from the approach row.
    double tcaJd = 0.0;          // time of closest approach, JD (TDB)
    double distanceAU = 0.0;     // nominal geocentric distance
    double vRelKms = 0.0;

    // Derived, in render units (Earth radius = 1).
    double radius = 0.0;         // |closest|, radialFromAu(distanceAU)
    sim::Vec3d closest;          // position at closest approach
    sim::Vec3d direction;        // unit vector of motion, perpendicular to `closest`
    double speed = 0.0;          // render units per day (proportional to v_rel)

    float markerPx = 4.0f;       // marker radius from the diameter
    bool  hollow = false;        // diameter estimated from H, or unknown
    bool  pha = false;
};

struct FlybyScene {
    EarthViewScale scale;
    std::vector<Flyby> flybys;   // result order: best first, each object's approaches by date
    std::size_t available = 0;   // matching approaches in the result, before the cap
    double firstJd = 0.0;
    double lastJd = 0.0;

    std::size_t truncated() const { return available > flybys.size() ? available - flybys.size() : 0; }
};

// The most flybys drawn at once (the frame-rate budget).
constexpr std::size_t kMaxFlybys = 1000;

// Direction hash of a designation and approach ordinal: FNV-1a of the designation
// mixed with the ordinal, so an object's successive approaches come from
// different directions but every run gives the same ones.
std::uint32_t flybyHash(const std::string& designation, std::uint32_t ordinal);

// Unit closest-approach direction and unit travel direction for a hash. The
// travel direction is perpendicular to the first, so the path passes the Earth at
// exactly `radius` and never nearer.
void flybyDirections(std::uint32_t hash, sim::Vec3d& towardsClosest, sim::Vec3d& travel);

// Marker radius in pixels from a diameter in km: logarithmic, monotonic, clamped.
float markerPixelsFromDiameter(double diameterKm);

// One Flyby from one approach row (the object and approach indices are set by the caller).
Flyby makeFlyby(const Asteroid& object, const CloseApproach& approach, std::uint32_t ordinal,
                const EarthViewScale& scale);

// Builds the scene from a query result: one flyby per MATCHING approach, capped at
// `maxFlybys`. Read-only on the dataset.
FlybyScene buildFlybyScene(const Dataset& dataset, const QueryResult& result,
                           const EarthViewScale& scale = EarthViewScale(), std::size_t maxFlybys = kMaxFlybys);

// Signed distance travelled along the path at time `jdNow`, in render units:
// negative before closest approach, zero at it, positive after.
double alongTrack(const Flyby& flyby, double jdNow);

// Position in render units at `jdNow`: closest + direction * alongTrack.
sim::Vec3d flybyPosition(const Flyby& flyby, double jdNow);

// True while the object is inside its drawn path (|alongTrack| <= half length).
bool flybyVisible(const Flyby& flyby, double jdNow, const EarthViewScale& scale);

// Simulation time (JD TDB) at which the object enters / leaves its path.
double flybyEntryJd(const Flyby& flyby, const EarthViewScale& scale);
double flybyExitJd(const Flyby& flyby, const EarthViewScale& scale);

// Index of the earliest flyby whose closest approach is after / before `jdNow`
// (by more than a second, so a jump lands ON an approach and the next press moves
// on), or -1.
int nextFlyby(const FlybyScene& scene, double jdNow);
int prevFlyby(const FlybyScene& scene, double jdNow);

} // namespace neo
