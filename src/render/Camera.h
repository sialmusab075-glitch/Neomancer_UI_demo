#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace render {

// Orbit camera around a target point.
//
// The target is stored in double precision in render units. All matrices are
// built *relative to the target*: the view matrix puts the target at the
// origin, and callers translate objects by (position - target) computed in
// double before converting to float. That keeps float precision where the
// camera is looking, with no jitter when following a distant body.
class OrbitCamera {
public:
    // Left-drag: rotate. Pixels -> radians.
    void rotate(float dxPixels, float dyPixels);
    // Wheel: exponential zoom, clamped.
    void zoom(float wheelSteps);
    // Right-drag: move the target in the view plane (1 px of drag = 1 px of motion at the target).
    void pan(float dxPixels, float dyPixels, float viewportHeightPx);

    void setTarget(const glm::dvec3& target) { target_ = target; }
    const glm::dvec3& target() const { return target_; }

    void setAngles(float yaw, float pitch);

    float distance() const { return distance_; }
    void setDistance(float d);
    void setDistanceLimits(float minD, float maxD);
    float fovY() const { return fovY_; }

    // Eye position relative to the target.
    glm::vec3 eyeOffset() const;
    // Target-relative view matrix (target at the origin).
    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;

    // Size in world units of one pixel at `depth` in front of the eye.
    float worldPerPixel(float depth, float viewportHeightPx) const;

private:
    glm::dvec3 target_{0.0};
    float yaw_ = 0.65f;        // radians around +Y
    float pitch_ = 0.45f;      // radians above the ecliptic plane
    float distance_ = 34.0f;
    float minDistance_ = 0.3f;
    float maxDistance_ = 400.0f;
    float fovY_ = 0.785398f;   // 45 degrees
};

} // namespace render
