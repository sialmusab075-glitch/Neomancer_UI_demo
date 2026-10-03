#include "sat/model/Epoch.h"

#include "sat/parse/detail/Number.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace sat {

namespace {

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeap(y) ? 29 : days[m - 1];
}

} // namespace

bool validCivilDate(int year, int month, int day) {
    return year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month);
}

double julianDay0h(int year, int month, int day) {
    int y = year;
    int m = month;
    if (m <= 2) {
        --y;
        m += 12;
    }
    const int a = y / 100;
    const int b = 2 - a + a / 4;
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + day + b - 1524.5;
}

bool epochFromYearDay(int year, double dayOfYear, double& whole, double& fraction) {
    const int length = isLeap(year) ? 366 : 365;
    if (year < 1 || !(dayOfYear >= 1.0) || !(dayOfYear < static_cast<double>(length) + 1.0)) {
        return false;
    }
    const double wholeDays = std::floor(dayOfYear);
    whole = julianDay0h(year, 1, 1) + wholeDays - 1.0;
    fraction = dayOfYear - wholeDays;
    return true;
}

bool epochFromIso(const std::string& text, double& whole, double& fraction, std::string& error) {
    // YYYY-MM-DD[T ]HH:MM:SS[.f...][Z]
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != ' ') || text[13] != ':' ||
        text[16] != ':') {
        error = "epoch is not YYYY-MM-DDTHH:MM:SS[.ffffff]: '" + text + "'";
        return false;
    }
    unsigned long year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!detail::parseUint(text.data(), 4, year) || !detail::parseUint(text.data() + 5, 2, month) ||
        !detail::parseUint(text.data() + 8, 2, day) || !detail::parseUint(text.data() + 11, 2, hour) ||
        !detail::parseUint(text.data() + 14, 2, minute) || !detail::parseUint(text.data() + 17, 2, second)) {
        error = "epoch has non-numeric parts: '" + text + "'";
        return false;
    }
    double subsecond = 0.0;
    std::size_t end = 19;
    if (end < text.size() && text[end] == '.') {
        std::size_t digits = end + 1;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
            ++digits;
        }
        if (digits == end + 1 || !detail::parseDecimal(text.data() + end, digits - end, subsecond)) {
            error = "epoch has a malformed fractional second: '" + text + "'";
            return false;
        }
        end = digits;
    }
    if (end < text.size() && text[end] == 'Z') {
        ++end;
    }
    if (end != text.size()) {
        error = "epoch has trailing characters: '" + text + "'";
        return false;
    }
    if (!validCivilDate(static_cast<int>(year), static_cast<int>(month), static_cast<int>(day)) || hour > 23 || minute > 59 ||
        second > 59) {
        error = "epoch is not a valid date and time: '" + text + "'";
        return false;
    }
    whole = julianDay0h(static_cast<int>(year), static_cast<int>(month), static_cast<int>(day));
    fraction = (static_cast<double>(hour) * 3600.0 + static_cast<double>(minute) * 60.0 + static_cast<double>(second) + subsecond) / 86400.0;
    return true;
}

std::string epochToIso(double whole, double fraction) {
    // microseconds since 00:00 of the `whole` day, carried into the day count
    double microseconds = std::round(fraction * 86400.0e6);
    double days = std::floor(microseconds / 86400.0e6);
    microseconds -= days * 86400.0e6;
    const double dayNumber = std::floor(whole + 0.5) + days; // integer Julian Day Number of the date

    // Julian Day Number -> Gregorian date (Meeus)
    const double alpha = std::floor((dayNumber - 1867216.25) / 36524.25);
    const double a = dayNumber + 1.0 + alpha - std::floor(alpha / 4.0);
    const double b = a + 1524.0;
    const double c = std::floor((b - 122.1) / 365.25);
    const double d = std::floor(365.25 * c);
    const double e = std::floor((b - d) / 30.6001);
    const int day = static_cast<int>(b - d - std::floor(30.6001 * e));
    const int month = static_cast<int>(e < 14.0 ? e - 1.0 : e - 13.0);
    const int year = static_cast<int>(month > 2 ? c - 4716.0 : c - 4715.0);

    const long long us = static_cast<long long>(microseconds);
    const int hour = static_cast<int>(us / 3600000000LL);
    const int minute = static_cast<int>(us / 60000000LL % 60);
    const int second = static_cast<int>(us / 1000000LL % 60);
    const int micro = static_cast<int>(us % 1000000LL);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%06d", year, month, day, hour, minute, second, micro);
    return buf;
}

void utcJdNow(double& whole, double& fraction) {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    const double seconds = std::chrono::duration<double>(since).count();
    const double days = std::floor(seconds / 86400.0);
    whole = 2440587.5 + days; // JD of 1970-01-01 00:00 UT plus whole days
    fraction = seconds / 86400.0 - days;
}

} // namespace sat
