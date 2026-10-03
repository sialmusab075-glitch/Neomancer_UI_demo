#include "sat/parse/TleParser.h"

#include "sat/model/Epoch.h"
#include "sat/parse/detail/Number.h"

#include <cstdio>
#include <cstring>

namespace sat {

namespace {

constexpr std::size_t kLineLength = 69;

std::string trimmed(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

int checksumOf(const std::string& line) {
    int sum = 0;
    for (std::size_t i = 0; i + 1 < kLineLength; ++i) {
        const char c = line[i];
        if (c >= '0' && c <= '9') {
            sum += c - '0';
        } else if (c == '-') {
            sum += 1;
        }
    }
    return sum % 10;
}

// Alpha-5: the first character may be a letter standing for 10..33 (I and O are skipped).
int alphaValue(char c) {
    if (c < 'A' || c > 'Z' || c == 'I' || c == 'O') {
        return -1;
    }
    int value = 10 + (c - 'A');
    if (c > 'I') {
        --value;
    }
    if (c > 'O') {
        --value;
    }
    return value;
}

bool separatorsOk(const std::string& line, const std::size_t* columns, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (line[columns[i]] != ' ') {
            return false;
        }
    }
    return true;
}

} // namespace

bool parseCatalogNumber(const char* f, std::uint32_t& out) {
    unsigned long rest = 0;
    const int lead = alphaValue(f[0]);
    if (lead >= 0) {
        if (!detail::parseUint(f + 1, 4, rest) || std::memchr(f + 1, ' ', 4) != nullptr) {
            return false;
        }
        out = static_cast<std::uint32_t>(lead) * 10000u + static_cast<std::uint32_t>(rest);
        return true;
    }
    if (!detail::parseUint(f, 5, rest)) {
        return false;
    }
    out = static_cast<std::uint32_t>(rest);
    return true;
}

std::string formatCatalogNumber5(std::uint32_t number) {
    char buf[8];
    if (number < 100000u) {
        std::snprintf(buf, sizeof buf, "%05u", static_cast<unsigned>(number));
        return buf;
    }
    if (number <= 339999u) {
        const unsigned lead = number / 10000u; // 10..33
        // inverse of alphaValue
        int letter = 'A' + static_cast<int>(lead) - 10;
        if (letter >= 'I') {
            ++letter;
        }
        if (letter >= 'O') {
            ++letter;
        }
        std::snprintf(buf, sizeof buf, "%c%04u", letter, static_cast<unsigned>(number % 10000u));
        return buf;
    }
    return "99999";
}

bool parseTle(const std::string& name, const std::string& rawLine1, const std::string& rawLine2, ElementSet& out,
              std::string& error, const TleParseOptions& options) {
    auto fail = [&error](const std::string& why) {
        error = why;
        return false;
    };
    // Only trailing blanks and a line ending may follow column 69.
    auto core = [](const std::string& raw) {
        std::size_t end = raw.size();
        while (end > 0 && (raw[end - 1] == ' ' || raw[end - 1] == '\r' || raw[end - 1] == '\n')) {
            --end;
        }
        return raw.substr(0, end);
    };
    const std::string l1 = core(rawLine1);
    const std::string l2 = core(rawLine2);
    if (l1.size() != kLineLength) {
        return fail("line 1 is " + std::to_string(l1.size()) + " characters, not 69");
    }
    if (l2.size() != kLineLength) {
        return fail("line 2 is " + std::to_string(l2.size()) + " characters, not 69");
    }
    if (l1[0] != '1') {
        return fail("line 1 does not start with '1'");
    }
    if (l2[0] != '2') {
        return fail("line 2 does not start with '2'");
    }
    static const std::size_t sep1[] = {1, 8, 17, 32, 43, 52, 61, 63};
    static const std::size_t sep2[] = {1, 7, 16, 25, 33, 42, 51};
    if (!separatorsOk(l1, sep1, sizeof sep1 / sizeof sep1[0])) {
        return fail("line 1 columns are misaligned");
    }
    if (!separatorsOk(l2, sep2, sizeof sep2 / sizeof sep2[0])) {
        return fail("line 2 columns are misaligned");
    }
    if (options.requireChecksum) {
        const int c1 = l1[68] - '0';
        const int c2 = l2[68] - '0';
        if (c1 < 0 || c1 > 9 || c1 != checksumOf(l1)) {
            return fail("line 1 checksum is " + std::string(1, l1[68]) + ", expected " + std::to_string(checksumOf(l1)));
        }
        if (c2 < 0 || c2 > 9 || c2 != checksumOf(l2)) {
            return fail("line 2 checksum is " + std::string(1, l2[68]) + ", expected " + std::to_string(checksumOf(l2)));
        }
    }

    ElementSet s;
    s.name = trimmed(name);

    std::uint32_t number1 = 0;
    std::uint32_t number2 = 0;
    if (!parseCatalogNumber(l1.data() + 2, number1) || !parseCatalogNumber(l2.data() + 2, number2)) {
        return fail("catalogue number is not valid");
    }
    if (number1 != number2) {
        return fail("the two lines name different satellites (" + std::to_string(number1) + " and " + std::to_string(number2) + ")");
    }
    s.noradId = number1;
    s.classification = l1[7];

    // international designator YYNNNPPP -> "YYYY-NNNPPP"; blank is allowed
    const std::string designator = trimmed(l1.substr(9, 8));
    if (designator.size() >= 5 && designator[0] >= '0' && designator[0] <= '9' && designator[1] >= '0' && designator[1] <= '9') {
        const int yy = (designator[0] - '0') * 10 + (designator[1] - '0');
        s.objectId = std::to_string(yy < 57 ? 2000 + yy : 1900 + yy) + "-" + designator.substr(2);
    }

    unsigned long yy = 0;
    double dayOfYear = 0.0;
    if (!detail::parseUint(l1.data() + 18, 2, yy) || !detail::parseDecimal(l1.data() + 20, 12, dayOfYear)) {
        return fail("epoch is not a number");
    }
    const int year = yy < 57 ? 2000 + static_cast<int>(yy) : 1900 + static_cast<int>(yy);
    if (!epochFromYearDay(year, dayOfYear, s.epochJdWhole, s.epochJdFraction)) {
        return fail("epoch day " + std::to_string(dayOfYear) + " is outside the year " + std::to_string(year));
    }

    unsigned long elementSet = 0;
    unsigned long ephemeris = 0;
    if (!detail::parseDecimal(l1.data() + 33, 10, s.meanMotionDot)) {
        return fail("first derivative of mean motion is not a number");
    }
    if (!detail::parseImplied(l1.data() + 44, 8, s.meanMotionDdot)) {
        return fail("second derivative of mean motion is not a number");
    }
    if (!detail::parseImplied(l1.data() + 53, 8, s.bstar)) {
        return fail("B* is not a number");
    }
    // Old element sets leave the ephemeris-type column blank; that means 0, as in Vallado's reader.
    if (l1[62] != ' ' && !detail::parseUint(l1.data() + 62, 1, ephemeris)) {
        return fail("ephemeris type is not a number");
    }
    if (!detail::parseUint(l1.data() + 64, 4, elementSet)) {
        return fail("element set number is not a number");
    }
    s.ephemerisType = static_cast<int>(ephemeris);
    s.elementSetNo = static_cast<int>(elementSet);

    unsigned long eccentricityDigits = 0;
    unsigned long revolutions = 0;
    if (!detail::parseDecimal(l2.data() + 8, 8, s.inclinationDeg) || !detail::parseDecimal(l2.data() + 17, 8, s.raanDeg) ||
        !detail::parseDecimal(l2.data() + 34, 8, s.argPerigeeDeg) || !detail::parseDecimal(l2.data() + 43, 8, s.meanAnomalyDeg)) {
        return fail("an angle is not a number");
    }
    if (std::memchr(l2.data() + 26, ' ', 7) != nullptr || !detail::parseUint(l2.data() + 26, 7, eccentricityDigits)) {
        return fail("eccentricity is not seven digits");
    }
    s.eccentricity = static_cast<double>(eccentricityDigits) / 1.0e7;
    if (!detail::parseDecimal(l2.data() + 52, 11, s.meanMotionRevPerDay)) {
        return fail("mean motion is not a number");
    }
    if (!detail::parseUint(l2.data() + 63, 5, revolutions)) {
        return fail("revolution number is not a number");
    }
    s.revAtEpoch = static_cast<int>(revolutions);

    std::string reason;
    if (!validateElementSet(s, reason)) {
        return fail(reason);
    }
    out = std::move(s);
    return true;
}

SetParseReport parseTleText(const std::string& text, std::vector<ElementSet>& out, const TleParseOptions& options) {
    SetParseReport report;
    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string line = text.substr(start, end - start);
            while (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!trimmed(line).empty() && trimmed(line)[0] != '#') {
                lines.push_back(std::move(line));
            }
            start = end + 1;
        }
    }
    auto isLine1 = [](const std::string& l) { return l.size() >= 2 && l[0] == '1' && l[1] == ' '; };
    auto isLine2 = [](const std::string& l) { return l.size() >= 2 && l[0] == '2' && l[1] == ' '; };

    std::size_t i = 0;
    std::size_t index = 0;
    while (i < lines.size()) {
        std::string name;
        if (!isLine1(lines[i])) {
            // a name line, which must be followed by line 1
            name = trimmed(lines[i]);
            if (name.size() > 2 && name[0] == '0' && name[1] == ' ') {
                name = trimmed(name.substr(2));
            }
            ++i;
            if (i >= lines.size() || !isLine1(lines[i])) {
                ++report.seen;
                report.reject(index++, "name '" + name + "' is not followed by a line 1");
                continue;
            }
        }
        ++report.seen;
        const std::string& line1 = lines[i];
        if (i + 1 >= lines.size() || !isLine2(lines[i + 1])) {
            report.reject(index++, "line 1 of '" + name + "' is not followed by a line 2");
            ++i;
            continue;
        }
        ElementSet set;
        std::string error;
        if (parseTle(name, line1, lines[i + 1], set, error, options)) {
            ++report.accepted;
            out.push_back(std::move(set));
        } else {
            report.reject(index, (name.empty() ? std::string("(no name)") : name) + ": " + error);
        }
        ++index;
        i += 2;
    }
    return report;
}

} // namespace sat
