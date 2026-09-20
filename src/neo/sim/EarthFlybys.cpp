#include "neo/sim/EarthFlybys.h"

#include "neo/model/Hash.h"
#include "sim/Constants.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace neo {

namespace {

constexpr double kSecondJd = 1.0 / 86400.0;

// splitmix64: a small, well-mixed generator, seeded from the designation hash.
class Mixer {
public:
    explicit Mixer(std::uint32_t seed)
        : state_(static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull) {}
    double unit() {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    std::uint64_t state_;
};

sim::Vec3d normalised(const sim::Vec3d& v) {
    const double l = sim::length(v);
    return l > 0.0 ? v / l : sim::Vec3d(1.0, 0.0, 0.0);
}

} // namespace

// --- radial scale -------------------------------------------------------------

double radialFromKm(double km, const EarthViewScale& s) {
    if (!(km > 0.0)) {
        return 0.0;
    }
    if (km <= s.earthRadiusKm) {
        return km / s.earthRadiusKm;
    }
    return 1.0 + s.logSlope * std::log10(km / s.earthRadiusKm);
}

double radialFromAu(double au, const EarthViewScale& s) { return radialFromKm(au * sim::kAU_km, s); }

double kmFromRadial(double radius, const EarthViewScale& s) {
    if (radius <= 0.0) {
        return 0.0;
    }
    if (radius <= 1.0) {
        return radius * s.earthRadiusKm;
    }
    return s.earthRadiusKm * std::pow(10.0, (radius - 1.0) / s.logSlope);
}

const std::vector<ReferenceRing>& referenceRings() {
    static const std::vector<ReferenceRing> rings = {
        {"GEO", 42164.0, true},
        {"1 LD", 384400.0, false},
        {"5 LD", 5.0 * 384400.0, false},
        {"0.05 AU", 0.05 * sim::kAU_km, false},
    };
    return rings;
}

// --- directions ----------------------------------------------------------------

std::uint32_t flybyHash(const std::string& designation, std::uint32_t ordinal) {
    std::uint64_t h = fnv1a64(designation);
    h ^= (static_cast<std::uint64_t>(ordinal) + 1u) * 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<std::uint32_t>(h >> 32);
}

void flybyDirections(std::uint32_t hash, sim::Vec3d& towardsClosest, sim::Vec3d& travel) {
    Mixer mix(hash);
    const double u1 = mix.unit();
    const double u2 = mix.unit();
    const double u3 = mix.unit();

    // A uniform point on the unit sphere (z uniform in [-1, 1], longitude uniform).
    const double z = 2.0 * u1 - 1.0;
    const double phi = sim::kTwoPi * u2;
    const double s = std::sqrt(std::max(0.0, 1.0 - z * z));
    const sim::Vec3d p(s * std::cos(phi), z, s * std::sin(phi));

    // An orthonormal basis of the plane perpendicular to p, then a uniform angle in it.
    const sim::Vec3d ref = std::fabs(p.y) > 0.9 ? sim::Vec3d(1.0, 0.0, 0.0) : sim::Vec3d(0.0, 1.0, 0.0);
    const sim::Vec3d e1 = normalised(sim::cross(p, ref));
    const sim::Vec3d e2 = sim::cross(p, e1);
    const double psi = sim::kTwoPi * u3;

    towardsClosest = p;
    travel = e1 * std::cos(psi) + e2 * std::sin(psi);
}

float markerPixelsFromDiameter(double diameterKm) {
    if (!(diameterKm > 0.0)) {
        return 3.5f;
    }
    // 10 m -> 4 px, 140 m -> 6.5 px, 1 km -> 8.4 px, 10 km -> 10.6 px.
    const double px = 4.0 + 2.2 * std::log10(diameterKm / 0.01);
    return static_cast<float>(std::clamp(px, 3.0, 12.0));
}

// --- flybys ------------------------------------------------------------------------

Flyby makeFlyby(const Asteroid& object, const CloseApproach& approach, std::uint32_t ordinal,
                const EarthViewScale& scale) {
    Flyby f;
    f.ordinal = ordinal;
    f.hash = flybyHash(object.pdes, ordinal);
    f.tcaJd = approach.jdTdb;
    f.distanceAU = approach.distanceAU;
    f.vRelKms = approach.relVelocityKms;

    f.radius = radialFromAu(approach.distanceAU, scale);
    sim::Vec3d toClosest;
    flybyDirections(f.hash, toClosest, f.direction);
    f.closest = toClosest * f.radius;
    f.speed = scale.unitsPerDayPerKms * approach.relVelocityKms;

    // Size: the measured diameter when there is one; otherwise the estimate from H
    // drawn hollow; otherwise a small hollow marker. Estimated is never solid.
    if (object.physical.diameterKm) {
        f.markerPx = markerPixelsFromDiameter(*object.physical.diameterKm);
        f.hollow = false;
    } else if (const std::optional<double> estimate = object.physical.estimatedDiameterKm()) {
        f.markerPx = markerPixelsFromDiameter(*estimate);
        f.hollow = true;
    } else {
        f.markerPx = markerPixelsFromDiameter(0.0);
        f.hollow = true;
    }
    f.pha = object.classification.isPHA.value_or(false);
    return f;
}

FlybyScene buildFlybyScene(const Dataset& dataset, const QueryResult& result, const EarthViewScale& scale,
                           std::size_t maxFlybys) {
    FlybyScene scene;
    scene.scale = scale;
    scene.flybys.reserve(std::min<std::size_t>(maxFlybys, result.approachPool.size()));
    for (const ResultRow& row : result.rows) {
        const AsteroidRecord& record = dataset.records()[row.object];
        for (std::uint32_t k = 0; k < row.approachCount; ++k) {
            const std::uint32_t ai = result.approachPool[row.approachBegin + k];
            ++scene.available;
            if (scene.flybys.size() >= maxFlybys) {
                continue; // still counted, so the HUD can say how many were left out
            }
            Flyby f = makeFlyby(record.object, dataset.approaches()[ai], ai - record.firstApproach, scale);
            f.object = row.object;
            f.approach = ai;
            scene.flybys.push_back(f);
        }
    }
    if (!scene.flybys.empty()) {
        scene.firstJd = scene.lastJd = scene.flybys.front().tcaJd;
        for (const Flyby& f : scene.flybys) {
            scene.firstJd = std::min(scene.firstJd, f.tcaJd);
            scene.lastJd = std::max(scene.lastJd, f.tcaJd);
        }
    }
    return scene;
}

// --- motion --------------------------------------------------------------------------

double alongTrack(const Flyby& flyby, double jdNow) { return flyby.speed * (jdNow - flyby.tcaJd); }

sim::Vec3d flybyPosition(const Flyby& flyby, double jdNow) {
    return flyby.closest + flyby.direction * alongTrack(flyby, jdNow);
}

bool flybyVisible(const Flyby& flyby, double jdNow, const EarthViewScale& scale) {
    return std::fabs(alongTrack(flyby, jdNow)) <= scale.pathHalfLength;
}

double flybyEntryJd(const Flyby& flyby, const EarthViewScale& scale) {
    if (!(flyby.speed > 0.0)) {
        return -std::numeric_limits<double>::infinity();
    }
    return flyby.tcaJd - scale.pathHalfLength / flyby.speed;
}

double flybyExitJd(const Flyby& flyby, const EarthViewScale& scale) {
    if (!(flyby.speed > 0.0)) {
        return std::numeric_limits<double>::infinity();
    }
    return flyby.tcaJd + scale.pathHalfLength / flyby.speed;
}

int nextFlyby(const FlybyScene& scene, double jdNow) {
    int best = -1;
    for (std::size_t i = 0; i < scene.flybys.size(); ++i) {
        const double t = scene.flybys[i].tcaJd;
        if (t > jdNow + kSecondJd && (best < 0 || t < scene.flybys[static_cast<std::size_t>(best)].tcaJd)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

int prevFlyby(const FlybyScene& scene, double jdNow) {
    int best = -1;
    for (std::size_t i = 0; i < scene.flybys.size(); ++i) {
        const double t = scene.flybys[i].tcaJd;
        if (t < jdNow - kSecondJd && (best < 0 || t > scene.flybys[static_cast<std::size_t>(best)].tcaJd)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

void FlybyChecks::reset(std::size_t n) {
    on_.assign(n, 1);
    count_ = n;
}

void FlybyChecks::set(std::size_t i, bool on) {
    if (i >= on_.size() || (on_[i] != 0) == on) {
        return;
    }
    on_[i] = on ? 1 : 0;
    if (on) {
        ++count_;
    } else {
        --count_;
    }
}

void FlybyChecks::setAll(bool on) {
    std::fill(on_.begin(), on_.end(), static_cast<std::uint8_t>(on ? 1 : 0));
    count_ = on ? on_.size() : 0;
}

void FlybyChecks::setRange(std::size_t a, std::size_t b, bool on) {
    if (a > b) {
        std::swap(a, b);
    }
    for (std::size_t i = a; i <= b && i < on_.size(); ++i) {
        set(i, on);
    }
}

std::size_t FlybyChecks::drawnCount(int selected) const {
    const bool extra = selected >= 0 && static_cast<std::size_t>(selected) < on_.size() && !checked(static_cast<std::size_t>(selected));
    return count_ + (extra ? 1 : 0);
}

std::vector<int> FlybyChecks::drawnList(int selected) const {
    std::vector<int> out;
    for (std::size_t i = 0; i < on_.size(); ++i) {
        if (drawn(i, selected)) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

} // namespace neo
