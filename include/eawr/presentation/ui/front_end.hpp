#pragma once

// The out-of-game front end's navigation and enable-state model (docs/ui/front-end-shell.md, rules
// FES-01 to FES-12). Engine-free; the viewer builds the dialogs and draws them. The model answers
// three questions: which screen is current and where Back goes, what a gadget of a menu dialog does
// when pressed, and whether it is shown and enabled (with the reason when it is not).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// The screens the shell moves between. The first three are catalogue dialogs the shell builds
// itself; the others are screens another part of the viewer draws (the skirmish setup screen, the
// settings screen, the campaign staging screen) and the shell only opens and closes.
enum class FrontEndScreen : std::uint8_t {
    main_menu,
    single_player,
    multiplayer,
    skirmish_setup,
    lan_setup,
    options,
    load_game,
    campaign_setup,
    online_setup,
    network_options,
};

[[nodiscard]] std::string_view to_string(FrontEndScreen screen) noexcept;

// The catalogue dialog a screen is built from, or empty for a screen drawn elsewhere (FES-01).
[[nodiscard]] std::string_view dialog_name(FrontEndScreen screen) noexcept;

// What pressing a gadget of a menu dialog does.
enum class GadgetKind : std::uint8_t {
    open,     // push `target`
    back,     // pop to the previous screen
    quit,     // leave the viewer
    disabled, // shown, not pressable; `reason` says why
    hidden,   // not shown (the dialog's own activation hides it)
    hint,     // the hint list: filled while the pointer is over a button
    plain,    // drawn, not acted on (a caption, a frame)
};

struct GadgetRule final {
    GadgetKind kind{GadgetKind::plain};
    FrontEndScreen target{FrontEndScreen::main_menu};
    std::string_view reason;
};

// What the viewer knows about the data a gadget came from, so unknown gadgets of a mod's dialog
// stay visible but inert instead of vanishing (mods add buttons; FES-10).
struct GadgetFacts final {
    bool is_button{};
};

// FES-06: build facts supplied by the viewer, never by game or replay state.
struct FrontEndBuild final {
    bool debug{};
};

// The rule for the gadget `control` (its IDC name as the dialog resource writes it) on `screen`.
[[nodiscard]] GadgetRule gadget_rule(FrontEndScreen screen, std::string_view control, const GadgetFacts& facts,
                                    FrontEndBuild build = {}) noexcept;

// FES-03: the retail menus start the main-menu music when they open, and Escape means Back on the
// two sub-menus and quit on the main menu.
[[nodiscard]] bool escape_quits(FrontEndScreen screen) noexcept;

// What the hint list shows while the pointer is over `control`: the text key whose caption it is.
[[nodiscard]] std::string hint_key(std::string_view control);

// The stack of screens. The root is the main menu and is never popped.
class FrontEndNav final {
public:
    [[nodiscard]] FrontEndScreen current() const noexcept { return stack_.back(); }
    [[nodiscard]] std::span<const FrontEndScreen> stack() const noexcept { return stack_; }
    [[nodiscard]] std::size_t depth() const noexcept { return stack_.size(); }
    // Pushes `screen`; opening the current screen again changes nothing.
    void open(FrontEndScreen screen);
    // Pops one screen. False at the root, where Back has nowhere to go.
    bool back();
    // Back to the main menu (a finished battle's menu chain is popped screen by screen instead).
    void reset();

private:
    std::vector<FrontEndScreen> stack_{FrontEndScreen::main_menu};
};

enum class Step : std::uint8_t { none, changed, quit };

// Applies a pressed gadget: opens, backs out or quits. A disabled, hidden or plain gadget changes
// nothing.
[[nodiscard]] Step press(FrontEndNav& nav, const GadgetRule& rule);
// Escape on the current screen.
[[nodiscard]] Step escape(FrontEndNav& nav);

// FES-11: the hand-off the campaign staging screen emits when Start is pressed (the host's
// campaign launcher takes it, apps/viewer/src/front_end_mode.hpp). The story selection does not
// emit it: that entry stays off until the story game mode exists.
struct CampaignStartRequest final {
    struct Economy final {
        std::int64_t credits{};
        std::int64_t starting_tech{};
        std::int64_t maximum_tech{};
    };
    // `story` is the story campaign selection; `custom` is the custom-campaign staging list.
    enum class Kind : std::uint8_t { story, custom } kind{Kind::custom};
    // The campaign entry to start, by the name the campaign data gives it: for the custom list the
    // entry of the listed row's Campaign_Set that is played as the chosen faction (WGP-29).
    std::string campaign;
    // The faction the player picked, as the data name it.
    std::string faction;
    // The staging dialog's difficulty radio buttons: easy, normal or hard.
    std::string difficulty;
    // The staging dialog's advanced-options check box.
    bool custom_options{};
    std::optional<Economy> economy{}; // WGP-190: displayed tech values; WGS-13 converts per player later
};

// FES-12: a battle's start from the setup screen is the skirmish seam that already exists
// (skirmish::FixtureOptions); the shell only decides which connection mode the screen opens in.
struct SkirmishEntry final {
    bool lan{};
};

} // namespace eawr::presentation::ui
