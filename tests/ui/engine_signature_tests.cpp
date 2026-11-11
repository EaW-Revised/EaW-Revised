// Engine guard, viewer side (owner rule 11-03): the pure startup decision in eawr/presentation/engine_signature.hpp, fed
// with fake Engine.get_version_info() values (no engine involved).

#include "eawr/presentation/engine_signature.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr;
namespace pres = presentation;
using Args = std::vector<std::string>;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

void test_patched_engine() {
    const auto signature = pres::check_engine_signature("eawr", "fad06b57944fbf17833fe3c65dc9ff1d7446c639",
                                                        "4.7.2.stable.eawr.fad06b579");
    expect(signature.checked && signature.patched, "the fork build (build eawr, hash fad06b579...) is patched");
    expect(signature.display == "eawr fad06b579", "patched display = 'eawr <9-char hash>'");
    expect(signature.log_line.starts_with("EAWR-ENGINE eawr fad06b579 patched"), "patched log line");
    expect(pres::engine_guard_action(signature, true, "") == pres::EngineGuardAction::proceed,
           "a patched engine proceeds even in automation");
}

void test_b1_engine() {
    const auto signature = pres::check_engine_signature("eawr", "5b733a90f6516916dc19d530ecdd433fdd0bc4ee",
                                                        "4.7.2.stable.eawr.5b733a90f");
    expect(signature.checked && signature.patched, "the B1 fork build (re-spirv off, hash 5b733a90f...) is patched");
    expect(signature.display == "eawr 5b733a90f", "B1 display = 'eawr <9-char hash>'");
}

void test_official_engine() {
    const auto signature = pres::check_engine_signature("official", "ed1daf0bf001b61586d9930840f2f1394092c079",
                                                        "4.7.2.stable.official.ed1daf0bf");
    expect(signature.checked && !signature.patched, "the official download is not patched");
    expect(signature.display == "UNPATCHED ENGINE (official)", "unpatched display names the build");
    expect(signature.log_line.starts_with("EAWR-ENGINE-UNPATCHED: 4.7.2.stable.official.ed1daf0bf"), "loud log line");
    expect(pres::engine_guard_action(signature, false, "") == pres::EngineGuardAction::warn, "normal play only warns");
    expect(pres::engine_guard_action(signature, true, "") == pres::EngineGuardAction::abort, "automation aborts");
    expect(pres::engine_guard_action(signature, true, "ab-control") == pres::EngineGuardAction::warn,
           "ab-control turns the abort into a warning (labelled A/B control arm)");
    for (const std::string_view loose : {"1", "true", "guard-test", "AB-CONTROL", " ab-control"}) {
        expect(pres::engine_guard_action(signature, true, loose) == pres::EngineGuardAction::abort,
               "only the exact ab-control value bypasses: " + std::string(loose));
    }
}

void test_launcher_unverified_flag() {
    const auto signature = pres::check_engine_signature("eawr", "fad06b57944f", "4.7.2.stable.eawr.fad06b579", true);
    expect(!signature.patched, "--eawr-engine-unverified makes a good identity unpatched");
    expect(signature.display == "UNPATCHED ENGINE (unverified)", "unverified display");
    expect(signature.log_line.starts_with("EAWR-ENGINE-UNPATCHED") && signature.log_line.find("--eawr-engine-unverified") != std::string::npos,
           "unverified log line names the launcher flag");
    expect(pres::engine_guard_action(signature, true, "") == pres::EngineGuardAction::abort, "unverified + automation aborts");
    expect(pres::engine_guard_action(signature, false, "") == pres::EngineGuardAction::warn, "unverified + play warns");
    expect(!pres::check_engine_signature("official", "ed1daf0bf", "v", true).patched, "unverified official stays unpatched");
    expect(pres::check_engine_signature("official", "ed1daf0bf", "v", true).display == "UNPATCHED ENGINE (official)",
           "an official engine keeps its own name");
    expect(!pres::engine_guard_automation_run(Args{}, Args{std::string(pres::engine_unverified_flag)}, false),
           "the launcher flag alone is normal play");
    expect(pres::engine_guard_action(signature, true, "ab-control") == pres::EngineGuardAction::warn
               && signature.display == "UNPATCHED ENGINE (unverified)",
           "unverified + ab-control warns and stays labelled unverified");
}

void test_hash_line_names_found_and_expected() {
    const auto signature = pres::check_engine_signature("official", "ed1daf0bf", "4.7.2.stable.official.ed1daf0bf");
    const std::string line = pres::unpatched_engine_hash_line(signature, "ab1824f85bfd");
    expect(line.starts_with("EAWR-ENGINE-UNPATCHED: UNPATCHED ENGINE: found sha256 ab1824f85bfd expected 96c03783"),
           "one line names the found and the pinned hash");
    expect(line.find("(tools/engine_pins.json); 4.7.2.stable.official.ed1daf0bf") != std::string::npos,
           "the version text follows the hashes");
    expect(pres::unpatched_engine_hash_line(signature, "").find("found sha256 unreadable") != std::string::npos,
           "an unreadable exe is named as such");
}

void test_lookalikes_are_unpatched() {
    expect(!pres::check_engine_signature("eawr", "", "4.7.2.stable.eawr").patched, "build eawr without a hash");
    expect(!pres::check_engine_signature("eawr", "ed1daf0bf001", "x").patched, "build eawr on the official commit");
    expect(!pres::check_engine_signature("custom", "fad06b57944f", "x").patched, "our commit under another build name");
    expect(!pres::check_engine_signature("EAWR", "fad06b57944f", "x").patched, "build names compare exactly");
    expect(pres::check_engine_signature("", "", "").display == "UNPATCHED ENGINE (unknown)", "an empty version is unknown");
}

void test_automation_detection() {
    const Args none;
    expect(!pres::engine_guard_automation_run(none, none, false), "no flags = normal play");
    expect(!pres::engine_guard_automation_run(Args{"--path", "project"},
                                              Args{"--eawr-report", "r.json", "--eawr-game-root", "g"}, false),
           "the player launcher's --eawr-report alone is normal play");
    expect(pres::engine_guard_automation_run(none, none, true), "a lab/test environment is automation");
    for (const char* engine : {"--quit-after", "--script", "--write-movie"}) {
        expect(pres::engine_guard_automation_run(Args{engine, "x"}, none, false), std::string("engine flag ") + engine);
    }
    for (const char* user : {"--preview-close-after", "--eawr-perf-trace", "--eawr-benchmark", "--eawr-render-counters",
                             "--eawr-drawtype-ablate", "--eawr-menu-timeline", "--eawr-load-bench-map",
                             "--eawr-load-bench-starts", "--eawr-particle-spawn-trace", "--eawr-lan-trace",
                             "--eawr-perf-trace=out.csv"}) {
        expect(pres::engine_guard_automation_run(none, Args{user}, false), std::string("viewer flag ") + user);
    }
    expect(!pres::engine_guard_automation_run(none, Args{"--eawr-trace-colour"}, false),
           "a flag that only starts with a trace word is not a trace output");
}

void test_current_signature_is_process_wide() {
    expect(!pres::current_engine_signature().checked, "unchecked until the viewer starts");
    pres::current_engine_signature() = pres::check_engine_signature("official", "ed1daf0bf", "v");
    expect(!pres::current_engine_signature().patched && pres::current_engine_signature().checked, "the startup result is kept");
}

} // namespace

int main() {
    test_patched_engine();
    test_b1_engine();
    test_official_engine();
    test_launcher_unverified_flag();
    test_hash_line_names_found_and_expected();
    test_lookalikes_are_unpatched();
    test_automation_detection();
    test_current_signature_is_process_wide();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " engine signature check(s) failed\n";
        return 1;
    }
    std::cout << "ui engine signature passed\n";
    return 0;
}
