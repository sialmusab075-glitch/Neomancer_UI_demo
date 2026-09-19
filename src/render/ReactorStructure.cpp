#include "render/ReactorStructure.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace render {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kDeg = kTwoPi / 360.0f;
constexpr float kLabelAngleDeg = -24.0f;  // angle of the first ring label
constexpr float kLabelStepDeg = -21.0f;   // each further ring's label moves round by this

// Small deterministic generator (same stream on every compiler).
class Rng {
public:
    explicit Rng(std::uint32_t seed) : s_(seed * 747796405u + 2891336453u) {}
    float uniform() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return static_cast<float>(s_ >> 8) * (1.0f / 16777216.0f);
    }
    int index(int n) { return std::min(n - 1, static_cast<int>(uniform() * static_cast<float>(n))); }

private:
    std::uint32_t s_;
};

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

glm::vec3 onPlane(float r, float theta) { return glm::vec3(r * std::cos(theta), 0.0f, r * std::sin(theta)); }

void addCircle(std::vector<LineVertex>& out, float r, int segments, float alpha) {
    for (int s = 0; s < segments; ++s) {
        const float a0 = kTwoPi * static_cast<float>(s) / static_cast<float>(segments);
        const float a1 = kTwoPi * static_cast<float>(s + 1) / static_cast<float>(segments);
        out.push_back({onPlane(r, a0), alpha});
        out.push_back({onPlane(r, a1), alpha});
    }
}

} // namespace

StructureGeometry generateStructure(const StructureParams& p) {
    StructureGeometry g;
    const float outer = p.outerRadius;
    // Everything fades out over the outer 15% so the structure has no hard edge.
    auto edgeFade = [&](float r) { return 1.0f - smoothstep(0.85f * outer, outer, r); };

    // --- planet rings + labels + ticks -------------------------------------------------
    const std::size_t n = p.ringRadii.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float r = p.ringRadii[i];
        addCircle(g.majorRings, r, p.ringSegments, edgeFade(r));

        // Ticks point outward: every 10 deg short, every 30 deg long.
        const float shortLen = std::max(0.018f * r, 0.012f * outer * 0.25f);
        for (int k = 0; k < 36; ++k) {
            const float theta = static_cast<float>(k) * 10.0f * kDeg;
            const bool major = k % 3 == 0;
            const float len = major ? 2.0f * shortLen : shortLen;
            const float a = (major ? 1.0f : 0.6f) * edgeFade(r);
            g.ticks.push_back({onPlane(r, theta), a});
            g.ticks.push_back({onPlane(r + len, theta), a});
        }

        RingLabel label{};
        label.pos = onPlane(r * 1.012f, (kLabelAngleDeg + kLabelStepDeg * static_cast<float>(i)) * kDeg);
        const double au = i < p.ringAU.size() ? p.ringAU[i] : 0.0;
        std::snprintf(label.text, sizeof label.text, au < 10.0 ? "%.2f AU" : "%.1f AU", au);
        g.labels.push_back(label);
    }

    // --- intermediate rings: inside the first, between neighbours, and the outer rim ---------
    std::vector<float> minor;
    if (n > 0) {
        minor.push_back(0.5f * p.ringRadii[0]);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            minor.push_back(0.5f * (p.ringRadii[i] + p.ringRadii[i + 1]));
        }
    }
    minor.push_back(outer);
    for (float r : minor) {
        addCircle(g.minorRings, r, p.ringSegments, r >= outer ? 0.6f : edgeFade(r));
    }

    // --- 12 dashed spokes from just outside the Sun to the outer ring ---------------------
    const float span = std::max(outer - p.innerRadius, 1e-3f);
    const int dashes = 36;
    const float period = span / static_cast<float>(dashes);
    for (int sIdx = 0; sIdx < kSpokeCount; ++sIdx) {
        const float theta = kTwoPi * static_cast<float>(sIdx) / static_cast<float>(kSpokeCount);
        for (int d = 0; d < dashes; ++d) {
            const float r0 = p.innerRadius + period * static_cast<float>(d);
            const float r1 = r0 + 0.55f * period;
            g.spokes.push_back({onPlane(r0, theta), edgeFade(r0)});
            g.spokes.push_back({onPlane(r1, theta), edgeFade(r1)});
        }
    }

    // --- scattered fragments and dot markers near the rings (seeded) ------------------------
    Rng rng(p.seed);
    if (n > 0) {
        for (int f = 0; f < kFragmentCount; ++f) {
            const float r = p.ringRadii[static_cast<std::size_t>(rng.index(static_cast<int>(n)))];
            const float theta = kTwoPi * rng.uniform();
            const float a = (0.5f + 0.5f * rng.uniform()) * edgeFade(r);
            if (rng.uniform() < 0.6f) {
                // Short arc hugging the ring, slightly inside or outside it.
                const float rr = r * (1.0f + (rng.uniform() < 0.5f ? -1.0f : 1.0f) * (0.015f + 0.02f * rng.uniform()));
                const float arc = (2.0f + 4.0f * rng.uniform()) * kDeg;
                const int segs = 4;
                for (int s = 0; s < segs; ++s) {
                    const float t0 = theta + arc * static_cast<float>(s) / static_cast<float>(segs);
                    const float t1 = theta + arc * static_cast<float>(s + 1) / static_cast<float>(segs);
                    g.fragments.push_back({onPlane(rr, t0), a});
                    g.fragments.push_back({onPlane(rr, t1), a});
                }
            } else {
                // Short radial dash crossing the ring.
                const float len = r * (0.02f + 0.03f * rng.uniform());
                g.fragments.push_back({onPlane(r - 0.5f * len, theta), a});
                g.fragments.push_back({onPlane(r + 0.5f * len, theta), a});
            }
        }
        for (int m = 0; m < kMarkerCount; ++m) {
            const float r = p.ringRadii[static_cast<std::size_t>(rng.index(static_cast<int>(n)))];
            const float rr = r * (1.0f + 0.08f * (rng.uniform() - 0.5f));
            const float theta = kTwoPi * rng.uniform();
            const float a = (0.4f + 0.6f * rng.uniform()) * edgeFade(rr);
            g.markers.push_back({onPlane(rr, theta), 1.5f + 1.2f * rng.uniform(), glm::vec4(1.0f, 1.0f, 1.0f, a)});
        }
    }
    return g;
}

std::vector<LineVertex> generateUnitCircle(int segments) {
    std::vector<LineVertex> v;
    v.reserve(static_cast<std::size_t>(2 * segments));
    for (int s = 0; s < segments; ++s) {
        const float a0 = kTwoPi * static_cast<float>(s) / static_cast<float>(segments);
        const float a1 = kTwoPi * static_cast<float>(s + 1) / static_cast<float>(segments);
        v.push_back({glm::vec3(std::cos(a0), std::sin(a0), 0.0f), 1.0f});
        v.push_back({glm::vec3(std::cos(a1), std::sin(a1), 0.0f), 1.0f});
    }
    return v;
}

std::vector<LineVertex> generateCrown(int ticks) {
    std::vector<LineVertex> v;
    v.reserve(static_cast<std::size_t>(2 * ticks));
    for (int k = 0; k < ticks; ++k) {
        const float a = kTwoPi * static_cast<float>(k) / static_cast<float>(ticks);
        const bool major = (k * 360 / ticks) % 30 == 0;
        const float r1 = major ? 1.14f : 1.07f;
        const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
        v.push_back({dir, major ? 1.0f : 0.6f});
        v.push_back({dir * r1, major ? 1.0f : 0.6f});
    }
    return v;
}

} // namespace render
