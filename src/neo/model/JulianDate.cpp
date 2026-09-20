#include "neo/model/JulianDate.h"

#include "sim/SimClock.h"

#include <cmath>
#include <cstdio>

namespace neo {

namespace {

// Days elapsed at the start of each month in a non-leap year.
constexpr int kMonthLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Meeus, "Astronomical Algorithms", ch. 7: calendar date -> Julian Day.
// Gregorian from 1582-10-15, proleptic Julian before it, matching the inverse
// already used by sim::calendarFromJulianDate.
double julianDayFromGregorian(int year, int month, int day) {
    int y = year;
    int m = month;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    const bool gregorian = (year > 1582) || (year == 1582 && (month > 10 || (month == 10 && day >= 15)));
    int b = 0;
    if (gregorian) {
        const int a = static_cast<int>(std::floor(y / 100.0));
        b = 2 - a + static_cast<int>(std::floor(a / 4.0));
    }
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + day + b - 1524.5;
}

} // namespace

std::string formatJulianDate(double jdTdb) {
    const sim::CalendarDate d = sim::calendarFromJulianDate(jdTdb);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", d.year, d.month, d.day, d.hour, d.minute);
    return buf;
}

std::string formatJulianDay(double jdTdb) {
    const sim::CalendarDate d = sim::calendarFromJulianDate(jdTdb);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", d.year, d.month, d.day);
    return buf;
}

bool julianDateFromIsoDate(const std::string& iso, double& jdOut) {
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < iso.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (iso[i] < '0' || iso[i] > '9') {
            return false;
        }
    }
    const int year = std::stoi(iso.substr(0, 4));
    const int month = std::stoi(iso.substr(5, 2));
    const int day = std::stoi(iso.substr(8, 2));
    if (month < 1 || month > 12 || day < 1) {
        return false;
    }
    int maxDay = kMonthLengths[month - 1];
    if (month == 2 && isLeapYear(year)) {
        maxDay = 29;
    }
    if (day > maxDay) {
        return false;
    }
    jdOut = julianDayFromGregorian(year, month, day);
    return true;
}

} // namespace neo
