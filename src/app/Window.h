#pragma once

#include "app/Input.h"

#include <string>

struct GLFWwindow;

namespace app {

// Owns GLFW, the main window, and its OpenGL 3.3 core context (loaded via GLAD).
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates a window sized to ~85% of the primary monitor's work area and
    // makes its context current. On failure, returns false and fills `error`.
    bool create(const char* title, std::string& error);
    void destroy();

    GLFWwindow* handle() const { return window_; }
    bool shouldClose() const;
    void pollEvents();
    void waitEvents(double timeoutSeconds);
    void swapBuffers();

    void framebufferSize(int& w, int& h) const;
    // Monitor content scale (1.0 = 96 DPI) of the monitor the window is on.
    float contentScale() const;

    Input& input() { return input_; }

private:
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);

    GLFWwindow* window_ = nullptr;
    bool glfwReady_ = false;
    Input input_;
};

} // namespace app
