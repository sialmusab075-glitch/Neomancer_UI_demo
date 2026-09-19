#include "sim/BodyTable.h"

#include "sim/Constants.h"

#include <cstring>

namespace sim {

// ============================================================================
//  BODY TABLE
// ============================================================================
//
//  Orbital elements: E. M. Standish (JPL/Caltech), "Keplerian Elements for
//  Approximate Positions of the Major Planets", Table 1 (valid 1800-2050 AD),
//  epoch J2000.0, mean ecliptic and equinox of J2000. Only the epoch values are
//  used; the tiny secular rates (per century) are ignored.
//  The "Earth" row uses the Earth-Moon barycentre elements, as that table does.
//
//  Physical data: NASA/NSSDCA planetary fact sheets.
//    mass_kg      kilograms
//    radius_km    volumetric mean radius, kilometres
//    period_d     sidereal orbital period, days
//
//  Columns of OrbitalElements:
//    a     semi-major axis                       [AU]
//    e     eccentricity                          [-]
//    i     inclination to the ecliptic           [deg]
//    Omega longitude of the ascending node       [deg]
//    varpi longitude of perihelion               [deg]
//    L     mean longitude at epoch               [deg]
//
//  The Sun sits at the origin of the heliocentric frame; its elements are zero.
// ============================================================================

namespace {

// clang-format off
const BodyData kTable[kBodyCount] = {
//   name       id tag          kind              parent  { a [AU],       e,           i [deg],      Omega [deg],   varpi [deg],   L [deg]      }  mass [kg]    radius [km]  period [d]  color
    {"SUN",     "ST-ID 00.00",  BodyKind::Star,   -1,     { 0.0,          0.0,         0.0,          0.0,           0.0,           0.0          }, kSunMass_kg, 695700.0,    0.0,        0xFFC45Cu},
    {"MERCURY", "PT-ID 01.00",  BodyKind::Planet, kSun,   { 0.38709927,   0.20563593,  7.00497902,   48.33076593,   77.45779628,   252.25032350 }, 3.3011e23,   2439.7,      87.969,     0xA8A29Au},
    {"VENUS",   "PT-ID 02.00",  BodyKind::Planet, kSun,   { 0.72333566,   0.00677672,  3.39467605,   76.67984255,   131.60246718,  181.97909950 }, 4.8675e24,   6051.8,      224.701,    0xE3C58Bu},
    {"EARTH",   "PT-ID 03.00",  BodyKind::Planet, kSun,   { 1.00000261,   0.01671123, -0.00001531,   0.0,           102.93768193,  100.46457166 }, 5.9722e24,   6371.0,      365.256,    0x4F8FD3u},
    {"MARS",    "PT-ID 04.00",  BodyKind::Planet, kSun,   { 1.52371034,   0.09339410,  1.84969142,   49.55953891,   -23.94362959,  -4.55343205  }, 6.4171e23,   3389.5,      686.980,    0xD2694Bu},
    {"JUPITER", "PT-ID 05.00",  BodyKind::Planet, kSun,   { 5.20288700,   0.04838624,  1.30439695,   100.47390909,  14.72847983,   34.39644051  }, 1.89819e27,  69911.0,     4332.589,   0xD9B38Cu},
    {"SATURN",  "PT-ID 06.00",  BodyKind::Planet, kSun,   { 9.53667594,   0.05386179,  2.48599187,   113.66242448,  92.59887831,   49.95424423  }, 5.6834e26,   58232.0,     10759.22,   0xE6D29Cu},
    {"URANUS",  "PT-ID 07.00",  BodyKind::Planet, kSun,   { 19.18916464,  0.04725744,  0.77263783,   74.01692503,   170.95427630,  313.23810451 }, 8.6813e25,   25362.0,     30685.4,    0x9FDCE0u},
    {"NEPTUNE", "PT-ID 08.00",  BodyKind::Planet, kSun,   { 30.06992276,  0.00859048,  1.77004347,   131.78422574,  44.96476227,   -55.12002969 }, 1.02413e26,  24622.0,     60189.0,    0x4B6FD9u},
};
// clang-format on

} // namespace

const BodyData& bodyData(int index) { return kTable[index]; }

int bodyTableSize() { return kBodyCount; }

int findBody(const char* name) {
    for (int i = 0; i < kBodyCount; ++i) {
        if (std::strcmp(kTable[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const BodyData& Body::data() const { return bodyData(tableIndex); }

} // namespace sim
