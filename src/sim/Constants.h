#pragma once
// Physical and astronomical constants. All in SI unless the name says otherwise.

namespace sim {

constexpr double kPi       = 3.14159265358979323846;
constexpr double kTwoPi    = 2.0 * kPi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

constexpr double kAU_m          = 1.495978707e11;   // astronomical unit (IAU 2012, exact)
constexpr double kAU_km         = kAU_m / 1000.0;
constexpr double kSecondsPerDay = 86400.0;

// Gravitational constant (CODATA 2018).
constexpr double kG = 6.67430e-11;                   // m^3 kg^-1 s^-2

// Heliocentric gravitational parameter mu = G * M_sun (IAU 2015 nominal).
// GM is known to ~10 significant figures while G alone is known to ~5, so the
// Sun's mass is derived from mu rather than the other way round.
constexpr double kMuSun_m3s2 = 1.32712440018e20;     // m^3 s^-2
constexpr double kSunMass_kg = kMuSun_m3s2 / kG;     // ~1.98841e30 kg

// J2000.0 epoch: 2000-01-01 12:00 TT, as a Julian Date.
// Simulation time is "days since J2000.0".
constexpr double kJ2000_JD = 2451545.0;

} // namespace sim
