#pragma once

#include <imgui.h>

namespace hud {

// The 3D viewport: the dockspace's transparent central node, in window coordinates.
struct ViewRect {
    ImVec2 min{0.0f, 0.0f};
    ImVec2 max{0.0f, 0.0f};
    float width() const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
};

// Full-window dockspace with a transparent pass-through central node where the
// 3D scene shows. The default arrangement is built with DockBuilder the first
// time (or after "RESET LAYOUT"); after that imgui.ini remembers the user's own.
//
//   +------------+----------------------------+---------------+
//   | STATUS     |                            | TARGET        |
//   +------------+                            |               |
//   | CONTROL    |        3D VIEW             +---------------+
//   |            |      (pass-through)        | STATE VECTOR  |
//   +------------+                            +---------------+
//   | EVENT LOG  |                            | HISTORY       |
//   +------------+----------------------------+---------------+
class HudLayout {
public:
    // Call right after ImGui::NewFrame(). Returns the 3D viewport rectangle.
    ViewRect begin();
    void requestReset() { resetRequested_ = true; }

    // Corner brackets and captions around the 3D viewport (drawn under the panels).
    void drawViewportFrame(const ViewRect& r, const char* topLeft, const char* topRight, const char* bottomLeft,
                           const char* bottomRight) const;

private:
    void buildDefault(unsigned id);
    bool resetRequested_ = false;
};

} // namespace hud
