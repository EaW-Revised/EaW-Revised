#pragma once

#include "map_camera_test_support.hpp"

namespace eawr_map_camera_test {

input::RawEvent ctrl(input::RawEvent event);
input::RawEvent pointer(const float x, const float y);

} // namespace eawr_map_camera_test
