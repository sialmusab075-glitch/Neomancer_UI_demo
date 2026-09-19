#include "render/OrbitRenderer.h"

#include "render/ScaleMapper.h"
#include "render/Shader.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"
#include "sim/SolarSystem.h"

namespace render {

namespace {

struct RingStyle {
    glm::vec4 color; // rgb may exceed 1 (HDR, feeds bloom)
    float widthPx;
    float soft;      // 0 crisp, 1 halo
    float trailMin;  // brightness far behind the body
};

// Unselected: thin, in the theme's quiet orbit colour.
RingStyle plainStyle(const glm::vec3& c, float a) { return {glm::vec4(c, a), 1.2f, 0.0f, 0.30f}; }
// Selected: a wide faint halo under a thin bright (HDR) core, in the selection colour.
RingStyle haloStyle(const glm::vec3& c) { return {glm::vec4(c * 0.9f, 0.28f), 11.0f, 1.0f, 0.20f}; }
RingStyle coreStyle(const glm::vec3& c) { return {glm::vec4(c * 1.7f, 1.0f), 1.6f, 0.0f, 0.28f}; }

} // namespace

OrbitRenderer::~OrbitRenderer() { destroy(); }

void OrbitRenderer::destroy() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = 0;
    rings_.clear();
}

void OrbitRenderer::build(const sim::SolarSystem& system, const ScaleMapper& mapper) {
    destroy();

    // Interleaved: position (3) + true anomaly (1).
    std::vector<float> vertices;
    for (int b = 0; b < system.bodyCount(); ++b) {
        const sim::Body& body = system.body(b);
        if (body.parentIndex < 0) {
            continue; // the Sun has no orbit
        }
        rings_.push_back({b, static_cast<GLint>(vertices.size() / 4)});
        const sim::OrbitalElements& el = body.data().elements;
        for (int i = 0; i < kPointsPerOrbit; ++i) {
            const double E = sim::kTwoPi * static_cast<double>(i) / kPointsPerOrbit;
            // Planets orbit the Sun at the origin, so the parent-relative ring is
            // already heliocentric. (Moon rings, parent-relative, come in Milestone 4.)
            const sim::Vec3d p = sim::orbitPointAtEccentricAnomaly(el, E);
            const glm::dvec3 r = mapper.toRender(p);
            vertices.push_back(static_cast<float>(r.x));
            vertices.push_back(static_cast<float>(r.y));
            vertices.push_back(static_cast<float>(r.z));
            vertices.push_back(static_cast<float>(sim::trueAnomalyFromEccentric(E, el.e)));
        }
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_STATIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(4 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

void OrbitRenderer::draw(const Shader& orbitShader, const sim::SolarSystem& system, const OrbitDrawParams& p) const {
    if (!vao_) {
        return;
    }
    orbitShader.use();
    orbitShader.set("uViewProj", p.viewProj);
    orbitShader.set("uTarget", p.target);
    orbitShader.set("uEye", p.eye);
    orbitShader.set("uViewport", p.viewportPx);
    orbitShader.set("uFadeNear", p.fadeNear);
    orbitShader.set("uFadeFar", p.fadeFar);
    orbitShader.set("uFadeMin", p.fadeMin);
    orbitShader.set("uDir", p.timeDir < 0.0f ? -1.0f : 1.0f);
    orbitShader.set("uTrail", 1.0f);

    auto drawRing = [&](const Ring& ring, const RingStyle& style) {
        orbitShader.set("uNu", static_cast<float>(system.body(ring.bodyIndex).orbit.nu));
        orbitShader.set("uColor", style.color);
        orbitShader.set("uWidth", style.widthPx * p.dpiScale);
        orbitShader.set("uSoft", style.soft);
        orbitShader.set("uTrailMin", style.trailMin);
        glDrawArrays(GL_LINE_LOOP, ring.first, kPointsPerOrbit);
    };

    glBindVertexArray(vao_);
    for (const Ring& ring : rings_) {
        if (ring.bodyIndex != p.selectedBody) {
            drawRing(ring, plainStyle(p.plainColor, p.plainAlpha));
        }
    }
    for (const Ring& ring : rings_) {
        if (ring.bodyIndex == p.selectedBody) {
            drawRing(ring, haloStyle(p.selectColor));
            drawRing(ring, coreStyle(p.selectColor));
        }
    }
    glBindVertexArray(0);
}

} // namespace render
