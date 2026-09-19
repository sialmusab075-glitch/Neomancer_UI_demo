#pragma once

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace sim {
class SolarSystem;
}

namespace render {

class ScaleMapper;
class Shader;

struct OrbitDrawParams {
    glm::mat4 viewProj{1.0f};
    glm::vec3 target{0.0f};      // camera target, world render units
    glm::vec3 eye{0.0f};         // eye relative to the target
    glm::vec2 viewportPx{1.0f};  // render-target size
    float     dpiScale = 1.0f;
    float     fadeNear = 0.0f;   // depth fade (see orbit.vert)
    float     fadeFar = 1.0f;
    float     fadeMin = 1.0f;
    float     timeDir = 1.0f;    // +1 time forward, -1 reverse (flips the trail)
    int       selectedBody = -1; // index into SolarSystem::bodies()
    glm::vec3 selectColor{1.0f};   // selected ring (theme accent)
    glm::vec3 plainColor{0.5f};    // unselected rings (theme token)
    float     plainAlpha = 0.8f;
};

// Orbit rings: a 512-point line loop per orbiting body, precomputed from its
// elements (sampled uniformly in eccentric anomaly, so points are denser near
// perihelion) and mapped through the ScaleMapper. Each vertex also stores its
// true anomaly so the shader can fade a trail behind the body. All rings share
// one VBO; rebuild whenever the scale mode changes.
//
// Lines are widened in screen space by orbit.geom (core-profile glLineWidth > 1
// is unreliable). Drawn additively: unselected rings thin and neutral, the
// selected ring as a wide soft halo plus a thin bright (HDR) core.
class OrbitRenderer {
public:
    static constexpr int kPointsPerOrbit = 512;

    OrbitRenderer() = default;
    ~OrbitRenderer();
    OrbitRenderer(const OrbitRenderer&) = delete;
    OrbitRenderer& operator=(const OrbitRenderer&) = delete;

    void build(const sim::SolarSystem& system, const ScaleMapper& mapper);
    void destroy();

    // Expects additive blending (GL_SRC_ALPHA, GL_ONE) and depth test on, depth write off.
    void draw(const Shader& orbitShader, const sim::SolarSystem& system, const OrbitDrawParams& p) const;

private:
    struct Ring {
        int bodyIndex;
        GLint first;
    };
    std::vector<Ring> rings_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
};

} // namespace render
