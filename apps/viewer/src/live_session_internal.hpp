#pragma once

#include "live_session_view.hpp"

namespace eawr::presentation::godot_backend::live_session_detail {

// How far a driven frame's presentation tick may sit from a capture tick it shows.
constexpr double capture_tolerance = 1.0e-6;

[[nodiscard]] std::string json(std::string_view text);
[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text);
[[nodiscard]] std::optional<double> parse_double(const std::string& text);
[[nodiscard]] std::string lower_path(std::string_view text);
[[nodiscard]] float to_float(sim::math::Fixed value);
[[nodiscard]] std::string tag_text(const data::EffectiveObject& object, std::string_view tag);
[[nodiscard]] std::string space_model_path(const data::EffectiveObject& object);

} // namespace eawr::presentation::godot_backend::live_session_detail
