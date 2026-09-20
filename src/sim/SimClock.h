#pragma once

#include <string>

namespace sim {

// Simulation time, in days since J2000.0, driven by real (wall-clock) time.
//
//   sim_dt = time_scale [days/s] * real_dt [s]
//
// A negative time scale runs time backwards.
class SimClock {
public:
    static constexpr double kMaxScale_daysPerSec = 365.0;
    static constexpr double kDefaultScale_daysPerSec = 10.0;
    // Real frame times longer than this are truncated so a debugger pause or a
    // window drag does not make the simulation jump.
    static constexpr double kMaxRealDt_s = 0.1;

    explicit SimClock(double epoch_days = 0.0);

    // Advances time by scale * realDt when running.
    void update(double realDt_s);

    double timeDays() const { return t_days_; }
    double epochDays() const { return epoch_days_; }
    double elapsedDays() const { return t_days_ - epoch_days_; }

    double scale() const { return scale_; }
    void setScale(double daysPerSec);   // clamped to +/- kMaxScale
    void reverse() { scale_ = -scale_; }

    bool paused() const { return paused_; }
    void setPaused(bool p) { paused_ = p; }
    void togglePause() { paused_ = !paused_; }

    void resetToEpoch() { t_days_ = epoch_days_; }
    // Sets the time outright (Earth view: a jump to a flyby's closest approach).
    void jumpTo(double t_days) { t_days_ = t_days; }
    // Steps by +/- days. Only acts while paused; returns whether it stepped.
    bool step(double days);

    // Log-feel slider mapping. Slider s in [-1, 1] maps to a scale in
    // [-365, 365] days/s with |scale| = 366^|s| - 1, so 0 -> 0 and 1 -> 365,
    // with fine control near zero.
    static double sliderToScale(double s);
    static double scaleToSlider(double daysPerSec);

private:
    double epoch_days_;
    double t_days_;
    double scale_ = kDefaultScale_daysPerSec;
    bool   paused_ = false;
};

// "T+DDDD:HH:MM" (or "T-..." before the epoch). Days are at least 4 digits.
std::string formatElapsed(double elapsedDays);

struct CalendarDate {
    int year   = 2000;
    int month  = 1;
    int day    = 1;
    int hour   = 12;
    int minute = 0;
};

// Gregorian calendar date (proleptic Julian before 1582-10-15) for a Julian Date.
// Meeus, "Astronomical Algorithms", ch. 7. Rounded down to the minute.
CalendarDate calendarFromJulianDate(double jd);

// "YYYY-MM-DD HH:MM" for simulation time t (days since J2000.0).
// The TT-UTC offset (~1 min) is ignored.
std::string formatCalendar(double t_days);

} // namespace sim
