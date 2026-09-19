#include "render/Starfield.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {

namespace {

constexpr float kPi = 3.14159265358979f;

// PCG32: tiny, fast, and identical on every compiler (unlike std distributions),
// so the sky looks the same on every machine.
class Pcg32 {
public:
    explicit Pcg32(std::uint64_t seed) {
        state_ = 0u;
        next();
        state_ += seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ull + 1442695040888963407ull;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    // Uniform in [0, 1).
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    // Standard normal (Box-Muller).
    float normal() {
        const float u1 = std::fmax(uniform(), 1e-7f);
        const float u2 = uniform();
        return std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * kPi * u2);
    }

private:
    std::uint64_t state_;
};

glm::vec3 uniformDirection(Pcg32& rng) {
    const float z = 2.0f * rng.uniform() - 1.0f;
    const float phi = 2.0f * kPi * rng.uniform();
    const float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
    return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

} // namespace

std::vector<PointVertex> generateStarfield(int count, std::uint32_t seed) {
    Pcg32 rng(seed);
    std::vector<PointVertex> stars;
    stars.reserve(static_cast<std::size_t>(count));

    // Galactic band: a great circle tilted ~60 degrees to the ecliptic (as the
    // real Milky Way is), with points scattered about it.
    const glm::vec3 bandNormal = glm::normalize(glm::vec3(0.0f, 0.5f, 0.866f));
    const glm::vec3 bandU = glm::normalize(glm::cross(bandNormal, glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::vec3 bandV = glm::cross(bandNormal, bandU);

    for (int i = 0; i < count; ++i) {
        glm::vec3 dir;
        if (rng.uniform() < 0.35f) {
            const float theta = 2.0f * kPi * rng.uniform();
            const float spread = 0.12f * rng.normal();
            dir = glm::normalize(bandU * std::cos(theta) + bandV * std::sin(theta) + bandNormal * spread);
        } else {
            dir = uniformDirection(rng);
        }

        // Magnitude-like distribution: many faint stars, few bright ones.
        const float m = std::pow(rng.uniform(), 3.0f);
        const float size = 1.0f + 2.4f * m;
        const float brightness = 0.22f + 0.78f * m;

        // Temperature 0..1 (cool .. warm); the theme maps it to colours in points.vert.
        // Most stars sit near the middle, with a few warm and a few cool ones.
        float temperature = 0.5f + 0.16f * (rng.uniform() - 0.5f);
        const float t = rng.uniform();
        if (t < 0.07f) {
            temperature = 0.85f + 0.15f * rng.uniform();
        } else if (t < 0.22f) {
            temperature = 0.15f * rng.uniform();
        }
        stars.push_back({dir, size, glm::vec4(temperature, temperature, temperature, brightness)});
    }
    return stars;
}

std::vector<PointVertex> generateSphereShell(int count, std::uint32_t seed) {
    Pcg32 rng(seed);
    std::vector<PointVertex> points;
    points.reserve(static_cast<std::size_t>(count));
    const float golden = kPi * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < count; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float r = std::sqrt(std::fmax(0.0f, 1.0f - y * y));
        const float theta = golden * static_cast<float>(i);
        // Slight radial jitter so the shell reads as particles, not a lattice.
        const float jitter = 1.0f + 0.04f * (rng.uniform() - 0.5f);
        const glm::vec3 p = glm::vec3(r * std::cos(theta), y, r * std::sin(theta)) * jitter;
        const float b = 0.55f + 0.45f * rng.uniform();
        // Heat 0..1 (falloff .. white-hot); the shell is the cooler outer layer.
        const float heat = 0.25f + 0.30f * rng.uniform();
        points.push_back({p, 1.3f + 0.7f * rng.uniform(), glm::vec4(heat, heat, heat, b)});
    }
    return points;
}

std::vector<PointVertex> generateSunParticles(int count, std::uint32_t seed) {
    std::vector<PointVertex> points = generateSphereShell(count * 55 / 100, seed);
    Pcg32 rng(seed ^ 0x9E3779B9u);
    const int interior = count - static_cast<int>(points.size());
    for (int i = 0; i < interior; ++i) {
        // r = u^1.6 puts more particles near the core than a uniform volume would.
        const float r = 0.96f * std::pow(rng.uniform(), 1.6f);
        const glm::vec3 p = uniformDirection(rng) * r;
        // Hotter toward the core.
        const float heat = std::min(1.0f, 0.50f + 0.50f * (1.0f - r / 0.96f));
        const float b = 0.25f + 0.55f * rng.uniform();
        points.push_back({p, 1.0f + 1.6f * rng.uniform(), glm::vec4(heat, heat, heat, b)});
    }
    // Shell particles: random size/brightness too (the shell generator's are narrower).
    for (std::size_t i = 0; i < points.size() && static_cast<int>(i) < count * 55 / 100; ++i) {
        points[i].size = 1.0f + 1.8f * rng.uniform();
        points[i].color.a = 0.35f + 0.65f * rng.uniform();
    }
    return points;
}

} // namespace render
