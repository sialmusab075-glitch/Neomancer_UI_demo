#pragma once

#include <glm/vec3.hpp>

#include <vector>

namespace render {

// Vertex of a line list with a per-vertex opacity multiplier.
struct LineVertex {
    glm::vec3 pos;
    float     alpha;
};

// Polar/radial reference grid on the ecliptic plane. The generator emits a flat
// grid (y = 0); line.vert bends it into a "gravity well" under the Sun,
//
//   y(r) = yOffset - wellK * ( 1/sqrt(r^2 + soft^2) - 1/sqrt(R^2 + soft^2) )
//
// (R = rim radius, so the rim sits exactly at yOffset), plus a small moving
// Gaussian dip under Jupiter. Purely decorative: it stays below the orbital
// plane so orbits are never hidden by it.
//
// Rings every `ringSpacing` units: every 2nd is a major ring, every 8th a
// bright one; minor rings are faint so the well's curvature reads smoothly.
// Opacity fades out towards the rim (no hard edge) and near the centre.
struct GridParams {
    float ringSpacing   = 1.25f;
    int   ringCount     = 22;
    int   spokes        = 24;
    int   ringSegments  = 384;
    int   spokeSegments = 160;
    float wellK         = 8.4f;
    float wellSoft      = 2.2f;
    float yOffset       = -0.03f;
    // Jupiter's dip (applied in the shader only).
    float dipK          = 0.5f;
    float dipWidth      = 0.9f;

    float rim() const { return ringSpacing * static_cast<float>(ringCount); }
};

// The Sun-well height at radius r (the same formula line.vert uses).
float gridHeight(const GridParams& p, float r);

// GL_LINES list (pairs of vertices), flat (y = 0).
std::vector<LineVertex> generateEclipticGrid(const GridParams& p);

} // namespace render
