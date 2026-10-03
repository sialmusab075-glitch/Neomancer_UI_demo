#pragma once

#include <cstddef>

namespace sat {
namespace detail {

// Locale-independent number parsing. The C library's strtod and sscanf read the decimal point
// from the current locale, so under a German locale "0.5" stops at the dot; element sets are
// always written with a point. These accept exactly the formats the parsers need.

// [spaces][+-]digits[.digits][e[+-]digits][spaces]; at least one digit; the whole range must match.
// At most 18 significant digits (a TLE field has at most 11).
bool parseDecimal(const char* text, std::size_t length, double& out);

// A TLE exponent field with an implied decimal point: [+-]ddddd[+-]e, e.g. " 28098-4" = 0.28098e-4,
// "-22483-4" = -0.22483e-4, " 00000+0" = 0. Surrounding spaces are ignored.
bool parseImplied(const char* text, std::size_t length, double& out);

// [spaces]digits[spaces], at most 9 digits.
bool parseUint(const char* text, std::size_t length, unsigned long& out);

} // namespace detail
} // namespace sat
