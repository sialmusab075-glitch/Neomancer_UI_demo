#pragma once
// Minimal double-precision 3-vector for the simulation layer.
// The simulation deliberately has no third-party dependencies (not even GLM),
// so it builds and tests under any C++17 compiler without a window or GPU.

#include <cmath>

namespace sim {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3d() = default;
    constexpr Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3d operator-() const { return {-x, -y, -z}; }
    Vec3d& operator+=(const Vec3d& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3d& operator-=(const Vec3d& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

constexpr Vec3d operator*(double s, const Vec3d& v) { return v * s; }
constexpr double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3d& v) { return std::sqrt(dot(v, v)); }

} // namespace sim
