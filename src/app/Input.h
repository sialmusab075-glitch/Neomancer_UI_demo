#pragma once

struct GLFWwindow;

namespace app {

// Per-frame mouse state for the 3D viewport.
//
// A gesture belongs to the viewport only if it *started* while ImGui did not
// want the mouse, so dragging a slider never spins the camera, and a camera
// drag keeps going even if the cursor passes over a panel.
//
// Left button: a press that moves less than kDragThresholdPx before release is
// a click (two clicks within kDoubleClickSeconds / kDoubleClickPx form a
// double-click); moving further turns it into a rotate-drag.
class Input {
public:
    static constexpr float  kDragThresholdPx = 4.0f;
    static constexpr double kDoubleClickSeconds = 0.35;
    static constexpr float  kDoubleClickPx = 6.0f;

    // Scroll events arrive through a GLFW callback between frames.
    void addScroll(double dy) { scrollAccum_ += dy; }

    // Call once per frame, after ImGui::NewFrame().
    void beginFrame(GLFWwindow* window, bool imguiWantsMouse, double timeSeconds);

    float cursorX() const { return static_cast<float>(lastX_); }
    float cursorY() const { return static_cast<float>(lastY_); }
    float cursorDx() const { return dx_; }
    float cursorDy() const { return dy_; }
    bool  leftDragging() const { return leftDrag_; }
    bool  rightDragging() const { return rightDrag_; }
    // Wheel steps this frame; zero when ImGui owns the mouse.
    float scroll() const { return scroll_; }

    // Set for exactly one frame. Positions are window coordinates.
    bool  clicked() const { return clicked_; }
    bool  doubleClicked() const { return doubleClicked_; }
    float clickX() const { return clickX_; }
    float clickY() const { return clickY_; }

private:
    double scrollAccum_ = 0.0;
    float  scroll_ = 0.0f;
    double lastX_ = 0.0, lastY_ = 0.0;
    bool   haveLast_ = false;
    float  dx_ = 0.0f, dy_ = 0.0f;

    bool   leftDown_ = false, rightDown_ = false;
    bool   leftOwned_ = false;   // current left press started in the viewport
    float  leftTravel_ = 0.0f;   // pixels moved since the press
    bool   leftDrag_ = false, rightDrag_ = false;

    bool   clicked_ = false, doubleClicked_ = false;
    float  clickX_ = 0.0f, clickY_ = 0.0f;
    double lastClickTime_ = -1.0;
    float  lastClickX_ = 0.0f, lastClickY_ = 0.0f;
};

} // namespace app
