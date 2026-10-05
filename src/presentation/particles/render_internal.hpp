#pragma once
#include "eawr/presentation/particles/render.hpp"
#include <cmath>

namespace eawr::presentation::particles::render_detail {
inline Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3 a, const float b) { return {a.x * b, a.y * b, a.z * b}; }
inline float dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 a, const Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3 value) { return std::sqrt(dot(value, value)); }
inline Vec3 normalized(const Vec3 value) {
    const float magnitude = length(value);
    return magnitude > 1.0e-12F ? value * (1.0F / magnitude) : Vec3{};
}
inline bool finite(const Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
inline bool finite(const Vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}
// Render basis (Y up) back to the ALO basis (Z up): inverse of (x,y,z)->(x,z,-y).
inline Vec3 render_to_particle(const float x, const float y, const float z) { return {x, -z, y}; }

} // namespace eawr::presentation::particles::render_detail
