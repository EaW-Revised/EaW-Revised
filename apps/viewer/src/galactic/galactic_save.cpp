#include "galactic_save.hpp"
#include "auto_summary_hint.hpp"
#include "galactic_save_hints.hpp"
#include "save_input.hpp"
#include "options_screen.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/kit.hpp"
#include "ui/battle_overlay.hpp"
#include "viewer_path.hpp"
#include "eawr/core/diagnostic.hpp"
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <limits>
#include <map>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string utf8(const String& text) { return std::string(text.utf8().get_data()); }
using galactic_view::ViewHints;
using galactic_view::view_hints;
}
std::filesystem::path save_directory(const GalacticMode::Options& options) {
    if (options.restore_save && !options.restore_save->directory.empty()) return options.restore_save->directory;
    if (!options.save_directory.empty()) return options.save_directory;
    return ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "saves";
}
void configure_save_repository(platform::LiveGameSession::Options& live, const GalacticMode::Options& options) {
    if (live.saves.directory.empty()) live.saves.directory = ViewerPath::utf8(save_directory(options));
    if (!options.mod_root.empty()) live.saves.mod_list_identity[0] = 1;
}
GalacticSaveControls::GalacticSaveControls(GalacticMode::State& state) : state_(state) {}
GalacticSaveControls::~GalacticSaveControls() {
    // Async readers retain the old live pointer until they finish; join before its teardown.
    if (listing_.valid()) listing_.wait();
    if (loading_.valid()) loading_.wait();
    if (options_ != nullptr && UtilityFunctions::is_instance_id_valid(options_id_)) {
        if (Node* parent = options_->get_parent()) parent->remove_child(options_);
        memdelete(options_);
    }
    if (layer_) { layer_->queue_free(); layer_ = nullptr; root_ = nullptr; }
}
bool GalacticSaveControls::visible() const noexcept { return root_ && root_->is_visible(); }
void GalacticSaveControls::message(std::string text) {
    text_ = std::move(text);
}
void GalacticSaveControls::open() {
    if (visible()) return;
    if (!state_.game || !state_.galaxy || (state_.loading && state_.loading->shown())) return;
    if (!build_widgets() || !admission().open) return;
    state_.held.clear(); state_.orbiting = false;
    menu_selection_ = state_.map_input.selected;
    const auto frame = state_.game->frame();
    if (frame.conflict) {
        paused_child_ = frame.conflict->id; child_was_paused_ = state_.game->battle_paused(paused_child_);
        if (auto feed = state_.game->battle_feed(paused_child_, sim::SeatId{state_.galaxy->local.seat})) feed->set_paused(true);
    } else {
        // One nested token, including when a player or conflict already owns a pause.
        state_.game->submit(platform::LiveGameOrder{sim::SeatId{state_.galaxy->local.seat}, sim::galactic::PauseGalaxy{}, {}});
        galaxy_pause_ = true;
    }
    root_->show(); show_screen(Screen::escape);
}
void GalacticSaveControls::close() {
    if (!visible() || token_ || loading_.valid() || pending_loaded_child_
        || modal_ == Modal::progress_save || modal_ == Modal::progress_load) return;
    observe_world_selection();
    if (galaxy_pause_ && state_.game && state_.galaxy)
        state_.game->submit(platform::LiveGameOrder{sim::SeatId{state_.galaxy->local.seat}, sim::galactic::ResumeGalaxy{}, {}});
    if (paused_child_ && state_.game && state_.galaxy)
        if (auto feed = state_.game->battle_feed(paused_child_, sim::SeatId{state_.galaxy->local.seat})) feed->set_paused(child_was_paused_);
    galaxy_pause_ = false; paused_child_ = 0; root_->hide();
    pending_quick_load_ = false; modal_ = Modal::none; modal_root_->hide(); quick_route_ = false;
}
bool GalacticSaveControls::input(const Ref<InputEvent>& event) {
    if (event.is_null()) return false;
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr()); key && key->is_pressed() && !key->is_echo()
        && key->get_keycode() == KEY_ESCAPE) {
        if (!visible()) open();
        else if (modal_ != Modal::none) dismiss_modal();
        else if (screen_ != Screen::escape) show_screen(Screen::escape);
        else close();
        return true;
    }
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        auto* settings = ProjectSettings::get_singleton();
        const auto save_key = utf8(settings->get_setting("eawr/input/quick_save", "F6"));
        const auto load_key = utf8(settings->get_setting("eawr/input/quick_load", "F5"));
        const auto action = viewer::galactic::save_input_action({save_key, load_key},
            utf8(OS::get_singleton()->get_keycode_string(key->get_keycode())), key->is_pressed(), key->is_echo(),
            key->is_shift_pressed() || key->is_ctrl_pressed() || key->is_alt_pressed() || key->is_meta_pressed());
        if (action != viewer::galactic::SaveInputAction::none) {
            if (token_ || loading_.valid() || pending_loaded_child_ || pending_quick_load_ || modal_ != Modal::none) {
                // Project busy-input policy: retain one F5 intent until the
                // current modal/result is dismissed; runtime parity is WGS-U04.
                if (action == viewer::galactic::SaveInputAction::quick_load) queued_quick_load_ = true;
                return true;
            }
            const bool was_visible = visible(); open();
            if (!visible()) return true;
            quick_route_ = !was_visible;
            if (action == viewer::galactic::SaveInputAction::quick_save) save(true);
            else { pending_quick_load_ = true; list(); } // Wait for any list reader before replacing its retained owner.
            return true;
        }
    }
    return visible();
}
void GalacticSaveControls::list() {
    if (listing_.valid() || loading_.valid() || pending_loaded_child_ || !state_.game) return;
    auto* owner = state_.game.get(); listing_ = std::async(std::launch::async, [owner] { return owner->save_slots(); });
}
std::string GalacticSaveControls::selected() const {
    if (selected_index_ < 0 || static_cast<std::size_t>(selected_index_) >= rows_.size()) return {};
    const auto index = rows_[static_cast<std::size_t>(selected_index_)];
    return index < listed_.size() ? listed_[index].name : std::string{};
}
void GalacticSaveControls::save(bool quick) {
    if (!state_.game || token_ || loading_.valid() || pending_loaded_child_ || modal_ != Modal::none) return;
    if (!quick && (!admission().save || screen_ != Screen::save || listing_.valid())) return;
    auto name = quick ? std::string("Quick Save") : utf8(name_->get_text());
    if (name.empty()) return; // WGS-157: ordinary button does not submit empty edits.
    if (!quick && (viewer::galactic::reserved_save_name(name) || name == "Quick Save" || name == "Auto Save")) {
        // Native reserved filenames differ from FoC; keep them out of manual entry.
        name_->clear(); return;
    }
    std::string description;
    if (selected_index_ >= 0 && static_cast<std::size_t>(selected_index_) < rows_.size()) {
        const auto index = rows_[static_cast<std::size_t>(selected_index_)];
        if (index < listed_.size()) description = listed_[index].description;
    }
    if (!quick && viewer::galactic::selected_save_overwrite(name, description)) {
        pending_name_ = name; show_modal(Modal::overwrite, localized("TEXT_SAVE_CONFIRM")); return;
    }
    begin_progress(true, std::move(name), quick, quick);
}
void GalacticSaveControls::publish_save() {
    // WGS-157: the outer Save button ignores an empty edit, but the inner
    // submission boundary still owns its explicit empty-name acknowledgement.
    if (pending_name_.empty()) {
        show_modal(Modal::acknowledgement, localized("TEXT_ENTER_SAVE_NAME")); return;
    }
    platform::LiveSaveRequest request; request.seat = sim::SeatId{state_.galaxy->local.seat};
    request.name = pending_name_; request.overwrite = pending_overwrite_; request.quick = pending_quick_;
    ViewHints hints{state_.acknowledged_battle_summary, state_.map_input.selected,
        state_.briefing_conflict, state_.briefing_done, state_.fleet_order_for_save()};
    const auto frame = state_.battle_dialog_frame();
    if (frame.auto_resolve_outcome
        && (frame.auto_resolve_outcome->invader == state_.galaxy->local.player || frame.auto_resolve_outcome->defender == state_.galaxy->local.player)
        && !hints.acknowledgement.contains(frame.auto_resolve_outcome->result.conflict)) {
        const auto planet = std::find_if(state_.metadata.planets.begin(), state_.metadata.planets.end(),
            [&](const auto& row) { return row.id == frame.auto_resolve_outcome->planet; });
        if (planet == state_.metadata.planets.end()) {
            show_modal(Modal::acknowledgement, "The battle summary's planet is unavailable."); return;
        }
        auto encoded = viewer::galactic::encode_auto_summary_hint(*frame.auto_resolve_outcome, planet->name);
        if (!encoded) { show_modal(Modal::acknowledgement, core::format_diagnostic(encoded.error())); return; }
        hints.automatic_summary = std::move(encoded).value();
    }
    request.presentation_hints = view_hints(std::move(hints));
    token_ = state_.game->request_save(std::move(request));
    save_dialog_->frame->hide(); screen_ = Screen::escape;
    if (!state_.interactive) state_.game->advance_to(state_.game->completed_tick() + 1);
}
void GalacticSaveControls::load(std::string name) {
    if (!state_.game || token_ || loading_.valid() || listing_.valid() || pending_loaded_child_ || modal_ != Modal::none) return;
    if (name.empty()) {
        if (!admission().load || screen_ != Screen::load) return;
        name = selected();
    }
    if (name.empty()) return;
    begin_progress(false, std::move(name));
}
void GalacticSaveControls::erase() {
    if (!state_.game || token_ || loading_.valid() || listing_.valid() || modal_ != Modal::none) return;
    const auto name = selected(); if (name.empty()) return;
    pending_name_ = name; show_modal(Modal::erase, localized("TEXT_DELETE_CONFIRM"));
}
bool GalacticSaveControls::refuse_load(std::string text, const bool at_start) {
    if (at_start) state_.failure = std::move(text);
    else show_modal(Modal::acknowledgement, std::move(text));
    return false;
}
bool GalacticSaveControls::restore_at_start(const std::string& name) {
    if (!state_.game || !state_.galaxy) return refuse_load("The campaign for this save did not start.", true);
    auto loaded = state_.game->prepare_load(name);
    if (!loaded) return refuse_load(core::format_diagnostic(loaded.error()), true);
    if (!accept_loaded(std::move(loaded).value(), true)) return false;
    started_from_save_ = true;
    return true;
}
bool GalacticSaveControls::accept_loaded(std::unique_ptr<platform::PreparedLiveRestore> prepared, const bool at_start) {
    // Validate selected seat against the restored layout before releasing any presentation.
    const auto seats = prepared->layout().seats;
    const auto seat = sim::SeatId{state_.galaxy->local.seat};
    if (std::none_of(seats.begin(), seats.end(), [seat](const auto& row) {
        return row.seat_id == seat && row.active && row.kind == sim::galactic::SeatKind::human;
    })) return refuse_load("The selected seat is unavailable in this save.", at_start);
    const auto restored_hash = prepared->state_sha256();
    platform::LiveGameSession::Options options; options.workers = state_.game->worker_count();
    const auto hints = view_hints(prepared->presentation_hints());
    std::shared_ptr<const platform::LiveAutoResolveOutcome> saved_summary;
    if (!hints.automatic_summary.empty()) {
        auto decoded = viewer::galactic::decode_auto_summary_hint(hints.automatic_summary);
        if (!decoded) return refuse_load(core::format_diagnostic(decoded.error()), at_start);
        saved_summary = std::move(decoded).value();
    }
    options.pacing = state_.interactive ? platform::LiveGameSession::Pacing::real_time : platform::LiveGameSession::Pacing::driven;
    options.await_presentation = true;
    auto started = platform::LiveGameSession::start(std::move(prepared), options);
    if (!started) return refuse_load(started.error().message, at_start);
    const auto first = started.value()->frame();
    if (!first.latest) return refuse_load("The restored galaxy is unavailable.", at_start);
    auto observer = galactic_view::build_live_view(*first.latest, state_.metadata, seat.value, state_.neutral_owner, state_.scene_objects);
    if (!observer.view) return refuse_load(observer.failure, at_start);
    restored_hash_ = restored_hash; restored_tick_ = first.session_tick; restored_conflict_ = first.conflict ? first.conflict->id : 0;
    restored_child_tick_ = first.battle ? first.battle->completed_tick() : 0;
    observe_world_selection();
    state_.release_battle_presentation(); state_.game->stop(); state_.game = std::move(started).value();
    state_.owner_lifetime = state_.game->lifetime(); galaxy_pause_ = false; paused_child_ = 0;
    state_.restored_auto_summary = std::move(saved_summary);
    state_.audio_event_cursor = 0; state_.acknowledged_battle_summary = hints.acknowledgement; state_.presented_conflict.reset();
    state_.briefing_conflict = hints.briefing; state_.briefing_done = hints.briefing_done;
    state_.fleet_gui_indices = hints.fleet_indices; state_.fleet_layout_dirty = true;
    state_.map_input.selected = hints.selected; state_.selected_planet = hints.selected; state_.selected_hud = {};
    state_.held.clear(); state_.orbiting = false; state_.scrolling = false; state_.pointer_warp.reset();
    state_.displayed_snapshot.reset(); state_.published_snapshot.reset(); state_.snapshot_events.clear(); state_.pause_guard = {};
    state_.corruption_visuals.reset(); state_.corruption_publication.reset();
    for (auto& particles : state_.corruption_particles) particles.reset();
    state_.corruption_lines.reset(); state_.corruption_paths.reset(); state_.corruption_path_quads = 0;
    state_.corruption_uploaded_quads.clear(); state_.corruption_quads.clear(); state_.corruption_particle_layers.clear();
    state_.economy_hud_snapshot.reset(); state_.hud_facts_snapshot.reset(); state_.hud_command_key.reset(); state_.hud_label_key.reset();
    state_.income_labels_snapshot.reset();
    if (!state_.refresh_game()) { message(state_.failure); return false; }
    state_.update_hud(); menu_selection_ = state_.map_input.selected;
    if (const auto conflict = state_.game->frame().conflict) {
        pending_loaded_child_ = conflict->id; message("Loading space battle…");
    } else {
        ++completed_loads_; state_.game->presentation_ready(); message("Game loaded.");
        modal_ = Modal::none; if (modal_root_) modal_root_->hide(); close();
    }
    return true;
}
void GalacticSaveControls::service() {
    using namespace std::chrono_literals;
    if (visible()) { observe_world_selection(); position_dates(); }
    if (listing_.valid() && listing_.wait_for(0s) == std::future_status::ready) {
        auto loaded = listing_.get();
        if (!loaded) show_modal(Modal::acknowledgement, loaded.error().message);
        else {
            listed_ = std::move(loaded).value(); populate_slots();
        }
    }
    if (visible() && (modal_ == Modal::progress_save || modal_ == Modal::progress_load)
        && fade_.is_valid() && !fade_->is_running() && modal_dialog_->frame->get_modulate().a >= 1.0F) {
        fade_.unref(); ++requests_after_fade_;
        if (modal_ == Modal::progress_save) publish_save();
        else {
            auto* owner = state_.game.get(); const auto name = pending_name_;
            loading_ = std::async(std::launch::async, [owner, name] { return owner->prepare_load(name); });
        }
    }
    if (queued_quick_load_ && modal_ == Modal::none && !loading_.valid() && !token_ && !pending_loaded_child_) {
        const bool was_visible = visible(); open();
        if (visible()) {
            queued_quick_load_ = false; pending_quick_load_ = true;
            quick_route_ = !was_visible; list();
        }
    }
    if (pending_quick_load_ && modal_ == Modal::none && !listing_.valid() && !loading_.valid() && !token_ && !pending_loaded_child_) {
        pending_quick_load_ = false; load("Quick Save");
    }
    if (token_ && state_.game) if (const auto terminal = state_.game->terminal_save_outcome(token_)) {
        const auto& outcome = *terminal;
        if (outcome.phase == platform::LiveSavePhase::completed) {
            ++completed_saves_; show_modal(Modal::acknowledgement, localized("TEXT_SAVED_OK"));
        } else show_modal(Modal::acknowledgement, outcome.diagnostic ? outcome.diagnostic->message : localized("TEXT_SAVE_FAILED"));
        token_ = 0;
    }
    if (loading_.valid() && loading_.wait_for(0s) == std::future_status::ready) {
        auto loaded = loading_.get();
        if (!loaded) show_modal(Modal::acknowledgement, loaded.error().message);
        else (void)accept_loaded(std::move(loaded).value());
    }
    if (pending_loaded_child_ && state_.game && state_.tactical && state_.game->battle_is_ready(pending_loaded_child_)) {
        pending_loaded_child_ = 0; ++completed_loads_;
        state_.game->presentation_ready(); message("Game loaded."); modal_ = Modal::none;
        if (modal_root_) modal_root_->hide(); // widgets are built lazily; a start restore has none
        close();
    }
    const bool busy = token_ != 0 || listing_.valid() || loading_.valid() || pending_loaded_child_ != 0
        || pending_quick_load_ || modal_ != Modal::none;
    const auto enabled = admission();
    const bool has_selection = selected_index_ >= 0 && static_cast<std::size_t>(selected_index_) < rows_.size()
        && rows_[static_cast<std::size_t>(selected_index_)] < listed_.size();
    const auto key = static_cast<std::uint32_t>(screen_) * 16U + (busy ? 8U : 0U)
        + (enabled.save ? 4U : 0U) + (enabled.load ? 2U : 0U) + (has_selection ? 1U : 0U);
    if (buttons_.empty() || button_key_ == key) return;
    button_key_ = key;
    for (auto* button : buttons_) {
        const String name = button->get_name();
        bool disabled = busy;
        if (name == "IDC_SAVE_GAME") disabled |= screen_ != Screen::escape || !enabled.save;
        else if (name == "IDC_LOAD_GAME") disabled |= screen_ != Screen::escape || !enabled.load;
        else if (name == "IDC_GAME_OPTIONS_RESUME") disabled |= screen_ != Screen::escape;
        else if (name == "IDC_SAVE_GAME_BUTTON") disabled |= !enabled.save;
        else if (name == "IDC_LOAD_GAME_BUTTON") disabled |= !enabled.load || !has_selection;
        else if (name == "IDC_DELETE_GAME_BUTTON") disabled |= !has_selection;
        else if (name == "IDC_INGAME_OPTIONS" || name == "IDC_GAME_OPTIONS_QUIT") disabled |= screen_ != Screen::escape;
        button->set_disabled(disabled);
    }
}
std::string GalacticSaveControls::report_json() const {
    std::ostringstream out;
    out << "{\"visible\":" << (visible() ? "true" : "false") << ",\"saved\":" << completed_saves_
        << ",\"loaded\":" << completed_loads_ << ",\"restored_tick\":" << restored_tick_
        << ",\"restored_hash\":" << galactic_detail::json(restored_hash_)
        << ",\"started_from_save\":" << (started_from_save_ ? "true" : "false")
        << ",\"restored_conflict\":" << restored_conflict_ << ",\"restored_child_tick\":" << restored_child_tick_
        << ",\"screen\":" << galactic_detail::json(screen_ == Screen::save ? "save" : screen_ == Screen::load ? "load" : "escape")
        << ",\"modal\":" << static_cast<int>(modal_) << ",\"modal_admissions\":" << modal_admissions_
        << ",\"world_leaks\":" << (world_leaks_ + (visible() && state_.map_input.selected != menu_selection_ ? 1 : 0))
        << ",\"requests_after_fade\":" << requests_after_fade_
        << ",\"quick_load_queued\":" << (queued_quick_load_ ? "true" : "false")
        << ",\"provider_seams\":[\"save-owner tutorial/movie/letterbox admission\",\"recording repository/admission\"]"
        << ",\"widget_failure\":" << galactic_detail::json(widget_failure_)
        << ",\"message\":" << galactic_detail::json(text_) << ",\"controls\":[";
    bool first = true;
    for (const auto& [name, control] : controls_) {
        if (!first) out << ','; first = false; const auto rect = control->get_global_rect();
        out << "{\"name\":\"" << name << "\",\"rect\":[" << rect.position.x << ',' << rect.position.y
            << ',' << rect.size.x << ',' << rect.size.y << "]}";
    }
    out << "]}"; return out.str();
}
} // namespace eawr::presentation::godot_backend
