#pragma once
#include <string_view>

namespace eawr::viewer::galactic {
struct SaveInputProfile {
    // WGS-97 qualifies this profile's bindings; other profiles can override them.
    std::string_view quick_save{"F6"}, quick_load{"F5"};
};
enum class SaveInputAction { none, quick_save, quick_load };
[[nodiscard]] inline SaveInputAction save_input_action(const SaveInputProfile& profile,
    std::string_view key, bool pressed, bool echo, bool modified) {
    if (!pressed || echo || modified || key.empty() || profile.quick_save == profile.quick_load)
        return SaveInputAction::none;
    if (key == profile.quick_save) return SaveInputAction::quick_save;
    if (key == profile.quick_load) return SaveInputAction::quick_load;
    return SaveInputAction::none;
}
[[nodiscard]] inline bool manual_save_load_allowed(bool pending_battle, bool busy) {
    return !pending_battle && !busy; // WGS-98: this gate does not gate F6.
}
} // namespace eawr::viewer::galactic
