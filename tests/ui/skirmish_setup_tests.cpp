// The skirmish staging screen's text and start blockers (docs/ui/menu-screens.md MNU-21 and MNU-22,
// docs/behaviour/skirmish-setup.md R-SETUP-01 and R-SETUP-05, WSS-16 to WSS-18, WSS-27): every caption the
// screen sets itself is a text-database key, the screen's source holds no caption literal, the land game
// types stay listed and disabled, and the start blockers pick the retail messages. With
// EAWR_EAW_GAME_ROOT set, every key is read from the installed game's text database.

#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/skirmish_setup.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

bool is_text_key(const std::string_view key) {
    constexpr std::string_view prefix = "TEXT_";
    if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) return false;
    return std::all_of(key.begin(), key.end(), [](const char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

void captions() {
    const auto keys = ui::skirmish_setup_text_keys();
    expect(keys.size() == static_cast<std::size_t>(ui::SetupText::count_), "SSC-01: one key per caption id");
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        expect(static_cast<std::size_t>(keys[i].text) == i, "SSC-01: the table is in enum order");
        expect(is_text_key(keys[i].key),
               "SSC-01: " + std::string(keys[i].key) + " is a TEXT_ database key, not literal text");
        expect(ui::skirmish_setup_text_key(keys[i].text) == keys[i].key, "SSC-01: lookup by id agrees with the table");
        seen.insert(keys[i].key);
    }
    expect(!seen.empty(), "SSC-01: keys exist");
    expect(ui::skirmish_setup_text_key(ui::SetupText::count_).empty(), "SSC-01: an id past the end has no key");
    for (std::size_t i = 0; i < static_cast<std::size_t>(ui::SetupNotice::count_); ++i) {
        const auto notice = ui::setup_notice(static_cast<ui::SetupNotice>(i));
        expect(!notice.empty() && !is_text_key(notice), "SSC-02: a notice is project text, kept apart from the keys");
    }
    expect(ui::setup_team_caption("Team", 0) == "Team 1" && ui::setup_team_caption("Team", 7) == "Team 8",
           "SSC-03: the team caption is the database word and the one-based number");
    expect(ui::setup_team_caption("", 1) == "2", "SSC-03: a missing word leaves the number");
}

void game_types() {
    const auto rows = ui::setup_game_types();
    expect(rows.size() == 3U, "SSC-04: the list offers land, space and land control");
    if (rows.size() != 3U) return;
    expect(rows[0].type == ui::SetupGameType::land && rows[1].type == ui::SetupGameType::space
               && rows[2].type == ui::SetupGameType::land_control,
           "SSC-04: the retail order: land, space, land control");
    expect(!rows[0].available && rows[1].available && !rows[2].available,
           "SSC-04: only the space game type is available; the land ones stay listed and disabled");
    expect(ui::setup_default_game_type() == ui::SetupGameType::space, "SSC-04: the list opens on space");
    expect(ui::skirmish_setup_text_key(rows[0].text) == "TEXT_GAME_TYPE_TEAM_LAND"
               && ui::skirmish_setup_text_key(rows[1].text) == "TEXT_GAME_TYPE_TEAM_SPACE"
               && ui::skirmish_setup_text_key(rows[2].text) == "TEXT_GAME_TYPE_CAPTURE_CONTROL_POINTS",
           "SSC-04: the rows' captions are the game-type keys");
}

void blockers() {
    using Rows = std::vector<ui::SetupRow>;
    expect(ui::setup_blocker(Rows{}) == ui::SetupBlocker::too_few_players, "SSC-05: no rows is too few players");
    expect(ui::setup_blocker(Rows{{0, "Rebel"}}) == ui::SetupBlocker::too_few_players,
           "SSC-05: one row is too few players");
    expect(ui::setup_blocker(Rows{{0, "Rebel"}, {0, "Rebel"}}) == ui::SetupBlocker::too_few_teams,
           "SSC-05: two rows on one team are too few teams");
    expect(ui::setup_blocker(Rows{{0, "Rebel"}, {0, "Empire"}}) == ui::SetupBlocker::too_few_teams,
           "SSC-05: a single team is too few teams before its factions are compared");
    expect(ui::setup_blocker(Rows{{0, "Rebel"}, {0, "Empire"}, {1, "Empire"}}) == ui::SetupBlocker::mixed_factions,
           "SSC-05: teammates with different factions");
    expect(ui::setup_blocker(Rows{{0, "Rebel"}, {1, "Rebel"}}) == ui::SetupBlocker::none,
           "SSC-05: opposing teams may use the same faction (WSS-16 to WSS-18)");
    expect(ui::setup_blocker(Rows{{0, "Rebel"}, {0, "Rebel"}, {1, "Empire"}}) == ui::SetupBlocker::none,
           "SSC-05: teammates of one faction start");
    expect(!ui::setup_blocker_text(ui::SetupBlocker::none), "SSC-05: no blocker shows no message");
    expect(ui::setup_blocker_text(ui::SetupBlocker::too_few_players) == ui::SetupText::too_few_players
               && ui::setup_blocker_text(ui::SetupBlocker::too_few_teams) == ui::SetupText::too_few_teams
               && ui::setup_blocker_text(ui::SetupBlocker::mixed_factions) == ui::SetupText::mixed_factions,
           "SSC-05: each blocker has its own caption");
    expect(ui::skirmish_setup_text_key(ui::SetupText::too_few_players) == "TEXT_LAN_HOST_MIN_PLAYERS"
               && ui::skirmish_setup_text_key(ui::SetupText::too_few_teams) == "TEXT_LAN_HOST_NOT_ENOUGH_TEAMS"
               && ui::skirmish_setup_text_key(ui::SetupText::mixed_factions) == "TEXT_LAN_HOST_MISMATCHED_FACTIONS",
           "SSC-05: the blockers show the retail start-check messages");
}

// MNU-22, WSS-25 and WSS-31: the advanced dialog's credits slider and win-condition list.
void advanced_options() {
    const auto choices = ui::setup_win_conditions();
    expect(choices.size() == 2U, "SSC-08: a space battle offers two win conditions (WSS-31)");
    if (choices.size() == 2U) {
        expect(choices[0].name == "SKIRMISH_ALL_ENEMY_UNITS_DESTROYED"
                   && choices[1].name == "SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED",
               "SSC-08: the choices are the two evidenced space conditions");
    }
    for (const auto& choice : choices) {
        expect(is_text_key(choice.text_key), "SSC-08: " + std::string(choice.name) + " is captioned by a database key");
    }
    expect(ui::setup_win_condition_index("SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED") == std::optional<std::size_t>{1U},
           "SSC-08: the data's default condition is the starbase choice");
    expect(!ui::setup_win_condition_index("SKIRMISH_ALL_ENEMY_BASE_COMPONENTS_DESTROYED"),
           "SSC-08: a land condition is not a space choice");
    expect(!ui::setup_win_condition_index(""), "SSC-08: an empty condition names no choice");

    const ui::SetupCreditsRange stock{2000, 8000};
    expect(ui::setup_credits_clamp(stock, 6000) == 6000, "SSC-09: an in-range authored value stays");
    expect(ui::setup_credits_clamp(stock, 0) == 2000 && ui::setup_credits_clamp(stock, 8500) == 8000,
           "SSC-09: a value outside the bounds clamps into them");
    expect(ui::setup_credits_clamp(stock, 2001) == 2001 && ui::setup_credits_clamp(stock, 7001) == 7001,
           "SSC-09: off-grid authored values do not round");
    const ui::SetupCreditsRange modded{2500, 7000};
    expect(ui::setup_credits_clamp(modded, 2500) == 2500 && ui::setup_credits_clamp(modded, 3600) == 3600,
           "SSC-09: modded bounds preserve an in-range authored value");
    expect(ui::setup_credits_clamp(modded, 6900) == 6900 && ui::setup_credits_clamp(modded, 7500) == 7000,
           "SSC-09: only values outside modded bounds clamp");
    expect(ui::setup_credits_clamp({5000, 1000}, 3000) == 5000, "SSC-09: an inverted range gives its minimum");
    expect(ui::setup_credits_button_step(stock) == 1000.0,
           "WSS-25: stock arrows span the data's seven positions");
    expect(ui::setup_credits_button_step(modded) == 1125.0,
           "WSS-25: modded arrow positions derive from the authored range");
    const std::array<std::int64_t, 5> expected{2500, 4500, 5500, 6500, 7500};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto position = static_cast<double>(modded.minimum) + static_cast<double>(index)
            * ui::setup_credits_button_step(modded);
        expect(ui::setup_credits_edit(modded, position) == expected[index],
               "WSS-25: modded choices use a minimum-relative ceil bucket, retaining endpoint overshoot");
    }
    expect(ui::setup_credits_edit(modded, 2500.49) == 2500
        && ui::setup_credits_edit(modded, 2500.5) == 3500,
           "WSS-25: track movement rounds its offset before selecting the upper bucket");
    expect(ui::setup_credits_edit(stock, 6000) == 6000,
           "WSS-25: the vanilla default is unchanged by editing its position");
}

// A quoted string with a letter in it, after `from` on the line.
bool has_literal_after(const std::string& line, const std::size_t from) {
    for (auto open = line.find('"', from); open != std::string::npos;) {
        const auto close = line.find('"', open + 1U);
        if (close == std::string::npos) return false;
        const auto body = line.substr(open + 1U, close - open - 1U);
        if (std::any_of(body.begin(), body.end(), [](const char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }))
            return true;
        open = line.find('"', close + 1U);
    }
    return false;
}

// The screen's source sets no caption from a string literal: every call that puts text on a control
// takes a value (a database caption or data). Empty strings, which clear a text, are allowed.
void source_has_no_caption_literal() {
#ifdef EAWR_VIEWER_SOURCE_DIR
    const std::string path = std::string(EAWR_VIEWER_SOURCE_DIR) + "/skirmish_setup_mode.cpp";
    std::ifstream stream(path);
    expect(static_cast<bool>(stream), "SSC-06: the setup screen's source is readable");
    if (!stream) return;
    const std::array<std::string, 7> calls{"set_text(", "add_item(", "add_icon_item(", "set_tooltip_text(",
                                            "set_item_text(", "set_item_tooltip(", "label("};
    std::string line;
    int number = 0;
    int checked = 0;
    while (std::getline(stream, line)) {
        ++number;
        for (const auto& call : calls) {
            const auto at = line.find(call);
            if (at == std::string::npos) continue;
            ++checked;
            expect(!has_literal_after(line, at + call.size()),
                   "SSC-06: caption literal at skirmish_setup_mode.cpp:" + std::to_string(number) + ": " + line);
        }
    }
    expect(checked > 20, "SSC-06: the scan saw the screen's caption calls");
#else
    std::cout << "SSC-06 skipped (no EAWR_VIEWER_SOURCE_DIR)\n";
#endif
}

void corpus() {
    auto filesystem = test::ui::foc_corpus("Skirmish setup");
    if (!filesystem) return;
    auto database = data::ui::load_language_text_database(*filesystem, ui::skirmish_setup_language);
    expect(static_cast<bool>(database), "SSC-07: the installed text database loads");
    if (!database) return;
    for (const auto& entry : ui::skirmish_setup_text_keys()) {
        const auto* found = database.value().find(entry.key);
        expect(found != nullptr && !found->value.empty(),
               "SSC-07: the installed database holds " + std::string(entry.key));
    }
    for (const auto& choice : ui::setup_win_conditions()) {
        const auto* found = database.value().find(choice.text_key);
        expect(found != nullptr && !found->value.empty(),
               "SSC-07: the installed database holds " + std::string(choice.text_key));
    }
    const auto* team = database.value().find(ui::skirmish_setup_text_key(ui::SetupText::team_number));
    expect(team != nullptr && ui::setup_team_caption(data::ui::to_utf8(team->value), 0) == "Team 1",
           "SSC-07: the stock team caption reads Team 1, as the retail capture does");
}

} // namespace

int main() {
    captions();
    game_types();
    blockers();
    advanced_options();
    source_has_no_caption_literal();
    corpus();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " skirmish setup contract(s) failed\n";
        return 1;
    }
    std::cout << "skirmish setup contracts passed\n";
    return 0;
}
