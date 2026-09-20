#include "neo/sim/SwarmLegend.h"

#include <cmath>

namespace neo {

const char* toString(SwarmLegend legend) {
    switch (legend) {
    case SwarmLegend::Distance: return "DISTANCE FROM EARTH";
    case SwarmLegend::Pha: return "PHA";
    case SwarmLegend::Diameter: return "DIAMETER";
    case SwarmLegend::Approach: return "TIME TO APPROACH";
    }
    return "?";
}

const char* legendNearText(SwarmLegend legend) {
    switch (legend) {
    case SwarmLegend::Distance: return "0.01 AU";
    case SwarmLegend::Pha: return "PHA";
    case SwarmLegend::Diameter: return "10 KM";
    case SwarmLegend::Approach: return "NOW";
    }
    return "";
}

const char* legendFarText(SwarmLegend legend) {
    switch (legend) {
    case SwarmLegend::Distance: return "5 AU";
    case SwarmLegend::Pha: return "OTHER";
    case SwarmLegend::Diameter: return "10 M";
    case SwarmLegend::Approach: return "1 YR+";
    }
    return "";
}

SwarmAttr makeSwarmAttr(const Asteroid& asteroid) {
    SwarmAttr a;
    if (const std::optional<double> d = asteroid.physical.bestDiameterKm()) {
        if (*d > 0.0) {
            a.logDiameterKm = static_cast<float>(std::log10(*d));
        }
    }
    const Flag pha = asteroid.classification.isPHA;
    a.pha = pha && *pha ? 1.0f : 0.0f;
    return a;
}

float daysToNextApproach(const Dataset& dataset, std::uint32_t record, double jdNow) {
    if (record >= dataset.records().size()) {
        return kNoApproach;
    }
    for (const CloseApproach& a : dataset.approachesOf(record)) { // chronological
        if (a.jdTdb > jdNow) {
            return static_cast<float>(a.jdTdb - jdNow);
        }
    }
    return kNoApproach;
}

} // namespace neo
