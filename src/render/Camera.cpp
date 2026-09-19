#include "render/Camera.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace render {

namespace {
constexpr float kRotateRadPerPixel = 0.005f;
constexpr float kZoomPerStep = 0.12f;
constexpr float kMaxPitch = 1.5533430f; // 89 degrees
}

void OrbitCamera::rotate(float dxPixels, float dyPixels) {
    yaw_ -= dxPixels * kRotateRadPerPixel;
    pitch_ = std::clamp(pitch_ + dyPixels * kRotateRadPerPixel, -kMaxPitch, kMaxPitch);
    yaw_ = std::remainder(yaw_, 6.28318530718f);
}

void OrbitCamera::setAngles(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -kMaxPitch, kMaxPitch);
}

void OrbitCamera::zoom(float wheelSteps) {
    setDistance(distance_ * std::exp(-wheelSteps * kZoomPerStep));
}

void OrbitCamera::setDistance(float d) { distance_ = std::clamp(d, minDistance_, maxDistance_); }

void OrbitCamera::setDistanceLimits(float minD, float maxD) {
    minDistance_ = minD;
    maxDistance_ = maxD;
    setDistance(distance_);
}

void OrbitCamera::pan(float dxPixels, float dyPixels, float viewportHeightPx) {
    const glm::vec3 eye = eyeOffset();
    const glm::vec3 forward = glm::normalize(-eye);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, forward);
    const float wpp = worldPerPixel(distance_, viewportHeightPx);
    const glm::vec3 delta = (-right * dxPixels + up * dyPixels) * wpp;
    target_ += glm::dvec3(delta);
}

glm::vec3 OrbitCamera::eyeOffset() const {
    const float cp = std::cos(pitch_);
    return distance_ * glm::vec3(cp * std::sin(yaw_), std::sin(pitch_), cp * std::cos(yaw_));
}

glm::mat4 OrbitCamera::view() const {
    return glm::lookAt(eyeOffset(), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projection(float aspect) const {
    const float nearPlane = std::max(distance_ * 0.01f, 1e-4f);
    const float farPlane = distance_ * 50.0f + 5000.0f;
    return glm::perspective(fovY_, aspect, nearPlane, farPlane);
}

float OrbitCamera::worldPerPixel(float depth, float viewportHeightPx) const {
    return 2.0f * depth * std::tan(0.5f * fovY_) / std::max(1.0f, viewportHeightPx);
}

} // namespace render
