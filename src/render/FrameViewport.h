#pragma once

#include <glm/vec2.hpp>

namespace render {

// Window/framebuffer geometry for one frame. The 3D scene is drawn only into
// the view rectangle (the HUD's central dock node), given in window coordinates.
// Shared by the solar-system renderer and the Earth-view renderer.
struct FrameViewport {
    glm::vec2 framebufferPx{1.0f}; // GL framebuffer size
    glm::vec2 windowPx{1.0f};      // ImGui / cursor coordinate space
    glm::vec2 viewMin{0.0f};       // 3D view rectangle, window coordinates
    glm::vec2 viewSize{1.0f};
    float     dpiScale = 1.0f;

    float fbPerWindow() const { return framebufferPx.y / windowPx.y; }
    float aspect() const { return viewSize.x / viewSize.y; }
    glm::vec2 viewSizeFb() const { return viewSize * fbPerWindow(); }
};

} // namespace render
