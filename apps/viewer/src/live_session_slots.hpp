#pragma once

// Draw-slot and clone state, including death playback and S-foil clips.

#include "space_populate.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

namespace live_session {

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
