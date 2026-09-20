#include "neo/model/Asteroid.h"

#include <cmath>

namespace neo {

ObjectKind objectKindFromSbdb(const std::string& kind) {
    if (kind.empty()) {
        return ObjectKind::Unknown;
    }
    switch (kind[0]) {
    case 'a': return ObjectKind::Asteroid;
    case 'c': return ObjectKind::Comet;
    default:  return ObjectKind::Unknown;
    }
}

const char* toString(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::Asteroid: return "asteroid";
    case ObjectKind::Comet:    return "comet";
    case ObjectKind::Unknown:  break;
    }
    return "unknown";
}

double diameterFromMagnitude(double absoluteMagnitudeH, double albedo) {
    return 1329.0 / std::sqrt(albedo) * std::pow(10.0, -0.2 * absoluteMagnitudeH);
}

std::optional<double> PhysicalProperties::estimatedDiameterKm() const {
    if (!absoluteMagnitudeH) {
        return std::nullopt;
    }
    // A measured albedo is used when available; otherwise the documented default.
    const double p = albedo && *albedo > 0.0 ? *albedo : kDefaultAlbedo;
    return diameterFromMagnitude(*absoluteMagnitudeH, p);
}

std::optional<double> PhysicalProperties::bestDiameterKm() const {
    if (diameterKm) {
        return diameterKm;
    }
    return estimatedDiameterKm();
}

} // namespace neo
