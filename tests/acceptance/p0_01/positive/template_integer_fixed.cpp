#include <cstdint>

namespace eawr::sim {

template <typename T>
T multiply_fixed(T left, T right) {
    return (left * right) >> 16;
}

std::int64_t integer_template_case(const std::int64_t left, const std::int64_t right) {
    return multiply_fixed(left, right);
}

} // namespace eawr::sim
