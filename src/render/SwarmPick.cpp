#include "render/SwarmPick.h"

#include "render/Picking.h"

#include <cmath>

namespace render {

int pickSwarm(const float* xyz, std::size_t count, const ScaleMapper& mapper, const OrbitCamera& camera,
              const FrameViewport& vp, float windowX, float windowY, float radiusPx) {
    if (xyz == nullptr || count == 0) {
        return -1;
    }
    const glm::mat4 viewProj = camera.projection(vp.aspect()) * camera.view();
    const glm::dvec3 target = camera.target();
    int best = -1;
    float bestDist = 0.0f;
    float bestDepth = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const glm::dvec3 world = mapper.toRender(sim::Vec3d{xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]});
        const ScreenPoint sp = projectToScreen(viewProj, glm::vec3(world - target), vp.viewSize.x, vp.viewSize.y);
        if (!sp.inFront) {
            continue;
        }
        const float dx = sp.px.x + vp.viewMin.x - windowX;
        const float dy = sp.px.y + vp.viewMin.y - windowY;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > radiusPx) {
            continue;
        }
        // Nearest wins; within a pixel of each other, the one closer to the camera does.
        if (best < 0 || d < bestDist - 1.0f || (d <= bestDist + 1.0f && sp.depth < bestDepth)) {
            best = static_cast<int>(i);
            bestDist = d;
            bestDepth = sp.depth;
        }
    }
    return best;
}

} // namespace render
