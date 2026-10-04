#pragma once

#include "space_test_support.hpp"

namespace eawr_space_test {

using eawr::assets::ParameterKind;
using eawr::assets::Vec4f;
bool same(const Vec4f& a, const Vec4f& b);
bool near(const float a, const float b, const float tolerance = 1.0e-5F);
void make_meshgloss(eawr::assets::Submesh& submesh);

} // namespace eawr_space_test
