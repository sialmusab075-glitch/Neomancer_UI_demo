#pragma once

#include "neo/sim/SwarmLegend.h"
#include "render/Camera.h"
#include "render/FrameViewport.h"
#include "render/ScaleMapper.h"
#include "render/Shader.h"

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace render {

// Everything the NEOS layer needs for one frame besides the camera.
struct SwarmDrawParams {
    bool enabled = false;
    neo::SwarmLegend legend = neo::SwarmLegend::Distance;
    glm::vec3 earthAu{0.0f};   // Earth, heliocentric ecliptic AU (for the distance legend)
    glm::vec3 colNear{1.0f};   // legend ramp, from the theme
    glm::vec3 colMid{0.5f};
    glm::vec3 colFar{0.2f};
    float intensity = 1.0f;
};

// The NEOS layer: N asteroids as points, one dynamic buffer of positions and one of
// per-object attributes, ONE draw call. Positions arrive as heliocentric ecliptic AU
// and are mapped to render units (compressed or true, like the planets) in the
// vertex shader.
class SwarmRenderer {
public:
    SwarmRenderer() = default;
    ~SwarmRenderer();
    SwarmRenderer(const SwarmRenderer&) = delete;
    SwarmRenderer& operator=(const SwarmRenderer&) = delete;

    bool init(std::string& error);
    void destroy();

    // A new object set: (re)allocates both buffers. Positions start at the origin
    // until the first updatePositions().
    void setObjects(const std::vector<neo::SwarmAttr>& attrs);
    // The days-to-next-approach column only (the "time to approach" legend).
    void updateApproachDays(const std::vector<float>& days);
    void updatePositions(const float* xyz, std::size_t count);

    std::size_t count() const { return count_; }
    void draw(const SwarmDrawParams& params, const ScaleMapper& mapper, const glm::mat4& viewProj,
              const glm::vec3& cameraTarget, float pixelScale) const;

private:
    Shader shader_;
    GLuint vao_ = 0, posVbo_ = 0, attrVbo_ = 0;
    std::size_t count_ = 0;
    std::vector<float> attrs_; // 3 floats per object, kept for updateApproachDays
};

} // namespace render
