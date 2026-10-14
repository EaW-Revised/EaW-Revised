#include "profile_store.hpp"

#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <system_error>

namespace eawr::presentation::godot_backend {
namespace ui = presentation::ui;

std::filesystem::path ProfileStore::file() const {
    if (!file_.empty()) return file_;
    // globalize_path gives UTF-8; ViewerPath turns it into a native path without an ANSI step.
    const godot::String path = godot::ProjectSettings::get_singleton()->globalize_path("user://eawr-profile.cfg");
    const godot::CharString utf8 = path.utf8();
    return ViewerPath{std::string(utf8.get_data(), static_cast<std::size_t>(utf8.length()))}.native();
}

bool ProfileStore::exists() const {
    std::error_code error;
    return std::filesystem::is_regular_file(file(), error);
}

ui::ProfileLoad ProfileStore::load() const { return ui::load_profile_file(file()); }

bool ProfileStore::save(const ui::PlayerProfile& profile, std::string& error) const {
    return ui::save_profile_file(file(), profile, error);
}

} // namespace eawr::presentation::godot_backend
