// Render profile selection (#184, apps/viewer/src/render_profile.hpp).

#include "../../support/self_test.hpp"
#include "../../support/check.hpp"
#include "render_profile.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using eawr::presentation::godot_backend::AaOverride;
using eawr::presentation::godot_backend::default_render_profile;
using eawr::presentation::godot_backend::effective_render_settings;
using eawr::presentation::godot_backend::EvidenceRun;
using eawr::presentation::godot_backend::galactic_layer_takes_render_profile;
using eawr::presentation::godot_backend::is_evidence_run;
using eawr::presentation::godot_backend::parse_aa_override;
using eawr::presentation::godot_backend::RenderScene;
using eawr::presentation::godot_backend::parse_render_profile;
using eawr::presentation::godot_backend::render_profile_name;
using eawr::presentation::godot_backend::render_settings;
using eawr::presentation::godot_backend::startup_render_profile;
using eawr::presentation::godot_backend::RenderProfile;
using eawr::presentation::godot_backend::RenderProfileRun;
using eawr::presentation::godot_backend::RenderSettings;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

void aa_levels_switch_and_budget() {
    using namespace eawr::presentation::godot_backend;
    constexpr auto raw_aa = effective_render_settings(RenderProfile::medium, RenderScene::general,
        *parse_aa_switch("msaa=8,ss=2,smaa=1"));
    check(raw_aa == RenderSettings{8, true, 16, 2.0F}, "raw switch still permits 8x MSAA with SS2 and SMAA");
    constexpr std::array expected{RenderSettings{0, false, 0, 1.0F}, RenderSettings{2, false, 16, 1.0F},
        RenderSettings{4, true, 16, 1.0F}, RenderSettings{8, true, 16, 1.0F}};
    check(aa_slider_profiles.size() == 4, "slider has exactly four steps");
    check(parse_render_profile("max") == RenderProfile::high, "removed Max resolves to High");
    check(profile_text_supersampling("max") == 2.0F, "removed Max keeps 2x supersampling");
    check(!profile_text_supersampling("high"), "other profile names leave supersampling to the saved choice");
    for (std::size_t i = 0; i < aa_slider_profiles.size(); ++i) {
        const auto profile = aa_slider_profiles[i];
        check(render_settings(profile) == expected[i], "four level settings match the owner table");
        check(render_settings(profile, RenderScene::space_battle) == expected[i], "explicit levels hold in space");
        check(aa_slider_index(profile) == static_cast<int>(i), "slider index round trip");
        check(parse_render_profile(render_profile_name(profile)) == profile, "level name round trip");
    }
    for (const auto profile : aa_slider_profiles) {
        for (const float ss : {1.0F, 1.5F, 2.0F}) {
            const auto settings = effective_render_settings(profile, RenderScene::general, {}, ss);
            check(settings.supersampling == ss, "supersampling applies independently of AA including Off");
            check(settings.msaa_samples == render_settings(profile).msaa_samples
                && settings.smaa == render_settings(profile).smaa, "supersampling does not change AA");
            const auto forced = effective_render_settings(profile, RenderScene::space_battle,
                *parse_aa_switch("ss=1.5"), ss);
            check(forced.supersampling == 1.5F, "raw SS override wins over saved supersampling");
        }
    }
    check(player_default_render_profile == RenderProfile::medium, "fresh player default is Medium");
    check(parse_aa_switch("msaa=8,ss=2,smaa=1") == AaOverride{8U, true, 2.0F}, "complete switch");
    check(parse_aa_switch("smaa=0,msaa=0,ss=1.5") == AaOverride{0U, false, 1.5F}, "order and zero values");
    check(parse_aa_switch("ss=2") == AaOverride{std::nullopt, std::nullopt, 2.0F}, "SS subset");
    check(parse_aa_switch("msaa=2") == AaOverride{2U, std::nullopt}, "MSAA subset");
    check(parse_aa_switch("smaa=0") == AaOverride{std::nullopt, false}, "SMAA subset");
    for (const std::string_view bad : {"", "msaa=", "msaa=3", "msaa=16", "ss=0", "ss=nan", "ss=inf",
        "ss=1.25", "ss=3", "smaa=on", "smaa=2", "x=1", "ss=2,", ",ss=2", "ss=2,,smaa=1",
        "msaa=2,msaa=4", "smaa=0,smaa=1", "ss=1,ss=2", "ss=2=1", " ss=2"})
        check(!parse_aa_switch(bad), "malformed combined switch rejected");
    check(effective_render_settings(RenderProfile::retail, RenderScene::general, *parse_aa_switch("ss=2"))
        == RenderSettings{0, false, 16, 2.0F}, "SS override enables aniso even over Off");
    check(effective_render_settings(RenderProfile::medium, RenderScene::space_battle, *parse_aa_switch("msaa=2"))
        == RenderSettings{2, true, 16, 1.0F}, "subset preserves other settings over a later level change");
    check(effective_render_settings(RenderProfile::medium, RenderScene::general, *parse_aa_switch("msaa=0,smaa=0"))
        == RenderSettings{0, false, 0, 1.0F}, "all-AA-off override also disables anisotropy");
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    struct Case final { std::uint64_t memory_gib; std::uint32_t width, height; float ss; std::uint64_t bytes; };
    constexpr std::array cases{
        Case{4, 1920, 1080, 2.0F, 1210982400}, Case{4, 2560, 1440, 1.5F, 1223884800},
        Case{4, 3440, 1440, 1.0F, 752947200}, Case{4, 3840, 2160, 1.0F, 1260748800},
        Case{8, 1920, 1080, 2.0F, 1210982400}, Case{8, 2560, 1440, 2.0F, 2152857600},
        Case{8, 3440, 1440, 2.0F, 2892902400}, Case{8, 3840, 2160, 1.5F, 2753740800},
        Case{24, 1920, 1080, 2.0F, 1210982400}, Case{24, 2560, 1440, 2.0F, 2152857600},
        Case{24, 3440, 1440, 2.0F, 2892902400}, Case{24, 3840, 2160, 2.0F, 4843929600}};
    for (const auto& item : cases) {
        const auto decision = budgeted_render_settings(raw_aa, item.width, item.height,
            item.memory_gib * gib);
        check(decision.settings.supersampling == item.ss && decision.settings.msaa_samples == 8,
              "4/8/24 GiB matrix steps SS down before MSAA");
        check(decision.estimated_bytes == item.bytes, "matrix target bytes include HDR, depth, resolve and SMAA");
        check(decision.fits && decision.estimated_bytes <= decision.limit_bytes, "matrix fits <=35% of device memory");
        check(decision.limit_bytes == item.memory_gib * gib * 35 / 100, "allowance is 35% without integer overflow");
    }
    const auto shared = budgeted_render_settings(raw_aa, 3840, 2160, 4 * gib, 2);
    check(shared.settings.supersampling == 1.0F && shared.settings.msaa_samples == 2,
          "root and full-size subviewport share the allowance; MSAA falls after SS reaches native");
    check(shared.estimated_bytes == 663552000 && shared.estimated_bytes * 2 <= 4 * gib * 35 / 100,
          "registered targets do not each spend 35% independently");
    const auto pressure = budgeted_render_settings(raw_aa, 2560, 1440,
        24 * gib, 1, 4 * gib);
    check(pressure.settings.supersampling == 1.5F, "smaller reported process budget wins over heap capacity");
    const auto previous = aa_target_bytes(raw_aa, 1920, 1080);
    const auto transition = budgeted_render_settings(raw_aa, 2560, 1440,
        24 * gib, 1, 8 * gib, previous);
    check(transition.settings.supersampling == 1.5F && transition.fits
        && transition.transient_bytes == previous + transition.estimated_bytes
        && transition.transient_bytes <= transition.limit_bytes,
        "live resize counts old plus new targets against the current process budget before allocating");
    const auto unknown = budgeted_render_settings(raw_aa, 2560, 1440, 0);
    check(unknown.settings.supersampling == 1.0F && unknown.limit_bytes == 0 && unknown.clamped,
          "unknown VRAM disables supersampling above 1080p");
    check(budgeted_render_settings(raw_aa, 1920, 1080, 0).settings.supersampling == 2.0F,
          "unknown VRAM fallback threshold includes 1080p");
    check(budgeted_render_settings(raw_aa, 0, 0, 4 * gib).settings.supersampling == 1.0F,
          "unallocated viewport does not request supersampling");
    check(!budgeted_render_settings(raw_aa, 16384, 16384, 4 * gib).fits,
          "unavoidable native target over-budget condition is reported, not disguised as safe");
    check(aa_target_bytes(raw_aa, 0xffffffffU, 0xffffffffU)
              == std::numeric_limits<std::uint64_t>::max(), "estimate saturates instead of wrapping");
    const auto off = budgeted_render_settings(render_settings(RenderProfile::retail), 2560, 1440, 4 * gib);
    check(off.settings == render_settings(RenderProfile::retail), "Off stays off under the budget model");
    check(clamped_supersampling(2.0F, 6000, 100) == 1.0F, "dimension guard remains independent of VRAM");
}

// Uses the same stateful allocation apply function as the Godot viewport wrapper, with a known sample.
void resize_recovers_the_steady_level() {
    using namespace eawr::presentation::godot_backend;
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    const auto requested = effective_render_settings(RenderProfile::medium, RenderScene::general,
        *parse_aa_switch("msaa=8,ss=2,smaa=1"));
    AaViewportState state{};
    auto current = apply_render_state(state, requested, {}, 1920, 1080, 8 * gib, 0, 1, 8 * gib).settings;
    check(current == requested && !state.recover, "8 GiB initial 1080p raw AA uses SS2");
    const auto old_bytes = state.decision.estimated_bytes;
    const auto bridge = apply_render_state(state, requested, current, 2560, 1440, 8 * gib, 1, 1, 8 * gib);
    check(state.steady.settings == requested && state.steady.previous_bytes == 0,
        "a resize retains the steady SS2 goal independently of the transient decision");
    check(state.recover && bridge.settings.supersampling == 1.0F && bridge.fits
        && bridge.previous_bytes == old_bytes && bridge.transient_bytes == old_bytes + bridge.estimated_bytes,
        "a safe bridge fits beside the old targets before replacement");
    check(bridge.estimated_bytes + state.steady.estimated_bytes <= bridge.limit_bytes,
        "the bridge also reserves room for the following steady replacement");
    current = bridge.settings;
    auto repartitioned = state;
    const auto smaller_share = apply_render_state(repartitioned, requested, current, 2560, 1440,
        8 * gib, 1, 2, 8 * gib);
    check(smaller_share.viewport_count == 2 && smaller_share.limit_bytes == bridge.limit_bytes / 2,
        "a same-frame registration change recomputes the allowance instead of retaining an obsolete bridge");
    const auto same_frame = apply_render_state(state, requested, current, 2560, 1440, 8 * gib, 1, 1, 8 * gib);
    check(same_frame.settings == current && state.recover, "repeated applies cannot retire a bridge before it renders");
    const auto recovered = apply_render_state(state, requested, current, 2560, 1440, 8 * gib, 2, 1, 8 * gib);
    check(recovered.settings == requested && recovered.fits && !state.recover,
        "the next frame recovers 1440p raw AA to SS2 without another allocation spike");
    check(recovered.previous_bytes == bridge.estimated_bytes
        && recovered.transient_bytes == bridge.estimated_bytes + recovered.estimated_bytes
        && recovered.transient_bytes <= recovered.limit_bytes, "recovery counts bridge plus steady targets");
    const auto stable = apply_render_state(state, requested, recovered.settings, 2560, 1440, 8 * gib, 3, 1, 8 * gib);
    check(stable.settings == requested && stable.previous_bytes == 0 && !state.recover,
        "settled applies stay at the recovered steady level");
}

void settings_follow_the_owner_decision() {
    check(render_settings(RenderProfile::retail) == RenderSettings{0, false, 0},
          "retail has no MSAA, no screen-space AA and no anisotropic filtering");
    check(render_settings(RenderProfile::enhanced) == RenderSettings{4, true, 16},
          "enhanced is 4x MSAA, SMAA and 16x anisotropic filtering");
}

// #6972 (owner decision): a space battle keeps the profile's own SMAA, like every other scene.
void space_battle_keeps_the_profile_smaa() {
    check(render_settings(RenderProfile::enhanced, RenderScene::space_battle) == RenderSettings{4, true, 16},
          "enhanced in a space battle keeps 4x MSAA, SMAA and 16x anisotropy");
    check(render_settings(RenderProfile::retail, RenderScene::space_battle) == RenderSettings{0, false, 0},
          "retail has no SMAA anywhere");
    for (const auto profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        check(render_settings(profile, RenderScene::general) == render_settings(profile),
              "the general scene (menu, attract, land, galactic) is the profile");
        check(render_settings(profile, RenderScene::space_battle) == render_settings(profile),
              "the space battle scene is the profile");
    }
    // The options screen and options mode re-apply through the same function: whatever profile they pick,
    // the space battle result is that profile's own settings.
    for (const auto profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        check(effective_render_settings(profile, RenderScene::space_battle) == render_settings(profile),
              "a re-apply in a space battle gives the profile's settings");
        check(effective_render_settings(profile, RenderScene::general) == render_settings(profile),
              "without an override the effective settings are the profile's");
    }
}

void evidence_override_wins_over_the_profile() {
    check(parse_aa_override("", "") == AaOverride{}, "no flag leaves the profile alone");
    check(parse_aa_override("4", "") == AaOverride{4U, std::nullopt}, "--eawr-msaa 4");
    check(parse_aa_override("", "on") == AaOverride{std::nullopt, true}, "--eawr-smaa on");
    check(parse_aa_override("0", "off") == AaOverride{0U, false}, "--eawr-msaa 0 --eawr-smaa off");
    for (const std::string_view bad : {"1", "16", "x", "04"}) check(!parse_aa_override(bad, ""), "msaa outside 0|2|4|8");
    for (const std::string_view bad : {"ON", "1", "true"}) check(!parse_aa_override("", bad), "smaa outside on|off");
    check(effective_render_settings(RenderProfile::retail, RenderScene::space_battle, AaOverride{std::nullopt, true})
              == RenderSettings{0, true, 16},
          "--eawr-smaa on enables SMAA and enabled-AA anisotropy in a space battle");
    check(effective_render_settings(RenderProfile::enhanced, RenderScene::space_battle, AaOverride{std::nullopt, false})
              == RenderSettings{4, false, 16},
          "--eawr-smaa off replaces the profile's SMAA in a space battle (the evidence pair)");
    check(effective_render_settings(RenderProfile::enhanced, RenderScene::general, AaOverride{2U, false})
              == RenderSettings{2, false, 16},
          "an override replaces only the field it names");
}

void names_round_trip() {
    for (const RenderProfile profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        check(parse_render_profile(render_profile_name(profile)) == profile, "a profile name parses back");
    }
    for (const std::string_view bad : {"", "Retail", "ENHANCED", "off", "retail "}) {
        check(!parse_render_profile(bad), "only the exact lower-case names parse");
    }
}

void shadow_budgets_are_bounded_and_ordered() {
    using eawr::presentation::godot_backend::shadow_settings;
    for (const auto profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        for (const bool space : {false, true}) {
            const auto settings = shadow_settings(profile, space);
            float previous = 0.0F;
            for (const float split : settings.split_offsets) {
                check(split > previous && split < 1.0F, "cascades have increasing nonempty depth intervals");
                previous = split;
            }
            check(settings.max_distance >= 4096.0F && settings.max_distance <= 8192.0F,
                  "tactical reach stays within the fixed land/space budget");
            check(settings.atlas_size <= 8192 && settings.atlas_size >= 4096,
                  "atlas stays within the supported GPU budget");
        }
    }
    check(shadow_settings(RenderProfile::enhanced, false).atlas_size
              > shadow_settings(RenderProfile::retail, false).atlas_size,
          "enhanced spends more texels on shadow edges");
    for (const auto profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        const auto land = shadow_settings(profile, false);
        const auto space = shadow_settings(profile, true);
        for (std::size_t split = 0; split < land.split_offsets.size(); ++split) {
            check(land.split_offsets[split] * land.max_distance
                      == space.split_offsets[split] * space.max_distance,
                  "space's far reach must not enlarge the three near cascades");
        }
    }
}

RenderProfileRun run(const bool interactive, const bool self_test, const bool capture, const bool resize_test,
                     const bool launcher_start = false) {
    return RenderProfileRun{.interactive = interactive, .launcher_start = launcher_start, .self_test = self_test,
        .capture = capture, .resize_test = resize_test};
}

void only_a_players_live_view_uses_the_default() {
    check(default_render_profile(run(true, false, false, false)) == RenderProfile::medium,
          "an interactive run without capture or test is a player's view");
    check(default_render_profile(run(false, false, false, false)) == RenderProfile::retail,
          "a fixed run keeps the retail look");
    check(default_render_profile(run(false, false, true, false)) == RenderProfile::retail,
          "a capture keeps the retail look");
    check(default_render_profile(run(true, true, false, false)) == RenderProfile::retail,
          "a camera self-test keeps the retail look");
    check(default_render_profile(run(true, false, true, false)) == RenderProfile::retail,
          "a capture-locked interactive run keeps the retail look");
    check(default_render_profile(run(true, false, false, true)) == RenderProfile::retail,
          "a window-resize test keeps the retail look");
    // The launcher's player start (--eawr-launcher-start): a player's view, unless a capture or a test is asked.
    check(default_render_profile(run(false, false, false, false, true)) == RenderProfile::medium,
          "a launcher start is a player's view (the front end the player launches)");
    check(default_render_profile(run(false, false, true, false, true)) == RenderProfile::retail,
          "a launcher start with a capture keeps the retail look");
    check(default_render_profile(run(false, true, false, false, true)) == RenderProfile::retail,
          "a launcher start with a camera self-test keeps the retail look");
    check(default_render_profile(run(false, false, false, true, true)) == RenderProfile::retail,
          "a launcher start with a window-resize test keeps the retail look");
    check(default_render_profile(run(false, false, false, false, false)) == RenderProfile::retail,
          "the front end without the launcher marker (evidence and preview runs) keeps the retail look");
}

// The start decision: which runs read the saved settings, and which profile a start applies.
void the_start_decision_matrix() {
    const RenderProfileRun player_launch{.launcher_start = true};
    const RenderProfileRun front_end{};
    const RenderProfileRun interactive{.interactive = true};
    const RenderProfileRun capture{.launcher_start = true, .capture = true};
    const RenderProfileRun self_test{.interactive = true, .self_test = true};
    const RenderProfileRun resize{.launcher_start = true, .resize_test = true};
    const std::optional<RenderProfile> none;
    const auto retail = std::optional<RenderProfile>{RenderProfile::retail};
    const auto enhanced = std::optional<RenderProfile>{RenderProfile::enhanced};

    // Saved automatic (no saved profile) or no file: the run's default.
    check(startup_render_profile(none, none, player_launch) == RenderProfile::medium,
          "launcher start + absent or automatic AA -> Medium");
    check(startup_render_profile(none, none, front_end) == RenderProfile::retail,
          "a front-end preview run without the marker -> retail");
    check(startup_render_profile(none, none, interactive) == RenderProfile::medium, "interactive -> Medium");
    check(startup_render_profile(none, none, capture) == RenderProfile::retail, "launcher start + capture -> retail");
    check(startup_render_profile(none, none, self_test) == RenderProfile::retail, "self-test -> retail");
    check(startup_render_profile(none, none, resize) == RenderProfile::retail, "launcher start + resize -> retail");
    // The player's saved choice beats the default, either way.
    check(startup_render_profile(none, retail, player_launch) == RenderProfile::retail,
          "launcher start + new explicit Off -> retail");
    check(startup_render_profile(none, enhanced, player_launch) == RenderProfile::enhanced,
          "launcher start + saved enhanced -> enhanced");
    check(startup_render_profile(none, enhanced, front_end) == RenderProfile::enhanced,
          "a player start without the marker + saved enhanced -> enhanced");
    // --eawr-render-profile beats everything.
    check(startup_render_profile(retail, enhanced, player_launch) == RenderProfile::retail, "forced retail wins");
    check(startup_render_profile(enhanced, retail, capture) == RenderProfile::enhanced, "forced enhanced wins");

    // Evidence runs never read the saved settings; the launcher's own report alone is not evidence.
    check(!is_evidence_run(EvidenceRun{}), "a bare start reads the saved settings");
    check(!is_evidence_run(EvidenceRun{.report = true, .launcher_start = true}),
          "launcher start + its own report is a player's view");
    check(is_evidence_run(EvidenceRun{.report = true}), "a report without the launcher marker is evidence");
    check(is_evidence_run(EvidenceRun{.capture = true}), "capture is evidence");
    check(is_evidence_run(EvidenceRun{.capture = true, .report = true, .launcher_start = true}), "launcher start + capture is evidence");
    check(is_evidence_run(EvidenceRun{.launcher_start = true, .self_test = true}), "launcher start + self-test is evidence");
    check(is_evidence_run(EvidenceRun{.launcher_start = true, .resize_test = true}), "launcher start + resize is evidence");
    check(is_evidence_run(EvidenceRun{.launcher_start = true, .headless_probe = true}), "launcher start + headless probe is evidence");
    check(is_evidence_run(EvidenceRun{.launcher_start = true, .benchmark = true}), "launcher start + benchmark is evidence");
    check(is_evidence_run(EvidenceRun{.launcher_start = true, .renderer_runtime_test = true}),
          "launcher start + renderer runtime test is evidence");
    check(is_evidence_run(EvidenceRun{.launcher_start = true, .startup_metrics = true}), "launcher start + perf trace is evidence");
}

// The galactic layer chain: the profile reaches only the layers whose geometry edges it smooths.
void galactic_layers_take_the_profile_where_it_changes_the_picture() {
    check(galactic_layer_takes_render_profile({.world = true}), "the world layer (planets) takes the profile");
    check(galactic_layer_takes_render_profile({.world = true, .authored_models = true}),
          "the world layer with models takes the profile");
    check(!galactic_layer_takes_render_profile({.background = true}), "the backdrop keeps a native target");
    check(!galactic_layer_takes_render_profile({.background = true, .authored_models = true}),
          "the backdrop keeps a native target even with authored backdrop models");
    check(!galactic_layer_takes_render_profile({}), "an empty middle layer keeps a native target");
    check(galactic_layer_takes_render_profile({.authored_models = true}),
          "a middle layer with authored models (a mod) takes the profile");
}

} // namespace

int main() {
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    aa_levels_switch_and_budget();
    resize_recovers_the_steady_level();
    settings_follow_the_owner_decision();
    space_battle_keeps_the_profile_smaa();
    evidence_override_wins_over_the_profile();
    names_round_trip();
    shadow_budgets_are_bounded_and_ordered();
    only_a_players_live_view_uses_the_default();
    the_start_decision_matrix();
    galactic_layers_take_the_profile_where_it_changes_the_picture();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "render profile contracts passed\n";
    return EXIT_SUCCESS;
}
