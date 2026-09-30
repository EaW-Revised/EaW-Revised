#pragma once

#include "eawr/sim/math/fixed.hpp"

#include <array>

namespace eawr::sim::math {

struct Vec2 {
    Fixed x;
    Fixed y;
    friend constexpr bool operator==(const Vec2&, const Vec2&) noexcept = default;
};

struct Vec3 {
    Fixed x;
    Fixed y;
    Fixed z;
    friend constexpr bool operator==(const Vec3&, const Vec3&) noexcept = default;
};

struct Quat {
    Fixed x;
    Fixed y;
    Fixed z;
    Fixed w;
    friend constexpr bool operator==(const Quat&, const Quat&) noexcept = default;
};

struct Mat3x4 {
    std::array<std::array<Fixed, 4>, 3> rows{};
    friend constexpr bool operator==(const Mat3x4&, const Mat3x4&) noexcept = default;
};

[[nodiscard]] core::Result<Fixed> dot(Vec2 left, Vec2 right);
[[nodiscard]] core::Result<Fixed> dot(Vec3 left, Vec3 right);
[[nodiscard]] core::Result<Fixed> dot(Quat left, Quat right);
[[nodiscard]] core::Result<Vec3> cross(Vec3 left, Vec3 right);
[[nodiscard]] core::Result<Fixed> length(Vec2 value);
[[nodiscard]] core::Result<Fixed> length(Vec3 value);
[[nodiscard]] core::Result<Fixed> length(Quat value);
// length(Vec2) and dot(Vec2) without a diagnostic for the hot loops (#520): false where the
// Result form fails; the value is always the Result form's.
[[nodiscard]] bool try_length(Vec2 value, Fixed& result) noexcept;
[[nodiscard]] bool try_dot(Vec2 left, Vec2 right, Fixed& result) noexcept;
[[nodiscard]] core::Result<Vec2> normalize(Vec2 value);
[[nodiscard]] core::Result<Vec3> normalize(Vec3 value);
[[nodiscard]] core::Result<Quat> normalize(Quat value);

[[nodiscard]] core::Result<Quat> compose(Quat left, Quat right);
[[nodiscard]] core::Result<Mat3x4> to_matrix(Quat rotation, Vec3 translation);
[[nodiscard]] core::Result<Vec3> transform_vector(const Mat3x4& matrix, Vec3 value);
[[nodiscard]] core::Result<Vec3> transform_point(const Mat3x4& matrix, Vec3 value);
[[nodiscard]] core::Result<Mat3x4> compose(const Mat3x4& left, const Mat3x4& right);

[[nodiscard]] constexpr Quat identity_quat() noexcept {
    return {Fixed{}, Fixed{}, Fixed{}, Fixed::from_raw(Fixed::scale)};
}

[[nodiscard]] constexpr Mat3x4 identity_matrix() noexcept {
    return {{{
        {Fixed::from_raw(Fixed::scale), Fixed{}, Fixed{}, Fixed{}},
        {Fixed{}, Fixed::from_raw(Fixed::scale), Fixed{}, Fixed{}},
        {Fixed{}, Fixed{}, Fixed::from_raw(Fixed::scale), Fixed{}},
    }}};
}

} // namespace eawr::sim::math
