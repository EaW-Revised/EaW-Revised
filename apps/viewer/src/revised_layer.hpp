#pragma once

#include "settings_store.hpp"
#include "viewer_path.hpp"
#include "eawr/vfs/vfs.hpp"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend {
inline void mount_revised_layer(std::vector<vfs::LayerRoot>& roots) {
    const auto* active = ActiveSettings::get();
    const auto fallback = active ? presentation::ui::PlayerSettings{} : SettingsStore{}.load().settings;
    const auto& settings = active ? *active : fallback;
    const auto executable = godot::OS::get_singleton()->get_executable_path().utf8();
    const auto player = ViewerPath{std::string(executable.get_data(), static_cast<std::size_t>(executable.length()))}.native().parent_path().parent_path();
    const auto override_root = settings.revised_layer_path ? ViewerPath{*settings.revised_layer_path}.native() : std::filesystem::path{};
    if (!vfs::insert_revised_layer(roots, settings.revised_layer_enabled, player, override_root)) {
        static bool reported = false;
        if (!reported) {
            godot::UtilityFunctions::print("LoadNote: Revised data layer missing; continuing without it.");
            reported = true;
        }
    }
}
} // namespace eawr::presentation::godot_backend
