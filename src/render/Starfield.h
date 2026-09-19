#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace render {

// One point sprite: position (or direction), size in pixels at DPI 1.0, and
// colour data. For stars rgb holds a temperature (0 cool .. 1 warm) and for the
// Sun a heat (0 falloff .. 1 white-hot) in every channel; the theme maps these
// to colours at draw time. Alpha is the brightness.
struct PointVertex {
    glm::vec3 pos;
    float     size;
    glm::vec4 color;
};

// ~`count` stars on the unit sphere (directions), deterministic for a given
// seed. Most are faint; a share is concentrated in a tilted band so the sky
// has a Milky Way-like structure. Rendered at infinity (see points.vert).
std::vector<PointVertex> generateStarfield(int count, std::uint32_t seed);

// `count` points evenly spread over the unit sphere (Fibonacci lattice): the
// particle shell drawn around the Sun.
std::vector<PointVertex> generateSphereShell(int count, std::uint32_t seed);

// The Sun as a particle volume inside the unit sphere: ~55% on a jittered
// shell (gives the outline), the rest inside with density biased toward the
// core. Random size and brightness per particle; inner particles are a little
// hotter (whiter). Rendered additively by points.vert/frag.
std::vector<PointVertex> generateSunParticles(int count, std::uint32_t seed);

} // namespace render
