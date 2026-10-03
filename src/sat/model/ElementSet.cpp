#include "sat/model/ElementSet.h"

#include <cmath>

namespace sat {

bool validateElementSet(const ElementSet& s, std::string& reason) {
    auto bad = [&reason](const char* text) {
        reason = text;
        return false;
    };
    if (!std::isfinite(s.epochJdWhole) || !std::isfinite(s.epochJdFraction)) {
        return bad("epoch is not a number");
    }
    if (!(s.meanMotionRevPerDay > 0.0) || !std::isfinite(s.meanMotionRevPerDay)) {
        return bad("mean motion must be positive");
    }
    if (!(s.eccentricity >= 0.0 && s.eccentricity < 1.0)) {
        return bad("eccentricity must be in [0, 1)");
    }
    if (!(s.inclinationDeg >= 0.0 && s.inclinationDeg <= 180.0)) {
        return bad("inclination must be in [0, 180] degrees");
    }
    if (!(s.raanDeg >= 0.0 && s.raanDeg <= 360.0)) {
        return bad("RAAN must be in [0, 360] degrees");
    }
    if (!(s.argPerigeeDeg >= 0.0 && s.argPerigeeDeg <= 360.0)) {
        return bad("argument of perigee must be in [0, 360] degrees");
    }
    if (!(s.meanAnomalyDeg >= 0.0 && s.meanAnomalyDeg <= 360.0)) {
        return bad("mean anomaly must be in [0, 360] degrees");
    }
    if (!std::isfinite(s.bstar) || !std::isfinite(s.meanMotionDot) || !std::isfinite(s.meanMotionDdot)) {
        return bad("B*, mean motion derivatives must be numbers");
    }
    return true;
}

} // namespace sat
