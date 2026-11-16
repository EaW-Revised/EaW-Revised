#pragma once

#include "render_profile.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/presentation/godot/adapter_memory.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// Applies a render profile to the root viewport and reads it back for the
// mode reports. The battle modes draw in the root viewport, so the host applies the profile once, before
// any mode starts. The galactic modes and the planet loading screen draw their 3D scenes in SubViewports
// (a viewport has its own msaa_3d / screen_space_aa / anisotropic level, off by default): each registers
// the viewport it creates with register_render_viewport() and the profile follows it from then on.
namespace eawr::presentation::godot_backend {

namespace render_profile_detail {

// Main-thread count of render-target/filter setters, for menu-cycle regression receipts.
[[nodiscard]] inline std::uint64_t& apply_count() noexcept {
    static std::uint64_t count{};
    return count;
}

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
        .supersampling = static_cast<float>(viewport.get_scaling_3d_scale()),
    };
}

} // namespace render_profile_detail

// The profile the viewer runs, the scene it shows and the evidence override, kept so that a later apply
// (the options screen, options mode, a scene change) recomputes the same decision (#6972).
// Touched on the main thread only, so it needs no lock.
struct RenderState final {
    std::optional<RenderProfile> profile;
    RenderScene scene{RenderScene::general};
    AaOverride override_aa{};
    float supersampling{1.0F};
    AdapterMemory memory{};
    bool budget_dirty{};
    std::uint64_t budget_viewports{};
    std::unordered_map<std::uint64_t, AaViewportState> targets;
    std::vector<std::uint64_t> extra_viewports; // SubViewports that draw 3D, by object id (registered at creation)
};

[[nodiscard]] inline RenderState& render_state() noexcept {
    static RenderState state{};
    return state;
}

[[nodiscard]] inline godot::Window* render_root() {
    auto* loop = godot::Engine::get_singleton() ? godot::Engine::get_singleton()->get_main_loop() : nullptr;
    auto* tree = godot::Object::cast_to<godot::SceneTree>(loop);
    return tree ? tree->get_root() : nullptr;
}

// The profile in force: what was applied, else (before any apply) what the viewport matches.
[[nodiscard]] inline RenderProfile active_render_profile() {
    if (render_state().profile) return *render_state().profile;
    const godot::Window* root = render_root();
    return root && render_profile_detail::settings_of(*root) == render_settings(RenderProfile::medium)
        ? RenderProfile::medium : RenderProfile::retail;
}

// A one-shot render-thread query is polled without a wait. A completed sample reapplies all targets.
inline void update_render_memory() {
    auto& state = render_state();
    if (!state.memory.pending) return;
    auto memory = query_adapter_memory();
    if (!memory.pending) {
        state.memory = std::move(memory);
        state.budget_dirty = true;
    }
}

inline void refresh_render_memory() {
    auto& state = render_state();
    state.memory = query_adapter_memory(true);
    state.budget_dirty = state.memory.pending;
}

[[nodiscard]] inline AaBudgetDecision viewport_aa_budget(const RenderSettings requested, const godot::Vector2 size,
    const std::uint64_t previous_bytes = 0) {
    const auto& state = render_state();
    return budgeted_render_settings(requested, static_cast<std::uint32_t>(std::max(0.0F, size.x)),
        static_cast<std::uint32_t>(std::max(0.0F, size.y)), state.memory.device_local_bytes,
        state.extra_viewports.size() + 1, state.memory.budget_bytes, previous_bytes);
}

// Every apply uses the requested level + override, followed by the same safety decision.
inline void apply_render_state(godot::Viewport& viewport) {
    RenderState& state = render_state();
    if (state.memory.pending) return; // Keep the old targets until the fresh render-thread sample completes.
    const RenderSettings requested = effective_render_settings(
        state.profile.value_or(RenderProfile::retail), state.scene, state.override_aa, state.supersampling);
    const auto size = viewport.get_texture()->get_size();
    const auto id = static_cast<std::uint64_t>(viewport.get_instance_id());
    const auto decision = apply_render_state(state.targets[id], requested,
        render_profile_detail::settings_of(viewport), static_cast<std::uint32_t>(std::max(0.0F, size.x)),
        static_cast<std::uint32_t>(std::max(0.0F, size.y)), state.memory.device_local_bytes,
        static_cast<std::uint64_t>(godot::Engine::get_singleton()->get_process_frames()),
        state.extra_viewports.size() + 1, state.memory.budget_bytes);
    const auto settings = decision.settings;
    const float scale = settings.supersampling;
    if (decision.clamped || !decision.fits) {
        godot::UtilityFunctions::print("EAWR AA budget: SS=", requested.supersampling, " -> ", scale,
            " MSAA=", requested.msaa_samples, " -> ", settings.msaa_samples,
            " estimate=", static_cast<int64_t>(decision.estimated_bytes),
            " transient=", static_cast<int64_t>(decision.transient_bytes),
            " allowance=", static_cast<int64_t>(decision.limit_bytes), " source=", state.memory.source.c_str());
    }
    // Above native resolution the pinned engine supports bilinear only.
    if (viewport.get_scaling_3d_mode() != godot::Viewport::SCALING_3D_MODE_BILINEAR) {
        viewport.set_scaling_3d_mode(godot::Viewport::SCALING_3D_MODE_BILINEAR);
        ++render_profile_detail::apply_count();
    }
    if (viewport.get_scaling_3d_scale() != scale) {
        viewport.set_scaling_3d_scale(scale);
        ++render_profile_detail::apply_count();
    }
    const auto msaa = render_profile_detail::msaa(settings.msaa_samples);
    const auto aa = settings.smaa ? godot::Viewport::SCREEN_SPACE_AA_SMAA : godot::Viewport::SCREEN_SPACE_AA_DISABLED;
    const auto anisotropy = render_profile_detail::anisotropy(settings.anisotropy);
    if (viewport.get_msaa_3d() != msaa) {
        viewport.set_msaa_3d(msaa);
        ++render_profile_detail::apply_count();
    }
    if (viewport.get_screen_space_aa() != aa) {
        viewport.set_screen_space_aa(aa);
        ++render_profile_detail::apply_count();
    }
    if (viewport.get_anisotropic_filtering_level() != anisotropy) {
        viewport.set_anisotropic_filtering_level(anisotropy);
        ++render_profile_detail::apply_count();
    }
}

// Pushes the state to every registered SubViewport that is still alive and forgets the freed ones.
inline void apply_render_state_to_registered() {
    auto& ids = render_state().extra_viewports;
    ids.erase(std::remove_if(ids.begin(), ids.end(), [](const std::uint64_t id) {
        auto* viewport = godot::Object::cast_to<godot::Viewport>(godot::ObjectDB::get_instance(id));
        if (viewport == nullptr) {
            render_state().targets.erase(id);
            render_state().budget_dirty = true;
            return true;
        }
        apply_render_state(*viewport);
        return false;
    }), ids.end());
    render_state().budget_viewports = ids.size() + 1;
}

// A SubViewport that draws a 3D scene takes the profile: applied now (the state in force) and again on every
// later apply (the options screen, a scene change). Registering twice is harmless.
inline void register_render_viewport(godot::Viewport& viewport) {
    auto& ids = render_state().extra_viewports;
    const auto id = static_cast<std::uint64_t>(viewport.get_instance_id());
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) return;
    ids.push_back(id);
    refresh_render_memory();
    // A new target shares the same total allowance; existing targets may need to step down.
    if (auto* root = render_root()) apply_render_state(*root);
    apply_render_state_to_registered();
}

// A registered SubViewport that is about to be freed leaves the list now (a viewport that is freed without it is
// pruned at the next apply).
inline void unregister_render_viewport(const godot::Viewport& viewport) {
    auto& ids = render_state().extra_viewports;
    const auto id = static_cast<std::uint64_t>(viewport.get_instance_id());
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    render_state().targets.erase(id);
    refresh_render_memory();
    if (auto* root = render_root(); root && root->is_inside_tree()) {
        apply_render_state(*root);
        apply_render_state_to_registered();
    }
}

// Resize can increase the allocation after Options applied a level. Check dimensions only;
// unchanged viewports do not incur render setters or repeated clamp messages.
inline void refresh_render_viewport_sizes() {
    auto& state = render_state();
    if (!state.profile) return;
    update_render_memory();
    if (state.memory.pending) return;
    const auto resized = [](godot::Viewport& viewport) {
        const auto found = render_state().targets.find(static_cast<std::uint64_t>(viewport.get_instance_id()));
        const auto size = viewport.get_texture()->get_size();
        return found == render_state().targets.end()
            || found->second.width != static_cast<std::uint32_t>(std::max(0.0F, size.x))
            || found->second.height != static_cast<std::uint32_t>(std::max(0.0F, size.y));
    };
    bool changed_size = render_root() && resized(*render_root());
    for (const auto id : state.extra_viewports)
        if (auto* viewport = godot::Object::cast_to<godot::Viewport>(godot::ObjectDB::get_instance(id)))
            changed_size = changed_size || resized(*viewport);
    if (changed_size && !state.budget_dirty) {
        refresh_render_memory();
        if (state.memory.pending) return;
        state.budget_dirty = true;
    }
    const bool recover = std::any_of(state.targets.begin(), state.targets.end(),
        [](const auto& entry) { return entry.second.recover; });
    if (recover || state.budget_dirty || state.budget_viewports != state.extra_viewports.size() + 1) {
        state.budget_dirty = false;
        if (auto* root = render_root()) apply_render_state(*root);
        apply_render_state_to_registered();
        return;
    }
}

inline void apply_render_profile(godot::Viewport& viewport, const RenderProfile profile,
                                 const std::optional<float> supersampling = {}) {
    if (supersampling) render_state().supersampling = *supersampling;
    render_state().profile = profile;
    refresh_render_memory();
    apply_render_state(viewport);
    apply_render_state_to_registered();
}

// Run-local AA overrides replace the profile fields they name; the other fields stay.
inline void apply_aa_override(godot::Viewport& viewport, const AaOverride& override_aa) {
    render_state().override_aa = override_aa;
    refresh_render_memory();
    apply_render_state(viewport);
    apply_render_state_to_registered();
}

// A mode that shows a space battle enters the scene once it has its viewport and leaves it when it ends;
// both are no-ops when the scene does not change. Leaving also tolerates an engine that is shutting down.
inline void enter_render_scene(const RenderScene scene) {
    RenderState& state = render_state();
    if (state.scene == scene) return;
    state.scene = scene;
    refresh_render_memory();
    if (godot::Window* root = render_root()) apply_render_state(*root);
    apply_render_state_to_registered();
}

inline void leave_render_scene() {
    RenderState& state = render_state();
    if (state.scene == RenderScene::general) return;
    state.scene = RenderScene::general;
    refresh_render_memory();
    godot::Window* root = render_root();
    if (root && root->is_inside_tree()) apply_render_state(*root);
    apply_render_state_to_registered();
}

// The root viewport's settings as a report object: the profile they match
// ("custom" for neither) and the values themselves.
[[nodiscard]] inline std::string render_profile_report() {
    godot::Window* root = render_root();
    if (!root) return "null";
    const RenderSettings settings = render_profile_detail::settings_of(*root);
    const bool fxaa = root->get_screen_space_aa() == godot::Viewport::SCREEN_SPACE_AA_FXAA;
    const auto selected = active_render_profile();
    const auto size = root->get_texture()->get_size();
    const auto expected = viewport_aa_budget(effective_render_settings(selected, render_state().scene, {}, render_state().supersampling), size).settings;
    const auto requested = effective_render_settings(selected, render_state().scene, render_state().override_aa, render_state().supersampling);
    const auto id = static_cast<std::uint64_t>(root->get_instance_id());
    const auto found = render_state().targets.find(id);
    const auto decision = found == render_state().targets.end() ? viewport_aa_budget(requested, size) : found->second.decision;
    auto* rendering = godot::RenderingServer::get_singleton();
    const std::string vendor = rendering ? rendering->get_video_adapter_vendor().utf8().get_data() : "unknown";
    const std::string adapter = rendering ? rendering->get_video_adapter_name().utf8().get_data() : "unknown";
    const auto& memory = render_state().memory;
    const std::string_view name = !fxaa && settings == expected ? render_profile_name(selected) : "custom";
    return "{\"name\": \"" + std::string(name) + "\", \"msaa_samples\": " + std::to_string(settings.msaa_samples)
        + ", \"screen_space_aa\": \"" + (settings.smaa ? "smaa" : fxaa ? "fxaa" : "disabled")
        + "\", \"anisotropic_filtering\": " + std::to_string(settings.anisotropy)
        + ", \"supersampling\": " + std::to_string(settings.supersampling)
        + ", \"selected\": \"" + std::string(render_profile_name(active_render_profile())) + "\""
        + ", \"budget\": {\"vendor\": " + core::json_quote(vendor)
        + ", \"adapter\": " + core::json_quote(adapter)
        + ", \"device_local_bytes\": " + std::to_string(memory.device_local_bytes)
        + ", \"process_budget_bytes\": " + std::to_string(memory.budget_bytes)
        + ", \"source\": " + core::json_quote(memory.source)
        + ", \"pending\": " + (memory.pending ? "true" : "false")
        + ", \"reason\": " + core::json_quote(!decision.fits ? "old plus native replacement targets exceed allowance"
            : memory.device_local_bytes == 0 && decision.clamped ? "VRAM unknown; supersampling disabled"
            : decision.clamped ? "render-target safety allowance" : "within allowance")
        + ", \"viewport_count\": " + std::to_string(decision.viewport_count)
        + ", \"requested_msaa\": " + std::to_string(requested.msaa_samples)
        + ", \"requested_ss\": " + std::to_string(requested.supersampling)
        + ", \"estimated_bytes\": " + std::to_string(decision.estimated_bytes)
        + ", \"previous_bytes\": " + std::to_string(decision.previous_bytes)
        + ", \"transient_bytes\": " + std::to_string(decision.transient_bytes)
        + ", \"limit_bytes\": " + std::to_string(decision.limit_bytes)
        + ", \"clamped\": " + (decision.clamped ? "true" : "false")
        + ", \"fits\": " + (decision.fits ? "true" : "false") + "}}";
}

} // namespace eawr::presentation::godot_backend
