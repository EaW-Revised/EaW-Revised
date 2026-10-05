#include "map_mode_internal.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "legacy/registry.hpp"

namespace eawr::presentation::godot_backend {

void MapMode::State::pose_units(const std::uint32_t sample) {
    if (!effect_clock_assets.empty() && effect_sample != sample) {
        effect_sample = sample;
        const float time = space::environment_effect_time(static_cast<std::uint64_t>(sample) + idle_offset.value_or(0U));
        for (const sim::AssetId asset : effect_clock_assets) {
            if (auto set = renderer->set_material_scalar(asset, "eawr_effect_time", time); !set
                && first_surface_failure.empty()) first_surface_failure = core::format_diagnostic(set.error());
        }
    }
    if (idle_placements.empty() || unit_sample == sample) return;
    unit_sample = sample;
    const std::uint64_t tick = static_cast<std::uint64_t>(sample) + idle_offset.value_or(0U);
    std::vector<std::optional<animation::Pose>> poses(idle_placements.size());
    for (std::size_t index = 0; index < idle_placements.size(); ++index) {
        const IdlePlacement& idle = idle_placements[index];
        auto pose = animation::sample_idle(*unit_clips[idle.clip], idle.playback, idle.start_frame, tick,
                                           particles::map_owner_ticks_per_second);
        if (pose) poses[index] = std::move(pose.value());
        else ++idle_sample_failures;
    }
    for (const AnimatedInstance& instance : animated_instances) {
        if (!poses[instance.idle]) continue;
        const auto posed = renderer->set_skin_pose(instance.entity, instance.asset, poses[instance.idle]->bones);
        if (!posed && first_surface_failure.empty()) first_surface_failure = core::format_diagnostic(posed.error());
    }
}

void MapMode::State::advance_wind(const double delta) {
    if (!renderer) return;
    const std::uint32_t offset = idle_offset.value_or(0U);
    if (options.interactive) {
        // Real frame time from the idle offset on, by the scene clock rule (W-01).
        wind_clock_seconds = frame <= 1U
            ? lighting::wind::clock_at(static_cast<double>(offset)
                                       / static_cast<double>(particles::map_owner_ticks_per_second))
            : lighting::wind::advance_clock(wind_clock_seconds, static_cast<float>(delta));
    } else {
        // The idle clips' held sample, so a capture repeats.
        const std::uint32_t sample = std::min(frame, options.particle_frames) - 1U;
        wind_clock_seconds = lighting::wind::clock_at((static_cast<double>(sample) + static_cast<double>(offset))
                                                      / static_cast<double>(particles::map_owner_ticks_per_second));
    }
    // Source (x, y, z) -> render (x, z, -y).
    const assets::Vec3f source = wind ? wind->vector : assets::Vec3f{};
    renderer->set_wind({.wind = {source.x, source.z, -source.y}, .scene_time = wind_clock_seconds});
}

} // namespace eawr::presentation::godot_backend
