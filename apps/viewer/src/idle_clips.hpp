#pragma once

#include "eawr/presentation/animation/idle_playback.hpp"
#include "eawr/scene/idle_tags.hpp"
#include "eawr/scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

// Idle clips of placed objects, shared by the land and space populate paths
// (#145 space, #157 land). The rule itself is engine-free in
// presentation/animation/idle_playback.hpp; this is only the glue from a
// scene placement and its XML tags to that rule.
namespace eawr::presentation::godot_backend {

// One animated placement: its bound clip, its start frame and the playback
// its XML declares. Instances of the placement index it.
struct IdlePlacement final {
    std::size_t clip{};
    std::uint32_t start_frame{};
    animation::IdlePlayback playback{};
};

// The playback `tags` declare. An Idle_Anim_00_Rate_Mod that does not parse
// keeps rate 1 and is returned in `rejected_rate` for the report.
struct DeclaredIdle final {
    animation::IdlePlayback playback{};
    std::optional<std::string> rejected_rate;
};
[[nodiscard]] inline DeclaredIdle declared_idle(const scene::IdleTags& tags) {
    DeclaredIdle declared;
    declared.playback.loop = tags.loop;
    declared.playback.restarts = tags.idle_behavior;
    declared.playback.random_start = !tags.dummy_starship;
    if (!tags.rate_mod.empty()) {
        if (const auto rate = animation::parse_rate_mod(tags.rate_mod)) declared.playback.rate = *rate;
        else declared.rejected_rate = tags.rate_mod;
    }
    return declared;
}

// Retail draws the start frame at random; here it is a hash of the
// placement's map path and TED record, so every capture repeats.
[[nodiscard]] inline std::uint32_t placement_start_frame(const scene::Placement& placement, const std::uint32_t frames) {
    return animation::idle_start_frame(
        placement.map_logical_path + "#" + std::to_string(placement.record_ordinal), frames);
}

// The live view (--eawr-camera-interactive) runs the idle clips' 30 Hz clock
// on real time: `seconds` accumulates the positive finite frame deltas.
[[nodiscard]] inline std::uint32_t live_idle_tick(double& seconds, const double delta) {
    if (std::isfinite(delta) && delta > 0.0) seconds += delta;
    return static_cast<std::uint32_t>(std::min(std::floor(seconds * 30.0),
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

inline constexpr std::string_view idle_rule =
    "retail model creation: Idle_Anim_00 plays at Idle_Anim_00_Rate_Mod from a random "
    "frame (here a hash of the TED placement), looping when Loop_Idle_Anim_00 is set; the IDLE behaviour "
    "restarts a finished clip from frame 0 at rate 1";

} // namespace eawr::presentation::godot_backend
