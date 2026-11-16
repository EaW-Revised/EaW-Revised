#pragma once
// The unit-table model facts store (platform::DerivedStore, platform::ModelFactsProvider): where it lives and when it opens.
// <user data>/mods/cache/facts, next to the mod content digest cache; `--eawr-facts-cache <dir>` names another folder
// (a measurement or test run with its own cold or warm store). An unusable folder leaves the store memory-only.
#include "eawr/platform/model_facts.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <string>
#include <system_error>

namespace eawr::presentation::godot_backend {

// Main thread, once, after the engine's user data path is known.
inline void configure_model_facts_store() {
    std::string folder;
    const auto args = godot::OS::get_singleton()->get_cmdline_user_args();
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == godot::String("--eawr-facts-cache")) {
            const auto value = godot::String(args[i + 1]).utf8();
            folder = std::string(value.get_data(), static_cast<std::size_t>(value.length()));
            break;
        }
    }
    if (folder.empty()) {
        const auto root = godot::ProjectSettings::get_singleton()->globalize_path("user://").utf8();
        const auto user = ViewerPath{std::string(root.get_data(), static_cast<std::size_t>(root.length()))}.native();
        if (!user.empty()) folder = ViewerPath::utf8(user / "mods" / "cache" / "facts");
    }
    platform::configure_model_facts_cache(folder);
}

} // namespace eawr::presentation::godot_backend
