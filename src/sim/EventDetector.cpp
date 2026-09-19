#include "sim/EventDetector.h"

#include "sim/BodyTable.h"
#include "sim/KeplerSolver.h"
#include "sim/SolarSystem.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sim {

namespace {

constexpr int kBisectionSteps = 48; // interval shrinks by 2^48: sub-second for any frame step

// Heliocentric position of body `i` at time t (parents are planets' Sun at origin,
// or a planet for moons), evaluated analytically.
Vec3d helioAt(const SolarSystem& system, int i, double t) {
    Vec3d p;
    while (i >= 0) {
        const Body& b = system.body(i);
        if (b.parentIndex < 0) {
            break;
        }
        p += propagate(b.data().elements, b.mu_m3s2, t).pos_AU;
        i = b.parentIndex;
    }
    return p;
}

double crossZ(const Vec3d& a, const Vec3d& b) { return a.x * b.y - a.y * b.x; }

// Finds the root of f on [ta, tb] given f(ta) and f(tb) of opposite sign (or zero).
template <typename F>
double bisect(F f, double ta, double tb, double fa) {
    for (int k = 0; k < kBisectionSteps; ++k) {
        const double tm = 0.5 * (ta + tb);
        const double fm = f(tm);
        if ((fm < 0.0) == (fa < 0.0) && fm != 0.0) {
            ta = tm;
            fa = fm;
        } else {
            tb = tm;
        }
    }
    return 0.5 * (ta + tb);
}

} // namespace

const char* eventName(EventType type) {
    switch (type) {
    case EventType::Perihelion:
        return "PERIHELION";
    case EventType::Aphelion:
        return "APHELION";
    case EventType::Opposition:
        return "OPPOSITION";
    case EventType::Conjunction:
        return "CONJUNCTION";
    }
    return "EVENT";
}

double radialVelocity(const OrbitState& s) {
    const double r = length(s.pos_AU);
    return r > 0.0 ? dot(s.pos_AU, s.vel_kms) / r : 0.0;
}

void EventDetector::reset() {
    have_ = false;
    prevRdot_.clear();
    prevCross_.clear();
}

void EventDetector::update(const SolarSystem& system, std::vector<SimEvent>& out) {
    const double t = system.timeDays();
    const std::size_t n = static_cast<std::size_t>(system.bodyCount());
    const int earth = system.indexOfTableRow(kEarth);
    const Vec3d earthPos = earth >= 0 ? system.body(earth).helioPos_AU : Vec3d{};
    const double earthA = earth >= 0 ? system.body(earth).data().elements.a_AU : 0.0;

    std::vector<double> rdot(n, 0.0), cross(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const Body& b = system.body(static_cast<int>(i));
        if (b.parentIndex >= 0) {
            rdot[i] = radialVelocity(b.orbit);
            cross[i] = crossZ(earthPos, b.helioPos_AU);
        }
    }

    const std::size_t firstNew = out.size();
    if (have_ && t != prevT_ && prevRdot_.size() == n) {
        // Order the two samples in sim time, so reverse playback classifies correctly.
        const bool forward = t > prevT_;
        const double ta = forward ? prevT_ : t;
        const double tb = forward ? t : prevT_;

        for (std::size_t i = 0; i < n; ++i) {
            const int bi = static_cast<int>(i);
            const Body& b = system.body(bi);
            if (b.parentIndex < 0) {
                continue;
            }
            const OrbitalElements& el = b.data().elements;
            const double mu = b.mu_m3s2;

            // --- perihelion / aphelion: sign change of dr/dt -------------------------
            const double fa = forward ? prevRdot_[i] : rdot[i];
            const double fb = forward ? rdot[i] : prevRdot_[i];
            const bool peri = fa < 0.0 && fb >= 0.0;
            const bool apo = fa > 0.0 && fb <= 0.0;
            if (peri || apo) {
                auto f = [&](double tt) { return radialVelocity(propagate(el, mu, tt)); };
                const double te = bisect(f, ta, tb, fa);
                out.push_back({peri ? EventType::Perihelion : EventType::Aphelion, bi, te,
                               propagate(el, mu, te).r_AU});
            }

            // --- opposition / conjunction for planets outside Earth's orbit ---------------
            if (earth < 0 || bi == earth || b.data().kind != BodyKind::Planet || el.a_AU <= earthA) {
                continue;
            }
            const double ca = forward ? prevCross_[i] : cross[i];
            const double cb = forward ? cross[i] : prevCross_[i];
            if ((ca < 0.0) != (cb < 0.0)) {
                auto g = [&](double tt) { return crossZ(helioAt(system, earth, tt), helioAt(system, bi, tt)); };
                const double te = bisect(g, ta, tb, ca);
                const Vec3d e = helioAt(system, earth, te);
                const Vec3d p = helioAt(system, bi, te);
                // Same side of the Sun as Earth -> opposition; far side -> conjunction.
                const EventType type = dot(e, p) > 0.0 ? EventType::Opposition : EventType::Conjunction;
                out.push_back({type, bi, te, length(p - e)});
            }
        }

        // Report in the order they happened along the direction of play.
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(firstNew), out.end(),
                  [forward](const SimEvent& a, const SimEvent& b) {
                      return forward ? a.t_days < b.t_days : a.t_days > b.t_days;
                  });
    }

    have_ = true;
    prevT_ = t;
    prevRdot_ = rdot;
    prevCross_ = cross;
}

} // namespace sim
