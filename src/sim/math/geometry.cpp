#include "eawr/sim/math/geometry.hpp"

#include "wide.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::sim::math {
namespace {

core::Diagnostic error(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    return diagnostic;
}

core::Result<Fixed> overflow_fixed() {
    return core::Result<Fixed>::failure(error(
        diagnostic_codes::overflow, "geometry result is outside the Q24 raw domain"));
}

core::Result<Fixed> finish(detail::SignedWide sum) {
    std::int64_t raw = 0;
    if (!detail::rounded_shift_to_raw(sum, Fixed::fractional_bits, raw)) {
        return overflow_fixed();
    }
    return core::Result<Fixed>::success(Fixed::from_raw(raw));
}

detail::SignedWide sum_products(
    const std::int64_t* left, const std::int64_t* right, std::size_t count) {
    detail::SignedWide sum{};
    for (std::size_t index = 0; index < count; ++index) {
        sum.add(detail::SignedWide::product(left[index], right[index]));
    }
    return sum;
}

detail::UInt192 squared_sum(const std::int64_t* values, std::size_t count) {
    detail::UInt192 sum{};
    for (std::size_t index = 0; index < count; ++index) {
        static_cast<void>(detail::add_magnitude(
            sum, detail::multiply_u64(
                     detail::unsigned_magnitude(values[index]),
                     detail::unsigned_magnitude(values[index]))));
    }
    return sum;
}

// The length as length_from rounds it, in 128 bits when the squared sum is below 2^126 (#520);
// false when it is not (length_from then takes the 192-bit form).
bool length_128(const std::int64_t* values, std::size_t count, std::int64_t& raw) noexcept {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t magnitude = detail::unsigned_magnitude(values[index]);
        std::uint64_t square_high = 0;
        std::uint64_t square_low = 0;
        detail::multiply_64(magnitude, magnitude, square_high, square_low);
        low += square_low;
        high += square_high + (low < square_low ? 1U : 0U);
        if (high >= std::uint64_t{1} << 62U) {
            return false;
        }
    }
    std::uint64_t root = detail::floor_sqrt_128(high, low);
    if (detail::sqrt_rounds_up(high, low, root)) {
        ++root;
    }
    if (root > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    raw = static_cast<std::int64_t>(root);
    return true;
}

// dot(Vec2)'s sum of two products rounded by 2^24, in 128 bits while the rounded magnitude
// stays below 2^62 (#520); false otherwise (dot then takes the 192-bit form).
bool dot_128(const std::int64_t a0, const std::int64_t b0, const std::int64_t a1, const std::int64_t b1,
    std::int64_t& raw) noexcept {
    struct Term {
        std::uint64_t high{};
        std::uint64_t low{};
        bool negative{};
    };
    const auto product = [](const std::int64_t left, const std::int64_t right) {
        Term term;
        detail::multiply_64(detail::unsigned_magnitude(left), detail::unsigned_magnitude(right), term.high, term.low);
        term.negative = (left < 0) != (right < 0);
        return term;
    };
    Term sum = product(a0, b0);
    const Term other = product(a1, b1);
    if (sum.negative == other.negative) {
        sum.low += other.low;
        sum.high += other.high + (sum.low < other.low ? 1U : 0U);
    } else if (sum.high > other.high || (sum.high == other.high && sum.low >= other.low)) {
        sum.high -= other.high + (sum.low < other.low ? 1U : 0U);
        sum.low -= other.low;
    } else {
        const std::uint64_t low = other.low - sum.low;
        sum.high = other.high - sum.high - (other.low < sum.low ? 1U : 0U);
        sum.low = low;
        sum.negative = other.negative;
    }
    constexpr unsigned shift = Fixed::fractional_bits;
    if ((sum.high >> shift) != 0) {
        return false;
    }
    std::uint64_t quotient = (sum.low >> shift) | (sum.high << (64U - shift));
    const std::uint64_t remainder = sum.low & ((std::uint64_t{1} << shift) - 1U);
    const std::uint64_t half = std::uint64_t{1} << (shift - 1U);
    if (remainder > half || (remainder == half && (quotient & 1U) != 0)) {
        ++quotient;
    }
    if (quotient >= std::uint64_t{1} << 62U) {
        return false;
    }
    raw = sum.negative ? -static_cast<std::int64_t>(quotient) : static_cast<std::int64_t>(quotient);
    return true;
}

// The rounded length; false on overflow.
bool length_raw(const std::int64_t* values, std::size_t count, std::int64_t& raw) noexcept {
    if (length_128(values, count, raw)) {
        return true;
    }
    const detail::UInt192 sum = squared_sum(values, count);
    bool exceeds = false;
    std::uint64_t root = detail::floor_sqrt(sum, exceeds);
    if (exceeds) {
        return false;
    }
    detail::UInt192 four_sum{};
    static_cast<void>(detail::multiply_by_u64(sum, 4U, four_sum));
    if (root != std::numeric_limits<std::uint64_t>::max()) {
        const std::uint64_t midpoint = root * 2U + 1U;
        if (detail::compare(four_sum, detail::multiply_u64(midpoint, midpoint)) > 0) {
            ++root;
        }
    }
    return detail::magnitude_to_raw(detail::from_u64(root), false, raw);
}

core::Result<Fixed> length_from(const std::int64_t* values, std::size_t count) {
    std::int64_t raw = 0;
    if (!length_raw(values, count, raw)) {
        return overflow_fixed();
    }
    return core::Result<Fixed>::success(Fixed::from_raw(raw));
}

std::int64_t normalized_component(std::int64_t component, detail::UInt192 norm_square) {
    if (component == 0) {
        return 0;
    }
    const detail::UInt192 numerator = detail::shift_left_u64(
        detail::unsigned_magnitude(component), Fixed::fractional_bits);
    detail::UInt192 numerator_square{};
    static_cast<void>(detail::multiply(numerator, numerator, numerator_square));

    std::uint64_t low = 0;
    std::uint64_t high = static_cast<std::uint64_t>(Fixed::scale);
    std::uint64_t floor_value = 0;
    while (low <= high) {
        const std::uint64_t middle = low + (high - low) / 2U;
        detail::UInt192 product{};
        static_cast<void>(detail::multiply_by_u64(
            norm_square, middle * middle, product));
        if (detail::compare(product, numerator_square) <= 0) {
            floor_value = middle;
            low = middle + 1U;
        } else {
            if (middle == 0) {
                break;
            }
            high = middle - 1U;
        }
    }

    detail::UInt192 four_numerator_square{};
    static_cast<void>(detail::multiply_by_u64(
        numerator_square, 4U, four_numerator_square));
    const std::uint64_t midpoint = floor_value * 2U + 1U;
    detail::UInt192 midpoint_product{};
    static_cast<void>(detail::multiply_by_u64(
        norm_square, midpoint * midpoint, midpoint_product));
    const int relation = detail::compare(four_numerator_square, midpoint_product);
    if (relation > 0 || (relation == 0 && (floor_value & 1U) != 0)) {
        ++floor_value;
    }
    const auto signed_value = static_cast<std::int64_t>(floor_value);
    return component < 0 ? -signed_value : signed_value;
}

core::Result<Vec2> normalize_impl(Vec2 value) {
    const std::array<std::int64_t, 2> raw{value.x.raw(), value.y.raw()};
    const detail::UInt192 square = squared_sum(raw.data(), raw.size());
    if (square.is_zero()) {
        return core::Result<Vec2>::failure(error(
            diagnostic_codes::zero_normalization, "cannot normalize the zero vector"));
    }
    return core::Result<Vec2>::success({
        Fixed::from_raw(normalized_component(raw[0], square)),
        Fixed::from_raw(normalized_component(raw[1], square)),
    });
}

core::Result<Vec3> normalize_impl(Vec3 value) {
    const std::array<std::int64_t, 3> raw{value.x.raw(), value.y.raw(), value.z.raw()};
    const detail::UInt192 square = squared_sum(raw.data(), raw.size());
    if (square.is_zero()) {
        return core::Result<Vec3>::failure(error(
            diagnostic_codes::zero_normalization, "cannot normalize the zero vector"));
    }
    return core::Result<Vec3>::success({
        Fixed::from_raw(normalized_component(raw[0], square)),
        Fixed::from_raw(normalized_component(raw[1], square)),
        Fixed::from_raw(normalized_component(raw[2], square)),
    });
}

core::Result<Quat> normalize_impl(Quat value) {
    const std::array<std::int64_t, 4> raw{
        value.x.raw(), value.y.raw(), value.z.raw(), value.w.raw()};
    const detail::UInt192 square = squared_sum(raw.data(), raw.size());
    if (square.is_zero()) {
        return core::Result<Quat>::failure(error(
            diagnostic_codes::zero_normalization, "cannot normalize the zero quaternion"));
    }
    return core::Result<Quat>::success({
        Fixed::from_raw(normalized_component(raw[0], square)),
        Fixed::from_raw(normalized_component(raw[1], square)),
        Fixed::from_raw(normalized_component(raw[2], square)),
        Fixed::from_raw(normalized_component(raw[3], square)),
    });
}

core::Result<Fixed> quat_component(
    std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d,
    std::int64_t e, std::int64_t f, std::int64_t g, std::int64_t h,
    std::array<bool, 4> negative) {
    const std::array<std::int64_t, 4> left{a, c, e, g};
    const std::array<std::int64_t, 4> right{b, d, f, h};
    detail::SignedWide sum{};
    for (std::size_t index = 0; index < 4; ++index) {
        auto term = detail::SignedWide::product(left[index], right[index]);
        term.negative = term.negative != negative[index];
        sum.add(term);
    }
    return finish(sum);
}

core::Result<Fixed> matrix_rotation_component(
    detail::SignedWide first, detail::SignedWide second, detail::SignedWide third) {
    first.add(second);
    first.add(third);
    return finish(first);
}

} // namespace

core::Result<Fixed> dot(Vec2 left, Vec2 right) {
    if (std::int64_t raw = 0; dot_128(left.x.raw(), right.x.raw(), left.y.raw(), right.y.raw(), raw)) {
        return core::Result<Fixed>::success(Fixed::from_raw(raw));
    }
    const std::array<std::int64_t, 2> a{left.x.raw(), left.y.raw()};
    const std::array<std::int64_t, 2> b{right.x.raw(), right.y.raw()};
    return finish(sum_products(a.data(), b.data(), a.size()));
}

core::Result<Fixed> dot(Vec3 left, Vec3 right) {
    const std::array<std::int64_t, 3> a{left.x.raw(), left.y.raw(), left.z.raw()};
    const std::array<std::int64_t, 3> b{right.x.raw(), right.y.raw(), right.z.raw()};
    return finish(sum_products(a.data(), b.data(), a.size()));
}

core::Result<Fixed> dot(Quat left, Quat right) {
    const std::array<std::int64_t, 4> a{
        left.x.raw(), left.y.raw(), left.z.raw(), left.w.raw()};
    const std::array<std::int64_t, 4> b{
        right.x.raw(), right.y.raw(), right.z.raw(), right.w.raw()};
    return finish(sum_products(a.data(), b.data(), a.size()));
}

core::Result<Vec3> cross(Vec3 left, Vec3 right) {
    auto component = [](std::int64_t a, std::int64_t b,
                        std::int64_t c, std::int64_t d) {
        auto sum = detail::SignedWide::product(a, b);
        auto term = detail::SignedWide::product(c, d);
        term.negative = !term.negative;
        sum.add(term);
        return finish(sum);
    };
    auto x = component(left.y.raw(), right.z.raw(), left.z.raw(), right.y.raw());
    auto y = component(left.z.raw(), right.x.raw(), left.x.raw(), right.z.raw());
    auto z = component(left.x.raw(), right.y.raw(), left.y.raw(), right.x.raw());
    if (!x) return core::Result<Vec3>::failure(x.error());
    if (!y) return core::Result<Vec3>::failure(y.error());
    if (!z) return core::Result<Vec3>::failure(z.error());
    return core::Result<Vec3>::success({x.value(), y.value(), z.value()});
}

core::Result<Fixed> length(Vec2 value) {
    const std::array<std::int64_t, 2> raw{value.x.raw(), value.y.raw()};
    return length_from(raw.data(), raw.size());
}

bool try_length(const Vec2 value, Fixed& result) noexcept {
    const std::array<std::int64_t, 2> values{value.x.raw(), value.y.raw()};
    std::int64_t raw = 0;
    if (!length_raw(values.data(), values.size(), raw)) {
        return false;
    }
    result = Fixed::from_raw(raw);
    return true;
}

bool try_dot(const Vec2 left, const Vec2 right, Fixed& result) noexcept {
    std::int64_t raw = 0;
    if (dot_128(left.x.raw(), right.x.raw(), left.y.raw(), right.y.raw(), raw)) {
        result = Fixed::from_raw(raw);
        return true;
    }
    const std::array<std::int64_t, 2> a{left.x.raw(), left.y.raw()};
    const std::array<std::int64_t, 2> b{right.x.raw(), right.y.raw()};
    if (!detail::rounded_shift_to_raw(sum_products(a.data(), b.data(), a.size()), Fixed::fractional_bits, raw)) {
        return false;
    }
    result = Fixed::from_raw(raw);
    return true;
}

core::Result<Fixed> length(Vec3 value) {
    const std::array<std::int64_t, 3> raw{value.x.raw(), value.y.raw(), value.z.raw()};
    return length_from(raw.data(), raw.size());
}

core::Result<Fixed> length(Quat value) {
    const std::array<std::int64_t, 4> raw{
        value.x.raw(), value.y.raw(), value.z.raw(), value.w.raw()};
    return length_from(raw.data(), raw.size());
}

core::Result<Vec2> normalize(Vec2 value) { return normalize_impl(value); }
core::Result<Vec3> normalize(Vec3 value) { return normalize_impl(value); }
core::Result<Quat> normalize(Quat value) { return normalize_impl(value); }

core::Result<Quat> compose(Quat left, Quat right) {
    auto x = quat_component(
        left.w.raw(), right.x.raw(), left.x.raw(), right.w.raw(),
        left.y.raw(), right.z.raw(), left.z.raw(), right.y.raw(),
        {false, false, false, true});
    auto y = quat_component(
        left.w.raw(), right.y.raw(), left.x.raw(), right.z.raw(),
        left.y.raw(), right.w.raw(), left.z.raw(), right.x.raw(),
        {false, true, false, false});
    auto z = quat_component(
        left.w.raw(), right.z.raw(), left.x.raw(), right.y.raw(),
        left.y.raw(), right.x.raw(), left.z.raw(), right.w.raw(),
        {false, false, true, false});
    auto w = quat_component(
        left.w.raw(), right.w.raw(), left.x.raw(), right.x.raw(),
        left.y.raw(), right.y.raw(), left.z.raw(), right.z.raw(),
        {false, true, true, true});
    if (!x) return core::Result<Quat>::failure(x.error());
    if (!y) return core::Result<Quat>::failure(y.error());
    if (!z) return core::Result<Quat>::failure(z.error());
    if (!w) return core::Result<Quat>::failure(w.error());
    return core::Result<Quat>::success({x.value(), y.value(), z.value(), w.value()});
}

core::Result<Mat3x4> to_matrix(Quat q, Vec3 translation) {
    const std::array<std::int64_t, 4> values{q.x.raw(), q.y.raw(), q.z.raw(), q.w.raw()};
    const detail::UInt192 norm = squared_sum(values.data(), values.size());
    const detail::UInt192 one = detail::multiply_u64(Fixed::scale, Fixed::scale);
    detail::UInt192 difference = detail::compare(norm, one) >= 0 ? norm : one;
    detail::subtract_magnitude(difference, detail::compare(norm, one) >= 0 ? one : norm);
    if (detail::compare(
            difference,
            detail::from_u64(static_cast<std::uint64_t>(8 * Fixed::scale))) > 0) {
        return core::Result<Mat3x4>::failure(error(
            diagnostic_codes::non_unit_quaternion,
            "quaternion squared norm differs from one by more than eight quanta"));
    }

    auto product = [](std::int64_t a, std::int64_t b, bool negate_term = false) {
        auto value = detail::SignedWide::product(a, b);
        value.negative = value.negative != negate_term;
        return value;
    };
    auto diagonal = [&](std::int64_t a, std::int64_t b) {
        auto base = detail::SignedWide::product(Fixed::scale, Fixed::scale);
        auto aa = product(a, a, true);
        aa.add(product(a, a, true));
        auto bb = product(b, b, true);
        bb.add(product(b, b, true));
        return matrix_rotation_component(base, aa, bb);
    };
    auto twice_pair = [&](std::int64_t a, std::int64_t b,
                          std::int64_t c, std::int64_t d, bool subtract_second) {
        auto first = product(a, b);
        first.add(product(a, b));
        auto second = product(c, d, subtract_second);
        second.add(product(c, d, subtract_second));
        return matrix_rotation_component(first, second, {});
    };

    Mat3x4 matrix{};
    auto assign = [&](std::size_t row, std::size_t column, core::Result<Fixed> value) -> bool {
        if (!value) return false;
        matrix.rows[row][column] = value.value();
        return true;
    };
    if (!assign(0, 0, diagonal(q.y.raw(), q.z.raw()))
        || !assign(0, 1, twice_pair(q.x.raw(), q.y.raw(), q.z.raw(), q.w.raw(), true))
        || !assign(0, 2, twice_pair(q.x.raw(), q.z.raw(), q.y.raw(), q.w.raw(), false))
        || !assign(1, 0, twice_pair(q.x.raw(), q.y.raw(), q.z.raw(), q.w.raw(), false))
        || !assign(1, 1, diagonal(q.x.raw(), q.z.raw()))
        || !assign(1, 2, twice_pair(q.y.raw(), q.z.raw(), q.x.raw(), q.w.raw(), true))
        || !assign(2, 0, twice_pair(q.x.raw(), q.z.raw(), q.y.raw(), q.w.raw(), true))
        || !assign(2, 1, twice_pair(q.y.raw(), q.z.raw(), q.x.raw(), q.w.raw(), false))
        || !assign(2, 2, diagonal(q.x.raw(), q.y.raw()))) {
        return core::Result<Mat3x4>::failure(
            error(diagnostic_codes::overflow, "quaternion matrix component overflow"));
    }
    matrix.rows[0][3] = translation.x;
    matrix.rows[1][3] = translation.y;
    matrix.rows[2][3] = translation.z;
    return core::Result<Mat3x4>::success(matrix);
}

core::Result<Vec3> transform_vector(const Mat3x4& matrix, Vec3 value) {
    const std::array<std::int64_t, 3> input{value.x.raw(), value.y.raw(), value.z.raw()};
    Vec3 output{};
    std::array<Fixed*, 3> destination{&output.x, &output.y, &output.z};
    for (std::size_t row = 0; row < 3; ++row) {
        const std::array<std::int64_t, 3> coefficients{
            matrix.rows[row][0].raw(), matrix.rows[row][1].raw(), matrix.rows[row][2].raw()};
        auto result = finish(sum_products(coefficients.data(), input.data(), input.size()));
        if (!result) return core::Result<Vec3>::failure(result.error());
        *destination[row] = result.value();
    }
    return core::Result<Vec3>::success(output);
}

core::Result<Vec3> transform_point(const Mat3x4& matrix, Vec3 value) {
    const std::array<std::int64_t, 4> input{
        value.x.raw(), value.y.raw(), value.z.raw(), Fixed::scale};
    Vec3 output{};
    std::array<Fixed*, 3> destination{&output.x, &output.y, &output.z};
    for (std::size_t row = 0; row < 3; ++row) {
        const std::array<std::int64_t, 4> coefficients{
            matrix.rows[row][0].raw(), matrix.rows[row][1].raw(),
            matrix.rows[row][2].raw(), matrix.rows[row][3].raw()};
        auto result = finish(sum_products(coefficients.data(), input.data(), input.size()));
        if (!result) return core::Result<Vec3>::failure(result.error());
        *destination[row] = result.value();
    }
    return core::Result<Vec3>::success(output);
}

core::Result<Mat3x4> compose(const Mat3x4& left, const Mat3x4& right) {
    Mat3x4 output{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            detail::SignedWide sum{};
            const std::size_t terms = column == 3 ? 4 : 3;
            for (std::size_t index = 0; index < terms; ++index) {
                const std::int64_t right_raw = index == 3
                    ? Fixed::scale : right.rows[index][column].raw();
                sum.add(detail::SignedWide::product(left.rows[row][index].raw(), right_raw));
            }
            auto value = finish(sum);
            if (!value) return core::Result<Mat3x4>::failure(value.error());
            output.rows[row][column] = value.value();
        }
    }
    return core::Result<Mat3x4>::success(output);
}

} // namespace eawr::sim::math
