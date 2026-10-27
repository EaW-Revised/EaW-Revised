#pragma once

#include "eawr/sim/tactical/combat.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace eawr::units::detail {

// HPT-09: observed maximum 0.000911215 (FoC/Remake/RaW); 1/256 is ~4.3x that.
// This is an admission safety bound, not a replacement for the authored directions.
inline constexpr std::int64_t authored_turret_shear_limit_raw = sim::math::Fixed::scale / 256;

inline core::Result<sim::math::Fixed> turret_frame_shear(const std::array<sim::math::Vec3, 3>& axes) {
    std::int64_t maximum{};
    for (std::size_t left = 0; left < axes.size(); ++left) {
        for (std::size_t right = left + 1; right < axes.size(); ++right) {
            auto product = sim::math::dot(axes[left], axes[right]);
            if (!product) return product;
            maximum = std::max<std::int64_t>(maximum, std::llabs(product.value().raw()));
        }
    }
    return core::Result<sim::math::Fixed>::success(sim::math::Fixed::from_raw(maximum));
}

// Input axes have already been individually normalized. Preserve the existing band
// byte-for-byte. In the second band use modified Gram-Schmidt in X, Y, Z order;
// retain X and the handedness of Z. Every operation is checked, deterministic Q24.
inline core::Result<bool> condition_turret_frame(std::array<sim::math::Vec3, 3>& axes) {
    using Result = core::Result<bool>;
    auto shear = turret_frame_shear(axes);
    if (!shear) return Result::failure(shear.error());
    if (shear.value().raw() <= sim::tactical::frame_tolerance_raw) return Result::success(false);
    if (shear.value().raw() > authored_turret_shear_limit_raw) {
        core::Diagnostic error;
        error.code = "EAWR-UNITS-0002";
        error.message = "HPT-09: authored turret frame shear exceeds 1/256";
        return Result::failure(std::move(error));
    }
    auto conditioned = axes;
    for (std::size_t axis = 1; axis < conditioned.size(); ++axis) {
        auto value = conditioned[axis];
        for (std::size_t previous = 0; previous < axis; ++previous) {
            const auto& basis = conditioned[previous];
            auto projection = sim::math::dot(value, basis);
            if (!projection) return Result::failure(projection.error());
            const auto component = [&](const sim::math::Fixed v, const sim::math::Fixed b) {
                auto amount = sim::math::multiply(projection.value(), b);
                return amount ? sim::math::subtract(v, amount.value()) : amount;
            };
            auto x = component(value.x, basis.x);
            auto y = component(value.y, basis.y);
            auto z = component(value.z, basis.z);
            if (!x) return Result::failure(x.error());
            if (!y) return Result::failure(y.error());
            if (!z) return Result::failure(z.error());
            value = {x.value(), y.value(), z.value()};
        }
        auto normalized = sim::math::normalize(value);
        if (!normalized) return Result::failure(normalized.error());
        conditioned[axis] = normalized.value();
    }
    axes = conditioned;
    return Result::success(true);
}

} // namespace eawr::units::detail
