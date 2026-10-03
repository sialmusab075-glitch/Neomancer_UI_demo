// Satellite library tests (src/sat): number and epoch parsing, the TLE and OMM parsers, the SGP4
// propagator against Vallado's published verification set, and the catalogue file. No network.
//
// SATELLITES ARE VISUALIZATION ONLY: this suite is separate from the NEO suites on purpose.
//
// The centrepiece is the Vallado reference set: 33 satellites (near Earth and deep space, including
// the ones that must FAIL with SGP4 error codes) and 667 reference rows produced by the official C++
// code. Every row is recomputed through this project's own TLE parser and Propagator and must agree to
// 2e-7 km / km/s, the tolerance python-sgp4 uses for the same file. The parser, the unit conversions
// and the vendored sources are all inside that one comparison.

#include "sat/model/ElementSet.h"
#include "sat/model/Epoch.h"
#include "sat/parse/OmmJsonParser.h"
#include "sat/parse/TleParser.h"
#include "sat/parse/detail/Number.h"
#include "sat/sim/Propagator.h"
#include "sat/store/Catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef SAT_FIXTURES_DIR
#define SAT_FIXTURES_DIR "tests/fixtures/sat"
#endif
#ifndef SGP4_FIXTURES_DIR
#define SGP4_FIXTURES_DIR "tests/fixtures/sgp4"
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

const char* kIssLine1 = "1 25544U 98067A   19343.69339541  .00001764  00000-0  38792-4 0  9991";
const char* kIssLine2 = "2 25544  51.6439 211.2001 0007417  17.6667  85.6398 15.50103472202482";

sat::ElementSet iss() {
    sat::ElementSet s;
    std::string error;
    const bool ok = sat::parseTle("ISS (ZARYA)", kIssLine1, kIssLine2, s, error);
    check(ok, "the ISS test TLE parses", error);
    return s;
}

// --- numbers ---------------------------------------------------------------------------------------

bool dec(const std::string& text, double& out) { return sat::detail::parseDecimal(text.data(), text.size(), out); }
bool imp(const std::string& text, double& out) { return sat::detail::parseImplied(text.data(), text.size(), out); }

void testNumbers() {
    std::printf("[numbers] locale-independent parsing\n");
    double v = 0;
    check(dec("51.6439", v) && v == 51.6439, "plain decimal is exact");
    check(dec("  .00001764", v) && v == 1.764e-5, "a leading point and spaces");
    check(dec("-.00000084", v) && v == -8.4e-7, "a negative leading point");
    check(dec("+3.5", v) && v == 3.5, "an explicit plus");
    check(dec("1e-5", v) && v == 1e-5 && dec("2.5E3", v) && v == 2500.0, "exponent forms");
    check(dec("15.50103472", v) && v == 15.50103472, "eight decimals round-trip exactly");
    check(!dec("", v) && !dec("   ", v) && !dec(".", v) && !dec("-", v), "empty and sign-only strings are rejected");
    check(!dec("1.2.3", v) && !dec("12a", v) && !dec("1e", v) && !dec("1,5", v), "malformed numbers are rejected");
    check(!dec("1234567890123456789", v), "more than 18 significant digits is rejected, not rounded");

    check(imp(" 28098-4", v) && v == 28098.0 / 1e5 * 1e-4 * 1.0 && near(v, 2.8098e-5, 1e-20), "B* '28098-4' is 0.28098e-4");
    check(imp("-22483-4", v) && near(v, -2.2483e-5, 1e-20), "a negative B*");
    check(imp(" 00000-0", v) && v == 0.0 && imp(" 00000+0", v) && v == 0.0, "zero in both exponent signs");
    check(imp(" 29408-3", v) && near(v, 2.9408e-4, 1e-19), "'29408-3'");
    check(imp("15680-2", v) && near(v, 1.568e-3, 1e-18), "no leading sign character");
    check(!imp(" 28098", v) && !imp("abcde-4", v) && !imp("", v) && !imp(" 28098-x", v), "malformed implied-exponent fields");

    unsigned long u = 0;
    check(sat::detail::parseUint("  42 ", 5, u) && u == 42, "an integer with spaces");
    check(!sat::detail::parseUint("4 2", 3, u) && !sat::detail::parseUint("", 0, u) && !sat::detail::parseUint("-1", 2, u),
          "an integer with a gap, empty, or negative is rejected");
}

// --- epochs ----------------------------------------------------------------------------------------

void testEpoch() {
    std::printf("[epoch] calendar and Julian Date\n");
    check(sat::julianDay0h(2000, 1, 1) == 2451544.5, "2000-01-01 00:00 is JD 2451544.5");
    check(sat::julianDay0h(1949, 12, 31) == 2433281.5, "1949-12-31 is SGP4's own epoch, JD 2433281.5");
    check(sat::julianDay0h(1970, 1, 1) == 2440587.5, "the Unix epoch is JD 2440587.5");
    check(sat::julianDay0h(2024, 3, 1) - sat::julianDay0h(2024, 2, 28) == 2.0, "2024 is a leap year");
    check(sat::julianDay0h(2023, 3, 1) - sat::julianDay0h(2023, 2, 28) == 1.0, "2023 is not");
    check(sat::julianDay0h(1900, 3, 1) - sat::julianDay0h(1900, 2, 28) == 1.0, "1900 is not a leap year");
    check(sat::julianDay0h(2000, 3, 1) - sat::julianDay0h(2000, 2, 28) == 2.0, "2000 is");

    check(sat::validCivilDate(2024, 2, 29) && !sat::validCivilDate(2023, 2, 29) && !sat::validCivilDate(1900, 2, 29) &&
              sat::validCivilDate(2000, 2, 29),
          "leap-day validity");
    check(!sat::validCivilDate(2024, 13, 1) && !sat::validCivilDate(2024, 0, 1) && !sat::validCivilDate(2024, 4, 31) &&
              !sat::validCivilDate(2024, 1, 0),
          "month and day ranges");

    double whole = 0, fraction = 0;
    check(sat::epochFromYearDay(2019, 343.69339541, whole, fraction) && whole == sat::julianDay0h(2019, 12, 9) &&
              near(fraction, 0.69339541, 1e-13),
          "2019 day 343.69339541 is 9 December");
    check(sat::epochFromYearDay(2024, 366.0, whole, fraction) && whole == sat::julianDay0h(2024, 12, 31), "day 366 of a leap year");
    check(!sat::epochFromYearDay(2023, 366.0, whole, fraction) && !sat::epochFromYearDay(2024, 0.5, whole, fraction) &&
              !sat::epochFromYearDay(2024, 367.0, whole, fraction),
          "days outside the year are rejected");

    std::string error;
    check(sat::epochFromIso("2019-12-09T16:38:29.363423", whole, fraction, error) && whole == sat::julianDay0h(2019, 12, 9) &&
              near(fraction, (16 * 3600 + 38 * 60 + 29.363423) / 86400.0, 1e-15),
          "an OMM epoch string", error);
    check(sat::epochFromIso("2025-02-14 14:36:48", whole, fraction, error) && sat::epochFromIso("2025-02-14T14:36:48Z", whole, fraction, error) &&
              sat::epochFromIso("2025-02-14T14:36:48.5Z", whole, fraction, error),
          "a space for T, a trailing Z, and no fraction are accepted");
    check(!sat::epochFromIso("2025-02-30T00:00:00", whole, fraction, error) && !sat::epochFromIso("2025-02-14T24:00:00", whole, fraction, error) &&
              !sat::epochFromIso("2025-2-14T00:00:00", whole, fraction, error) && !sat::epochFromIso("garbage", whole, fraction, error) &&
              !sat::epochFromIso("2025-02-14T00:00:00.", whole, fraction, error) && !sat::epochFromIso("2025-02-14T00:00:00xyz", whole, fraction, error),
          "impossible dates and malformed strings are rejected");

    check(sat::epochToIso(sat::julianDay0h(2000, 1, 1), 0.5) == "2000-01-01T12:00:00.000000", "J2000 noon");
    check(sat::epochToIso(sat::julianDay0h(1949, 12, 31), 0.0) == "1949-12-31T00:00:00.000000", "SGP4's epoch");
    check(sat::epochToIso(sat::julianDay0h(2024, 2, 29), 0.999999999999) == "2024-03-01T00:00:00.000000", "a fraction that rounds up carries into the next day");
    check(sat::epochToIso(sat::julianDay0h(2019, 12, 9), 0.69339541) == "2019-12-09T16:38:29.363424", "the ISS epoch, rounded to the microsecond");

    // "now" comes from the system clock; check it is a sane Julian Date, advances, and agrees with its own text
    double nowWhole = 0, nowFraction = 0;
    sat::utcJdNow(nowWhole, nowFraction);
    check(nowWhole + nowFraction > 2461000.0 && nowWhole + nowFraction < 2470000.0 && nowFraction >= 0.0 && nowFraction < 1.0 &&
              std::fabs(nowWhole - std::floor(nowWhole) - 0.5) < 1e-9,
          "utcJdNow is a Julian Date between 2026 and 2050, a whole day ending in .5 plus a fraction in [0, 1)", num(nowWhole + nowFraction));
    double again = 0, againFraction = 0;
    sat::utcJdNow(again, againFraction);
    check((again - nowWhole) + (againFraction - nowFraction) >= 0.0 && (again - nowWhole) + (againFraction - nowFraction) < 1.0 / 86400.0 * 5.0,
          "and two readings a moment apart are in order and within five seconds");
    const std::string nowText = sat::epochToIso(nowWhole, nowFraction);
    double backWhole = 0, backFraction = 0;
    check(sat::epochFromIso(nowText, backWhole, backFraction, error) && backWhole == nowWhole && std::fabs(backFraction - nowFraction) < 1.2e-11,
          "its text form parses back to the same instant (to a microsecond)", nowText);

    bool roundTrip = true;
    for (int year = 1957; year <= 2099 && roundTrip; ++year) {
        for (int month = 1; month <= 12 && roundTrip; ++month) {
            for (const int day : {1, 15, 28}) {
                char expected[40];
                std::snprintf(expected, sizeof expected, "%04d-%02d-%02dT00:00:00.000000", year, month, day);
                roundTrip = roundTrip && sat::epochToIso(sat::julianDay0h(year, month, day), 0.0) == expected;
            }
        }
    }
    check(roundTrip, "every 1st, 15th and 28th from 1957 to 2099 survives date -> JD -> text");
}

// --- catalogue numbers -------------------------------------------------------------------------------

void testCatalogNumbers() {
    std::printf("[tle] catalogue numbers, Alpha-5\n");
    std::uint32_t n = 0;
    check(sat::parseCatalogNumber("25544", n) && n == 25544, "plain");
    check(sat::parseCatalogNumber("00005", n) && n == 5 && sat::parseCatalogNumber("    5", n) && n == 5, "leading zeros and spaces");
    check(sat::parseCatalogNumber("A0000", n) && n == 100000, "A0000 is 100000");
    check(sat::parseCatalogNumber("T0001", n) && n == 270001, "T0001 is 270001 (A=10 ... H=17, J=18 ... N=22, P=23 ... T=27)");
    check(sat::parseCatalogNumber("Z9999", n) && n == 339999, "Z9999 is the Alpha-5 limit, 339999");
    check(!sat::parseCatalogNumber("I0001", n) && !sat::parseCatalogNumber("O0001", n), "I and O are not used");
    check(!sat::parseCatalogNumber("A 001", n) && !sat::parseCatalogNumber("ABCDE", n) && !sat::parseCatalogNumber("12x45", n), "malformed");

    bool round = true;
    for (std::uint32_t id = 0; id <= 339999; id += 7) {
        const std::string text = sat::formatCatalogNumber5(id);
        std::uint32_t back = 0;
        round = round && text.size() == 5 && sat::parseCatalogNumber(text.c_str(), back) && back == id;
    }
    check(round, "format then parse is the identity for every 7th number up to 339999");
    check(sat::formatCatalogNumber5(339999) == "Z9999" && sat::formatCatalogNumber5(100000) == "A0000" && sat::formatCatalogNumber5(5) == "00005",
          "the ends");
    check(sat::formatCatalogNumber5(340000) == "99999" && sat::formatCatalogNumber5(4000000000u).size() == 5,
          "beyond Alpha-5 the five-character string is cosmetic and never longer than five characters");
}

// --- TLE ---------------------------------------------------------------------------------------------

void testTle() {
    std::printf("[tle] one element set\n");
    const sat::ElementSet s = iss();
    check(s.name == "ISS (ZARYA)" && s.noradId == 25544 && s.classification == 'U' && s.objectId == "1998-067A",
          "identity: name, catalogue number, classification, international designator", s.objectId);
    check(s.epochJdWhole == sat::julianDay0h(2019, 12, 9) && near(s.epochJdFraction, 0.69339541, 1e-13), "epoch 2019 day 343.69339541");
    check(s.inclinationDeg == 51.6439 && s.raanDeg == 211.2001 && s.argPerigeeDeg == 17.6667 && s.meanAnomalyDeg == 85.6398,
          "the four angles are exact");
    check(s.eccentricity == 0.0007417, "eccentricity 0007417 is 0.0007417", num(s.eccentricity));
    check(s.meanMotionRevPerDay == 15.50103472 && s.revAtEpoch == 20248, "mean motion and revolution number");
    check(s.meanMotionDot == 1.764e-5 && s.meanMotionDdot == 0.0 && near(s.bstar, 3.8792e-5, 1e-19), "ndot, nddot and B*");
    check(s.elementSetNo == 999 && s.ephemerisType == 0, "element set number and ephemeris type");
    check(near(s.periodMinutes(), 1440.0 / 15.50103472, 1e-12) && !s.deepSpace(), "92.9 minute period, near Earth");

    std::string error;
    sat::ElementSet t;
    // Vanguard 1 in 2020: a negative ndot and B*, a 1950s designator (year pivot 57)
    check(sat::parseTle("VANGUARD 1", "1 00005U 58002B   20287.20333880 -.00000016  00000-0 -22483-4 0  9998",
                        "2 00005  34.2443 225.5254 1845686 162.2516 205.2356 10.84869164218149", t, error),
          "Vanguard 1 parses", error);
    check(t.noradId == 5 && t.objectId == "1958-002B" && t.meanMotionDot == -1.6e-7 && near(t.bstar, -2.2483e-5, 1e-19) &&
              t.eccentricity == 0.1845686,
          "negative derivatives, a 1958 designator, eccentricity 0.1845686");
    check(t.epochJdWhole == sat::julianDay0h(2020, 10, 13), "2020 day 287 is 13 October");
    // MARIO: every field non-zero, large ndot and nddot
    check(sat::parseTle("MARIO", "1 55123U 98067UQ  23115.44827133  .00787702  29408-3  15680-2 0  9999",
                        "2 55123  51.6242 216.2930 0014649 331.8976  28.1241 15.99081912 18396", t, error),
          "MARIO parses", error);
    check(t.noradId == 55123 && t.objectId == "1998-067UQ" && t.meanMotionDot == 0.00787702 && near(t.meanMotionDdot, 2.9408e-4, 1e-19) &&
              near(t.bstar, 1.568e-3, 1e-18) && t.revAtEpoch == 1839 && t.elementSetNo == 999,
          "a multi-letter piece, a positive ndot, non-zero nddot and B*");

    // checksums
    std::string bad1 = kIssLine1;
    bad1[68] = '0';
    check(!sat::parseTle("X", bad1, kIssLine2, t, error) && error.find("line 1 checksum") != std::string::npos, "a wrong line 1 checksum is rejected", error);
    std::string bad2 = kIssLine2;
    bad2[68] = '9';
    check(!sat::parseTle("X", kIssLine1, bad2, t, error) && error.find("line 2 checksum") != std::string::npos, "a wrong line 2 checksum is rejected", error);
    sat::TleParseOptions lax;
    lax.requireChecksum = false;
    check(sat::parseTle("X", bad1, bad2, t, error, lax), "and accepted when the checksum is not required", error);
    std::string flipped = kIssLine2;
    flipped[10] = '7';
    check(!sat::parseTle("X", kIssLine1, flipped, t, error), "a flipped digit anywhere breaks the checksum");

    // structure
    check(!sat::parseTle("X", std::string(kIssLine1).substr(0, 68), kIssLine2, t, error) && error.find("68") != std::string::npos, "a short line", error);
    check(!sat::parseTle("X", std::string(kIssLine1) + "X", kIssLine2, t, error), "a long line");
    check(sat::parseTle("X", std::string(kIssLine1) + "   \r\n", std::string(kIssLine2) + "\r", t, error), "trailing blanks and a line ending are fine", error);
    check(!sat::parseTle("X", kIssLine2, kIssLine1, t, error), "the lines in the wrong order");
    std::string other = kIssLine2;
    other[6] = '5';
    check(!sat::parseTle("X", kIssLine1, other, t, error, lax) && error.find("different satellites") != std::string::npos, "two different catalogue numbers", error);
    std::string shifted = kIssLine1;
    shifted[8] = 'X';
    check(!sat::parseTle("X", shifted, kIssLine2, t, error, lax) && error.find("misaligned") != std::string::npos, "a separator column that is not a space", error);

    // ranges
    auto tleWith = [&](std::size_t col, const std::string& text) {
        std::string l2 = kIssLine2;
        l2.replace(col, text.size(), text);
        return l2;
    };
    check(!sat::parseTle("X", kIssLine1, tleWith(8, "181.0000"), t, error, lax), "inclination above 180");
    check(!sat::parseTle("X", kIssLine1, tleWith(17, "361.0000"), t, error, lax), "RAAN above 360");
    check(!sat::parseTle("X", kIssLine1, tleWith(52, " 0.00000000"), t, error, lax), "zero mean motion");
    check(!sat::parseTle("X", kIssLine1, tleWith(26, "00 4417"), t, error, lax), "a space inside the eccentricity digits");
    std::string badEpoch = kIssLine1;
    badEpoch.replace(20, 12, "400.12345678");
    check(!sat::parseTle("X", badEpoch, kIssLine2, t, error, lax) && error.find("epoch") != std::string::npos, "day 400 of the year", error);
    std::string alpha1 = kIssLine1;
    std::string alpha2 = kIssLine2;
    alpha1.replace(2, 5, "A0001");
    alpha2.replace(2, 5, "A0001");
    check(sat::parseTle("X", alpha1, alpha2, t, error, lax) && t.noradId == 100001, "an Alpha-5 catalogue number through the whole parser", error);
}

void testTleText() {
    std::printf("[tle] element-set files\n");
    const std::string text = readFile(std::string(SAT_FIXTURES_DIR) + "/celestrak_3le_sample.txt");
    check(!text.empty(), "the 3LE sample fixture is present");
    std::vector<sat::ElementSet> sets;
    sat::SetParseReport report = sat::parseTleText(text, sets);
    check(report.seen == 3 && report.accepted == 3 && report.rejected == 0 && sets.size() == 3, "three sets from the CelesTrak-format sample");
    check(sets.size() == 3 && sets[0].name == "ISS (ZARYA)" && sets[1].name == "VANGUARD 1" && sets[2].name == "MARIO", "names are trimmed of their padding");
    check(sets.size() == 3 && sets[0].noradId == 25544 && sets[1].noradId == 5 && sets[2].noradId == 55123, "catalogue numbers");

    // CRLF line endings (Windows checkouts and CelesTrak both produce them)
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') {
            crlf += "\r\n";
        } else {
            crlf += c;
        }
    }
    std::vector<sat::ElementSet> crlfSets;
    report = sat::parseTleText(crlf, crlfSets);
    check(report.accepted == 3 && crlfSets.size() == 3 && crlfSets[1].name == "VANGUARD 1", "the same file with CRLF line endings");

    // two-line sets, blank lines, comments, Space-Track's "0 " prefix
    std::vector<sat::ElementSet> mixed;
    const std::string two = std::string("# a comment\n\n") + kIssLine1 + "\n" + kIssLine2 + "\n\n0 VANGUARD 1\n" +
                            "1 00005U 58002B   20287.20333880 -.00000016  00000-0 -22483-4 0  9998\n" +
                            "2 00005  34.2443 225.5254 1845686 162.2516 205.2356 10.84869164218149\n";
    report = sat::parseTleText(two, mixed);
    check(report.accepted == 2 && mixed.size() == 2 && mixed[0].name.empty() && mixed[1].name == "VANGUARD 1",
          "a two-line set (no name), a comment, blank lines, and a '0 ' name prefix");

    // bad sets are reported, the rest still parsed
    std::string damaged = text;
    const std::size_t vanguardAt = damaged.find("1 00005U");
    check(vanguardAt != std::string::npos, "the Vanguard line is in the fixture");
    damaged[vanguardAt + 40] = '9'; // a digit inside Vanguard's line 1: its checksum no longer matches
    std::vector<sat::ElementSet> some;
    report = sat::parseTleText(damaged, some);
    check(report.accepted == 2 && report.rejected == 1 && some.size() == 2 && report.rejectedSamples.size() == 1 &&
              report.rejectedSamples[0].second.find("VANGUARD 1") != std::string::npos,
          "one damaged set is rejected by name, the other two survive");
    std::vector<sat::ElementSet> orphan;
    report = sat::parseTleText(std::string("LONELY NAME\n") + kIssLine1 + "\n", orphan);
    check(report.accepted == 0 && report.rejected == 1 && orphan.empty(), "a line 1 with no line 2 is rejected");
    std::vector<sat::ElementSet> none;
    report = sat::parseTleText("this is not a TLE file\nnor is this\n", none);
    check(report.accepted == 0 && report.rejected >= 1, "text that is not a TLE file yields nothing, with a reason");
}

// --- OMM JSON ----------------------------------------------------------------------------------------

void testOmm() {
    std::printf("[omm] CelesTrak GP JSON\n");
    std::vector<sat::ElementSet> sets;
    sat::OmmParseResult result = sat::parseOmmJson(readFile(std::string(SGP4_FIXTURES_DIR) + "/sample_omm.json"), sets);
    check(result.ok && result.report.accepted == 1 && sets.size() == 1, "the python-sgp4 sample (VANGUARD 1) parses");
    if (sets.size() == 1) {
        const sat::ElementSet& s = sets[0];
        check(s.name == "VANGUARD 1" && s.objectId == "1958-002B" && s.noradId == 5 && s.classification == 'U', "identity");
        check(s.meanMotionRevPerDay == 10.85873516 && s.eccentricity == 0.1841322 && s.inclinationDeg == 34.2493 &&
                  s.raanDeg == 19.2327 && s.argPerigeeDeg == 100.1057 && s.meanAnomalyDeg == 281.1229,
              "the six elements are exact");
        check(s.bstar == 0.00035436 && s.meanMotionDot == 2.64e-6 && s.meanMotionDdot == 0.0 && s.elementSetNo == 999 && s.revAtEpoch == 39027,
              "B*, derivatives, element set number, revolution number");
        check(s.epochJdWhole == sat::julianDay0h(2025, 2, 14) && near(s.epochJdFraction, (14 * 3600 + 36 * 60 + 48.662784) / 86400.0, 1e-15),
              "epoch 2025-02-14T14:36:48.662784");
    }

    // The same ISS in both formats must give the same element set.
    std::vector<sat::ElementSet> ommIss;
    result = sat::parseOmmJson(readFile(std::string(SAT_FIXTURES_DIR) + "/iss_omm_readme.json"), ommIss);
    check(result.ok && ommIss.size() == 1, "the README's ISS OMM listing parses, extra fields ignored");
    const sat::ElementSet tleIss = iss();
    if (ommIss.size() == 1) {
        const sat::ElementSet& o = ommIss[0];
        check(o.noradId == tleIss.noradId && o.name == tleIss.name && o.objectId == tleIss.objectId && o.classification == tleIss.classification,
              "TLE and OMM agree on identity");
        check(near(o.meanMotionRevPerDay, tleIss.meanMotionRevPerDay, 1e-12) && o.eccentricity == tleIss.eccentricity &&
                  o.inclinationDeg == tleIss.inclinationDeg && o.raanDeg == tleIss.raanDeg && o.argPerigeeDeg == tleIss.argPerigeeDeg &&
                  o.meanAnomalyDeg == tleIss.meanAnomalyDeg,
              "and on the six elements");
        check(near(o.bstar, tleIss.bstar, 1e-18) && o.meanMotionDot == tleIss.meanMotionDot && o.meanMotionDdot == tleIss.meanMotionDdot &&
                  o.elementSetNo == tleIss.elementSetNo && o.revAtEpoch == tleIss.revAtEpoch,
              "and on B*, the derivatives, element set and revolution numbers");
        check(o.epochJdWhole == tleIss.epochJdWhole && near(o.epochJdFraction, tleIss.epochJdFraction, 2.0e-6 / 86400.0), "and on the epoch to 2 microseconds");
        // and they propagate to the same place
        sat::Propagator a, b;
        std::string error;
        sat::Propagator::State pa, pb;
        const bool ok = a.init(o, error) && b.init(tleIss, error);
        check(ok && a.propagateMinutes(600.0, pa) == 0 && b.propagateMinutes(600.0, pb) == 0 && near(pa.r[0], pb.r[0], 1e-6) &&
                  near(pa.r[1], pb.r[1], 1e-6) && near(pa.r[2], pb.r[2], 1e-6),
              "and propagate to the same position after ten hours (within a millimetre)", error);
    }

    // numbers as strings, and a single object instead of an array
    std::vector<sat::ElementSet> strings;
    const std::string asStrings =
        R"({"OBJECT_NAME":"STR","OBJECT_ID":"2000-001A","EPOCH":"2024-01-02T03:04:05.5","MEAN_MOTION":"15.5","ECCENTRICITY":"0.001",)"
        R"("INCLINATION":"51.6","RA_OF_ASC_NODE":"10","ARG_OF_PERICENTER":"20","MEAN_ANOMALY":"30","NORAD_CAT_ID":"12345",)"
        R"("BSTAR":"1e-4","MEAN_MOTION_DOT":"2e-5","MEAN_MOTION_DDOT":"0"})";
    result = sat::parseOmmJson(asStrings, strings);
    check(result.ok && strings.size() == 1 && strings[0].noradId == 12345 && strings[0].bstar == 1e-4 && strings[0].classification == 'U' &&
              strings[0].revAtEpoch == 0,
          "numeric strings, a single object, and the optional fields absent");

    // rejected rows are named, the good one survives
    std::vector<sat::ElementSet> some;
    const std::string good = readFile(std::string(SGP4_FIXTURES_DIR) + "/sample_omm.json");
    std::string body = "[";
    body += good.substr(good.find('{'), good.rfind('}') - good.find('{') + 1);
    body += R"(,{"OBJECT_NAME":"NO EPOCH","NORAD_CAT_ID":1,"MEAN_MOTION":15,"ECCENTRICITY":0.1,"INCLINATION":50,"RA_OF_ASC_NODE":1,)"
            R"("ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0})";
    body += R"(,{"OBJECT_NAME":"HYPERBOLIC","EPOCH":"2024-01-01T00:00:00","NORAD_CAT_ID":2,"MEAN_MOTION":15,"ECCENTRICITY":1.5,"INCLINATION":50,)"
            R"("RA_OF_ASC_NODE":1,"ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0})";
    body += R"(,{"OBJECT_NAME":"BAD NUMBER","EPOCH":"2024-01-01T00:00:00","NORAD_CAT_ID":3,"MEAN_MOTION":"fast","ECCENTRICITY":0.1,"INCLINATION":50,)"
            R"("RA_OF_ASC_NODE":1,"ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0},42])";
    result = sat::parseOmmJson(body, some);
    check(result.ok && result.report.seen == 5 && result.report.accepted == 1 && result.report.rejected == 4 && some.size() == 1,
          "five rows: one good, four bad", std::to_string(result.report.accepted) + " accepted");
    std::string reasons;
    for (const auto& r : result.report.rejectedSamples) {
        reasons += r.second + " | ";
    }
    check(reasons.find("NO EPOCH: missing EPOCH") != std::string::npos && reasons.find("HYPERBOLIC") != std::string::npos &&
              reasons.find("BAD NUMBER: MEAN_MOTION is not a number") != std::string::npos && reasons.find("not a JSON object") != std::string::npos,
          "each rejection says which row and why", reasons);

    // hostile numbers must be rejected, not cast into garbage (a double above the int range is undefined behaviour to cast)
    std::vector<sat::ElementSet> hostile;
    const std::string huge =
        R"([{"OBJECT_NAME":"HUGE REV","EPOCH":"2024-01-01T00:00:00","NORAD_CAT_ID":4,"MEAN_MOTION":15,"ECCENTRICITY":0.1,"INCLINATION":50,)"
        R"("RA_OF_ASC_NODE":1,"ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0,"REV_AT_EPOCH":1e30},)"
        R"({"OBJECT_NAME":"FRACTIONAL SET","EPOCH":"2024-01-01T00:00:00","NORAD_CAT_ID":5,"MEAN_MOTION":15,"ECCENTRICITY":0.1,"INCLINATION":50,)"
        R"("RA_OF_ASC_NODE":1,"ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0,"ELEMENT_SET_NO":2.5},)"
        R"({"OBJECT_NAME":"HUGE ID","EPOCH":"2024-01-01T00:00:00","NORAD_CAT_ID":1e20,"MEAN_MOTION":15,"ECCENTRICITY":0.1,"INCLINATION":50,)"
        R"("RA_OF_ASC_NODE":1,"ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0},)"
        R"({"OBJECT_NAME":"NEGATIVE ID","EPOCH":"2024-01-01T00:00:00","NORAD_CAT_ID":-3,"MEAN_MOTION":15,"ECCENTRICITY":0.1,"INCLINATION":50,)"
        R"("RA_OF_ASC_NODE":1,"ARG_OF_PERICENTER":1,"MEAN_ANOMALY":1,"BSTAR":0,"MEAN_MOTION_DOT":0,"MEAN_MOTION_DDOT":0}])";
    result = sat::parseOmmJson(huge, hostile);
    check(result.ok && result.report.seen == 4 && result.report.rejected == 4 && hostile.empty(),
          "a 1e30 revolution count, a fractional element-set number, a 1e20 and a negative catalogue number are all rejected",
          std::to_string(result.report.accepted) + " accepted");

    // a body that is not OMM at all
    std::vector<sat::ElementSet> none;
    result = sat::parseOmmJson("No GP data found", none);
    check(!result.ok && result.error.find("No GP data found") != std::string::npos && none.empty(), "CelesTrak's plain-text 'No GP data found' is a failure, not an empty list");
    result = sat::parseOmmJson("[]", none);
    check(result.ok && result.report.seen == 0 && none.empty(), "an empty array is a successful empty result");
    result = sat::parseOmmJson("\"a string\"", none);
    check(!result.ok, "JSON that is not an array or object is a failure");
    result = sat::parseOmmJson("", none);
    check(!result.ok, "an empty body is a failure");
}

// --- propagator --------------------------------------------------------------------------------------

double norm(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void testPropagatorBasics() {
    std::printf("[propagator] physical sanity, time handling, errors\n");
    const sat::ElementSet s = iss();
    sat::Propagator p;
    check(!p.valid(), "a new propagator is not valid");
    sat::Propagator::State st;
    check(p.propagateMinutes(0.0, st) == sat::Propagator::kNotInitialised && std::isnan(st.r[0]), "propagating before init is an error, with NaN output");

    std::string error;
    check(p.init(s, error) && p.valid(), "the ISS elements initialise", error);
    check(p.propagateMinutes(0.0, st) == 0, "propagation at the epoch succeeds");
    const double radius = norm(st.r);
    const double speed = norm(st.v);
    check(radius - 6378.135 > 380.0 && radius - 6378.135 < 440.0, "the ISS is 380 to 440 km above the surface at that epoch",
          num(radius - 6378.135) + " km");
    check(speed > 7.5 && speed < 7.8, "and moves at 7.5 to 7.8 km/s", num(speed));
    const double hx = st.r[1] * st.v[2] - st.r[2] * st.v[1];
    const double hy = st.r[2] * st.v[0] - st.r[0] * st.v[2];
    const double hz = st.r[0] * st.v[1] - st.r[1] * st.v[0];
    const double inclination = std::acos(hz / std::sqrt(hx * hx + hy * hy + hz * hz)) * 180.0 / 3.14159265358979323846;
    check(near(inclination, 51.6439, 0.1), "its orbit plane is inclined 51.64 degrees (osculating within 0.1)", num(inclination));

    // a day later the orbit has decayed a little, not changed shape
    sat::Propagator::State later;
    check(p.propagateMinutes(1440.0, later) == 0 && norm(later.r) - 6378.135 > 380.0 && norm(later.r) - 6378.135 < 440.0, "a day later it is still in orbit");

    // Julian-date and minutes entry points agree
    sat::Propagator::State byJd;
    const double minutes = 90.0;
    const double jd = s.epochJdWhole;
    const double fraction = s.epochJdFraction + minutes / 1440.0;
    check(p.propagateJd(jd, fraction, byJd) == 0 && p.propagateMinutes(minutes, st) == 0 && near(byJd.r[0], st.r[0], 1e-9) &&
              near(byJd.r[1], st.r[1], 1e-9) && near(byJd.r[2], st.r[2], 1e-9),
          "propagateJd(epoch + 90 min) equals propagateMinutes(90)");
    check(p.propagateJd(jd + 1.0, fraction - 1.0, byJd) == 0 && near(byJd.r[0], st.r[0], 1e-6), "and a whole day moved between the two parts changes nothing");

    // a satellite that cannot be initialised says why, and does not stay half-valid
    sat::ElementSet broken = s;
    broken.eccentricity = 1.2;
    check(!p.init(broken, error) && !p.valid() && !error.empty(), "elements SGP4 refuses are an error with a reason, and leave the propagator invalid", error);
    sat::ElementSet decayed = s;
    decayed.meanMotionRevPerDay = 0.0;
    check(!p.init(decayed, error), "zero mean motion is refused");

    // a perigee below the surface: SGP4 accepts the elements, but the satellite falls through the Earth
    // within the first orbit, and the propagator reports decay (code 6) with NaN output, not numbers
    sat::ElementSet low = s;
    low.eccentricity = 0.2;
    low.meanMotionRevPerDay = 16.4;
    sat::Propagator lp;
    int initCode = 0;
    if (lp.init(low, error, &initCode)) {
        int firstError = 0;
        sat::Propagator::State fallen;
        for (double t = 0.0; t <= 200.0 && firstError == 0; t += 1.0) {
            firstError = lp.propagateMinutes(t, fallen);
        }
        check(firstError == 6 && std::isnan(fallen.r[0]) && std::isnan(fallen.v[2]), "an underground perigee ends in decay (code 6) with NaN output",
              std::to_string(firstError));
    } else {
        check(initCode == 6, "or SGP4 refuses it at init with the decay code", std::to_string(initCode));
    }
    check(std::string(sat::Propagator::errorMessage(6)).find("decayed") != std::string::npos && std::string(sat::Propagator::errorMessage(1)).find("eccentricity") != std::string::npos &&
              std::string(sat::Propagator::errorMessage(-1)).find("not initialised") != std::string::npos,
          "error codes have messages");

    // move semantics
    sat::Propagator a;
    a.init(s, error);
    sat::Propagator moved(std::move(a));
    check(moved.valid() && !a.valid(), "a moved-from propagator is empty, the new one is valid");
    sat::Propagator assigned;
    assigned = std::move(moved);
    check(assigned.valid() && assigned.elements().noradId == 25544, "move assignment keeps the elements");
    sat::Propagator::State s1, s2;
    check(assigned.propagateMinutes(123.0, s1) == 0 && p.init(s, error) && p.propagateMinutes(123.0, s2) == 0 && s1.r[0] == s2.r[0] && s1.v[2] == s2.v[2],
          "two propagators built from the same elements give identical results");
}

struct VerSat {
    std::string line1, line2;
    double start = 0, stop = 0, step = 0;
};

std::vector<VerSat> readVerificationTles() {
    std::vector<VerSat> sats;
    std::istringstream in(readFile(std::string(SGP4_FIXTURES_DIR) + "/SGP4-VER.TLE"));
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.rfind("1 ", 0) != 0) {
            continue;
        }
        VerSat v;
        v.line1 = line;
        std::string second;
        std::getline(in, second);
        while (!second.empty() && (second.back() == '\r' || second.back() == '\n')) {
            second.pop_back();
        }
        v.line2 = second.substr(0, 69);
        std::istringstream extra(second.substr(69));
        extra >> v.start >> v.stop >> v.step;
        sats.push_back(v);
    }
    return sats;
}

void testDeepSpaceOrderIndependence() {
    std::printf("[propagator] deep-space results do not depend on the order of the calls\n");
    sat::TleParseOptions lax;
    lax.requireChecksum = false;
    int deep = 0;
    bool agree = true;
    std::string detail;
    for (const VerSat& v : readVerificationTles()) {
        sat::ElementSet s;
        std::string error;
        if (!sat::parseTle("", v.line1, v.line2, s, error, lax) || !s.deepSpace()) {
            continue;
        }
        sat::Propagator first, second;
        if (!first.init(s, error) || !second.init(s, error)) {
            continue;
        }
        ++deep;
        const double times[] = {1000.0, 2000.0, 500.0, 3000.0, -700.0, 1500.0};
        std::vector<sat::Propagator::State> a(6), b(6);
        std::vector<int> ca(6), cb(6);
        for (int i = 0; i < 6; ++i) {
            ca[static_cast<std::size_t>(i)] = first.propagateMinutes(times[i], a[static_cast<std::size_t>(i)]);
        }
        for (int i = 5; i >= 0; --i) {
            cb[static_cast<std::size_t>(i)] = second.propagateMinutes(times[i], b[static_cast<std::size_t>(i)]);
        }
        for (std::size_t i = 0; i < 6; ++i) {
            if (ca[i] != cb[i]) {
                agree = false;
                detail = "error codes differ for satellite " + std::to_string(s.noradId);
            } else if (ca[i] == 0 && (!near(a[i].r[0], b[i].r[0], 1e-9) || !near(a[i].r[1], b[i].r[1], 1e-9) || !near(a[i].r[2], b[i].r[2], 1e-9))) {
                agree = false;
                detail = "positions differ for satellite " + std::to_string(s.noradId) + " at t=" + num(times[i]);
            }
        }
    }
    check(deep >= 8, "the verification set has deep-space satellites to test", std::to_string(deep));
    check(agree, "forward order and reverse order give the same positions (1e-9 km)", detail);
}

// --- Vallado reference vectors ------------------------------------------------------------------------

struct RefRow {
    double v[7];
};

// One block per satellite, IN FILE ORDER: catalogue number 20413 appears twice in the verification set
// (the second time with a much later start time), so the number is not a key.
using RefBlock = std::pair<unsigned long, std::vector<RefRow>>;

std::vector<RefBlock> readReference() {
    std::vector<RefBlock> blocks;
    std::istringstream in(readFile(std::string(SGP4_FIXTURES_DIR) + "/tcppver.out"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("xx") != std::string::npos) {
            blocks.emplace_back(std::stoul(line), std::vector<RefRow>());
            continue;
        }
        std::istringstream fields(line);
        RefRow row{};
        bool ok = true;
        for (double& x : row.v) {
            ok = ok && static_cast<bool>(fields >> x);
        }
        if (ok && !blocks.empty()) {
            blocks.back().second.push_back(row);
        }
    }
    return blocks;
}

void testValladoVectors() {
    std::printf("[vallado] the reference verification set, through this project's parser and propagator\n");
    const std::vector<VerSat> sats = readVerificationTles();
    const std::vector<RefBlock> reference = readReference();
    check(sats.size() == 33, "the verification file holds 33 satellites", std::to_string(sats.size()));
    std::size_t refRows = 0;
    for (const RefBlock& block : reference) {
        refRows += block.second.size();
    }
    check(reference.size() == 33 && refRows == 667, "and the reference output 33 blocks with 667 data rows", std::to_string(refRows));

    // Three satellites (33333, 33334, 33335) carry checksums that do not verify; by default the parser says so.
    sat::TleParseOptions strict;
    sat::TleParseOptions lax;
    lax.requireChecksum = false;
    int strictOk = 0;
    for (const VerSat& v : sats) {
        sat::ElementSet s;
        std::string error;
        strictOk += sat::parseTle("", v.line1, v.line2, s, error, strict) ? 1 : 0;
    }
    check(strictOk == 30, "with checksums required, exactly the 3 documented satellites are refused", std::to_string(strictOk) + " accepted");

    const double tolerance = 2.0e-7; // python-sgp4's tolerance on this file
    std::vector<int> errors;
    std::size_t compared = 0, stale = 0;
    int mismatches = 0;
    int propagated = 0;
    std::string firstMismatch;
    bool idsMatch = true, countsOk = true;
    std::string countDetail;

    for (std::size_t n = 0; n < sats.size() && n < reference.size(); ++n) {
        const VerSat& v = sats[n];
        const std::vector<RefRow>& rows = reference[n].second;
        sat::ElementSet s;
        std::string error;
        if (!sat::parseTle("", v.line1, v.line2, s, error, lax)) {
            check(false, "every verification TLE parses", v.line1 + " : " + error);
            continue;
        }
        idsMatch = idsMatch && s.noradId == reference[n].first;
        sat::Propagator p;
        int initCode = 0;
        if (!p.init(s, error, &initCode)) {
            // SGP4 runs a step at the epoch while initialising: a satellite it cannot propagate at t = 0 fails
            // here, and the reference prints one stale line for it and records the error.
            check(initCode != 0, "an element set SGP4 rejects reports SGP4's error code", std::to_string(s.noradId) + " : " + error);
            errors.push_back(initCode);
            ++stale;
            if (rows.size() != 1) {
                countsOk = false;
                countDetail = "satellite " + std::to_string(s.noradId) + " failed at t=0 but the reference has " + std::to_string(rows.size()) + " rows";
            }
            continue;
        }
        ++propagated;

        // times, in the order the reference was written: t=0 first, then start..stop by step (skipping a
        // duplicate 0), then the stop time itself if the loop stepped past it
        std::vector<double> times;
        times.push_back(0.0);
        double t = v.start;
        while (t <= v.stop) {
            if (!(t == v.start && v.start == 0.0)) {
                times.push_back(t);
            }
            t += v.step;
        }
        if (t - v.stop < v.step - 1e-6) {
            times.push_back(v.stop);
        }

        std::size_t produced = 0;
        for (std::size_t i = 0; i < times.size(); ++i) {
            sat::Propagator::State st;
            const int code = p.propagateMinutes(times[i], st);
            if (code != 0) {
                errors.push_back(code);
                break;
            }
            ++produced;
            if (i >= rows.size()) {
                break;
            }
            const double got[7] = {times[i], st.r[0], st.r[1], st.r[2], st.v[0], st.v[1], st.v[2]};
            for (int k = 0; k < 7; ++k) {
                if (!(std::fabs(got[k] - rows[i].v[k]) < tolerance)) {
                    ++mismatches;
                    if (firstMismatch.empty()) {
                        char buf[200];
                        std::snprintf(buf, sizeof buf, "satellite %u row %zu field %d: got %.9f expected %.9f", s.noradId, i, k, got[k],
                                      rows[i].v[k]);
                        firstMismatch = buf;
                    }
                }
            }
            ++compared;
        }
        if (produced != rows.size()) {
            countsOk = false;
            countDetail = "satellite " + std::to_string(s.noradId) + ": " + std::to_string(produced) + " rows produced, " +
                          std::to_string(rows.size()) + " in the reference";
        }
    }
    check(idsMatch, "the satellites and the reference blocks line up one to one, in the same order");
    check(propagated == 32, "32 satellites propagate (the 33rd is refused by SGP4 at the epoch)", std::to_string(propagated));
    check(mismatches == 0, "every compared field agrees with the reference to 2e-7", firstMismatch + " (" + std::to_string(mismatches) + " mismatches)");
    check(compared + stale == refRows, "every one of the 667 reference rows was either compared or is the stale line", std::to_string(compared) + " + " + std::to_string(stale));
    check(countsOk, "each satellite produced exactly as many rows as the reference (up to its error)", countDetail);
    const std::vector<int> expectedErrors = {1, 1, 6, 6, 4, 3, 6};
    std::string got;
    for (const int e : errors) {
        got += std::to_string(e) + " ";
    }
    check(errors == expectedErrors, "the SGP4 errors are, in order, 1, 1, 6, 6, 4, 3, 6 (python-sgp4's list for the same file)", got);
    check(stale == 1, "exactly one satellite (33334) fails at t = 0, where the reference repeats a stale line", std::to_string(stale));
    std::printf("  compared %zu reference rows (+%zu stale line), %zu SGP4 errors reproduced, 0 tolerated mismatches\n", compared, stale,
                errors.size());
}

// --- catalogue ----------------------------------------------------------------------------------------

void testCatalog() {
    std::printf("[catalogue] the JSON file the app reads\n");
    std::vector<sat::ElementSet> sets;
    sat::parseTleText(readFile(std::string(SAT_FIXTURES_DIR) + "/celestrak_3le_sample.txt"), sets);
    check(sets.size() == 3, "three element sets to store");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "solsim_sat_tests";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "elements.json").string();

    sat::CatalogMeta meta;
    meta.createdUtc = "2026-10-03T00:00:00Z";
    meta.source = "test";
    meta.groups = {"stations", "visual"};
    std::string error;
    check(sat::saveCatalog(path, sets, meta, error), "saved", error);
    check(!std::filesystem::exists(path + ".tmp"), "no temporary file is left behind");

    std::vector<sat::ElementSet> loaded;
    sat::CatalogMeta loadedMeta;
    check(sat::loadCatalog(path, loaded, loadedMeta, error), "loaded", error);
    check(loadedMeta.createdUtc == meta.createdUtc && loadedMeta.source == "test" && loadedMeta.groups == meta.groups &&
              loadedMeta.schemaVersion == sat::kCatalogSchemaVersion,
          "the metadata comes back");
    bool exact = loaded.size() == sets.size();
    for (std::size_t i = 0; i < loaded.size() && exact; ++i) {
        const sat::ElementSet& a = sets[i];
        const sat::ElementSet& b = loaded[i];
        exact = a.name == b.name && a.objectId == b.objectId && a.noradId == b.noradId && a.classification == b.classification &&
                a.epochJdWhole == b.epochJdWhole && a.epochJdFraction == b.epochJdFraction &&
                a.meanMotionRevPerDay == b.meanMotionRevPerDay && a.eccentricity == b.eccentricity &&
                a.inclinationDeg == b.inclinationDeg && a.raanDeg == b.raanDeg && a.argPerigeeDeg == b.argPerigeeDeg &&
                a.meanAnomalyDeg == b.meanAnomalyDeg && a.bstar == b.bstar && a.meanMotionDot == b.meanMotionDot &&
                a.meanMotionDdot == b.meanMotionDdot && a.ephemerisType == b.ephemerisType && a.elementSetNo == b.elementSetNo &&
                a.revAtEpoch == b.revAtEpoch;
    }
    check(exact, "every field of every set round-trips bit-exactly");
    const std::string text = readFile(path);
    check(text.find("\"schema\": 1") != std::string::npos && text.find("2019-12-09T16:38:29.363424") != std::string::npos,
          "the file carries its schema version and a human-readable epoch");

    // the OMM-style records re-parse through the OMM parser
    std::vector<sat::ElementSet> viaOmm;
    const sat::OmmParseResult omm = sat::parseOmmJson(sat::toOmmJson(sets), viaOmm);
    check(omm.ok && viaOmm.size() == 3 && viaOmm[0].noradId == 25544 && viaOmm[0].epochJdWhole == sets[0].epochJdWhole &&
              near(viaOmm[0].epochJdFraction, sets[0].epochJdFraction, 1e-11),
          "the stored records are valid OMM, readable by the OMM parser");

    // overwriting replaces the old file completely
    std::vector<sat::ElementSet> one(sets.begin(), sets.begin() + 1);
    check(sat::saveCatalog(path, one, meta, error), "saving over an existing file works", error);
    loaded.clear();
    check(sat::loadCatalog(path, loaded, loadedMeta, error) && loaded.size() == 1, "and holds only the new content");

    // refusal cases
    check(!sat::loadCatalog((dir / "missing.json").string(), loaded, loadedMeta, error) && error.find("cannot open") != std::string::npos,
          "a missing file is an error", error);
    {
        std::ofstream f(dir / "truncated.json", std::ios::binary);
        f << text.substr(0, text.size() / 2);
    }
    check(!sat::loadCatalog((dir / "truncated.json").string(), loaded, loadedMeta, error), "a truncated file is refused", error);
    {
        std::string future = text;
        future.replace(future.find("\"schema\": 1"), 11, "\"schema\": 2");
        std::ofstream f(dir / "future.json", std::ios::binary);
        f << future;
    }
    check(!sat::loadCatalog((dir / "future.json").string(), loaded, loadedMeta, error) && error.find("schema 2") != std::string::npos &&
              error.find("sat_ingest") != std::string::npos,
          "a different schema version says to run sat_ingest again", error);
    {
        std::string poisoned = text;
        const std::string key = "\"ECCENTRICITY\": ";
        const std::size_t at = poisoned.find(key) + key.size();
        const std::size_t end = poisoned.find_first_of(",\n", at);
        poisoned.replace(at, end - at, "1.5");
        std::ofstream f(dir / "poisoned.json", std::ios::binary);
        f << poisoned;
    }
    check(!sat::loadCatalog((dir / "poisoned.json").string(), loaded, loadedMeta, error) && error.find("not a valid element set") != std::string::npos,
          "a record that is not a valid element set is refused, not loaded", error);
    {
        // hand-edited metadata of the wrong type must not throw (json::value() would), it is just ignored
        std::string odd = text;
        const std::size_t at = odd.find("\"created_utc\": ");
        const std::size_t end = odd.find_first_of(",\n", at);
        odd.replace(at, end - at, "\"created_utc\": 12345");
        const std::size_t srcAt = odd.find("\"source\": ");
        const std::size_t srcEnd = odd.find_first_of(",\n", srcAt);
        odd.replace(srcAt, srcEnd - srcAt, "\"source\": [1, 2]");
        std::ofstream f(dir / "odd_meta.json", std::ios::binary);
        f << odd;
    }
    std::vector<sat::ElementSet> oddSets;
    sat::CatalogMeta oddMeta;
    bool threw = false;
    bool oddLoaded = false;
    try {
        oddLoaded = sat::loadCatalog((dir / "odd_meta.json").string(), oddSets, oddMeta, error);
    } catch (...) {
        threw = true;
    }
    check(!threw && oddLoaded && oddMeta.createdUtc.empty() && oddMeta.source.empty() && oddSets.size() == 3,
          "metadata of the wrong type is ignored; loading never throws", error);
    {
        std::string hostile = text;
        const std::string key = "\"REV_AT_EPOCH\": ";
        const std::size_t at = hostile.find(key) + key.size();
        const std::size_t end = hostile.find_first_of(",\n}", at);
        hostile.replace(at, end - at, "1e30");
        std::ofstream f(dir / "hostile.json", std::ios::binary);
        f << hostile;
    }
    check(!sat::loadCatalog((dir / "hostile.json").string(), loaded, loadedMeta, error) && error.find("not a valid element set") != std::string::npos,
          "a 1e30 revolution count in the file is refused, not cast", error);
    check(!sat::saveCatalog((dir / "no_such_dir" / "x.json").string(), sets, meta, error) && !error.empty(), "an unwritable path is an error");
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    testNumbers();
    testEpoch();
    testCatalogNumbers();
    testTle();
    testTleText();
    testOmm();
    testPropagatorBasics();
    testDeepSpaceOrderIndependence();
    testValladoVectors();
    testCatalog();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
