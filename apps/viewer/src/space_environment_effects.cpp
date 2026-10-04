#include "space_environment_internal.hpp"
#include "eawr/core/load_profile.hpp"
#include "render_profile_viewport.hpp"
#include "shutdown_trace.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"

namespace eawr::presentation::godot_backend::space_environment_detail {

bool EnvironmentView::initialize_idle() {
    // Size the sampled pose and every per-instance palette once at load.
    const std::uint64_t initial_tick = options_.clock_offset;
    for (EnvironmentIdle& idle : environment_idle_) {
        auto initialized = idle.pose.advance(*idle.player, idle.playback.playback, idle.playback.start_frame,
            initial_tick, particles::map_owner_ticks_per_second, idle.instances,
            [this](const auto entity, const auto asset, const auto& bones) {
                return renderer_->set_skin_pose(entity, asset, bones);
            });
        if (!initialized) return fail("environment idle initialization: " + core::format_diagnostic(initialized.error()));
    }
    return true;
}

std::optional<int> EnvironmentView::advance_effects(const std::uint32_t tick) {
    // The effects' TIME runs on the same clock as the idle clips: held after
    // the particle frames in a fixed capture, plus the idle offset (#185).
    const std::uint64_t effect_tick = static_cast<std::uint64_t>(options_.real_time_clock
        ? tick : std::min(tick, options_.clock_hold_ticks - 1U)) + options_.clock_offset;
    if (effect_tick_ != effect_tick) {
        effect_tick_ = effect_tick;
        const float time = space::environment_effect_time(effect_tick);
        for (const sim::AssetId asset : effect_clock_assets_) {
            if (auto set = renderer_->set_material_scalar(asset, "eawr_effect_time", time); !set) {
                completed_ = true;
                static_cast<void>(fail("environment effect clock: " + core::format_diagnostic(set.error())));
                return 2;
            }
        }
        for (EnvironmentIdle& idle : environment_idle_) {
            auto pose = idle.pose.advance(*idle.player, idle.playback.playback,
                idle.playback.start_frame, effect_tick, particles::map_owner_ticks_per_second, idle.instances,
                [this](const auto entity, const auto asset, const auto& bones) {
                    return renderer_->set_skin_pose(entity, asset, bones);
                });
            if (!pose) {
                completed_ = true;
                static_cast<void>(fail("environment idle sample: " + core::format_diagnostic(pose.error())));
                return 2;
            }
        }
    }
    return std::nullopt;
}

bool EnvironmentView::bind_idle(const scene::Placement& placement, const assets::Model& model, const vfs::Vfs& filesystem, Placed& placed) {
            if (!placement.idle_animation.empty()) {
                auto clip = assets::load_animation(filesystem, placement.idle_animation);
                if (!clip) return fail("environment idle clip: " + core::format_diagnostic(clip.error()));
                auto player = animation::Player::create(model, &clip.value());
                if (!player) return fail("environment idle binding: " + core::format_diagnostic(player.error()));
                EnvironmentIdle idle;
                idle.object = placement.object_id;
                idle.clip_path = placement.idle_animation;
                idle.player = std::make_shared<const animation::Player>(std::move(player.value()));
                idle.playback.start_frame = placement_start_frame(placement, idle.player->playable_frames());
                if (auto object = options_.catalog->resolve(placement.object_id)) {
                    idle.playback.playback = declared_idle(scene::idle_tags(object.value(), assets::MapKind::space)).playback;
                }
                placed.idle = environment_idle_.size();
                environment_idle_.push_back(std::move(idle));
            }
    return true;
}

} // namespace eawr::presentation::godot_backend::space_environment_detail
