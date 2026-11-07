#pragma once

// Engine guard at viewer startup (owner rule 11-03; decision in eawr/presentation/engine_signature.hpp). Reads
// Engine.get_version_info(), keeps the result for HUD labels (current_engine_signature), prints one log line and:
//   unpatched + automation run (test/gate/perf flags, EAWR_LAB_RUN, EAWR_GODOT_VIEWER_RUNTIME_TEST) -> quits with
//   exit code 7 before anything loads, unless EAWR_ALLOW_UNPATCHED_ENGINE=ab-control;
//   unpatched + normal play -> play continues, never aborted; the window title (viewer_host_ready.cpp, the one writer)
//   and the perf overlay show "UNPATCHED ENGINE (...)" from current_engine_signature().
// The launcher's --eawr-engine-unverified (runtime.json hash mismatch) makes an otherwise good identity unpatched.

#include "eawr/presentation/engine_signature.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// False when the viewer must stop: it has already asked the tree to quit with unpatched_engine_exit_code.
inline bool apply_engine_guard(godot::Node& host) {
    using godot::String;
    const godot::Dictionary info = godot::Engine::get_singleton()->get_version_info();
    const auto field = [&](const char* key) {
        const String value = info.get(key, String());
        return std::string(value.utf8().get_data());
    };
    godot::OS* os = godot::OS::get_singleton();
    const bool unverified = os->get_cmdline_user_args().has(String(engine_unverified_flag.data()));
    EngineSignature& signature = current_engine_signature();
    signature = check_engine_signature(field("build"), field("hash"), field("string"), unverified);
    if (signature.patched) {
        godot::UtilityFunctions::print(String::utf8(signature.log_line.c_str()));
        return true;
    }
    // Only on this failure path: hash the running exe so the line names the found and the pinned hash.
    const String found = godot::FileAccess::get_sha256(os->get_executable_path());
    signature.log_line = unpatched_engine_hash_line(signature, std::string(found.utf8().get_data()));
    godot::UtilityFunctions::printerr(String::utf8(signature.log_line.c_str()));
    const auto strings = [](const godot::PackedStringArray& values) {
        std::vector<std::string> result;
        for (int64_t index = 0; index < values.size(); ++index) result.emplace_back(values[index].utf8().get_data());
        return result;
    };
    const bool lab = os->has_environment("EAWR_LAB_RUN") || os->has_environment("EAWR_GODOT_VIEWER_RUNTIME_TEST");
    const bool automation = engine_guard_automation_run(strings(os->get_cmdline_args()), strings(os->get_cmdline_user_args()), lab);
    const std::string override_value(os->get_environment("EAWR_ALLOW_UNPATCHED_ENGINE").utf8().get_data());
    if (engine_guard_action(signature, automation, override_value) == EngineGuardAction::abort) {
        godot::UtilityFunctions::printerr(String::utf8((std::string(unpatched_engine_marker)
            + ": test/gate/perf run refused (exit " + std::to_string(unpatched_engine_exit_code)
            + "); EAWR_ALLOW_UNPATCHED_ENGINE=ab-control runs a labelled A/B control arm").c_str()));
        // _ready always has a tree; without one there is nothing to quit and the caller still stops (returns false).
        if (godot::SceneTree* tree = host.get_tree()) tree->quit(unpatched_engine_exit_code);
        return false;
    }
    if (!override_value.empty() && override_value == unpatched_engine_override) {
        godot::UtilityFunctions::printerr(String::utf8((std::string(unpatched_engine_marker)
            + ": EAWR_ALLOW_UNPATCHED_ENGINE=ab-control: running as a labelled A/B control arm").c_str()));
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
