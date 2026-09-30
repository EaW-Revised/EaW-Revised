#pragma once

// Private to the camera input translation units (camera_input.cpp and
// camera_binding_config.cpp): the helpers both share.
#include "camera_input.hpp"

#include <string>
#include <string_view>

namespace eawr::viewer::camera_input {
namespace camera_input_detail {

[[nodiscard]] core::Diagnostic failure(std::string_view code, std::string message);
[[nodiscard]] bool finite(float value) noexcept;

} // namespace camera_input_detail

using namespace camera_input_detail;

} // namespace eawr::viewer::camera_input
