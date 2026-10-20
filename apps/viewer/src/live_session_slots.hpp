#pragma once

// Draw-slot and clone state, including death playback and S-foil clips.

#include "space_populate.hpp"
#include "eawr/battle/arrival_plan.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

namespace live_session {

struct ArrivalSlotState final {
    battle::ArrivalPlan plan;
    const vfs::Vfs* filesystem{};
    const data::Catalog* catalog{};
    SpacePopulation* population{};
    GodotRenderer* renderer{};
    std::size_t remaining{}, grown{};
    std::vector<sim::EntityId> undrawn;
    std::set<std::pair<sim::tactical::PlayerId, sim::tactical::TypeId>> purchases;
    std::set<std::pair<sim::tactical::PlayerId, sim::tactical::TypeId>> failed;
    std::string failure;
};

struct CloneFrame final {
    SpacePopulation::LivePose pose;
    std::optional<animation::DeathFrame> death;
};

struct LaunchSlot {
    sim::tactical::TypeId type{};
    std::size_t ship{};
    bool bound{};
    std::optional<std::size_t> clone;  // index in launch_clones_
    sim::tactical::TypeId required_station{};
    sim::tactical::PlayerId station_owner{};
    // #4218: the slot as the reservation policy would hold it (terminal-retired sources stay reserved).
    // Only the seed identity reads this; `bound` is the real binding, which frees at once for a terminal source.
    bool shadow_bound{};
};

// #4218: the slot a launched unit takes its particle seeds and idle start from, and whether the shadow
// pool holds it now (false after the unit released it, or when the physical slot stands in for the shadow).
struct SlotIdentity final {
    std::size_t ship{};
    bool held{};
};

struct PlacementClone {
    sim::tactical::TypeId type{};
    std::size_t ship{};
    sim::math::Vec3 offset{};
    sim::math::Fixed layer_z{};
};

struct DeathClone final {
    std::size_t ship{};
    std::string type;
    std::string clip;  // empty: no clip of the type; the clone keeps its pose
    bool remove_upon_death{};
    animation::DeathPlayback playback{};
};

struct ActiveClone final {
    sim::EntityId unit{};
    std::uint64_t death_tick{};  // the first completed tick without the unit
    SpacePopulation::LivePose pose;
    // Elapsed clone tick after emitter catch-up approved terminal retirement.
    std::optional<std::uint64_t> hidden_since;
};

struct SFoil final {
    bool on{};
    bool started{};
    bool alternate{};
    double since{};
    std::uint32_t start_frame{};
};

} // namespace live_session
} // namespace eawr::presentation::godot_backend
