#include "sim/KeplerSolver.h"

#include "sim/Constants.h"

#include <cmath>

namespace sim {

double wrapPi(double angle) {
    double a = std::fmod(angle + kPi, kTwoPi);
    if (a <= 0.0) {
        a += kTwoPi;
    }
    return a - kPi;
}

KeplerResult solveKepler(double M, double e, double tolerance, int maxIterations) {
    KeplerResult result;
    const double m = wrapPi(M);

    if (e == 0.0) {
        result.E = M;
        result.converged = true;
        return result;
    }

    // Danby's starting value: robust for every 0 <= e < 1.
    double E = m + 0.85 * e * (std::sin(m) >= 0.0 ? 1.0 : -1.0);

    for (int k = 0; k < maxIterations; ++k) {
        const double f  = E - e * std::sin(E) - m;
        const double fp = 1.0 - e * std::cos(E);
        const double dE = f / fp;
        E -= dE;
        result.iterations = k + 1;
        if (std::fabs(dE) < tolerance) {
            result.converged = true;
            break;
        }
    }

    // Report E consistent with the caller's (unwrapped) M so M and E stay
    // on the same revolution.
    result.E = E + (M - m);
    return result;
}

double trueAnomalyFromEccentric(double E, double e) {
    const double half = 0.5 * E;
    return wrapPi(2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(half),
                                   std::sqrt(1.0 - e) * std::cos(half)));
}

double meanMotionRadPerDay(double a_AU, double mu_m3s2) {
    const double a_m = a_AU * kAU_m;
    return std::sqrt(mu_m3s2 / (a_m * a_m * a_m)) * kSecondsPerDay;
}

double orbitalPeriodDays(double a_AU, double mu_m3s2) {
    return kTwoPi / meanMotionRadPerDay(a_AU, mu_m3s2);
}

double visVivaSpeedKms(double r_AU, double a_AU, double mu_m3s2) {
    const double r_m = r_AU * kAU_m;
    const double a_m = a_AU * kAU_m;
    return std::sqrt(mu_m3s2 * (2.0 / r_m - 1.0 / a_m)) / 1000.0;
}

Vec3d orbitalPlaneToEcliptic(double xp, double yp, double omega, double Omega, double inc) {
    const double cw = std::cos(omega), sw = std::sin(omega);
    const double cO = std::cos(Omega), sO = std::sin(Omega);
    const double ci = std::cos(inc),   si = std::sin(inc);

    // R = Rz(Omega) * Rx(i) * Rz(omega), applied to (xp, yp, 0).
    return {
        (cw * cO - sw * sO * ci) * xp + (-sw * cO - cw * sO * ci) * yp,
        (cw * sO + sw * cO * ci) * xp + (-sw * sO + cw * cO * ci) * yp,
        (sw * si) * xp + (cw * si) * yp,
    };
}

Vec3d orbitPointAtEccentricAnomaly(const OrbitalElements& el, double E) {
    const double e  = el.e;
    const double xp = el.a_AU * (std::cos(E) - e);
    const double yp = el.a_AU * std::sqrt(1.0 - e * e) * std::sin(E);
    return orbitalPlaneToEcliptic(xp, yp,
                                  el.argPerihelion_deg() * kDegToRad,
                                  el.Omega_deg * kDegToRad,
                                  el.i_deg * kDegToRad);
}

OrbitState propagate(const OrbitalElements& el, double mu_m3s2, double t_days) {
    OrbitState s;
    const double e = el.e;
    const double n = meanMotionRadPerDay(el.a_AU, mu_m3s2);

    // 1) Mean anomaly at t.
    const double M = el.meanAnomalyAtEpoch_deg() * kDegToRad + n * t_days;
    s.M = wrapPi(M);

    // 2) Kepler's equation.
    const KeplerResult k = solveKepler(s.M, e);
    s.E = k.E;
    s.keplerIterations = k.iterations;
    s.keplerConverged = k.converged;

    // 3) True anomaly and radius.
    s.nu   = trueAnomalyFromEccentric(s.E, e);
    s.r_AU = el.a_AU * (1.0 - e * std::cos(s.E));

    // 4) Position and velocity in the orbital plane.
    const double cosE = std::cos(s.E);
    const double sinE = std::sin(s.E);
    const double b    = el.a_AU * std::sqrt(1.0 - e * e);
    const double xp   = el.a_AU * (cosE - e);
    const double yp   = b * sinE;

    const double Edot    = n / (1.0 - e * cosE);            // rad/day
    const double auPerDayToKms = kAU_km / kSecondsPerDay;
    const double vxp = -el.a_AU * sinE * Edot * auPerDayToKms;
    const double vyp =  b * cosE * Edot * auPerDayToKms;

    // 5) Rotate into the ecliptic frame.
    const double omega = el.argPerihelion_deg() * kDegToRad;
    const double Omega = el.Omega_deg * kDegToRad;
    const double inc   = el.i_deg * kDegToRad;
    s.pos_AU  = orbitalPlaneToEcliptic(xp, yp, omega, Omega, inc);
    s.vel_kms = orbitalPlaneToEcliptic(vxp, vyp, omega, Omega, inc);
    return s;
}

} // namespace sim
