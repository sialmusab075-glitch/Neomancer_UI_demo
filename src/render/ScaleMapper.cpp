#include "render/ScaleMapper.h"

#include "sim/Constants.h"

#include <algorithm>
#include <cmath>

namespace render {

namespace {
constexpr double kEarthRadius_km = 6371.0;
constexpr double kSystemExtent_AU = 32.0; // beyond Neptune's aphelion (30.3 AU)
}

double ScaleMapper::distanceToRender(double d_AU) const {
    const double d = std::max(0.0, d_AU);
    if (mode == ScaleMode::True) {
        return d * trueUnitsPerAU;
    }
    return k * std::log10(1.0 + d * c);
}

glm::dvec3 ScaleMapper::toRender(const sim::Vec3d& ecliptic_AU) const {
    if (mode == ScaleMode::True) {
        return eclipticToRenderAxes(ecliptic_AU) * trueUnitsPerAU;
    }
    const double d = sim::length(ecliptic_AU);
    if (d <= 0.0) {
        return glm::dvec3(0.0);
    }
    const glm::dvec3 dir = eclipticToRenderAxes(ecliptic_AU) / d;
    return dir * distanceToRender(d);
}

float ScaleMapper::bodyRadius(const sim::BodyData& body) const {
    if (mode == ScaleMode::True) {
        return static_cast<float>(body.radius_km / sim::kAU_km * trueUnitsPerAU);
    }
    const double ratio = body.radius_km / kEarthRadius_km;
    const float r = planetBase * static_cast<float>(std::pow(ratio, static_cast<double>(planetExponent)));
    if (body.kind == sim::BodyKind::Star) {
        return std::min(r, sunRadiusCap);
    }
    return r;
}

float ScaleMapper::systemExtent() const { return static_cast<float>(distanceToRender(kSystemExtent_AU)); }

} // namespace render
