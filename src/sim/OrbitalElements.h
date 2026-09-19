#pragma once

namespace sim {

// Classical Keplerian elements at the J2000.0 epoch, in the ecliptic frame of
// the parent body (heliocentric ecliptic J2000 for planets).
//
// Angles are stored in degrees exactly as published; conversions to radians
// happen in KeplerSolver.
struct OrbitalElements {
    double a_AU      = 0.0; // semi-major axis
    double e         = 0.0; // eccentricity (0 <= e < 1)
    double i_deg     = 0.0; // inclination to the ecliptic
    double Omega_deg = 0.0; // longitude of the ascending node (Omega)
    double varpi_deg = 0.0; // longitude of perihelion (varpi = Omega + omega)
    double L_deg     = 0.0; // mean longitude at epoch (L = varpi + M)

    // Argument of perihelion omega = varpi - Omega.
    constexpr double argPerihelion_deg() const { return varpi_deg - Omega_deg; }
    // Mean anomaly at epoch M0 = L - varpi.
    constexpr double meanAnomalyAtEpoch_deg() const { return L_deg - varpi_deg; }
    constexpr double perihelion_AU() const { return a_AU * (1.0 - e); }
    constexpr double aphelion_AU() const { return a_AU * (1.0 + e); }
};

} // namespace sim
