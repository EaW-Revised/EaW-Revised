#pragma once

// #2927: FoC's options dialogs as one screen (docs/ui/ui-settings.md). The hub (the main
// menu's or the in-game menu's options list) and its audio, video, advanced video, gameplay,
// keyboard and network pages are the retail dialogs built from the dialog resources through the
// kit's dialog builder, with the retail Accept / Cancel / Defaults / Apply meanings (ST-A3,
// ST-V5, ST-G3). The screen edits a draft of the settings record and commits it to the
// SettingsStore on Accept; an option this build cannot honour shows disabled, with its reason
// as a tooltip (ST-O1).
//
// Reuse: the main menu and the in-game menu create one screen, call build() once with the
// content they already mounted, then show(parent). The screen emits `closed` when the hub's
// Back (or Esc on the hub) is pressed and removes itself from the tree. It is a modal layer:
// while it is shown, no key or pointer event reaches the world (UI-I1).

#include "settings_store.hpp"

#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrOptionsScreen final : public godot::CanvasLayer {
    GDCLASS(EawrOptionsScreen, godot::CanvasLayer)

public:
    // Which menu opened the screen: it picks the hub (ST-H1).
    enum class Context : std::uint8_t { main_menu, in_game };
    enum class Page : std::uint8_t { hub, audio, video, advanced_video, gameplay, keyboard, network };

    struct Options final {
        // The content the caller already mounted (its VFS); otherwise the roots below are mounted
        // as the UI gallery does (mod over expansion over base).
        std::shared_ptr<const vfs::Vfs> filesystem;
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        // Empty: --eawr-font-cache, else EAWR_FONT_CACHE, else the checkout's out/fonts.
        std::filesystem::path font_cache;
        presentation::ui::LayoutRules rules{presentation::ui::LayoutRules::aspect_correct};
        Context context{Context::main_menu};
        // The file the settings persist in; empty is the user data folder's.
        std::filesystem::path settings_file;
        // False in a capture run, whose window is pinned: the window's mode and size are saved
        // but not applied.
        bool apply_window{true};
    };

    // One state a control showed, for the report and the GPU case.
    struct ControlState final {
        std::string id;
        std::string kind;
        bool enabled{};
        bool visible{};
        double value{};      // slider value, check/radio 0/1, list or combo index
        std::string text;    // caption, edit text or the selected item's text
        std::string reason;  // why it is disabled, when it is
    };

    EawrOptionsScreen();
    ~EawrOptionsScreen() override;

    // Loads the dialog catalogue, skin, fonts and text and the settings. False with failure() set
    // when the data could not be read; the screen then cannot be shown.
    [[nodiscard]] bool build(Options options);
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }

    // Adds the screen to `parent` (a CanvasLayer of its own, above the world and the HUD) and opens
    // `start`. Emits `closed` once the player leaves the hub; the screen is then out of the tree.
    void show(godot::Node& parent, Page start = Page::hub);
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    [[nodiscard]] Page page() const noexcept { return page_; }
    [[nodiscard]] static const char* page_name(Page which) noexcept;
    [[nodiscard]] static bool parse_page(const std::string& name, Page& result) noexcept;

    // --- Scripted use (the standalone mode and the GPU case) ---
    // Switches to a page as a menu click would (the draft starts from the saved settings).
    void open_page(Page target);
    // Sets a control as the player would: a slider's value, a check's state, a list's or
    // combo's selected index. False when the control is not on the page or is disabled.
    [[nodiscard]] bool set_control(const std::string& id, double value);
    [[nodiscard]] bool set_text_control(const std::string& id, const std::string& value);
    // Presses a button as a click would. False when it is missing or disabled.
    [[nodiscard]] bool press(const std::string& id);

    // The controls of the page as the player sees them.
    [[nodiscard]] std::vector<ControlState> control_states() const;
    [[nodiscard]] const presentation::ui::PlayerSettings& committed() const noexcept { return committed_; }
    [[nodiscard]] const presentation::ui::PlayerSettings& draft() const noexcept { return draft_; }
    [[nodiscard]] const ApplyReport& last_apply() const noexcept { return last_apply_; }
    [[nodiscard]] const std::vector<std::string>& settings_notes() const noexcept { return settings_notes_; }
    [[nodiscard]] bool settings_file_existed() const noexcept { return file_existed_; }
    [[nodiscard]] std::string settings_file() const;

    void _input(const godot::Ref<godot::InputEvent>& event) override;

protected:
    static void _bind_methods();

private:
    struct Resources;
    struct PageState;

    void rebuild_page();
    void clear_page();
    void wire_page();
    void disable(godot::Control& control, const std::string& id, const std::string& reason);
    void load_draft_into_controls();
    void sync_draft_from_controls();
    void apply_preview();

    // Signal sinks, bound with the gadget id.
    void on_pressed(godot::String id);
    void on_toggled(bool pressed, godot::String id);
    void on_value_changed(double value, godot::String id);
    void on_item_selected(int64_t index, godot::String id);

    void handle_button(const std::string& id);
    void commit_page(bool leave);
    void cancel_page();
    void defaults_page();
    void leave_to_hub();
    void close_screen();
    void apply_committed();

    std::unique_ptr<Resources> resources_;
    std::unique_ptr<PageState> state_;
    Options options_;
    SettingsStore store_;
    presentation::ui::PlayerSettings committed_;
    presentation::ui::PlayerSettings draft_;
    ApplyReport last_apply_;
    std::vector<std::string> settings_notes_;
    std::vector<std::string> notes_;
    std::string failure_;
    Page page_{Page::hub};
    int keyboard_page_{1};
    bool file_existed_{};
    bool shown_{};
    bool built_{};
    // The page being filled: control signals fire while values are set and must not edit the draft.
    bool loading_{};
    bool dirty_{};
};

} // namespace eawr::presentation::godot_backend
