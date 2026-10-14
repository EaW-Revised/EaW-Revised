#include "eawr/presentation/ui/front_end.hpp"

#include <algorithm>
#include <array>

namespace eawr::presentation::ui {
namespace {

using S = FrontEndScreen;

struct Row final {
    FrontEndScreen screen;
    std::string_view control;
    GadgetRule rule;
};

// FES-05..FES-08: the gadgets the three retail menu dialogs act on, by their resource names. These
// are the dialogs' own button ids, so a mod that resizes or re-captions a dialog keeps its
// behaviour; ids this table does not know are handled by gadget_rule's fallback.
constexpr std::array<Row, 25> rows{{
    // FES-05, main menu.
    {S::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES", {GadgetKind::open, S::single_player, {}}},
    {S::main_menu, "IDC_MAIN_MENU_MULTIPLAYER", {GadgetKind::open, S::multiplayer, {}}},
    {S::main_menu, "IDC_OPTIONS", {GadgetKind::open, S::options, {}}},
    {S::main_menu, "IDC_MAIN_MENU_EXIT2", {GadgetKind::quit, S::main_menu, {}}},
    {S::main_menu, "IDC_MAIN_MENU_TUTORIAL", {GadgetKind::disabled, S::main_menu, "The tutorial is not built yet."}},
    {S::main_menu, "IDC_LOAD_GAME_MAIN",
        {GadgetKind::open, S::load_game, {}}},
    {S::main_menu, "IDC_MAIN_MENU_RESUME_GAME",
        {GadgetKind::disabled, S::main_menu, "There is no autosave to resume: autosaves are not written yet."}},
    {S::main_menu, "IDC_MAIN_MENU_PLAY_DEMO", {GadgetKind::hidden, S::main_menu, {}}},
    // FES-06, single-player menu.
    {S::single_player, "IDC_SKIRMISH_GAMES_BUTTON", {GadgetKind::open, S::skirmish_setup, {}}},
    {S::single_player, "IDC_BACK_BUTTON_MAIN", {GadgetKind::back, S::single_player, {}}},
    {S::single_player, "IDC_NEW_CAMPAIGN_BUTTON",
        {GadgetKind::disabled, S::single_player, "Story campaigns need the story game mode, which is not built yet."}},
    // FES-13: Custom Campaign opens the campaign staging screen; the shell turns it off, with the
    // reason, while the data lists no campaign. The retail dialog has no Continue button (its
    // handler has no control): continuing a campaign is Load Game, which lists campaign saves. A
    // mod's dialog that carries the control keeps it visible, disabled.
    {S::single_player, "IDC_CUSTOM_CAMPAIGN_BUTTON", {GadgetKind::open, S::campaign_setup, {}}},
    {S::single_player, "IDC_CONTINUE_CAMPAIGN_BUTTON",
        {GadgetKind::disabled, S::single_player, "Continue a campaign from Load Game."}},
    {S::single_player, "IDC_HINT_LIST", {GadgetKind::hint, S::single_player, {}}},
    {S::single_player, "IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON", {GadgetKind::hidden, S::single_player, {}}},
    // FES-07, multiplayer menu.
    {S::multiplayer, "IDC_MULTIPLAYER_LAN_BUTTON", {GadgetKind::open, S::lan_setup, {}}},
    {S::multiplayer, "IDC_OK_MAIN", {GadgetKind::back, S::multiplayer, {}}},
    {S::multiplayer, "IDC_MULTIPLAYER_INTERNET_BUTTON",
        {GadgetKind::disabled, S::multiplayer, "Internet games need the online lobby, which is not built yet."}},
    {S::multiplayer, "IDC_MAIN_OPTIONS_NETWORK",
        {GadgetKind::disabled, S::multiplayer, "Internet options belong to the online lobby, which is not built yet."}},
    // FES-08, the credits emblem and the petroglyph logo button exist on all three dialogs.
    {S::main_menu, "IDC_MENU_CREDITS", {GadgetKind::disabled, S::main_menu, "The credits movie is not played yet."}},
    {S::single_player, "IDC_MENU_CREDITS", {GadgetKind::disabled, S::single_player, "The credits movie is not played yet."}},
    {S::multiplayer, "IDC_MENU_CREDITS", {GadgetKind::disabled, S::multiplayer, "The credits movie is not played yet."}},
    {S::main_menu, "IDC_MENU_PETRO_LOGO", {GadgetKind::hidden, S::main_menu, {}}},
    {S::single_player, "IDC_MENU_PETRO_LOGO", {GadgetKind::hidden, S::single_player, {}}},
    {S::multiplayer, "IDC_MENU_PETRO_LOGO", {GadgetKind::hidden, S::multiplayer, {}}},
}};

} // namespace

std::string_view to_string(const FrontEndScreen screen) noexcept {
    switch (screen) {
    case S::main_menu: return "main_menu";
    case S::single_player: return "single_player";
    case S::multiplayer: return "multiplayer";
    case S::skirmish_setup: return "skirmish_setup";
    case S::lan_setup: return "lan_setup";
    case S::options: return "options";
    case S::load_game: return "load_game";
    case S::campaign_setup: return "campaign_setup";
    }
    return "main_menu";
}

std::string_view dialog_name(const FrontEndScreen screen) noexcept {
    switch (screen) {
    case S::main_menu: return "IDD_MAIN_MENU_DIALOG";
    case S::single_player: return "IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG";
    case S::multiplayer: return "IDD_MULTIPLAYER_DIALOG";
    default: return {};
    }
}

GadgetRule gadget_rule(const FrontEndScreen screen, const std::string_view control, const GadgetFacts& facts,
                      const FrontEndBuild build) noexcept {
    // FES-04: the close button is hidden on every menu dialog.
    if (control == "IDC_DIALOG_CLOSE_BUTTON") return {GadgetKind::hidden, screen, {}};
    // FES-06, MNU-23: debug builds expose the load-map list, which is not built yet.
    if (screen == S::single_player && control == "IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON" && build.debug)
        return {GadgetKind::disabled, screen, "The debug skirmish map list is not built yet."};
    for (const Row& row : rows) {
        if (row.screen == screen && row.control == control) return row.rule;
    }
    // FES-10: a button this build has no behaviour for stays visible, disabled: a mod adds
    // buttons whose behaviour is its own, and its skin may draw them without a caption.
    if (facts.is_button) return {GadgetKind::disabled, screen, "This entry is not available in this build."};
    return {GadgetKind::plain, screen, {}};
}

bool escape_quits(const FrontEndScreen screen) noexcept { return screen == S::main_menu; }

std::string hint_key(const std::string_view control) {
    std::string key = "Hint_";
    key.append(control);
    return key;
}

void FrontEndNav::open(const FrontEndScreen screen) {
    if (stack_.back() != screen) stack_.push_back(screen);
}

bool FrontEndNav::back() {
    if (stack_.size() <= 1) return false;
    stack_.pop_back();
    return true;
}

void FrontEndNav::reset() { stack_.assign(1, FrontEndScreen::main_menu); }

Step press(FrontEndNav& nav, const GadgetRule& rule) {
    switch (rule.kind) {
    case GadgetKind::open:
        if (nav.current() == rule.target) return Step::none;
        nav.open(rule.target);
        return Step::changed;
    case GadgetKind::back: return nav.back() ? Step::changed : Step::none;
    case GadgetKind::quit: return Step::quit;
    default: return Step::none;
    }
}

Step escape(FrontEndNav& nav) {
    if (escape_quits(nav.current())) return Step::quit;
    return nav.back() ? Step::changed : Step::none;
}

} // namespace eawr::presentation::ui
