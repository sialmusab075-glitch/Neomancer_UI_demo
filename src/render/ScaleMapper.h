#pragma once

#include "sim/Body.h"
#include "sim/Vec3.h"

#include <glm/vec3.hpp>

namespace render {

enum class ScaleMode {
    Compressed, // log-compressed distances, exaggerated radii
    True,       // real ratios; bodies drawn as minimum-size markers
};

// Maps physical quantities (AU, km) to render units. Physics never sees this;
// only drawing does.
//
// Axes: the simulation uses the ecliptic frame (x toward the vernal equinox,
// z toward ecliptic north). The renderer is y-up, so ecliptic (x, y, z) maps to
// render (x, z, -y), a proper rotation, so handedness and orbit direction are kept.
//
// Compressed: distance_render = k * log10(1 + d_AU * c), applied radially, so
//             directions are exact and only distances are compressed.
//             radius_render   = planetBase * (R / R_earth)^planetExponent, Sun capped.
// True:       distance_render = d_AU * trueUnitsPerAU,
//             radius_render   = R_km / AU_km * trueUnitsPerAU (real ratio).
// In both modes the renderer additionally enforces a minimum on-screen size.
class ScaleMapper {
public:
    ScaleMode mode = ScaleMode::Compressed;

    double k = 10.0;               // compressed: render units per decade
    double c = 10.0;               // compressed: AU multiplier inside the log
    float planetBase = 0.25f;      // compressed: Earth's render radius
    float planetExponent = 0.45f;  // compressed: radius exaggeration curve
    float sunRadiusCap = 1.6f;     // compressed: Sun radius cap

    double trueUnitsPerAU = 10.0;  // true scale: render units per AU

    glm::dvec3 toRender(const sim::Vec3d& ecliptic_AU) const;
    double distanceToRender(double d_AU) const;

    // Base render radius before the on-screen minimum is applied.
    float bodyRadius(const sim::BodyData& body) const;

    // Extent of the planetary system in render units (Neptune's aphelion plus margin).
    float systemExtent() const;

    static glm::dvec3 eclipticToRenderAxes(const sim::Vec3d& v) { return glm::dvec3(v.x, v.z, -v.y); }
};

} // namespace render
