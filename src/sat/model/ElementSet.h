#pragma once

#include <cstdint>
#include <string>

namespace sat {

// One satellite's mean orbital elements as TLE and OMM publish them (SGP4's input).
//
// SATELLITES ARE VISUALIZATION ONLY. This library (src/sat) is separate from the NEO data and
// DSA layer (src/neo); nothing here is part of the graded query work, and src/neo never includes
// anything from here. See docs/SATELLITES.md.
//
// Units are the published ones, so a record can be compared with its source by eye:
//   angles in degrees, mean motion in rev/day, its derivatives in rev/day^2 and rev/day^3 (the
//   values TLE and OMM carry, not divided or multiplied by anything), B* in 1/Earth radii.
// The Propagator converts to SGP4's internal units (rad, rad/min).
struct ElementSet {
    std::string   name;            // "ISS (ZARYA)"; may be empty
    std::string   objectId;        // international designator, OMM style "1998-067A"; may be empty
    std::uint32_t noradId = 0;     // catalogue number
    char          classification = 'U';

    // Epoch (UTC) as a Julian Date split in two so no precision is lost: `whole` is the Julian
    // Date of 00:00 UT of the epoch day (an x.5 value), `fraction` is the fraction of that day.
    double epochJdWhole = 0.0;
    double epochJdFraction = 0.0;
    double epochJd() const { return epochJdWhole + epochJdFraction; }

    double meanMotionRevPerDay = 0.0;
    double eccentricity = 0.0;
    double inclinationDeg = 0.0;
    double raanDeg = 0.0;          // right ascension of the ascending node
    double argPerigeeDeg = 0.0;
    double meanAnomalyDeg = 0.0;

    double bstar = 0.0;
    double meanMotionDot = 0.0;    // rev/day^2
    double meanMotionDdot = 0.0;   // rev/day^3
    int    ephemerisType = 0;
    int    elementSetNo = 0;
    int    revAtEpoch = 0;

    double periodMinutes() const { return meanMotionRevPerDay > 0.0 ? 1440.0 / meanMotionRevPerDay : 0.0; }
    // SGP4 switches to its deep-space (SDP4) branch for orbital periods of 225 minutes or more.
    bool deepSpace() const { return periodMinutes() >= 225.0; }
};

// The ranges both parsers enforce; false (with a reason) when an element set is not physically meaningful.
bool validateElementSet(const ElementSet& set, std::string& reason);

} // namespace sat
