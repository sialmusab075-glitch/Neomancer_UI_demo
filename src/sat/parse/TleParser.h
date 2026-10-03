#pragma once

#include "sat/model/ElementSet.h"
#include "sat/parse/ParseReport.h"

#include <string>
#include <vector>

namespace sat {

struct TleParseOptions {
    // The checksum is the last character of each line: the sum of its digits, with each '-' counting
    // as 1, modulo 10. Real feeds always carry a valid one; the Vallado verification file does not
    // for three of its satellites, which is why this is an option and not a constant.
    bool requireChecksum = true;
};

// One two-line element set. `line1` and `line2` are exactly 69 characters (trailing spaces and a
// line ending are ignored). Columns follow the NORAD TLE format; the separator columns must be
// spaces. Fills everything the TLE carries (name comes from the caller). On failure returns false
// and sets `error`.
bool parseTle(const std::string& name, const std::string& line1, const std::string& line2, ElementSet& out,
              std::string& error, const TleParseOptions& options = TleParseOptions());

// A text with many element sets: three lines each (name, line 1, line 2) as CelesTrak's 3LE, or two
// lines each (no name). Blank lines and lines starting with '#' are skipped; a name line may carry
// Space-Track's "0 " prefix. Both "\n" and "\r\n" are accepted. Bad sets are reported, not fatal.
SetParseReport parseTleText(const std::string& text, std::vector<ElementSet>& out,
                            const TleParseOptions& options = TleParseOptions());

// "A0001" style (Alpha-5) and plain numeric catalogue numbers of a TLE's five-character field.
bool parseCatalogNumber(const char* field5, std::uint32_t& out);

// The inverse, for passing the number to code that only takes five characters. Numbers beyond
// 339999 (the Alpha-5 limit) are returned as "99999": the string is cosmetic there.
std::string formatCatalogNumber5(std::uint32_t number);

} // namespace sat
