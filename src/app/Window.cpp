#include "app/Window.h"

#include "app/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>

namespace app {

namespace {

void glfwErrorCallback(int code, const char* description) {
    logError("GLFW error %d: %s", code, description);
}

} // namespace

Window::~Window() { destroy(); }

bool Window::create(const char* title, std::string& error) {
    glfwSetErrorCallback(glfwErrorCallback);
    // GLFW 3.4 also opts the process into per-monitor-v2 DPI awareness; the
    // application manifest declares it too so it is set before any window exists.
    if (!glfwInit()) {
        error = "Failed to initialise GLFW.";
        return false;
    }
    glfwReady_ = true;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    // The 3D view is antialiased in its own multisampled FBO (render/PostProcess);
    // the window framebuffer only receives the composite and the HUD.
    glfwWindowHint(GLFW_SAMPLES, 0);
    // Resize the window when it moves to a monitor with a different DPI.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifndef NDEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

    // Size to ~85% of the primary monitor's work area. With SCALE_TO_MONITOR
    // the requested size is multiplied by the content scale, so divide first.
    int wx = 0, wy = 0, ww = 1280, wh = 720;
    float sx = 1.0f, sy = 1.0f;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
        glfwGetMonitorContentScale(monitor, &sx, &sy);
    }
    const float scale = std::max(sx, 1.0f);
    const int reqW = std::max(960, static_cast<int>(static_cast<float>(ww) * 0.85f / scale));
    const int reqH = std::max(600, static_cast<int>(static_cast<float>(wh) * 0.85f / scale));

    window_ = glfwCreateWindow(reqW, reqH, title, nullptr, nullptr);
    if (!window_) {
        error = "Could not create an OpenGL 3.3 core window.\n"
                "Update your graphics driver; OpenGL 3.3 or newer is required.";
        return false;
    }

    int actualW = 0, actualH = 0;
    glfwGetWindowSize(window_, &actualW, &actualH);
    glfwSetWindowPos(window_, wx + std::max(0, (ww - actualW) / 2), wy + std::max(0, (wh - actualH) / 2));

    glfwSetWindowUserPointer(window_, this);
    // Installed before ImGui's backend, which chains to it.
    glfwSetScrollCallback(window_, scrollCallback);

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    const int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0 || GLAD_VERSION_MAJOR(version) < 3 ||
        (GLAD_VERSION_MAJOR(version) == 3 && GLAD_VERSION_MINOR(version) < 3)) {
        error = "OpenGL 3.3 core functions could not be loaded.";
        return false;
    }
    logInfo("OpenGL %s | %s | %s",
            reinterpret_cast<const char*>(glGetString(GL_VERSION)),
            reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
            reinterpret_cast<const char*>(glGetString(GL_VENDOR)));

    glfwMaximizeWindow(window_);
    glfwShowWindow(window_);
    return true;
}

void Window::destroy() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (glfwReady_) {
        glfwTerminate();
        glfwReady_ = false;
    }
}

bool Window::shouldClose() const { return glfwWindowShouldClose(window_) != 0; }

void Window::pollEvents() { glfwPollEvents(); }

void Window::waitEvents(double timeoutSeconds) { glfwWaitEventsTimeout(timeoutSeconds); }

void Window::swapBuffers() { glfwSwapBuffers(window_); }

void Window::framebufferSize(int& w, int& h) const { glfwGetFramebufferSize(window_, &w, &h); }

float Window::contentScale() const {
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window_, &sx, &sy);
    return sx > 0.0f ? sx : 1.0f;
}

void Window::scrollCallback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    if (auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window))) {
        self->input_.addScroll(yoffset);
    }
}

} // namespace app
