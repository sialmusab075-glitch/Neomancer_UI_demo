#include "render/Picking.h"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include <cmath>

namespace render {

Ray makePickRay(const glm::mat4& viewProj, float px, float py, float width, float height) {
    const float x = 2.0f * px / width - 1.0f;
    const float y = 1.0f - 2.0f * py / height;
    const glm::mat4 inv = glm::inverse(viewProj);
    glm::vec4 nearP = inv * glm::vec4(x, y, -1.0f, 1.0f);
    glm::vec4 farP = inv * glm::vec4(x, y, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    Ray r;
    r.origin = glm::vec3(nearP);
    r.dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));
    return r;
}

float raySphere(const Ray& ray, const glm::vec3& center, float radius) {
    const glm::vec3 oc = ray.origin - center;
    const float b = glm::dot(oc, ray.dir);
    const float c = glm::dot(oc, oc) - radius * radius;
    if (c <= 0.0f) {
        return 0.0f; // origin inside the sphere
    }
    const float disc = b * b - c;
    if (disc < 0.0f) {
        return -1.0f;
    }
    const float t = -b - std::sqrt(disc);
    return t >= 0.0f ? t : -1.0f;
}

ScreenPoint projectToScreen(const glm::mat4& viewProj, const glm::vec3& p, float width, float height) {
    ScreenPoint s;
    const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
    s.depth = clip.w;
    s.inFront = clip.w > 1e-6f;
    if (!s.inFront) {
        return s;
    }
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    s.px = glm::vec2((ndc.x + 1.0f) * 0.5f * width, (1.0f - ndc.y) * 0.5f * height);
    s.onScreen = std::fabs(ndc.x) <= 1.0f && std::fabs(ndc.y) <= 1.0f;
    return s;
}

} // namespace render
