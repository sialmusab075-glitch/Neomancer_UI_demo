#include "sim/SimClock.h"

#include "sim/Constants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sim {

namespace {
const double kSliderBase = SimClock::kMaxScale_daysPerSec + 1.0; // 366
}

SimClock::SimClock(double epoch_days) : epoch_days_(epoch_days), t_days_(epoch_days) {}

void SimClock::update(double realDt_s) {
    if (paused_ || !(realDt_s > 0.0)) {
        return;
    }
    const double maxDt = kMaxRealDt_s;
    t_days_ += scale_ * std::min(realDt_s, maxDt);
}

void SimClock::setScale(double daysPerSec) {
    const double maxScale = kMaxScale_daysPerSec;
    scale_ = std::max(-maxScale, std::min(maxScale, daysPerSec));
}

bool SimClock::step(double days) {
    if (!paused_) {
        return false;
    }
    t_days_ += days;
    return true;
}

double SimClock::sliderToScale(double s) {
    s = std::max(-1.0, std::min(1.0, s));
    const double mag = std::pow(kSliderBase, std::fabs(s)) - 1.0;
    return s < 0.0 ? -mag : mag;
}

double SimClock::scaleToSlider(double daysPerSec) {
    const double maxScale = kMaxScale_daysPerSec;
    const double mag = std::min(std::fabs(daysPerSec), maxScale);
    const double s = std::log(mag + 1.0) / std::log(kSliderBase);
    return daysPerSec < 0.0 ? -s : s;
}

std::string formatElapsed(double elapsedDays) {
    const bool negative = elapsedDays < 0.0;
    // Whole minutes, rounded down in magnitude.
    // int holds +/- 1.49 million days of minutes (~4000 years), far beyond the
    // reachable range at 365 d/s in any realistic session; clamp to be safe.
    const double minutesD = std::min(std::floor(std::fabs(elapsedDays) * 1440.0 + 1e-6), 2.0e9);
    const int totalMinutes = static_cast<int>(minutesD);
    const int days    = totalMinutes / 1440;
    const int hours   = (totalMinutes / 60) % 24;
    const int minutes = totalMinutes % 60;

    char buf[48];
    std::snprintf(buf, sizeof buf, "T%c%04d:%02d:%02d",
                  negative ? '-' : '+', days, hours, minutes);
    return buf;
}

CalendarDate calendarFromJulianDate(double jd) {
    // Round to the minute first so 23:59:59.9999 does not print as 23:59 of the
    // wrong day after floating-point noise.
    const double jdMinutes = std::floor((jd + 0.5) * 1440.0 + 1e-6);
    const double Z = std::floor(jdMinutes / 1440.0);
    const int minuteOfDay = static_cast<int>(jdMinutes - Z * 1440.0);

    double A = Z;
    if (Z >= 2299161.0) {
        const double alpha = std::floor((Z - 1867216.25) / 36524.25);
        A = Z + 1.0 + alpha - std::floor(alpha / 4.0);
    }
    const double B = A + 1524.0;
    const double C = std::floor((B - 122.1) / 365.25);
    const double D = std::floor(365.25 * C);
    const double E = std::floor((B - D) / 30.6001);

    CalendarDate out;
    out.day    = static_cast<int>(B - D - std::floor(30.6001 * E));
    out.month  = static_cast<int>(E < 14.0 ? E - 1.0 : E - 13.0);
    out.year   = static_cast<int>(out.month > 2 ? C - 4716.0 : C - 4715.0);
    out.hour   = minuteOfDay / 60;
    out.minute = minuteOfDay % 60;
    return out;
}

std::string formatCalendar(double t_days) {
    const CalendarDate d = calendarFromJulianDate(kJ2000_JD + t_days);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d",
                  d.year, d.month, d.day, d.hour, d.minute);
    return buf;
}

} // namespace sim
