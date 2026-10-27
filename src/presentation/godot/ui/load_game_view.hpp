#pragma once

// The Load Game screen (MNU-39 and MNU-41, docs/ui/menu-screens.md): a modal CanvasLayer that builds
// IDD_LOAD_GAME_DIALOG through the dialog builder with the retail skin and drives it from the
// engine-free LoadGameModel. The saved-game list shows the save directory's checkpoint saves in the
// retail order and columns (the name, then the local date and time at 68% of the row). Load on a
// saved game shows TEXT_SAVELOAD_LOADING_MSG, then hands the request to the context's launcher: an
// empty answer emits `save_chosen(directory, name)` and closes, a refusal is shown in a box over the
// screen. With no launcher Load is off, with the reason as a tooltip. The "show recordings" check
// switches the list to the replay store, whose Load plays the selected file: the screen emits
// `replay_chosen(path, map)` and closes. Delete asks TEXT_DELETE_CONFIRM through the runtime message
// box on either list. Cancel and Esc emit `closed` (back to the in-game menu or the main menu, which
// the caller knows).

#include "eawr/platform/ui_files.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/input_routing.hpp"
#include "ui/message_box_view.hpp"

#include "eawr/presentation/ui/load_game.hpp"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input_event.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// Starts a restore of the chosen save; an empty answer accepts, anything else is the reason it was
// refused (shown to the player).
using SavedGameLauncher = std::function<std::string(const presentation::ui::SavedGameRequest&)>;

struct LoadGameContext final {
    MessageBoxContext boxes; // the catalogue, theme and text the dialogs are built with
    platform::Path store; // the replay store directory
    // The saved games of the save directory; null lists none.
    std::shared_ptr<presentation::ui::SavedGameStore> saves;
    // Null keeps Load on a saved game off, with its stated reason.
    SavedGameLauncher launcher;
};

class EawrLoadGameScreen final : public godot::CanvasLayer {
    GDCLASS(EawrLoadGameScreen, godot::CanvasLayer)

public:
    static constexpr int layer_index = 90; // under the message box (100)

    EawrLoadGameScreen() = default;

    // Builds the dialog and adds the screen under `parent`; a false return leaves it unopened with the
    // reason in failure(), and the caller frees it.
    [[nodiscard]] bool open(godot::Node& parent, const LoadGameContext& context);

    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] const presentation::ui::LoadGameModel& model() const noexcept { return model_; }
    [[nodiscard]] godot::Control* gadget(const std::string& id) const;
    [[nodiscard]] godot::Control* frame() const noexcept { return built_.frame; }
    [[nodiscard]] bool box_open() const noexcept;
    [[nodiscard]] EawrMessageBox* box() const noexcept;
    [[nodiscard]] std::string load_tooltip() const;
    void refresh() { model_.refresh(); sync(); }
    // The text each list row shows, after the text database; tests read it.
    [[nodiscard]] std::string row_text(std::size_t index) const;
    [[nodiscard]] std::string row_date(std::size_t index) const;
    // True from Load until the launcher answered, and how many times the Loading box has opened.
    [[nodiscard]] bool loading() const noexcept { return pending_load_.has_value(); }
    [[nodiscard]] int loads_started() const noexcept { return loads_started_; }

    // Raises a one-button box with `reason` over the screen (a Play the replay mode refused).
    [[nodiscard]] bool show_notice(const std::string& reason);

    void _input(const godot::Ref<godot::InputEvent>& event) override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    void sync();
    void place_dates();
    [[nodiscard]] std::string resolved_row(const presentation::ui::LoadGameEntry& entry) const;
    void on_check_toggled(bool pressed);
    void on_item_selected(int64_t index);
    void on_item_activated(int64_t index);
    void on_load();
    void on_delete();
    void on_cancel();
    void on_delete_answer(int64_t result);
    void finish_launch();

    presentation::ui::LoadGameModel model_;
    LoadGameContext context_;
    BuiltDialog built_;
    EawrUiModalLayer* modal_{};
    EawrMessageBox* box_{};
    int64_t box_id_{};
    std::string failure_;
    std::optional<std::string> pending_delete_; // the file the open confirmation asks about
    // The request the Loading box waits on, and the frames left before the launcher runs, so the box
    // is on screen first.
    std::optional<presentation::ui::SavedGameRequest> pending_load_;
    int launch_frames_{};
    int loads_started_{};
    // The date column: one label per saved-game row, laid over the list at 68% of its width.
    std::vector<std::pair<int32_t, EawrUiLabel*>> dates_;
    bool finished_{};
};

void register_load_game_classes();

struct SaveReplayContext final {
    MessageBoxContext boxes;
    std::shared_ptr<data::ui::TextLookup> text_owner;
    platform::Path store;
    std::vector<std::uint8_t> recording;
    std::function<std::vector<std::uint8_t>(std::string&)> recorder;
    std::function<std::vector<std::uint8_t>(std::string&)> session_recorder{};
    std::vector<std::uint8_t> session_recording{};
    std::string recording_problem;
};

// One store path for Save Replay and Load Game, including the command-line override.
[[nodiscard]] platform::Path personal_replay_store(const platform::Path& override_path = {});

// MNU-41: an immutable finished recording; choosing a name never changes its bytes.
class EawrSaveReplayScreen final : public godot::CanvasLayer {
    GDCLASS(EawrSaveReplayScreen, godot::CanvasLayer)
public:
    [[nodiscard]] bool open(godot::Node& parent, SaveReplayContext context);
    [[nodiscard]] godot::Control* gadget(const std::string& id) const;
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] bool box_open() const noexcept;
    [[nodiscard]] EawrMessageBox* box() const noexcept;
    void _input(const godot::Ref<godot::InputEvent>& event) override;
protected:
    static void _bind_methods();
private:
    void changed(const godot::String& value);
    void selected(int64_t index);
    void save();
    void cancel();
    void answer(int64_t result);
    void write(const std::string& name, bool overwrite);
    void notice(const std::string& reason);
    void close();
    SaveReplayContext context_;
    presentation::ui::ThemeModel theme_model_;
    BuiltDialog built_;
    EawrMessageBox* box_{};
    int64_t box_id_{};
    std::vector<presentation::ui::LoadGameEntry> entries_;
    std::string pending_name_;
    std::string failure_;
    bool finished_{};
};

} // namespace eawr::presentation::godot_backend
