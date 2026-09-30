#pragma once

// #453 and #459: the battle's screen messages. The win/lose message (docs/behaviour/battle-end.md
// BE-02, BE-03), the end panel's texts (BEP-03) and the pause banner's texts
// (docs/behaviour/tactical-time-controls.md TM-09), with their looks from GameConstants.xml.

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// What GameConstants.xml says; each value missing or malformed keeps the FoC value written here
// and adds one EAWR-UI-0323 warning.
struct BattleMessageLooks {
    std::string font{"EmpireAtWar-Bold"};           // Win_Lose_Message_Font
    std::int32_t point_size{24};                    // Win_Lose_Message_Font_Size
    data::ui::Rgba8 win{223, 243, 255, 255};        // Win_Message_Color
    data::ui::Rgba8 lose{255, 244, 223, 255};       // Lose_Message_Color
    data::ui::Rgba8 pending{255, 32, 32, 240};      // Battle_Pending_Message_Color: the pause text
    std::vector<core::Diagnostic> diagnostics;
};

[[nodiscard]] BattleMessageLooks battle_message_looks(const data::XmlNode& constants_root);
// Reads data/xml/gameconstants.xml.
[[nodiscard]] BattleMessageLooks battle_message_looks(const vfs::Vfs& filesystem);

// BE-02 and BEP-03: the local player's result names the message and the panel's title.
enum class BattleResult : std::uint8_t { victory, defeat };
[[nodiscard]] std::string_view battle_message_key(BattleResult result) noexcept; // TEXT_WIN/LOSE_TACTICAL
[[nodiscard]] std::string_view battle_end_title_key(BattleResult result) noexcept; // TEXT_VICTORY/DEFEAT
inline constexpr std::string_view battle_end_quit_key = "TEXT_BUTTON_QUIT_GAME";
// TM-09.
inline constexpr std::string_view pause_text_key = "TEXT_GAME_PAUSED";
inline constexpr std::string_view pause_resume_key = "TEXT_BUTTON_RESUME_GAME";

// BE-03: where the message's text box starts on a screen of `screen_width` x `screen_height`
// pixels for a text `text_width` pixels wide: centred horizontally, its top at 0.4 of the height
// (BE-U1: the top edge is the remake's reading).
struct MessagePlacement {
    double x{};
    double y{};
};
[[nodiscard]] MessagePlacement battle_message_placement(double screen_width, double screen_height,
                                                        double text_width) noexcept;

namespace diagnostic_codes {
inline constexpr std::string_view battle_message_constant = "EAWR-UI-0323"; // a GameConstants look fell back
} // namespace diagnostic_codes

} // namespace eawr::presentation::ui
