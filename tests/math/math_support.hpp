#pragma once

#include "eawr/sim/math/math.hpp"
#include "../../src/sim/math/cordic_constants.hpp"
#include "../../src/sim/math/wide.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace math_test_support {

using namespace eawr::sim::math;
extern int failures;



void expect(bool condition, std::string_view message);
void wide_limb_tests();
void wide_fast_path_tests();
void fixed_fast_path_tests();


} // namespace math_test_support
