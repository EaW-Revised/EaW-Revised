#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <string_view>

// Render profiles (#184, owner decision D2 = B on #189; docs/rendering.md,
// render profiles). Players get the enhanced profile: 4x MSAA, SMAA and 16x
// anisotropic filtering. Captures and tests stay pinned to the retail look:
// no MSAA, no screen-space AA and no anisotropic filtering, which also makes
// the terrain sample linear like the retail TerrainRenderBump samplers. There
// is no player setting until M6 (D1 = C); `--eawr-render-profile retail|enhanced`
// is an internal switch for evidence runs.
namespace eawr::presentation::godot_backend {

enum class RenderProfile { retail, enhanced };

// Fixed view-depth budgets: camera motion must not change cascade scale.
// Godot 4.7.2 fits each split to a sphere and snaps its light-space bounds.
struct ShadowSettings final {
    std::int32_t atlas_size;
    std::array<float, 3> split_offsets;
    float max_distance;
    bool high_filter;
};

[[nodiscard]] constexpr ShadowSettings shadow_settings(const RenderProfile profile, const bool space) noexcept {
    // Keep the near three split depths identical in land and space. Space's
    // longer last cascade must not spend close-up texels on its distant sky.
    auto offsets = profile == RenderProfile::enhanced ? std::array{0.0625F, 0.1875F, 0.5F}
                                                     : std::array{0.125F, 0.25F, 0.5F};
    if (space) for (float& offset : offsets) offset *= 0.5F;
    return {profile == RenderProfile::enhanced ? 8192 : 4096,
        offsets,
        space ? 8192.0F : 4096.0F, profile == RenderProfile::enhanced};
}

// What a profile asks of the root viewport. Zero turns a feature off.
struct RenderSettings final {
    std::uint32_t msaa_samples{};
    bool smaa{};
    std::uint32_t anisotropy{};

    friend constexpr bool operator==(const RenderSettings&, const RenderSettings&) = default;
};

[[nodiscard]] constexpr RenderSettings render_settings(const RenderProfile profile) noexcept {
    return profile == RenderProfile::enhanced ? RenderSettings{4, true, 16} : RenderSettings{};
}

[[nodiscard]] constexpr std::string_view render_profile_name(const RenderProfile profile) noexcept {
    return profile == RenderProfile::enhanced ? "enhanced" : "retail";
}

[[nodiscard]] constexpr std::optional<RenderProfile> parse_render_profile(const std::string_view text) noexcept {
    if (text == "retail") return RenderProfile::retail;
    if (text == "enhanced") return RenderProfile::enhanced;
    return std::nullopt;
}

// The run facts that pick the default profile.
struct RenderProfileRun final {
    bool interactive{}; // --eawr-camera-interactive
    bool self_test{};   // --eawr-camera-input-selftest
    bool capture{};     // --eawr-capture
    bool resize_test{}; // --eawr-window-resize-test
};

// An interactive run that captures and tests nothing is a player's view;
// every other run (fixed captures, probes, self-tests, reports) is evidence
// and keeps the retail look unless --eawr-render-profile says otherwise.
[[nodiscard]] constexpr RenderProfile default_render_profile(const RenderProfileRun& run) noexcept {
    return run.interactive && !run.self_test && !run.capture && !run.resize_test
        ? RenderProfile::enhanced : RenderProfile::retail;
}

} // namespace eawr::presentation::godot_backend
