#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace render {

// Pure geometry helpers for picking and screen-space overlays. No OpenGL.
// All positions are camera-target-relative render units (see Camera.h).

struct Ray {
    glm::vec3 origin;
    glm::vec3 dir; // unit length
};

// Ray from the camera through a window pixel (origin top-left).
Ray makePickRay(const glm::mat4& viewProj, float px, float py, float width, float height);

// Distance along the ray to the first intersection with the sphere, 0 if the
// origin is inside it, or a negative value on a miss.
float raySphere(const Ray& ray, const glm::vec3& center, float radius);

struct ScreenPoint {
    glm::vec2 px{0.0f};    // window pixels, origin top-left
    float     depth = 0.0f; // clip w (distance along the view axis)
    bool      inFront = false;
    bool      onScreen = false;
};

ScreenPoint projectToScreen(const glm::mat4& viewProj, const glm::vec3& p, float width, float height);

} // namespace render
