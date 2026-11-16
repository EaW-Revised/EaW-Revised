#pragma once

#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/instance.hpp"
#include "eawr/units/attack_clip.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

// #6554, WSPL-08: the SUPER_LASER's attack clip in the viewer. The simulation times the beam by the clip
// `<model stem>_attack_NN.ala` (NN = Attack_Anim_Sub_Index), at the clip's own rate, held two frames short of its end.
// The viewer plays the same file from the same start tick, so the shown frame is the frame the fire stage waited for.
namespace eawr::presentation::godot_backend::laser_clip {

// The attack clip a type's SUPER_LASER handler selects, or empty when the type has none or its model has no such file.
// It is the simulation's own resolution (units::attack_clip_path: the model stem and the index, by number), so both read
// the same clip even when the numbering has a gap.
template <typename Exists>
[[nodiscard]] inline std::string attack_clip_path(const units::UnitType& type, const std::string& model_path, const Exists& exists) {
    const auto handler = std::find_if(type.special_abilities.begin(), type.special_abilities.end(), [](const auto& entry) {
        return entry.kind == sim::tactical::SpecialAbilityKind::super_laser;
    });
    if (handler == type.special_abilities.end() || handler->attack_clip_frames == 0) return {};
    auto path = units::attack_clip_path(model_path, handler->laser_anim_sub_index);
    return !path.empty() && exists(path) ? path : std::string{};
}

// The clip frame the sim's gate shows `ticks` frames after the clip started: the clip plays once at its rate and is held
// at its last playable frame.
[[nodiscard]] inline double clip_frame(const double elapsed_ticks, const double frames_per_second,
                                       const std::uint32_t playable_frames, const double ticks_per_second) noexcept {
    const double last = playable_frames > 0 ? static_cast<double>(playable_frames - 1U) : 0.0;
    return std::clamp(std::max(0.0, elapsed_ticks) * frames_per_second / ticks_per_second, 0.0, last);
}

// Whether the status asks for the attack clip now: from the clip start through the continuation.
[[nodiscard]] inline bool clip_shown(const sim::tactical::AbilityStatus& status) noexcept {
    return status.kind == sim::tactical::AbilityKind::super_laser && status.active
        && (status.laser_phase == sim::tactical::LaserPhase::clip || status.laser_phase == sim::tactical::LaserPhase::fired
            || status.laser_phase == sim::tactical::LaserPhase::continuing);
}

} // namespace eawr::presentation::godot_backend::laser_clip
