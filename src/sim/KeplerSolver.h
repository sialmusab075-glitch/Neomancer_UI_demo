#pragma once

#include "sim/Body.h"
#include "sim/OrbitalElements.h"
#include "sim/Vec3.h"

namespace sim {

struct KeplerResult {
    double E          = 0.0;  // eccentric anomaly, rad
    int    iterations = 0;
    bool   converged  = false;
};

constexpr double kKeplerTolerance     = 1e-10;
constexpr int    kKeplerMaxIterations = 15;

// Wraps an angle to (-pi, pi].
double wrapPi(double angle);

// Solves Kepler's equation E - e*sin(E) = M for E by Newton iteration.
// M in radians (any value), 0 <= e < 1.
KeplerResult solveKepler(double M, double e,
                         double tolerance = kKeplerTolerance,
                         int maxIterations = kKeplerMaxIterations);

// True anomaly from eccentric anomaly, in (-pi, pi].
double trueAnomalyFromEccentric(double E, double e);

// Mean motion n = sqrt(mu / a^3), in rad/day.
double meanMotionRadPerDay(double a_AU, double mu_m3s2);

// Orbital period 2*pi/n, in days.
double orbitalPeriodDays(double a_AU, double mu_m3s2);

// Vis-viva speed v = sqrt(mu * (2/r - 1/a)), in km/s.
double visVivaSpeedKms(double r_AU, double a_AU, double mu_m3s2);

// Rotates a vector from the orbital plane (x toward perihelion, y along motion
// at perihelion) into the parent's ecliptic frame using omega, Omega, i (rad).
Vec3d orbitalPlaneToEcliptic(double xp, double yp,
                             double omega, double Omega, double inc);

// Position on the orbit at eccentric anomaly E, ecliptic frame, AU.
// Used to precompute orbit rings.
Vec3d orbitPointAtEccentricAnomaly(const OrbitalElements& el, double E);

// Full propagation: mean anomaly at t -> Kepler -> true anomaly -> position and
// velocity in the parent's ecliptic frame.
// t_days: days since J2000.0.
OrbitState propagate(const OrbitalElements& el, double mu_m3s2, double t_days);

} // namespace sim
