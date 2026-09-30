#include "eawr/sim/math/math.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>

namespace {

using eawr::sim::math::Fixed;
using eawr::sim::math::Mat3x4;
using eawr::sim::math::Quat;
using eawr::sim::math::Vec2;
using eawr::sim::math::Vec3;

void print_fixed(const Fixed& value) { std::cout << value.raw(); }

template <typename T>
void print_vec(const T& value);

template <>
void print_vec(const Vec2& value) {
    print_fixed(value.x); std::cout << ' '; print_fixed(value.y);
}

template <>
void print_vec(const Vec3& value) {
    print_fixed(value.x); std::cout << ' '; print_fixed(value.y); std::cout << ' ';
    print_fixed(value.z);
}

template <>
void print_vec(const Quat& value) {
    print_fixed(value.x); std::cout << ' '; print_fixed(value.y); std::cout << ' ';
    print_fixed(value.z); std::cout << ' '; print_fixed(value.w);
}

void print_matrix(const Mat3x4& value) {
    for (const auto& row : value.rows) {
        for (const auto& cell : row) { std::cout << ' '; print_fixed(cell); }
    }
}

template <typename T>
void print_result(const eawr::core::Result<T>& result, const T& value) {
    if (!result) { std::cout << "ERR " << result.error().code << '\n'; return; }
    std::cout << "OK";
    if constexpr (std::is_same_v<T, Fixed>) {
        std::cout << ' '; print_fixed(value);
    } else if constexpr (std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3>
                         || std::is_same_v<T, Quat>) {
        std::cout << ' '; print_vec(value);
    } else if constexpr (std::is_same_v<T, Mat3x4>) {
        print_matrix(value);
    }
    std::cout << '\n';
}

template <typename T>
void print_result(const eawr::core::Result<T>& result) {
    if (!result) { std::cout << "ERR " << result.error().code << '\n'; return; }
    print_result(result, result.value());
}

std::int64_t next_i64(std::istringstream& input) {
    std::int64_t value = 0;
    input >> value;
    return value;
}

Vec2 vec2(std::istringstream& input) {
    return {Fixed::from_raw(next_i64(input)), Fixed::from_raw(next_i64(input))};
}

Vec2 vec2_pair(std::istringstream& input, Vec2& right) {
    const Vec2 left = vec2(input); right = vec2(input); return left;
}

Vec3 vec3(std::istringstream& input) {
    return {Fixed::from_raw(next_i64(input)), Fixed::from_raw(next_i64(input)),
            Fixed::from_raw(next_i64(input))};
}

Vec3 vec3_pair(std::istringstream& input, Vec3& right) {
    const Vec3 left = vec3(input); right = vec3(input); return left;
}

Quat quat(std::istringstream& input) {
    return {Fixed::from_raw(next_i64(input)), Fixed::from_raw(next_i64(input)),
            Fixed::from_raw(next_i64(input)), Fixed::from_raw(next_i64(input))};
}

Quat quat_pair(std::istringstream& input, Quat& right) {
    const Quat left = quat(input); right = quat(input); return left;
}

Mat3x4 matrix(std::istringstream& input) {
    Mat3x4 value{};
    for (auto& row : value.rows) for (auto& cell : row) cell = Fixed::from_raw(next_i64(input));
    return value;
}

void run(const std::string& line) {
    std::istringstream input(line);
    std::string op;
    input >> op;
    if (op == "decimal") {
        std::string text; input.get(); std::getline(input, text);
        print_result(Fixed::from_decimal(text)); return;
    }
    if (op == "integer") { print_result(Fixed::from_integer(next_i64(input))); return; }
    if (op == "ratio") { const auto a=next_i64(input); const auto b=next_i64(input); print_result(Fixed::from_ratio(a, b)); return; }
    if (op == "add") { const auto a=next_i64(input); const auto b=next_i64(input); print_result(eawr::sim::math::add(Fixed::from_raw(a), Fixed::from_raw(b))); return; }
    if (op == "sub") { const auto a=next_i64(input); const auto b=next_i64(input); print_result(eawr::sim::math::subtract(Fixed::from_raw(a), Fixed::from_raw(b))); return; }
    if (op == "neg") { print_result(eawr::sim::math::negate(Fixed::from_raw(next_i64(input)))); return; }
    if (op == "mul") { const auto a=next_i64(input); const auto b=next_i64(input); print_result(eawr::sim::math::multiply(Fixed::from_raw(a), Fixed::from_raw(b))); return; }
    if (op == "div") { const auto a=next_i64(input); const auto b=next_i64(input); print_result(eawr::sim::math::divide(Fixed::from_raw(a), Fixed::from_raw(b))); return; }
    if (op == "sqrt") { print_result(eawr::sim::math::sqrt(Fixed::from_raw(next_i64(input)))); return; }
    if (op == "wrap" || op == "sin" || op == "cos") {
        const Fixed value = Fixed::from_raw(next_i64(input));
        if (op == "wrap") { std::cout << "OK " << eawr::sim::math::wrap_turn(value).raw() << '\n'; return; }
        if (op == "sin") { std::cout << "OK " << eawr::sim::math::sin_turn(value).raw() << '\n'; return; }
        std::cout << "OK " << eawr::sim::math::cos_turn(value).raw() << '\n'; return;
    }
    if (op == "atan2") { const auto y=next_i64(input); const auto x=next_i64(input); print_result(eawr::sim::math::atan2_turn(Fixed::from_raw(y), Fixed::from_raw(x))); return; }
    if (op == "dot2") { Vec2 right{}; const Vec2 left=vec2_pair(input, right); print_result(eawr::sim::math::dot(left, right)); return; }
    if (op == "dot3") { Vec3 right{}; const Vec3 left=vec3_pair(input, right); print_result(eawr::sim::math::dot(left, right)); return; }
    if (op == "dot4") { Quat right{}; const Quat left=quat_pair(input, right); print_result(eawr::sim::math::dot(left, right)); return; }
    if (op == "cross") { Vec3 right{}; const Vec3 left=vec3_pair(input, right); print_result(eawr::sim::math::cross(left, right)); return; }
    if (op == "len2") { print_result(eawr::sim::math::length(vec2(input))); return; }
    if (op == "len3") { print_result(eawr::sim::math::length(vec3(input))); return; }
    if (op == "len4") { print_result(eawr::sim::math::length(quat(input))); return; }
    if (op == "norm2") { print_result(eawr::sim::math::normalize(vec2(input))); return; }
    if (op == "norm3") { print_result(eawr::sim::math::normalize(vec3(input))); return; }
    if (op == "norm4") { print_result(eawr::sim::math::normalize(quat(input))); return; }
    if (op == "composeq") { print_result(eawr::sim::math::compose(quat(input), quat(input))); return; }
    if (op == "tomatrix") {
        const Quat q = quat(input); const Vec3 t = vec3(input);
        print_result(eawr::sim::math::to_matrix(q, t)); return;
    }
    if (op == "transformv" || op == "transformp") {
        const Mat3x4 m = matrix(input); const Vec3 v = vec3(input);
        if (op == "transformv") print_result(eawr::sim::math::transform_vector(m, v));
        else print_result(eawr::sim::math::transform_point(m, v));
        return;
    }
    if (op == "composem") {
        const Mat3x4 left = matrix(input); const Mat3x4 right = matrix(input);
        print_result(eawr::sim::math::compose(left, right)); return;
    }
    std::cout << "ERR UNKNOWN\n";
}

} // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty()) run(line);
    }
}
