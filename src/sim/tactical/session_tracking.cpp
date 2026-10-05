#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_impl.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace eawr::sim::tactical {

namespace session_detail {

// A unit the tracking system holds (AV-01, research E71-12, E71-17): a ship with a motion
// profile in a space layer, or a SPACE_OBSTACLE type.
[[nodiscard]] const Footprint* tracked_footprint(const MotionTable& table, const TypeId type) noexcept {
    const auto* footprint = table.footprint(type);
    if (footprint == nullptr || footprint->layer == SpaceLayer::none) return nullptr;
    if (!footprint->obstacle && table.find(type) == nullptr) return nullptr;
    return footprint;
}

// `clip`: a waiting group member's plan frame; later frames predict the unit held where its plan
// puts it then (FM-10).
[[nodiscard]] core::Result<std::vector<Prediction>> sample_windows(const MotionState& motion, const std::uint64_t start,
    const std::uint32_t interval, const std::uint32_t windows, const math::Vec3 position, const math::Fixed yaw,
    const std::optional<std::uint64_t> clip) {
    std::vector<Prediction> result;
    result.reserve(windows + 1U);
    for (std::uint32_t index = 0; index <= windows; ++index) {
        auto frame = start + std::uint64_t{index} * interval;
        if (clip) frame = std::min(frame, *clip);
        auto at = predict(motion, frame, position, yaw);
        if (!at) return core::Result<std::vector<Prediction>>::failure(at.error());
        result.push_back(at.value());
    }
    return core::Result<std::vector<Prediction>>::success(std::move(result));
}

// PC-09 (#613): a ship whose sliced search has ended before its landing is predicted along
// `current` (at rest when null) up to the `landing` frame and along the search's `plan` from
// then on: what FoC's layer holds for a ship that has planned (PC-02).
[[nodiscard]] core::Result<std::vector<Prediction>> sample_ahead(const MotionState* current, const std::uint64_t landing,
    const MotionState& plan, const std::uint64_t start, const std::uint32_t interval, const std::uint32_t windows,
    const math::Vec3 position, const math::Fixed yaw) {
    std::vector<Prediction> result;
    result.reserve(windows + 1U);
    for (std::uint32_t index = 0; index <= windows; ++index) {
        const auto frame = start + std::uint64_t{index} * interval;
        core::Result<Prediction> at = core::Result<Prediction>::success(Prediction{position, yaw});
        if (frame >= landing) {
            at = predict(plan, frame, position, yaw);
        } else if (current != nullptr) {
            at = predict(*current, frame, position, yaw);
        }
        if (!at) return core::Result<std::vector<Prediction>>::failure(at.error());
        result.push_back(at.value());
    }
    return core::Result<std::vector<Prediction>>::success(std::move(result));
}

// Builds the view of dynamic layer `index` from the staged units and their samples: windows
// from this frame when a submission rebuilt the layer this tick, else from its rolled anchor.
[[nodiscard]] core::Result<void> ensure_view(Tracking& tracking, const MotionTable& table,
    const UnitStage& staged, const std::array<bool, 4>& rebuilt, const std::size_t index) {
    if (tracking.views[index]) return core::Result<void>::success();
    TrackingLayerView view;
    view.start_frame = rebuilt[index] ? tracking.frame : tracking.current_start[index];
    view.windows.resize(tracking.windows);
    for (const auto& [id, unit] : staged) {
        if (tracking.suspended.contains(id)) continue;
        const auto* footprint = tracked_footprint(table, unit.state.type_id);
        if (footprint == nullptr || dynamic_layer_index(footprint->layer) != index) continue;
        const std::vector<Prediction>* samples = nullptr;
        if (const auto found = tracking.samples.find(id); found != tracking.samples.end()) {
            const bool fresh = rebuilt[index] && !found->second.fresh.empty();
            samples = fresh ? &found->second.fresh : &found->second.current;
            if (samples->empty()) samples = nullptr;
        }
        auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const Prediction still{unit.state.position, yaw.value()};
        for (std::uint32_t window = 0; window < tracking.windows; ++window) {
            const auto& from = samples != nullptr ? (*samples)[window] : still;
            const auto& to = samples != nullptr ? (*samples)[window + 1U] : still;
            auto leaf = tracking_leaf(id, *footprint, from, to);
            if (!leaf) return core::Result<void>::failure(leaf.error());
            view.windows[window].push_back(leaf.value());
        }
    }
    tracking.views[index] = std::move(view);
    return core::Result<void>::success();
}

// The static layer's objects (one window; research E71-13, E71-17).
[[nodiscard]] core::Result<void> ensure_statics(
    Tracking& tracking, const MotionTable& table, const UnitStage& staged) {
    if (tracking.statics) return core::Result<void>::success();
    std::vector<TrackedLeaf> leaves;
    for (const auto& [id, unit] : staged) {
        if (tracking.suspended.contains(id)) continue;
        const auto* footprint = tracked_footprint(table, unit.state.type_id);
        if (footprint == nullptr || footprint->layer != SpaceLayer::static_object) continue;
        auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const Prediction still{unit.state.position, yaw.value()};
        auto leaf = tracking_leaf(id, *footprint, still, still);
        if (!leaf) return core::Result<void>::failure(leaf.error());
        leaves.push_back(leaf.value());
    }
    tracking.statics = std::move(leaves);
    return core::Result<void>::success();
}

// One frame of a unit's movement (MV-30 to MV-33): the unit goes where its plan puts it at
// `tick`, level at the sampled yaw; a finished plan leaves it at rest. Its roll banks with the
// frame's turn while it follows a plan and levels at rest (BK-02 to BK-04).
[[nodiscard]] core::Result<void> advance(LiveUnit& unit, const MotionProfile* profile, const std::uint64_t tick) {
    if (!unit.motion || profile == nullptr) {
        return core::Result<void>::success();
    }
    if (unit.motion->kind == MotionKind::none) {
        unit.speed = math::Fixed{};
        auto roll = level_roll(*profile, unit.roll);
        if (!roll) {
            return core::Result<void>::failure(roll.error());
        }
        unit.roll = roll.value();
        return core::Result<void>::success();
    }
    const auto yaw = yaw_degrees(unit.state.rotation);
    if (!yaw) {
        return core::Result<void>::failure(yaw.error());
    }
    const auto sample = sample_motion(*unit.motion, tick, unit.state.position, yaw.value());
    if (!sample) {
        return core::Result<void>::failure(sample.error());
    }
    unit.state.position = sample.value().position;
    if (sample.value().yaw != yaw.value() || !sample.value().finished) {
        auto rotation = yaw_rotation(sample.value().yaw);
        if (!rotation) {
            return core::Result<void>::failure(rotation.error());
        }
        unit.state.rotation = rotation.value();
    }
    // BK-02: every frame a plan moves the unit banks it; the frame a path ends does not
    // (MV-32), the last frame of a turn in place does unless the turn took no time.
    const auto& nodes = unit.motion->nodes;
    const bool banks = !sample.value().finished
        || (unit.motion->kind == MotionKind::turn && nodes.size() >= 2 && nodes.front().frame < nodes.back().frame);
    if (banks) {
        auto roll = bank_roll(*profile, unit.roll, yaw.value(), sample.value().yaw);
        if (!roll) {
            return core::Result<void>::failure(roll.error());
        }
        unit.roll = roll.value();
    }
    unit.speed = sample.value().speed;
    if (sample.value().finished) {
        unit.motion = MotionState{};
        unit.speed = math::Fixed{};
    }
    return core::Result<void>::success();
}

// One level placement matrix in the owning movement partition, shared by later phases.
[[nodiscard]] core::Result<math::Mat3x4> level_transform(LiveUnit& unit) {
    if (unit.placement_cache && unit.placement_cache->rotation == unit.state.rotation) {
        auto& cache = *unit.placement_cache;
        cache.position = unit.state.position;
        cache.matrix.rows[0][3] = cache.position.x;
        cache.matrix.rows[1][3] = cache.position.y;
        cache.matrix.rows[2][3] = cache.position.z;
        return core::Result<math::Mat3x4>::success(cache.matrix);
    }
    auto matrix = math::to_matrix(unit.state.rotation, unit.state.position);
    ++unit.matrix_builds;
    if (matrix) unit.placement_cache = PlacementCache{unit.state.position, unit.state.rotation, matrix.value()};
    return matrix;
}

// The snapshot placement additionally rolls the heading by its bank (BK-05).
[[nodiscard]] core::Result<math::Mat3x4> unit_transform(LiveUnit& unit) {
    if (unit.roll.raw() == 0) return level_transform(unit);
    ++unit.banked_matrix_builds;
    const auto rotation = banked_rotation(unit.state.rotation, unit.roll);
    if (!rotation) {
        return core::Result<math::Mat3x4>::failure(rotation.error());
    }
    return math::to_matrix(rotation.value(), unit.state.position);
}

} // namespace session_detail

core::Result<void> TacticalSession::Impl::plan_order(LiveUnit& unit, const CommandPayload& payload, const std::uint64_t start,
    const CollisionWorld* world) const {
        if (!unit.motion) {
            return core::Result<void>::success(); // MV-03: no motion profile, the unit stays
        }
        if (std::holds_alternative<StopPayload>(payload)) {
            unit.motion = MotionState{};
            unit.speed = math::Fixed{};
            return core::Result<void>::success();
        }
        const auto* move = std::get_if<MovePayload>(&payload);
        const auto* face = std::get_if<FacePayload>(&payload);
        if (move == nullptr && face == nullptr) {
            return core::Result<void>::success();
        }
        const auto& profile = *motion.find(unit.state.type_id);
        const auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) {
            return core::Result<void>::failure(yaw.error());
        }
        if (face != nullptr) {
            auto plan = plan_face(profile, start, unit.state.position, yaw.value(), face->target);
            if (!plan) {
                return core::Result<void>::failure(plan.error());
            }
            unit.motion = std::move(plan).value();
            unit.speed = math::Fixed{};
            return core::Result<void>::success();
        }
        return plan_path(unit, move->destination, start, world, std::nullopt, nullptr);
    }

core::Result<MotionProfile> TacticalSession::Impl::limits_of(const LiveUnit& unit) const {
        auto factor = math::Fixed::from_raw(math::Fixed::scale);
        if (unit.durability) {
            factor = max_speed_factor(*health_profile(unit), durability.rules, *unit.durability);
        }
        // AB-24: an active ability's speed multiplier scales them the same way.
        const auto speed = ability_factor(unit, AbilityModifier::speed);
        if (speed.raw() != math::Fixed::scale) {
            auto scaled = math::multiply(factor, speed);
            if (!scaled) return core::Result<MotionProfile>::failure(scaled.error());
            factor = scaled.value();
        }
        const auto upgraded_speed = math::multiply(factor,
            math::Fixed::from_raw(math::Fixed::scale + unit.upgrade_bonuses[5].raw()));
        if (!upgraded_speed) return core::Result<MotionProfile>::failure(upgraded_speed.error());
        factor = upgraded_speed.value();
        auto limits = scaled_profile(*motion.find(unit.state.type_id), factor);
        // IS-05: an ion stun cuts the maximum speed only (the ability phase clears an ended stun
        // before any plan of its tick).
        if (limits && unit.ion_stun) {
            const auto cut = math::Fixed::from_raw(math::Fixed::scale - unit.ion_stun->speed_reduction.raw());
            auto slowed = math::multiply(limits.value().max_speed, cut);
            if (!slowed) return core::Result<MotionProfile>::failure(slowed.error());
            auto profile = std::move(limits).value();
            profile.max_speed = slowed.value();
            return core::Result<MotionProfile>::success(std::move(profile));
        }
        return limits;
    }

core::Result<void> TacticalSession::Impl::plan_path(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
    const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats) const {
        auto planned = plan_path_within(unit, target, start, world, max_speed, stats, std::nullopt);
        if (!planned) return core::Result<void>::failure(planned.error());
        return core::Result<void>::success();
    }

core::Result<TacticalSession::Impl::PathInputs> TacticalSession::Impl::path_inputs(const LiveUnit& unit, const std::optional<math::Fixed> max_speed) const {
        const auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<PathInputs>::failure(yaw.error());
        auto limits = limits_of(unit);
        if (!limits) return core::Result<PathInputs>::failure(limits.error());
        if (max_speed) limits.value().max_speed = std::min(limits.value().max_speed, *max_speed);
        return core::Result<PathInputs>::success(PathInputs{unit.state.position, yaw.value(), unit.speed, limits.value()});
    }

void TacticalSession::Impl::take_plan(LiveUnit& unit, MotionState plan) {
        unit.motion = std::move(plan);
        if (unit.motion->kind == MotionKind::none) {
            unit.speed = math::Fixed{};
        }
    }

core::Result<bool> TacticalSession::Impl::plan_path_within(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
    const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats,
    const std::optional<std::uint64_t> budget) const {
        const auto inputs = path_inputs(unit, max_speed);
        if (!inputs) return core::Result<bool>::failure(inputs.error());
        const auto& in = inputs.value();
        if (max_speed && in.limits.max_speed.raw() <= 0) {
            unit.motion = MotionState{};
            unit.speed = math::Fixed{};
            return core::Result<bool>::success(true);
        }
        const auto* footprint = tracked_footprint(motion, unit.state.type_id);
        if (world != nullptr && footprint != nullptr && !footprint->obstacle) {
            // WHZ-08a: slot placement may move a formation destination out of a
            // hazard; filtering still tests the original point order endpoint.
            const auto& order = unit.state.order;
            const std::optional<math::Vec3> filter_destination = order.kind == OrderKind::move
                || ((order.kind == OrderKind::attack_move || order.kind == OrderKind::guard)
                    && order.target == invalid_entity_id)
                ? std::optional<math::Vec3>{order.destination} : std::nullopt;
            if (budget) {
                auto within = plan_space_move_within(motion, in.limits, *footprint, *world, unit.state.entity_id, start,
                    in.position, in.yaw, in.speed, target, *budget, stats, order.through_hazards, filter_destination);
                if (!within) return core::Result<bool>::failure(within.error());
                if (!within.value()) return core::Result<bool>::success(false);
                take_plan(unit, std::move(*within.value()));
                return core::Result<bool>::success(true);
            }
            auto plan = plan_space_move(
                motion, in.limits, *footprint, *world, unit.state.entity_id, start, in.position, in.yaw, in.speed, target, stats,
                PathSearchMode::bounded, order.through_hazards, filter_destination);
            if (!plan) return core::Result<bool>::failure(plan.error());
            take_plan(unit, std::move(plan).value());
            return core::Result<bool>::success(true);
        }
        auto plan = plan_move(in.limits, motion.rules, start, in.position, in.yaw, in.speed, target);
        if (!plan) return core::Result<bool>::failure(plan.error());
        take_plan(unit, std::move(plan).value());
        return core::Result<bool>::success(true);
    }

core::Result<TacticalSession::Impl::SlicedSearch> TacticalSession::Impl::start_sliced(const LiveUnit& unit, const math::Vec3 target,
    const std::optional<math::Fixed> max_speed, const std::uint64_t now, const std::uint64_t frame,
    const CollisionWorld& world) const {
        // The movement phase of each tick up to `frame` advances the unit along its plan.
        LiveUnit ahead = unit;
        for (auto tick = now + 1; tick <= frame; ++tick) {
            if (auto advanced = advance(ahead, motion.find(unit.state.type_id), tick); !advanced) {
                return core::Result<SlicedSearch>::failure(advanced.error());
            }
        }
        auto inputs = path_inputs(ahead, max_speed);
        if (!inputs) return core::Result<SlicedSearch>::failure(inputs.error());
        const auto& in = inputs.value();
        const auto* footprint = tracked_footprint(motion, unit.state.type_id);
        const auto& order = unit.state.order;
        const std::optional<math::Vec3> filter_destination = order.kind == OrderKind::move
            || ((order.kind == OrderKind::attack_move || order.kind == OrderKind::guard)
                && order.target == invalid_entity_id)
            ? std::optional<math::Vec3>{order.destination} : std::nullopt;
        SlicedPathSearch search(motion, in.limits, *footprint, world, unit.state.entity_id, frame, in.position, in.yaw,
            in.speed, target, order.through_hazards, filter_destination);
        return core::Result<SlicedSearch>::success(SlicedSearch{std::move(search), frame, target, max_speed, in, std::nullopt});
    }

EntityId TacticalSession::Impl::approach_target(const LiveUnit& unit) noexcept {
        if (!unit.motion) return invalid_entity_id;
        const auto& order = unit.state.order;
        switch (order.kind) {
        case OrderKind::attack:
            if (unit.combat && !(unit.combat->direct && unit.combat->attack_target == order.target)) {
                return invalid_entity_id;
            }
            return order.target;
        case OrderKind::attack_move:
        case OrderKind::guard:
            return order.target;
        default:
            return invalid_entity_id;
        }
    }

bool TacticalSession::Impl::approach_due(const LiveUnit& unit, const std::uint64_t tick) const noexcept {
        const auto issued = unit.state.order.issued_tick;
        return approach_target(unit) != invalid_entity_id && tick > issued
            && (tick - issued) % motion.rules.reevaluation_frames == 0;
    }

math::Fixed TacticalSession::Impl::approach_range(const LiveUnit& unit, const bool guard,
    const detail::CombatUnit* target) const noexcept {
        const auto* profile = combat.find(unit.state.type_id);
        auto range = detail::approach_distance(
            guard, profile != nullptr ? profile->max_attack_distance : std::nullopt, motion.rules.guard_range);
        if (guard || target == nullptr || profile == nullptr || !profile->max_attack_distance) return range;
        // OR-03: slot mapping uses the soft radius for a hardpoint aim, otherwise the
        // smaller hard extent. These validated dimensions sum well within Q24 bounds.
        auto extension = detail::target_soft_radius(&motion, *target);
        if (!detail::has_aim_hardpoint(*target)) {
            extension = {};
            if (const auto* footprint = motion.footprint(target->type_id))
                extension = std::min(footprint->x_extent, footprint->y_extent);
            else if (target->profile != nullptr && target->profile->collision) {
                const auto& box = *target->profile->collision;
                extension = math::Fixed::from_raw(std::min(box.max.x.raw() - box.min.x.raw(),
                    box.max.y.raw() - box.min.y.raw()) / 2);
            }
        }
        return math::Fixed::from_raw(range.raw() + extension.raw());
    }

bool TacticalSession::Impl::target_hardpoint_standing(const LiveUnit& target, const std::uint32_t index) const {
        const auto* profile = combat.find(target.state.type_id);
        if (profile == nullptr) return false;
        const auto hardpoint = std::find_if(profile->hardpoints.begin(), profile->hardpoints.end(),
            [index](const TargetHardpoint& entry) { return entry.hardpoint == index && entry.targetable; });
        if (hardpoint == profile->hardpoints.end()) return false;
        const auto* health = health_profile(target);
        return health == nullptr || !target.durability || index >= health->hardpoints.size()
            || !hardpoint_destroyed(*health, *target.durability, index);
    }

math::Vec3 TacticalSession::Impl::approach_point(const bool guard, const LiveUnit& target,
    const detail::CombatUnit* target_view, const math::Vec3& from, const std::uint32_t hardpoint) {
        if (guard || target_view == nullptr) return target.state.position;
        return detail::ordered_aim_point(*target_view, from, hardpoint);
    }

core::Result<math::Vec3> TacticalSession::Impl::movement_end(const LiveUnit& unit) {
        if (!unit.motion || unit.motion->kind != MotionKind::path || unit.motion->nodes.size() < 2) {
            return core::Result<math::Vec3>::success(unit.state.position);
        }
        const auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<math::Vec3>::failure(yaw.error());
        const auto last = std::max<std::int64_t>(unit.motion->nodes.back().frame.raw(), 0);
        const auto end_tick = static_cast<std::uint64_t>((last + math::Fixed::scale - 1) / math::Fixed::scale);
        auto end = predict(*unit.motion, std::max(end_tick, unit.motion->start_tick), unit.state.position, yaw.value());
        if (!end) return core::Result<math::Vec3>::failure(end.error());
        return core::Result<math::Vec3>::success(end.value().position);
    }

core::Result<math::Vec3> TacticalSession::Impl::predicted_position(
    const LiveUnit& target, const std::uint64_t frame, const std::uint64_t at) {
        if (at <= frame || !target.motion || target.motion->kind != MotionKind::path) {
            return core::Result<math::Vec3>::success(target.state.position);
        }
        const auto yaw = yaw_degrees(target.state.rotation);
        if (!yaw) return core::Result<math::Vec3>::failure(yaw.error());
        auto sample = sample_motion(*target.motion, at, target.state.position, yaw.value());
        if (!sample) return core::Result<math::Vec3>::failure(sample.error());
        return core::Result<math::Vec3>::success(sample.value().position);
    }

core::Result<TacticalSession::Impl::ApproachPlan> TacticalSession::Impl::approach_mapping(
    const LiveUnit& unit, const LiveUnit& target, const math::Fixed range, const std::uint64_t frame) const {
        const auto limits = limits_of(unit);
        if (!limits) return core::Result<ApproachPlan>::failure(limits.error());
        const auto target_yaw = yaw_degrees(target.state.rotation);
        if (!target_yaw) return core::Result<ApproachPlan>::failure(target_yaw.error());
        // The target's top speed on its current path; zero at rest or turning in place.
        math::Fixed target_speed{};
        if (target.motion && target.motion->kind == MotionKind::path) {
            for (const auto& node : target.motion->nodes) target_speed = std::max(target_speed, node.speed);
        }
        ApproachPlan plan;
        plan.prediction_frame = detail::prediction_frame(frame, unit.state.position, limits.value().max_speed,
            target.state.position, target_yaw.value(), target_speed, range);
        const auto predicted = predicted_position(target, frame, plan.prediction_frame);
        if (!predicted) return core::Result<ApproachPlan>::failure(predicted.error());
        auto slot = detail::approach_slot(unit.state.position, predicted.value(), range);
        if (!slot) return core::Result<ApproachPlan>::failure(slot.error());
        plan.slot = slot.value();
        return core::Result<ApproachPlan>::success(plan);
    }

core::Result<std::optional<TacticalSession::Impl::ApproachPlan>> TacticalSession::Impl::approach_check(const LiveUnit& unit, const LiveUnit& target,
    const detail::CombatUnit* target_view, const std::uint64_t frame) const {
        using Checked = core::Result<std::optional<ApproachPlan>>;
        const bool guard = unit.state.order.kind == OrderKind::guard;
        const auto range = approach_range(unit, guard, target_view);
        const auto now = approach_point(guard, target, target_view, unit.state.position,
            unit.state.order.kind == OrderKind::attack ? unit.state.order.hardpoint : attack_hull);
        auto reach = core::Result<math::Fixed>::success(range);
        if (!guard && target_view != nullptr) {
            const auto* profile = combat.find(unit.state.type_id);
            reach = detail::target_attack_distance(&motion, *target_view, unit.state.position,
                profile != nullptr ? profile->max_attack_distance.value_or(math::Fixed{}) : math::Fixed{},
                detail::has_aim_hardpoint(*target_view));
        }
        if (!reach) return Checked::failure(reach.error());
        if (within_range(unit.state.position, now, reach.value(), RangeMetric::planar)) return Checked::success(std::nullopt);
        const auto end = movement_end(unit);
        if (!end) return Checked::failure(end.error());
        const auto predicted =
            predicted_position(target, frame, unit.approach ? unit.approach->prediction_frame : std::uint64_t{0});
        if (!predicted) return Checked::failure(predicted.error());
        if (within_range(end.value(), predicted.value(), range, RangeMetric::planar)) {
            return Checked::success(std::nullopt);
        }
        auto plan = approach_mapping(unit, target, range, frame);
        if (!plan) return Checked::failure(plan.error());
        return Checked::success(plan.value());
    }

} // namespace eawr::sim::tactical
