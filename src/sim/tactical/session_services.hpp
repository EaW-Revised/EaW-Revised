#pragma once

#include "session_impl.hpp"
#include "motion_internal.hpp"

namespace eawr::sim::tactical::session_detail {

[[nodiscard]] std::string command_context(const CommandKey& key);

void apply_squadron_order(SquadronState& state, const CommandPayload& payload, const math::Vec3& position,
    const SquadronTable& table);

[[nodiscard]] const Footprint* tracked_footprint(const MotionTable& table, const TypeId type) noexcept;

[[nodiscard]] core::Result<std::vector<Prediction>> sample_windows(const MotionState& motion, const std::uint64_t start,
    const std::uint32_t interval, const std::uint32_t windows, const math::Vec3 position, const math::Fixed yaw,
    const std::optional<std::uint64_t> clip = std::nullopt);

[[nodiscard]] core::Result<std::vector<Prediction>> sample_ahead(const MotionState* current, const std::uint64_t landing,
    const MotionState& plan, const std::uint64_t start, const std::uint32_t interval, const std::uint32_t windows,
    const math::Vec3 position, const math::Fixed yaw);

[[nodiscard]] core::Result<void> ensure_view(Tracking& tracking, const MotionTable& table,
    const UnitStage& staged, const std::array<bool, 4>& rebuilt, const std::size_t index);

[[nodiscard]] core::Result<void> ensure_statics(
    Tracking& tracking, const MotionTable& table, const UnitStage& staged);

[[nodiscard]] core::Result<void> advance(LiveUnit& unit, const MotionProfile* profile, const std::uint64_t tick);

[[nodiscard]] core::Result<math::Mat3x4> level_transform(LiveUnit& unit);

[[nodiscard]] core::Result<math::Mat3x4> unit_transform(LiveUnit& unit);

// The first frame of the window that holds `frame` when windows roll every `interval` frames
// from `anchor` (the object tracking layer's service step, research E71-13).
[[nodiscard]] constexpr std::uint64_t rolled_start(
    const std::uint64_t anchor, const std::uint64_t frame, const std::uint32_t interval) noexcept {
    return frame <= anchor ? anchor : anchor + (frame - anchor) / interval * interval;
}

// Tracked units in the dynamic layers, as (layer, ID) in ascending order: a change marks the
// layer for rebuilding like a submission (AV-03).
template <typename Units>
[[nodiscard]] std::vector<std::pair<std::size_t, EntityId>> layer_members(const MotionTable& table, const Units& units) {
    std::vector<std::pair<std::size_t, EntityId>> members;
    for (const auto& unit : units) {
        if (const auto* footprint = tracked_footprint(table, unit.state.type_id)) {
            if (const auto index = dynamic_layer_index(footprint->layer)) {
                members.emplace_back(*index, unit.state.entity_id);
            }
        }
    }
    std::sort(members.begin(), members.end());
    return members;
}

} // namespace eawr::sim::tactical::session_detail
