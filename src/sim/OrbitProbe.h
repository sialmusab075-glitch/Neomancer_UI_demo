#pragma once

#include "sim/Vec3.h"

namespace sim {

// Measures a body's orbital period geometrically: records each time its
// ecliptic longitude atan2(y, x) crosses 0 (the J2000 vernal-equinox direction)
// and reports the sim-time between consecutive crossings.
//
// This is independent of the Kepler bookkeeping (mean anomaly), so it checks
// the whole chain clock -> propagator -> position. Works with negative time
// scales and single steps. Crossing times are linearly interpolated between
// frames.
class OrbitProbe {
public:
    void reset();
    // Feed the body's parent-relative position once per frame.
    void observe(double t_days, const Vec3d& pos);

    int    crossings() const { return crossings_; }
    bool   hasPeriod() const { return crossings_ >= 2; }
    double lastPeriodDays() const { return lastPeriod_; }
    double lastCrossingDays() const { return lastCrossing_; }

private:
    bool   havePrev_     = false;
    double prevT_        = 0.0;
    double prevLon_      = 0.0;
    int    crossings_    = 0;
    double lastCrossing_ = 0.0;
    double lastPeriod_   = 0.0;
};

} // namespace sim
