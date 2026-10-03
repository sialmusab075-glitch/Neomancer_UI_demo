#pragma once

#include <string>

namespace sat {

// Calendar helpers for element-set epochs. Gregorian calendar, UTC, no leap seconds (as TLE and
// OMM epochs are used by SGP4). Julian Dates are kept as a (whole, fraction) pair, see ElementSet.

bool validCivilDate(int year, int month, int day);

// Julian Date of 00:00 UT on a Gregorian date (an x.5 number): 2000-01-01 -> 2451544.5.
double julianDay0h(int year, int month, int day);

// Epoch from a TLE-style year and day of year (1.0 = 00:00 on 1 January).
bool epochFromYearDay(int year, double dayOfYear, double& whole, double& fraction);

// Epoch from "YYYY-MM-DDTHH:MM:SS[.ffffff]" (a space instead of T, and a trailing Z, are accepted).
bool epochFromIso(const std::string& text, double& whole, double& fraction, std::string& error);

// "YYYY-MM-DDTHH:MM:SS.ffffff", rounded to the microsecond.
std::string epochToIso(double whole, double fraction);

// The current UTC time as a (whole, fraction) Julian Date, from the system clock. (Unix time is UTC without
// leap seconds, the same convention TLE and OMM epochs use.)
void utcJdNow(double& whole, double& fraction);

} // namespace sat
