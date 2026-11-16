#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <limits>
#include <string_view>

// AA levels and launch selection shared by every viewport and report.
namespace eawr::presentation::godot_backend {

// Saved legacy AA migration lives in settings.cpp; new player starts use Medium.
enum class RenderProfile { retail, enhanced, low, medium, high };
inline constexpr RenderProfile player_default_render_profile = RenderProfile::medium;
inline constexpr std::array aa_slider_profiles{RenderProfile::retail, RenderProfile::low,
    RenderProfile::medium, RenderProfile::high};

[[nodiscard]] constexpr int aa_slider_index(const RenderProfile profile) noexcept {
    for (std::size_t i = 0; i < aa_slider_profiles.size(); ++i)
        if (aa_slider_profiles[i] == profile) return static_cast<int>(i);
    return 2; // The launch default has Medium's AA.
}


// What is on screen can decide a setting of the profile (render_settings). Today no scene overrides the
// profile (owner decision on #6972: space battles keep SMAA); the scene stays so a later per-scene rule
// has one place to go. General covers the front end, its attract, land battles, galactic and every probe mode.
enum class RenderScene { general, space_battle };

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
    auto offsets = profile != RenderProfile::retail ? std::array{0.0625F, 0.1875F, 0.5F}
                                                     : std::array{0.125F, 0.25F, 0.5F};
    if (space) for (float& offset : offsets) offset *= 0.5F;
    return {profile != RenderProfile::retail ? 8192 : 4096,
        offsets,
        space ? 8192.0F : 4096.0F, profile != RenderProfile::retail};
}

// What a profile asks of the root viewport. Zero turns a feature off.
struct RenderSettings final {
    std::uint32_t msaa_samples{};
    bool smaa{};
    std::uint32_t anisotropy{};
    float supersampling{1.0F};

    friend constexpr bool operator==(const RenderSettings&, const RenderSettings&) = default;
};

// The one place that decides what a profile asks of the root viewport in a scene. Everything that
// applies, re-applies or reports a profile goes through this function (render_profile_viewport.hpp).
[[nodiscard]] constexpr RenderSettings render_settings(const RenderProfile profile,
                                                       const RenderScene /*scene*/ = RenderScene::general) noexcept {
    RenderSettings settings{};
    switch (profile) {
    case RenderProfile::retail: break;
    case RenderProfile::enhanced: settings = {4, true, 16, 1.0F}; break;
    case RenderProfile::low: settings = {2, false, 16, 1.0F}; break;
    case RenderProfile::medium: settings = {4, true, 16, 1.0F}; break;
    case RenderProfile::high: settings = {8, true, 16, 1.0F}; break;
    }
    return settings;
}

[[nodiscard]] constexpr std::string_view render_profile_name(const RenderProfile profile) noexcept {
    switch (profile) {
    case RenderProfile::retail: return "retail";
    case RenderProfile::enhanced: return "enhanced";
    case RenderProfile::low: return "low";
    case RenderProfile::medium: return "medium";
    case RenderProfile::high: return "high";
    }
    return "retail";
}

[[nodiscard]] constexpr std::optional<RenderProfile> parse_render_profile(const std::string_view text) noexcept {
    if (text == "retail") return RenderProfile::retail;
    if (text == "enhanced") return RenderProfile::enhanced;
    if (text == "max") return RenderProfile::high; // Removed level: High AA, plus SS 2x via profile_text_supersampling.
    for (const auto profile : aa_slider_profiles)
        if (text == render_profile_name(profile)) return profile;
    return std::nullopt;
}

// The removed "max" profile keeps its old capture meaning: High AA plus 2x supersampling.
[[nodiscard]] constexpr std::optional<float> profile_text_supersampling(const std::string_view text) noexcept {
    if (text == "max") return 2.0F;
    return std::nullopt;
}

// --eawr-msaa 0|2|4|8 and --eawr-smaa on|off (evidence runs, #6972): a flag that was not given leaves
// its field empty and the profile's value stands in every scene.
struct AaOverride final {
    std::optional<std::uint32_t> msaa_samples;
    std::optional<bool> smaa;
    std::optional<float> supersampling{};

    friend constexpr bool operator==(const AaOverride&, const AaOverride&) = default;
};

// An empty text means "not given"; nullopt when a given value is not valid.
[[nodiscard]] constexpr std::optional<AaOverride> parse_aa_override(const std::string_view msaa,
                                                                    const std::string_view smaa) noexcept {
    AaOverride result{};
    if (msaa == "0" || msaa == "2" || msaa == "4" || msaa == "8") {
        result.msaa_samples = static_cast<std::uint32_t>(msaa[0] - '0');
    } else if (!msaa.empty()) {
        return std::nullopt;
    }
    if (smaa == "on") result.smaa = true;
    else if (smaa == "off") result.smaa = false;
    else if (!smaa.empty()) return std::nullopt;
    return result;
}

// Strict run-local switch: unknown/duplicate keys, empty fields and noncanonical values fail.
// An empty argument is invalid (absence is represented by the caller, not an empty flag).
[[nodiscard]] constexpr std::optional<AaOverride> parse_aa_switch(std::string_view text) noexcept {
    AaOverride result{};
    if (text.empty()) return std::nullopt;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto field = text.substr(0, comma);
        const auto equal = field.find('=');
        if (equal == std::string_view::npos) return std::nullopt;
        const auto key = field.substr(0, equal);
        const auto value = field.substr(equal + 1);
        if (key == "msaa" && !result.msaa_samples) {
            const auto parsed = parse_aa_override(value, "");
            if (!parsed || !parsed->msaa_samples) return std::nullopt;
            result.msaa_samples = parsed->msaa_samples;
        } else if (key == "ss" && !result.supersampling) {
            if (value == "1" || value == "1.0") result.supersampling = 1.0F;
            else if (value == "1.5") result.supersampling = 1.5F;
            else if (value == "2" || value == "2.0") result.supersampling = 2.0F;
            else return std::nullopt;
        } else if (key == "smaa" && !result.smaa) {
            if (value == "0") result.smaa = false;
            else if (value == "1") result.smaa = true;
            else return std::nullopt;
        } else return std::nullopt;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty()) return std::nullopt;
    }
    return result;
}

// The dimension bound is a final size guard, separate from the adapter-dependent memory budget.
[[nodiscard]] constexpr float clamped_supersampling(const float requested, const std::uint32_t width,
                                                    const std::uint32_t height) noexcept {
    if (width == 0 || height == 0) return 1.0F;
    for (const float scale : {2.0F, 1.5F, 1.0F})
        if (scale <= requested && static_cast<double>(width) * scale <= 8192.0
            && static_cast<double>(height) * scale <= 8192.0) return scale;
    return 1.0F;
}

[[nodiscard]] constexpr std::uint64_t saturated_product(const std::uint64_t a, const std::uint64_t b) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return b != 0 && a > maximum / b ? maximum : a * b;
}

[[nodiscard]] constexpr std::uint64_t saturated_sum(const std::uint64_t a, const std::uint64_t b) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return a > maximum - b ? maximum : a + b;
}

// Conservative planning estimate, not a claim about exact engine allocations: single-sample HDR
// colour (8), depth (4), two HDR resolve/post targets (16), scratch (4), plus MSAA colour/depth
// (12 per sample), internal-resolution SMAA targets (16) when requested, and native output (8).
[[nodiscard]] constexpr std::uint64_t aa_target_bytes(const RenderSettings& settings,
    const std::uint32_t width, const std::uint32_t height) noexcept {
    const auto twice_scale = static_cast<std::uint64_t>(settings.supersampling * 2.0F);
    const auto internal = saturated_product((static_cast<std::uint64_t>(width) * twice_scale + 1) / 2,
        (static_cast<std::uint64_t>(height) * twice_scale + 1) / 2);
    const auto native = saturated_product(width, height);
    const auto render = saturated_product(internal, 32U + 12U * settings.msaa_samples + (settings.smaa ? 16U : 0U));
    const auto output = saturated_product(native, 8U);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return render > maximum - output ? maximum : render + output;
}

struct AaBudgetDecision final {
    RenderSettings settings{};
    std::uint64_t estimated_bytes{};
    std::uint64_t previous_bytes{};
    std::uint64_t transient_bytes{};
    std::uint64_t limit_bytes{}; // zero: VRAM unknown; resolution fallback applies instead
    std::uint64_t viewport_count{1};
    bool clamped{};
    bool fits{true}; // false when old targets plus a native, non-AA replacement exceed the allowance
};

// Divide <=35% of the device-local memory (or its smaller process budget) among root and registered
// 3D viewports. Even with several full-screen layers, their sum cannot consume 35% each.
[[nodiscard]] constexpr AaBudgetDecision budgeted_render_settings(const RenderSettings requested,
    const std::uint32_t width, const std::uint32_t height, const std::uint64_t device_bytes,
    const std::uint64_t viewports = 1, const std::uint64_t process_budget = 0,
    const std::uint64_t previous_bytes = 0) noexcept {
    AaBudgetDecision result{};
    result.settings = requested;
    result.viewport_count = viewports == 0 ? 1 : viewports;
    result.previous_bytes = previous_bytes;
    result.settings.supersampling = clamped_supersampling(requested.supersampling, width, height);
    const auto memory = process_budget != 0 && process_budget < device_bytes ? process_budget : device_bytes;
    result.limit_bytes = (memory / 100 * 35 + memory % 100 * 35 / 100) / result.viewport_count;
    if (device_bytes == 0 && static_cast<std::uint64_t>(width) * height > 1920U * 1080U)
        result.settings.supersampling = 1.0F;
    const auto estimate = [&] {
        result.estimated_bytes = aa_target_bytes(result.settings, width, height);
        result.transient_bytes = saturated_sum(previous_bytes, result.estimated_bytes);
    };
    estimate();
    if (device_bytes != 0) {
        while (result.transient_bytes > result.limit_bytes && result.settings.supersampling > 1.0F) {
            result.settings.supersampling = result.settings.supersampling > 1.5F ? 1.5F : 1.0F;
            estimate();
        }
        while (result.transient_bytes > result.limit_bytes && result.settings.msaa_samples != 0) {
            result.settings.msaa_samples = result.settings.msaa_samples > 2 ? result.settings.msaa_samples / 2 : 0;
            estimate();
        }
        if (result.transient_bytes > result.limit_bytes && result.settings.smaa) {
            result.settings.smaa = false;
            estimate();
        }
        result.fits = result.transient_bytes <= result.limit_bytes;
    }
    result.clamped = result.settings != requested;
    return result;
}

// Main-thread allocation state, shared with the native apply-path regression. A transient clamp must
// never replace the steady goal. Recovery runs on the next frame after the bridge was rendered.
struct AaViewportState final {
    std::uint32_t width{}, height{};
    bool initialized{};
    bool recover{};
    std::uint64_t applied_frame{};
    RenderSettings requested{};
    AaBudgetDecision steady{};
    AaBudgetDecision decision{};
};

[[nodiscard]] constexpr AaBudgetDecision apply_render_state(AaViewportState& state,
    const RenderSettings requested, const RenderSettings current,
    const std::uint32_t width, const std::uint32_t height, const std::uint64_t device_bytes,
    const std::uint64_t frame, const std::uint64_t viewports = 1, const std::uint64_t process_budget = 0) noexcept {
    const auto steady = budgeted_render_settings(requested, width, height, device_bytes, viewports, process_budget);
    if (state.recover && state.applied_frame == frame && state.width == width && state.height == height
        && state.requested == requested && state.decision.settings == current
        && state.steady.limit_bytes == steady.limit_bytes && state.steady.viewport_count == steady.viewport_count
        && state.steady.settings == steady.settings) return state.decision;
    state.steady = steady;
    const bool replacing = state.initialized
        && (state.width != width || state.height != height || state.steady.settings != current);
    const auto previous = replacing ? aa_target_bytes(current, state.width, state.height) : 0;
    auto decision = budgeted_render_settings(requested, width, height, device_bytes, viewports,
        process_budget, previous);
    state.recover = false;
    if (decision.settings != state.steady.settings && state.steady.fits) {
        // Choose a bridge that fits beside BOTH the old targets and the eventual steady targets.
        // Otherwise promoting on the next frame could repeat the very allocation spike being avoided.
        const auto reserve = previous > state.steady.estimated_bytes ? previous : state.steady.estimated_bytes;
        const auto bridge = budgeted_render_settings(requested, width, height, device_bytes, viewports,
            process_budget, reserve);
        if (bridge.fits) {
            decision = budgeted_render_settings(bridge.settings, width, height, device_bytes, viewports,
                process_budget, previous);
            decision.clamped = decision.settings != requested;
            state.recover = decision.settings != state.steady.settings;
        }
    }
    state.applied_frame = frame;
    state.requested = requested;
    state.width = width;
    state.height = height;
    state.initialized = true;
    state.decision = decision;
    return decision;
}

// What the viewport shows for a profile, a scene and the evidence override: every apply, re-apply and
// report uses this, including later Options changes and scene changes.
[[nodiscard]] constexpr RenderSettings effective_render_settings(const RenderProfile profile, const RenderScene scene,
                                                                 const AaOverride& override_aa = {},
                                                                 const float supersampling = 1.0F) noexcept {
    RenderSettings settings = render_settings(profile, scene);
    settings.supersampling = supersampling;
    if (override_aa.msaa_samples) settings.msaa_samples = *override_aa.msaa_samples;
    if (override_aa.smaa) settings.smaa = *override_aa.smaa;
    if (override_aa.supersampling) settings.supersampling = *override_aa.supersampling;
    settings.anisotropy = settings.msaa_samples || settings.smaa || settings.supersampling > 1.0F ? 16U : 0U;
    return settings;
}

// The galactic map draws its 3D scene as a chain of full-size SubViewport layers: background, midground,
// midforeground, then world, each layer's colour copied under the next one as its canvas background. MSAA,
// SMAA and supersampling on every layer redid the whole chain per layer (four MSAA resolves, four SMAA passes,
// four supersampled targets per frame) and resampled the backdrop again in every layer. The profile goes to the
// layers whose geometry edges it smooths: the world layer, which also resolves SMAA and supersampling once over
// the composited picture, and a middle layer only when the galaxy authors models on it. The background and empty
// middle layers keep native, non-multisampled targets, as they had before 3D SubViewports followed the profile.
struct GalacticLayerFacts final {
    bool world{};           // the last layer of the chain (planets, lanes, the selection cursor)
    bool background{};      // the first layer: the backdrop
    bool authored_models{}; // the galaxy's art places at least one model on this layer
};

[[nodiscard]] constexpr bool galactic_layer_takes_render_profile(const GalacticLayerFacts& layer) noexcept {
    return layer.world || (!layer.background && layer.authored_models);
}

// The run facts that pick the default profile.
struct RenderProfileRun final {
    bool interactive{};    // --eawr-camera-interactive
    bool launcher_start{}; // --eawr-launcher-start: the launcher's player start (it omits the marker for evidence arguments)
    bool self_test{};      // --eawr-camera-input-selftest
    bool capture{};     // --eawr-capture
    bool resize_test{}; // --eawr-window-resize-test
};

// An interactive run or a launcher start that captures and tests nothing is a player's view;
// every other run (fixed captures, probes, self-tests, reports) is evidence
// and keeps the retail look unless --eawr-render-profile says otherwise.
[[nodiscard]] constexpr RenderProfile default_render_profile(const RenderProfileRun& run) noexcept {
    return (run.interactive || run.launcher_start) && !run.self_test && !run.capture && !run.resize_test
        ? player_default_render_profile : RenderProfile::retail;
}

// The facts that decide whether a start is an evidence run (it never reads the player's saved settings).
struct EvidenceRun final {
    bool capture{};                // --eawr-capture
    bool report{};                 // --eawr-report (the launcher always names one)
    bool launcher_start{};         // --eawr-launcher-start: the launcher marks a player start; its report is no evidence
    bool self_test{};              // --eawr-camera-input-selftest
    bool resize_test{};            // --eawr-window-resize-test
    bool headless_probe{};         // --eawr-headless-probe
    bool benchmark{};              // --eawr-benchmark
    bool renderer_runtime_test{};  // --eawr-renderer-runtime-test
    bool startup_metrics{};        // --eawr-perf-trace
};

[[nodiscard]] constexpr bool is_evidence_run(const EvidenceRun& run) noexcept {
    return run.capture || (run.report && !run.launcher_start) || run.self_test || run.resize_test
        || run.headless_probe || run.benchmark || run.renderer_runtime_test || run.startup_metrics;
}

// The profile a start applies: --eawr-render-profile wins, then the player's saved choice (read only when the
// start is not an evidence run; empty for "automatic" or no file), then the default for the run.
[[nodiscard]] constexpr RenderProfile startup_render_profile(const std::optional<RenderProfile> requested,
                                                             const std::optional<RenderProfile> saved,
                                                             const RenderProfileRun& run) noexcept {
    if (requested) return *requested;
    if (saved) return *saved;
    return default_render_profile(run);
}

} // namespace eawr::presentation::godot_backend
