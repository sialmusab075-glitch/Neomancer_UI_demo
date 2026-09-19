#include "sim/OrbitProbe.h"

#include "sim/Constants.h"

#include <cmath>

namespace sim {

void OrbitProbe::reset() { *this = OrbitProbe{}; }

void OrbitProbe::observe(double t_days, const Vec3d& pos) {
    const double lon = std::atan2(pos.y, pos.x); // (-pi, pi]

    if (havePrev_ && t_days != prevT_) {
        const bool signChange = (prevLon_ < 0.0) != (lon < 0.0);
        // A jump of more than pi is the +/-pi seam, not a crossing of 0.
        const bool nearZero = std::fabs(lon - prevLon_) < kPi;
        if (signChange && nearZero) {
            const double f = prevLon_ / (prevLon_ - lon);
            const double tc = prevT_ + f * (t_days - prevT_);
            if (crossings_ > 0) {
                lastPeriod_ = std::fabs(tc - lastCrossing_);
            }
            lastCrossing_ = tc;
            ++crossings_;
        }
    }
    havePrev_ = true;
    prevT_ = t_days;
    prevLon_ = lon;
}

} // namespace sim
