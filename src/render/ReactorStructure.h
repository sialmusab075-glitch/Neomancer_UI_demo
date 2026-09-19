#pragma once

#include "render/EclipticGrid.h"
#include "render/Starfield.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace render {

// "Reactor display" reference structure on the ecliptic plane (render y = 0):
// concentric range rings at each planet's semi-major axis (already mapped to
// render units) plus faint intermediate rings, outward tick marks every 10 deg
// (longer every 30 deg), 12 dashed radial spokes, and a sparse, seeded scatter of
// line fragments and dot markers near the rings. Pure geometry: built once per
// scale mode and drawn from static buffers. Vertex alpha carries per-element
// weight and the outer-edge fade; colours come from the theme at draw time.

struct RingLabel {
    glm::vec3 pos;  // world render units, just outside the ring
    char text[16];  // e.g. "1.00 AU"
};

struct StructureParams {
    std::vector<float>  ringRadii;  // planet rings, render units, ascending
    std::vector<double> ringAU;     // the same rings in AU (for the labels)
    float innerRadius = 1.0f;       // spokes start here (just outside the Sun)
    float outerRadius = 30.0f;      // outermost ring; everything fades out towards it
    int   ringSegments = 256;
    std::uint32_t seed = 0x5EEDu;
};

struct StructureGeometry {
    std::vector<LineVertex>  majorRings; // GL_LINES
    std::vector<LineVertex>  minorRings; // GL_LINES
    std::vector<LineVertex>  ticks;      // GL_LINES, pointing outward
    std::vector<LineVertex>  spokes;     // GL_LINES, dashed
    std::vector<LineVertex>  fragments;  // GL_LINES
    std::vector<PointVertex> markers;    // GL_POINTS
    std::vector<RingLabel>   labels;     // one per planet ring
};

constexpr int kSpokeCount = 12;
constexpr int kFragmentCount = 40;
constexpr int kMarkerCount = 60;

StructureGeometry generateStructure(const StructureParams& p);

// Unit meshes for the Sun "core" (drawn billboarded, scaled per frame):
// a circle of radius 1 in the XY plane, and a crown of radial ticks from r = 1
// outward (every 5 deg; every 30 deg a longer one).
std::vector<LineVertex> generateUnitCircle(int segments);
std::vector<LineVertex> generateCrown(int ticks);

} // namespace render
