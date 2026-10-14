#pragma once
#include <cstdint>
#include <string_view>

namespace eawr::viewer::galactic {
// WGS-152: actual published facts, not an Is_Story_Campaign proxy. Optional
// presentation/tutorial providers remain a named seam in the live adapter.
struct SaveMenuAdmission {
    bool cinematic{}, letterbox{}, pending_battle{}, tutorial{};
    bool multiplayer_skirmish{}, multiplayer_campaign{}, tactical{};
};
struct SaveMenuEnabled { bool open{}, save{}, load{}; };
[[nodiscard]] constexpr SaveMenuEnabled save_menu_enabled(SaveMenuAdmission facts) {
    const bool both = facts.cinematic || facts.letterbox || facts.pending_battle
        || facts.tutorial || facts.multiplayer_skirmish;
    return {!facts.cinematic, !both && !(facts.multiplayer_campaign && facts.tactical),
        !both && !facts.multiplayer_campaign};
}
[[nodiscard]] constexpr bool reserved_save_name(std::string_view name) {
    return name == "[AutoSave]" || name == "[QuickSave]";
}
// WGS-156 local calendar key; equal keys intentionally have no name tie-break.
[[nodiscard]] constexpr std::int64_t save_list_key(std::string_view description,
    int year, int month, int day, int hour, int minute, int second) {
    if (description == "[AutoSave]") return 0;
    if (description == "[QuickSave]") return 1;
    return 2147483647LL - ((static_cast<std::int64_t>(year) - 2000) * 31536000
        + month * 2678400LL + day * 86400LL + hour * 3600LL + minute * 60LL + second);
}
[[nodiscard]] constexpr bool selected_save_overwrite(std::string_view edited,
    std::string_view selected_description) {
    return !edited.empty() && !selected_description.empty() && edited == selected_description;
}
} // namespace eawr::viewer::galactic
