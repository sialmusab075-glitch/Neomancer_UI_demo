#include "app/Input.h"

#include <GLFW/glfw3.h>

#include <cmath>

namespace app {

void Input::beginFrame(GLFWwindow* window, bool imguiWantsMouse, double timeSeconds) {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (haveLast_) {
        dx_ = static_cast<float>(x - lastX_);
        dy_ = static_cast<float>(y - lastY_);
    } else {
        dx_ = dy_ = 0.0f;
        haveLast_ = true;
    }
    lastX_ = x;
    lastY_ = y;

    clicked_ = false;
    doubleClicked_ = false;

    // --- left button: click vs rotate-drag -------------------------------------
    const bool left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (left && !leftDown_) {
        leftOwned_ = !imguiWantsMouse;
        leftTravel_ = 0.0f;
        leftDrag_ = false;
    } else if (left && leftOwned_) {
        leftTravel_ += std::sqrt(dx_ * dx_ + dy_ * dy_);
        if (leftTravel_ > kDragThresholdPx) {
            leftDrag_ = true;
        }
    } else if (!left && leftDown_) {
        if (leftOwned_ && !leftDrag_) {
            clicked_ = true;
            clickX_ = static_cast<float>(x);
            clickY_ = static_cast<float>(y);
            const float ddx = clickX_ - lastClickX_;
            const float ddy = clickY_ - lastClickY_;
            if (lastClickTime_ >= 0.0 && timeSeconds - lastClickTime_ <= kDoubleClickSeconds &&
                std::sqrt(ddx * ddx + ddy * ddy) <= kDoubleClickPx) {
                doubleClicked_ = true;
                lastClickTime_ = -1.0; // a third click starts a new pair
            } else {
                lastClickTime_ = timeSeconds;
                lastClickX_ = clickX_;
                lastClickY_ = clickY_;
            }
        }
        leftOwned_ = false;
        leftDrag_ = false;
    }
    leftDown_ = left;

    // --- right button: pan-drag -------------------------------------------------
    const bool right = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (right && !rightDown_) {
        rightDrag_ = !imguiWantsMouse;
    } else if (!right) {
        rightDrag_ = false;
    }
    rightDown_ = right;

    scroll_ = imguiWantsMouse ? 0.0f : static_cast<float>(scrollAccum_);
    scrollAccum_ = 0.0;
}

} // namespace app
