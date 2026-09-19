#pragma once

#include "sim/OrbitalElements.h"
#include "sim/Vec3.h"

#include <cstdint>

namespace sim {

enum class BodyKind { Star, Planet, Moon };

// Static, published data for one body. One row of the table in BodyTable.cpp.
struct BodyData {
    const char*      name;                // upper-case display name, e.g. "EARTH"
    const char*      idTag;               // HUD identifier, e.g. "PT-ID 03.00"
    BodyKind         kind;
    int              parent;              // index into the body table; -1 for the Sun
    OrbitalElements  elements;            // relative to the parent
    double           mass_kg;
    double           radius_km;           // mean radius
    double           siderealPeriod_days; // published value, for display and cross-checks
    std::uint32_t    colorRGB;            // presentation hint 0xRRGGBB (not used by physics)
};

// Kinematic state produced by the Kepler propagator for one instant.
struct OrbitState {
    double M     = 0.0; // mean anomaly, rad, wrapped to (-pi, pi]
    double E     = 0.0; // eccentric anomaly, rad
    double nu    = 0.0; // true anomaly, rad, (-pi, pi]
    double r_AU  = 0.0; // distance to the parent
    int    keplerIterations = 0;
    bool   keplerConverged  = true;
    Vec3d  pos_AU;      // position relative to the parent, ecliptic frame
    Vec3d  vel_kms;     // velocity relative to the parent, ecliptic frame
};

// A body instantiated in a SolarSystem, updated every frame.
struct Body {
    int        tableIndex  = -1;  // row in the body table
    int        parentIndex = -1;  // index into SolarSystem::bodies(); -1 for the Sun
    double     mu_m3s2     = 0.0; // gravitational parameter of the parent (0 for the Sun)
    double     meanMotion_radPerDay = 0.0;
    OrbitState orbit;             // parent-relative
    Vec3d      helioPos_AU;       // heliocentric position
    Vec3d      helioVel_kms;      // heliocentric velocity

    const BodyData& data() const;
};

} // namespace sim
