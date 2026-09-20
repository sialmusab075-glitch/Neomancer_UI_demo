#pragma once

#include "sim/Constants.h"

#include <string>

namespace neo {

// Times in the NEO layer are Julian Dates in the TDB time scale, exactly as JPL
// publishes them (SBDB "epoch", CAD "jd"). They stay doubles: a JD near 2.46e6
// has ~1e-10 day (~10 us) of resolution in a double, far finer than the data.
//
// The renderer and the simulation clock work in days since J2000.0, so the two
// conversions below are the only bridge between the two conventions. Anything
// shown to a user goes through formatJulianDate().

constexpr double kJ2000Jd = sim::kJ2000_JD; // 2451545.0

constexpr double daysSinceJ2000(double jdTdb) { return jdTdb - kJ2000Jd; }
constexpr double julianDateFromDaysSinceJ2000(double days) { return days + kJ2000Jd; }

// "YYYY-MM-DD HH:MM" (TDB). Gregorian, proleptic Julian before 1582-10-15.
std::string formatJulianDate(double jdTdb);

// "YYYY-MM-DD" (TDB), for table columns that have no room for a time.
std::string formatJulianDay(double jdTdb);

// Current UTC as "YYYY-MM-DDTHH:MMZ", for cache metadata and report headers.
// Built from the system clock through the Julian Date path rather than
// std::gmtime, which has no portable thread-safe form (and is deprecated under
// MSVC). The ~1 minute TT/TDB-UTC offset is irrelevant for a timestamp.
std::string utcNowIso();

// Julian Date for a calendar date at 00:00 TDB. Used for CLI date options
// (--date-min / --date-max), not for parsing API payloads: those carry a JD.
// Returns false when the string is not exactly "YYYY-MM-DD" or the date is invalid.
bool julianDateFromIsoDate(const std::string& iso, double& jdOut);

} // namespace neo
