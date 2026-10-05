#include "space_effect_support.hpp"

namespace eawr_space_test {

using eawr::assets::ParameterKind;
using eawr::assets::Vec4f;
bool same(const Vec4f& a, const Vec4f& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
bool near(const float a, const float b, const float tolerance) { return std::abs(a - b) <= tolerance; }


} // namespace eawr_space_test
