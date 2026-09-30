#pragma once

#include "render_profile.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>

#include <cstdint>
#include <string>

// Applies a render profile to the root viewport and reads it back for the
// mode reports. Everything the viewer draws goes through the root viewport,
// so the host applies the profile once, before any mode starts.
namespace eawr::presentation::godot_backend {

namespace render_profile_detail {

[[nodiscard]] inline godot::Viewport::MSAA msaa(const std::uint32_t samples) noexcept {
    switch (samples) {
    case 2: return godot::Viewport::MSAA_2X;
    case 4: return godot::Viewport::MSAA_4X;
    case 8: return godot::Viewport::MSAA_8X;
    default: return godot::Viewport::MSAA_DISABLED;
    }
}

[[nodiscard]] inline godot::Viewport::AnisotropicFiltering anisotropy(const std::uint32_t ratio) noexcept {
    switch (ratio) {
    case 2: return godot::Viewport::ANISOTROPY_2X;
    case 4: return godot::Viewport::ANISOTROPY_4X;
    case 8: return godot::Viewport::ANISOTROPY_8X;
    case 16: return godot::Viewport::ANISOTROPY_16X;
    default: return godot::Viewport::ANISOTROPY_DISABLED;
    }
}

[[nodiscard]] inline RenderSettings settings_of(const godot::Viewport& viewport) {
    constexpr std::uint32_t msaa_samples[] = {0, 2, 4, 8};
    constexpr std::uint32_t ratios[] = {0, 2, 4, 8, 16};
    const auto sample_index = static_cast<std::size_t>(viewport.get_msaa_3d());
    const auto ratio_index = static_cast<std::size_t>(viewport.get_anisotropic_filtering_level());
    return RenderSettings{
        .msaa_samples = sample_index < 4 ? msaa_samples[sample_index] : 0U,
        .smaa = viewport.get_screen_space_aa() == godot::Viewport::SCREEN_SPACE_AA_SMAA,
        .anisotropy = ratio_index < 5 ? ratios[ratio_index] : 0U,
    };
}

} // namespace render_profile_detail

[[nodiscard]] inline RenderProfile active_render_profile() {
    auto* tree = godot::Object::cast_to<godot::SceneTree>(godot::Engine::get_singleton()->get_main_loop());
    const godot::Window* root = tree ? tree->get_root() : nullptr;
    return root && render_profile_detail::settings_of(*root) == render_settings(RenderProfile::enhanced)
        ? RenderProfile::enhanced : RenderProfile::retail;
}

inline void apply_render_profile(godot::Viewport& viewport, const RenderProfile profile) {
    const RenderSettings settings = render_settings(profile);
    viewport.set_msaa_3d(render_profile_detail::msaa(settings.msaa_samples));
    viewport.set_screen_space_aa(
        settings.smaa ? godot::Viewport::SCREEN_SPACE_AA_SMAA : godot::Viewport::SCREEN_SPACE_AA_DISABLED);
    viewport.set_anisotropic_filtering_level(render_profile_detail::anisotropy(settings.anisotropy));
}

// The root viewport's settings as a report object: the profile they match
// ("custom" for neither) and the values themselves.
[[nodiscard]] inline std::string render_profile_report() {
    auto* tree = godot::Object::cast_to<godot::SceneTree>(godot::Engine::get_singleton()->get_main_loop());
    godot::Window* root = tree ? tree->get_root() : nullptr;
    if (!root) return "null";
    const RenderSettings settings = render_profile_detail::settings_of(*root);
    const bool fxaa = root->get_screen_space_aa() == godot::Viewport::SCREEN_SPACE_AA_FXAA;
    const std::string_view name = fxaa ? "custom"
        : settings == render_settings(RenderProfile::retail) ? "retail"
        : settings == render_settings(RenderProfile::enhanced) ? "enhanced" : "custom";
    return "{\"name\": \"" + std::string(name) + "\", \"msaa_samples\": " + std::to_string(settings.msaa_samples)
        + ", \"screen_space_aa\": \"" + (settings.smaa ? "smaa" : fxaa ? "fxaa" : "disabled")
        + "\", \"anisotropic_filtering\": " + std::to_string(settings.anisotropy) + '}';
}

} // namespace eawr::presentation::godot_backend
