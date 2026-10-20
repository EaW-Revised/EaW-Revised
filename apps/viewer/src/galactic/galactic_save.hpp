#pragma once
#include "galactic_mode.hpp"
#include "eawr/platform/live_game_session.hpp"
#include <future>
#include "ui/dialog_builder.hpp"
#include "save_dialog_model.hpp"
#include <godot_cpp/classes/tween.hpp>

namespace godot { class CanvasLayer; class Control; class ItemList; class LineEdit; class Label; class BaseButton; }
namespace eawr::presentation::godot_backend {

class EawrOptionsScreen;

// GHUD-S18/19: WGS-152..164 resource routes over the session save provider.
class GalacticSaveControls final {
public:
    explicit GalacticSaveControls(GalacticMode::State&);
    ~GalacticSaveControls();
    bool input(const godot::Ref<godot::InputEvent>&);
    void service();
    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] std::string report_json() const;
    void select_slot(std::int64_t index);
    void activate_slot(std::int64_t index);
    // The options screen opened from the menu's Options button closed: the menu shows again.
    void options_closed();
    // The HUD's options button: the same menu Escape opens.
    void open_menu() { open(); }
    // #3035: the same validation and owner replacement as an in-campaign load, before the
    // first presented frame. A refusal sets the mode failure instead of opening a modal.
    [[nodiscard]] bool restore_at_start(const std::string& name);
private:
    void open();
    void close();
    void list();
    void save(bool quick = false);
    void load(std::string name = {});
    void erase();
    void message(std::string text);
    [[nodiscard]] std::string selected() const;
    bool accept_loaded(std::unique_ptr<platform::PreparedLiveRestore>, bool at_start = false);
    bool refuse_load(std::string text, bool at_start);
    enum class Screen { escape, save, load };
    enum class Modal { none, overwrite, erase, progress_save, progress_load, acknowledgement, quit };
    void show_screen(Screen);
    void open_options();
    bool build_widgets();
    void show_modal(Modal, std::string text);
    void dismiss_modal(bool yes = false);
    void begin_progress(bool save, std::string name, bool overwrite = false, bool quick = false);
    void publish_save();
    void begin_quit();
    void populate_slots();
    void position_dates();
    void observe_world_selection();
    [[nodiscard]] viewer::galactic::SaveMenuEnabled admission() const;
    [[nodiscard]] std::string localized(std::string_view) const;
    std::function<BuiltDialog(std::string_view)> build_;
    std::function<std::string(std::string_view)> lookup_;
    std::optional<BuiltDialog> escape_dialog_, save_dialog_, load_dialog_, modal_dialog_;
    godot::Control* modal_root_{};
    EawrOptionsScreen* options_{};
    std::int64_t options_id_{};
    godot::ItemList* save_slots_{}, *load_slots_{};
    godot::Ref<godot::Tween> fade_;
    Screen screen_{Screen::escape};
    Modal modal_{Modal::none};
    std::string pending_name_;
    bool pending_overwrite_{}, pending_quick_{}, quick_route_{}, pending_autosave_quit_{};
    std::optional<platform::LiveSavePhase> quit_autosave_phase_; // reported, never gating
    std::int64_t selected_index_{-1};
    std::vector<std::size_t> rows_;
    std::vector<std::pair<std::int32_t, EawrUiLabel*>> dates_;
    sim::EntityId menu_selection_{};
    std::uint64_t world_leaks_{};
    std::uint64_t modal_admissions_{}, requests_after_fade_{};
    std::optional<std::uint32_t> button_key_;
    std::string widget_failure_;
    GalacticMode::State& state_;
    godot::CanvasLayer* layer_{};
    godot::Control* root_{};
    godot::ItemList* slots_{};
    godot::LineEdit* name_{};
    std::vector<godot::BaseButton*> buttons_;
    std::vector<platform::LiveSaveSlot> listed_;
    std::future<core::Result<std::vector<platform::LiveSaveSlot>>> listing_;
    std::future<core::Result<std::unique_ptr<platform::PreparedLiveRestore>>> loading_;
    std::uint64_t token_{}, completed_saves_{}, completed_loads_{}, restored_tick_{};
    std::string restored_hash_;
    bool started_from_save_{};
    sim::EntityId restored_conflict_{};
    std::uint64_t restored_child_tick_{};
    sim::EntityId paused_child_{};
    sim::EntityId pending_loaded_child_{};
    bool child_was_paused_{}, galaxy_pause_{};
    bool pending_quick_load_{}, queued_quick_load_{};
    std::string text_;
    std::vector<std::pair<std::string, godot::Control*>> controls_;
};
void configure_save_repository(platform::LiveGameSession::Options&, const GalacticMode::Options&);
// The restore_save directory, else save_directory, else the viewer's user-data saves.
[[nodiscard]] std::filesystem::path save_directory(const GalacticMode::Options&);
} // namespace eawr::presentation::godot_backend
