#pragma once

#include "ai_engine.hpp"

namespace eawr::script::foc::ai::engine_internal {

using authoritative::ScriptEvent;
using authoritative::Value;
using authoritative::ValueList;

inline constexpr std::int64_t fps = tactical::logical_frames_per_second;
extern const Real huge;

Real fixed(math::Fixed value);
std::int64_t next_frame(std::int64_t frame, Real delay);
std::string text_of(const Value& value);
std::optional<std::uint64_t> handle_id(const Value& value, std::uint32_t kind);
std::optional<Real> number_of(const Value& value);
std::string show(Real value);
Real xy_distance_squared(const math::Vec3& a, Real x, Real y);
std::string members_of(const std::vector<sim::EntityId>& units);

} // namespace eawr::script::foc::ai::engine_internal
