// The front end's navigation and enable-state model (docs/ui/front-end-shell.md, rules FES-01 to
// FES-12): the screen stack, Back and Escape, what each gadget of the three menu dialogs does, and
// the fallback that keeps a mod's unknown button visible but inert. No game data is needed.

#include "eawr/presentation/ui/front_end.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>

namespace {
namespace ui = eawr::presentation::ui;
using ui::FrontEndScreen;
using ui::GadgetKind;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

ui::GadgetRule rule(const FrontEndScreen screen, const char* control, const ui::GadgetFacts facts = {true}) {
    return ui::gadget_rule(screen, control, facts);
}

void screens() {
    expect(ui::dialog_name(FrontEndScreen::main_menu) == "IDD_MAIN_MENU_DIALOG"
               && ui::dialog_name(FrontEndScreen::single_player) == "IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG"
               && ui::dialog_name(FrontEndScreen::multiplayer) == "IDD_MULTIPLAYER_DIALOG",
           "FES-01: the three menu screens are catalogue dialogs");
    expect(ui::dialog_name(FrontEndScreen::skirmish_setup).empty() && ui::dialog_name(FrontEndScreen::lan_setup).empty()
               && ui::dialog_name(FrontEndScreen::options).empty(),
           "FES-01: the setup and settings screens are drawn elsewhere");
}

void navigation() {
    ui::FrontEndNav nav;
    expect(nav.current() == FrontEndScreen::main_menu && nav.depth() == 1, "FES-02: the stack starts at the main menu");
    expect(!nav.back(), "FES-02: Back at the root has nowhere to go");
    expect(ui::press(nav, rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES")) == ui::Step::changed
               && nav.current() == FrontEndScreen::single_player,
           "FES-05: Single Player opens the single-player menu");
    expect(ui::press(nav, rule(FrontEndScreen::single_player, "IDC_SKIRMISH_GAMES_BUTTON")) == ui::Step::changed
               && nav.current() == FrontEndScreen::skirmish_setup && nav.depth() == 3,
           "FES-06: Skirmish opens the setup screen on top of the menu chain");
    expect(nav.stack()[0] == FrontEndScreen::main_menu && nav.stack()[1] == FrontEndScreen::single_player,
           "FES-02: the chain below the setup screen is kept for Back");
    expect(ui::escape(nav) == ui::Step::changed && nav.current() == FrontEndScreen::single_player,
           "FES-03: Escape on the setup screen goes back one screen");
    expect(ui::press(nav, rule(FrontEndScreen::single_player, "IDC_BACK_BUTTON_MAIN")) == ui::Step::changed
               && nav.current() == FrontEndScreen::main_menu,
           "FES-06: Back returns to the main menu");
    expect(ui::press(nav, rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_MULTIPLAYER")) == ui::Step::changed
               && ui::press(nav, rule(FrontEndScreen::multiplayer, "IDC_MULTIPLAYER_LAN_BUTTON")) == ui::Step::changed
               && nav.current() == FrontEndScreen::lan_setup,
           "FES-07: Multiplayer, then LAN, opens the setup screen in LAN mode");
    nav.reset();
    expect(nav.current() == FrontEndScreen::main_menu && nav.depth() == 1, "FES-02: reset goes to the main menu");
    expect(ui::press(nav, rule(FrontEndScreen::main_menu, "IDC_OPTIONS")) == ui::Step::changed
               && nav.current() == FrontEndScreen::options,
           "FES-05: Options opens the settings screen");
    nav.open(FrontEndScreen::options);
    expect(nav.depth() == 2, "FES-02: opening the current screen again changes nothing");
}

void quitting() {
    ui::FrontEndNav nav;
    expect(ui::press(nav, rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_EXIT2")) == ui::Step::quit, "FES-05: Exit quits");
    expect(ui::escape(nav) == ui::Step::quit, "FES-03: Escape on the main menu quits");
    nav.open(FrontEndScreen::multiplayer);
    expect(ui::escape(nav) == ui::Step::changed && nav.current() == FrontEndScreen::main_menu,
           "FES-03: Escape on a sub-menu goes back");
}

void enable_state() {
    ui::FrontEndNav load_nav;
    const auto load = rule(FrontEndScreen::main_menu, "IDC_LOAD_GAME_MAIN");
    expect(load.kind == GadgetKind::open && load.reason.empty(), "Load Game is enabled");
    expect(ui::press(load_nav, load) == ui::Step::changed && load_nav.current() == FrontEndScreen::load_game,
           "Load Game opens its screen");
    expect(ui::escape(load_nav) == ui::Step::changed && load_nav.current() == FrontEndScreen::main_menu,
           "cancel returns to the menu");
    for (const char* control : {"IDC_MAIN_MENU_TUTORIAL", "IDC_MAIN_MENU_RESUME_GAME", "IDC_MENU_CREDITS"}) {
        const auto found = rule(FrontEndScreen::main_menu, control);
        expect(found.kind == GadgetKind::disabled && !found.reason.empty(),
               std::string("FES-05: ") + control + " is disabled with its reason");
    }
    expect(rule(FrontEndScreen::single_player, "IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON").kind == GadgetKind::hidden,
           "FES-06: builds without the debug marker hide the debug map button");
    {
        // Story campaigns stay off until the story game mode exists.
        const auto found = rule(FrontEndScreen::single_player, "IDC_NEW_CAMPAIGN_BUTTON");
        expect(found.kind == GadgetKind::disabled && !found.reason.empty(),
               "FES-06: the story campaign entry is disabled with its reason");
    }
    for (const char* control : {"IDC_MULTIPLAYER_INTERNET_BUTTON", "IDC_MAIN_OPTIONS_NETWORK"}) {
        const auto found = rule(FrontEndScreen::multiplayer, control);
        expect(found.kind == GadgetKind::disabled && !found.reason.empty(),
               std::string("FES-07: ") + control + " is disabled with its reason");
    }
    ui::FrontEndNav nav;
    const auto disabled = rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_TUTORIAL");
    expect(ui::press(nav, disabled) == ui::Step::none && nav.depth() == 1, "FES-05: a disabled gadget changes nothing");
    expect(rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_PLAY_DEMO").kind == GadgetKind::hidden,
           "FES-05: the demo button is hidden (it shares the Single Player button's rectangle)");
    for (const FrontEndScreen screen : {FrontEndScreen::main_menu, FrontEndScreen::single_player, FrontEndScreen::multiplayer}) {
        expect(rule(screen, "IDC_DIALOG_CLOSE_BUTTON").kind == GadgetKind::hidden,
               "FES-04: the close button is hidden on every menu dialog");
        expect(rule(screen, "IDC_MENU_PETRO_LOGO").kind == GadgetKind::hidden,
               "FES-08: the petroglyph logo button is not drawn (the backdrop carries the logo)");
    }
    expect(rule(FrontEndScreen::single_player, "IDC_HINT_LIST").kind == GadgetKind::hint, "FES-06: the hint list is the hint target");
    expect(ui::hint_key("IDC_SKIRMISH_GAMES_BUTTON") == "Hint_IDC_SKIRMISH_GAMES_BUTTON",
           "FES-06: hints come from the Hint_ text key of the gadget's name");
}

void debug_map_entry() {
    for (const bool debug : {false, true}) {
        const ui::FrontEndBuild build{debug};
        const auto found = ui::gadget_rule(FrontEndScreen::single_player,
                                         "IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON", {true}, build);
        expect(found.kind == (debug ? GadgetKind::disabled : GadgetKind::hidden),
               "FES-06: only the debug marker exposes the unfinished map list");
        expect(debug ? !found.reason.empty() : found.reason.empty(),
               "FES-06: the visible debug entry explains why the map list is unavailable");
        ui::FrontEndNav nav;
        nav.open(FrontEndScreen::single_player);
        expect(ui::press(nav, found) == ui::Step::none && nav.current() == FrontEndScreen::single_player && nav.depth() == 2,
               "FES-06: the unfinished debug entry cannot navigate or start a game");
        expect(ui::gadget_rule(FrontEndScreen::single_player, "IDC_SKIRMISH_GAMES_BUTTON", {true}, build).kind
                   == GadgetKind::open,
               "FES-06: ordinary skirmish is available in both build states");
        expect(ui::gadget_rule(FrontEndScreen::multiplayer, "IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON", {true}, build).kind
                   == GadgetKind::disabled,
               "FES-10: a mod's same-named button on another screen keeps the unknown-button rule");
    }
}

void mods() {
    const auto added = ui::gadget_rule(FrontEndScreen::main_menu, "IDC_MOD_GALACTIC_WAR", {true});
    expect(added.kind == GadgetKind::disabled && !added.reason.empty(),
           "FES-10: a mod's added button is shown, disabled, with a reason");
    const auto caption = ui::gadget_rule(FrontEndScreen::main_menu, "IDC_STATIC_OPTIONS", {false});
    expect(caption.kind == GadgetKind::plain, "FES-10: a caption is drawn and not acted on");
    // A mod that renames nothing keeps behaviour by control id, wherever the dialog places it.
    expect(ui::gadget_rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_EXIT2", {true}).kind == GadgetKind::quit,
           "FES-10: behaviour follows the control id, not the caption or rectangle");
}

void campaigns() {
    expect(ui::dialog_name(FrontEndScreen::campaign_setup).empty()
               && ui::to_string(FrontEndScreen::campaign_setup) == "campaign_setup",
           "FES-13: the campaign staging screen is drawn by the screen the shell opens, not a menu dialog");
    ui::FrontEndNav nav;
    expect(ui::press(nav, rule(FrontEndScreen::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES")) == ui::Step::changed,
           "FES-05: Single Player opens the single-player menu");
    const auto custom = rule(FrontEndScreen::single_player, "IDC_CUSTOM_CAMPAIGN_BUTTON");
    expect(custom.kind == GadgetKind::open && custom.target == FrontEndScreen::campaign_setup && custom.reason.empty(),
           "FES-13: Custom Campaign opens the campaign staging screen");
    expect(ui::press(nav, custom) == ui::Step::changed && nav.current() == FrontEndScreen::campaign_setup
               && nav.depth() == 3,
           "FES-13: the staging screen opens on top of the single-player menu");
    expect(ui::escape(nav) == ui::Step::changed && nav.current() == FrontEndScreen::single_player,
           "FES-13: Escape on the staging screen goes back to the single-player menu");
    expect(ui::press(nav, rule(FrontEndScreen::single_player, "IDC_CUSTOM_CAMPAIGN_BUTTON")) == ui::Step::changed
               && ui::press(nav, rule(FrontEndScreen::single_player, "IDC_BACK_BUTTON_MAIN")) == ui::Step::changed
               && nav.current() == FrontEndScreen::single_player,
           "FES-13: Back leaves the staging screen, not the menu chain below it");

    // The retail dialog has no Continue control; a mod's that has one gets it shown, disabled, with the
    // pointer to Load Game, which lists campaign saves.
    const auto resume = rule(FrontEndScreen::single_player, "IDC_CONTINUE_CAMPAIGN_BUTTON");
    expect(resume.kind == GadgetKind::disabled && !resume.reason.empty(),
           "FES-13: a Continue control stays disabled and points to Load Game");
    ui::FrontEndNav idle;
    idle.open(FrontEndScreen::single_player);
    expect(ui::press(idle, resume) == ui::Step::none && idle.current() == FrontEndScreen::single_player,
           "FES-13: pressing it changes nothing");
    // Story campaigns stay off.
    ui::FrontEndNav story;
    story.open(FrontEndScreen::single_player);
    expect(ui::press(story, rule(FrontEndScreen::single_player, "IDC_NEW_CAMPAIGN_BUTTON")) == ui::Step::none
               && story.current() == FrontEndScreen::single_player,
           "FES-06: the story campaign entry changes nothing");
}

void seams() {
    const ui::CampaignStartRequest request;
    expect(request.kind == ui::CampaignStartRequest::Kind::custom && request.campaign.empty() && !request.custom_options,
           "FES-11: the campaign hand-off request is a plain value");
    const ui::SkirmishEntry entry{true};
    expect(entry.lan, "FES-12: the setup screen opens in LAN mode on request");
}

} // namespace

int main() {
    screens();
    navigation();
    quitting();
    enable_state();
    debug_map_entry();
    mods();
    campaigns();
    seams();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " front end contract(s) failed\n";
        return 1;
    }
    std::cout << "front end contracts passed\n";
    return 0;
}
