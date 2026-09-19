#include "render/EclipticGrid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

float gridHeight(const GridParams& p, float r) {
    const float s2 = p.wellSoft * p.wellSoft;
    const float R = p.rim();
    return p.yOffset - p.wellK * (1.0f / std::sqrt(r * r + s2) - 1.0f / std::sqrt(R * R + s2));
}

std::vector<LineVertex> generateEclipticGrid(const GridParams& p) {
    std::vector<LineVertex> v;
    v.reserve(static_cast<std::size_t>(2 * (p.ringCount * p.ringSegments + p.spokes * p.spokeSegments)));
    const float R = p.rim();

    // Fade out over the outer 30% (no hard edge) and in over the first rings.
    auto fade = [&](float r) {
        return (1.0f - smoothstep(0.70f * R, R, r)) * smoothstep(0.0f, 2.5f * p.ringSpacing, r);
    };
    auto point = [&](float r, float theta, float alpha) {
        return LineVertex{glm::vec3(r * std::cos(theta), 0.0f, r * std::sin(theta)), alpha};
    };

    // Concentric rings.
    for (int k = 1; k <= p.ringCount; ++k) {
        const float r = p.ringSpacing * static_cast<float>(k);
        const float weight = (k % 8 == 0) ? 0.50f : (k % 2 == 0) ? 0.26f : 0.10f;
        const float alpha = weight * fade(r);
        for (int s = 0; s < p.ringSegments; ++s) {
            const float a0 = kTwoPi * static_cast<float>(s) / static_cast<float>(p.ringSegments);
            const float a1 = kTwoPi * static_cast<float>(s + 1) / static_cast<float>(p.ringSegments);
            v.push_back(point(r, a0, alpha));
            v.push_back(point(r, a1, alpha));
        }
    }

    // Radial spokes, finely subdivided so they follow the well's curvature.
    for (int s = 0; s < p.spokes; ++s) {
        const float theta = kTwoPi * static_cast<float>(s) / static_cast<float>(p.spokes);
        const float base = (s % 6 == 0) ? 0.40f : 0.20f;
        for (int i = 0; i < p.spokeSegments; ++i) {
            const float r0 = R * static_cast<float>(i) / static_cast<float>(p.spokeSegments);
            const float r1 = R * static_cast<float>(i + 1) / static_cast<float>(p.spokeSegments);
            v.push_back(point(r0, theta, base * fade(r0)));
            v.push_back(point(r1, theta, base * fade(r1)));
        }
    }
    return v;
}

} // namespace render
