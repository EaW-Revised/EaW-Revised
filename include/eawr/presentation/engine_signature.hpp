#pragma once

// Engine guard, viewer side (owner rule 11-03): the viewer runs on OUR patched Godot (fork Thom-Ernst/godot,
// eawr/4.7.2-stable, built with BUILD_NAME=eawr), never on the official download. The engine reports itself through
// Engine.get_version_info(): build "eawr" and the fork commit (official: build "official", hash ed1daf0bf...); the same
// fields make up `--version` (4.7.2.stable.eawr.fad06b579), which the player zip's runtime.json records.
// This header is the pure decision; apps/viewer/src/engine_guard_startup.hpp feeds it the engine's values at startup:
//   patched                        -> one log line;
//   unpatched + automation run     -> loud log line, exit with unpatched_engine_exit_code (gates, tests, perf);
//   unpatched + normal play        -> loud log line, "UNPATCHED ENGINE (...)" in the window title and red on the perf overlay; never aborted;
//   EAWR_ALLOW_UNPATCHED_ENGINE=ab-control (exact) -> warn only: a labelled control arm of an engine A/B.
// The accepted commits mirror tools/engine_pins.json (tests/ci/test_engine_signature_pins.py keeps them equal).

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace eawr::presentation {

// The fork commits whose builds tools/engine_pins.json accepts (short form; the engine's hash starts with it).
inline constexpr std::array<std::string_view, 1> patched_engine_commits{"fad06b579"};
inline constexpr std::string_view patched_engine_build = "eawr";
// The shipped player exe (tools/engine_pins.json default, GUI file): the hash an unpatched run is told it should have.
inline constexpr std::string_view pinned_player_engine_sha256 =
    "453b4ad56a8f5c64b39c1d75e77468c5227475b2483922dee44ade51cff43442";
// The viewer's exit code for an automation run on an unpatched engine (1-3 are other startup failures).
inline constexpr int unpatched_engine_exit_code = 7;
inline constexpr std::string_view unpatched_engine_marker = "EAWR-ENGINE-UNPATCHED";
inline constexpr std::string_view unpatched_engine_override = "ab-control";
// The player launcher (EAWR.exe, MM #5796) passes this when the engine exe does not match runtime.json godot_sha256:
// the engine counts as unpatched even when its own build and hash look right (a renamed or rebuilt exe).
inline constexpr std::string_view engine_unverified_flag = "--eawr-engine-unverified";

struct EngineSignature {
    bool checked{false};
    bool patched{true};
    // What a HUD shows: "eawr fad06b579", or "UNPATCHED ENGINE (official)" in red.
    std::string display{"engine unchecked"};
    // The log line the viewer prints (unpatched: starts with unpatched_engine_marker).
    std::string log_line;
};

// `launcher_unverified`: the launcher's --eawr-engine-unverified (its runtime.json hash check failed).
[[nodiscard]] inline EngineSignature check_engine_signature(const std::string_view build, const std::string_view hash,
                                                            const std::string_view version,
                                                            const bool launcher_unverified = false) {
    EngineSignature result;
    result.checked = true;
    const std::string_view short_hash = hash.substr(0, std::min<std::size_t>(hash.size(), 9));
    const bool identity = build == patched_engine_build && !hash.empty()
        && std::any_of(patched_engine_commits.begin(), patched_engine_commits.end(),
                       [&](const std::string_view commit) { return hash.starts_with(commit); });
    result.patched = identity && !launcher_unverified;
    if (identity && launcher_unverified) {
        result.display = "UNPATCHED ENGINE (unverified)";
        result.log_line = std::string(unpatched_engine_marker) + ": " + std::string(version)
            + " reports our build, but the launcher found its exe does not match runtime.json godot_sha256 ("
            + std::string(engine_unverified_flag) + "); numbers from this run are invalid.";
    } else if (result.patched) {
        result.display = std::string(build) + " " + std::string(short_hash);
        result.log_line = "EAWR-ENGINE " + result.display + " patched (" + std::string(version) + ")";
    } else {
        result.display = "UNPATCHED ENGINE (" + std::string(build.empty() ? std::string_view("unknown") : build) + ")";
        result.log_line = std::string(unpatched_engine_marker) + ": " + std::string(version) + " (build '" + std::string(build)
            + "', hash '" + std::string(hash) + "') is NOT our patched engine; numbers from this run are invalid. "
            "Use the patched exe from tools/engine_pins.json.";
    }
    return result;
}

// The unpatched log line with the exe's own SHA-256 (found) and the pinned one (expected) in one line.
[[nodiscard]] inline std::string unpatched_engine_hash_line(const EngineSignature& signature, const std::string_view found) {
    const std::string_view rest = std::string_view(signature.log_line).substr(
        std::min(signature.log_line.size(), unpatched_engine_marker.size() + 2));
    return std::string(unpatched_engine_marker) + ": UNPATCHED ENGINE: found sha256 "
        + std::string(found.empty() ? std::string_view("unreadable") : found) + " expected "
        + std::string(pinned_player_engine_sha256) + " (tools/engine_pins.json); " + std::string(rest);
}

// A run whose result is a measurement, a gate or a test (never normal play). The player launcher always passes
// --eawr-report and may pass --quit-after for its own smoke option, so --eawr-report alone is NOT automation.
// `lab_environment`: EAWR_LAB_RUN (rig run.cmd) or EAWR_GODOT_VIEWER_RUNTIME_TEST (runtime suites) is set.
template <typename Arguments>
[[nodiscard]] bool engine_guard_automation_run(const Arguments& engine_arguments, const Arguments& user_arguments,
                                               const bool lab_environment) {
    if (lab_environment) return true;
    for (const auto& argument : engine_arguments) {
        const std::string_view value(argument);
        if (value == "--quit-after" || value == "--script" || value == "--write-movie") return true;
    }
    static constexpr std::array<std::string_view, 9> exact{
        "--preview-close-after", "--eawr-perf-trace", "--eawr-benchmark", "--eawr-render-counters", "--eawr-drawtype-ablate",
        "--eawr-menu-timeline", "--eawr-live-lan-quit-tick", "--eawr-front-end-test-quit-open", "--eawr-loading-planet-report"};
    for (const auto& argument : user_arguments) {
        const std::string_view value(argument);
        const std::string_view name = value.substr(0, value.find('='));
        if (std::find(exact.begin(), exact.end(), name) != exact.end()) return true;
        if (name.starts_with("--eawr-load-bench") || (name.starts_with("--eawr-") && name.ends_with("-trace"))) return true;
    }
    return false;
}

enum class EngineGuardAction { proceed, warn, abort };

// `override_value` = EAWR_ALLOW_UNPATCHED_ENGINE; only the exact value ab-control turns an abort into a warning.
[[nodiscard]] inline EngineGuardAction engine_guard_action(const EngineSignature& signature, const bool automation,
                                                           const std::string_view override_value) {
    if (signature.patched) return EngineGuardAction::proceed;
    if (override_value == unpatched_engine_override) return EngineGuardAction::warn;
    return automation ? EngineGuardAction::abort : EngineGuardAction::warn;
}

// The process's engine status, set once at viewer startup; HUD labels (SK #5787's build label) read it.
[[nodiscard]] inline EngineSignature& current_engine_signature() {
    static EngineSignature signature;
    return signature;
}

} // namespace eawr::presentation
