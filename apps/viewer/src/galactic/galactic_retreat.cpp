#include "galactic_mode_internal.hpp"

#include "eawr/presentation/camera/arrival_camera.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/binder_common.hpp>

#include <filesystem>
#include <string>

namespace eawr::presentation::godot_backend {
using namespace godot;

bool GalacticMode::State::build_retreat_controls() {
    if (!battle_dialog_builder) { failure = "Campaign retreat dialog resources are unavailable"; return false; }
    retreat_layer = memnew(CanvasLayer); retreat_layer->set_layer(25); host->add_child(retreat_layer);
    retreat_root = memnew(Control); retreat_layer->add_child(retreat_root);
    retreat_root->set_name("CampaignRetreat");
    retreat_root->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    retreat_root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    auto built = battle_dialog_builder("IDD_RETREAT_DIALOG", host->get_viewport()->get_visible_rect().size);
    if (!built.frame) { failure = "IDD_RETREAT_DIALOG is unavailable"; return false; }
    retreat_dialog = std::move(built);
    retreat_root->add_child(retreat_dialog->frame);
    retreat_dialog->frame->hide();
    auto connect = [&](BaseButton* button, std::function<void()> callback) {
        auto* relay = memnew(EawrOverlayButton); retreat_root->add_child(relay); relay->hide();
        relay->set_action(std::move(callback));
        button->connect("pressed", callable_mp(relay, &EawrOverlayButton::activate));
    };
    // HCB-15b: the child's command bar owns the authored b_retreat slot; a shell without it
    // offers no retreat control.
    retreat_hud = tactical ? tactical->tactical_hud() : nullptr;
    if (retreat_hud) retreat_hud->set_retreat_handler([this] {
        service_retreat_controls();
        if (!retreat_view) return;
        if (retreat_view->state.phase == sim::tactical::RetreatPhase::countdown
            && retreat_view->state.player == retreat_view->player) {
            // HCB-15b: a forced countdown keeps the blink, but a press queues no command.
            if (!retreat_view->state.forced) request_retreat(sim::tactical::RetreatAction::cancel);
        }
        else if (retreat_view->refusal == sim::tactical::RetreatRefusal::none) {
            // WAR-65: the published solver gate chooses direct withdrawal.
            const auto frame = game->frame(sim::SeatId{galaxy->local.seat});
            const auto observed = platform::campaign_tactical_view_for_observer(frame, sim::SeatId{galaxy->local.seat});
            if (!observed || !observed->solver_dialog_allowed())
                request_retreat(sim::tactical::RetreatAction::start);
            else retreat_dialog_open = !retreat_dialog_open;
        }
        else if (tactical) tactical->retreat_refused(retreat_view->refusal); // RP-05
        service_retreat_controls();
    });
    for (auto& gadget : retreat_dialog->gadgets) if (auto* button = Object::cast_to<BaseButton>(gadget.control)) {
        if (gadget.id == "IDC_RETREAT") connect(button, [this] { request_retreat(sim::tactical::RetreatAction::start); });
        if (gadget.id == "IDC_AUTORESOLVE") connect(button, [this] { request_retreat(sim::tactical::RetreatAction::auto_resolve); });
        if (gadget.id == "IDC_RETRY_CANCEL") connect(button, [this] {
            if (retreat_view && retreat_view->state.phase == sim::tactical::RetreatPhase::countdown
                && retreat_view->state.player == retreat_view->player && !retreat_view->state.forced)
                request_retreat(sim::tactical::RetreatAction::cancel);
            retreat_dialog_open = false;
            service_retreat_controls();
        });
    }
    service_retreat_controls();
    return true;
}

// Eye-check proof only (--eawr-galactic-space-proof retreat): one lit frame per simulation cinematic frame, named by
// its frame number, beside the proof's own captures. The image is the last frame drawn, so it can lag the name by one.
void GalacticMode::State::capture_retreat_cinematic_frame(const std::uint32_t frame) {
    if (proof_cinematic_frame && *proof_cinematic_frame == frame) return;
    proof_cinematic_frame = frame;
    const auto directory = options.report_path.parent_path() / "space-cycle" / "retreat-cinematic";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { failure = "Cannot create the retreat cinematic capture directory"; return; }
    std::string name = std::to_string(frame);
    name.insert(0, name.size() < 4 ? 4 - name.size() : 0, '0');
    const auto image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->save_png(String((directory / ("k" + name + ".png")).generic_string().c_str())) != OK)
        failure = "Cannot capture retreat cinematic frame " + std::to_string(frame);
}

void GalacticMode::State::request_retreat(sim::tactical::RetreatAction action) {
    if (!game || !conflict || !retreat_view) return;
    using namespace sim::tactical;
    const auto frame = game->frame(sim::SeatId{galaxy->local.seat});
    if (!frame.conflict || frame.conflict->id != conflict || !frame.battle) return;
    const auto observed = platform::campaign_tactical_view_for_observer(frame, sim::SeatId{galaxy->local.seat});
    if (!observed || !observed->retreat) return;
    const auto* published = observed->retreat;
    if (action == RetreatAction::start && published->refusal != RetreatRefusal::none) return;
    if (action == RetreatAction::cancel && (published->state.phase != RetreatPhase::countdown
        || published->state.player != published->player || published->state.forced)) return;
    if (action == RetreatAction::auto_resolve
        && !observed->solver_dialog_allowed()) return;
    if (const auto feed = game->battle_feed(conflict, sim::SeatId{galaxy->local.seat}))
        feed->submit({published->player, {}, RetreatPayload{action}, {}});
}

void GalacticMode::State::service_retreat_controls() {
    if (!retreat_dialog || !game || !galaxy) return;
    const auto frame = game->frame(sim::SeatId{galaxy->local.seat});
    const auto observed = platform::campaign_tactical_view_for_observer(frame, sim::SeatId{galaxy->local.seat});
    if (!observed || !frame.battle || !frame.conflict || frame.conflict->id != conflict) {
        retreat_view.reset(); retreat_snapshot.reset(); retreat_dialog_open = false;
        if (tactical) { tactical->update_retreat_countdown({}, 0, false); tactical->end_retreat_cinematic(); }
        retreat_dialog->frame->hide(); if (retreat_hud) retreat_hud->set_retreat({});
        retreat_root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        return;
    }
    const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [&](const auto& seat) { return seat.seat == sim::SeatId{galaxy->local.seat}; });
    if (binding == frame.conflict->seats.end()) {
        retreat_view.reset(); retreat_snapshot.reset(); retreat_dialog_open = false;
        if (tactical) { tactical->update_retreat_countdown({}, 0, false); tactical->end_retreat_cinematic(); }
        retreat_dialog->frame->hide(); if (retreat_hud) retreat_hud->set_retreat({});
        retreat_root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        return;
    }
    const auto views = frame.battle->retreat_views();
    const auto published = std::find_if(views.begin(), views.end(), [&](const auto& view) { return view.player == binding->player; });
    const bool changed = retreat_snapshot != frame.battle;
    if (changed) {
        retreat_view = published == views.end() ? std::nullopt : std::optional(*published);
        retreat_snapshot = frame.battle;
    }
    using namespace sim::tactical;
    const bool eligible = retreat_view && retreat_view->refusal == RetreatRefusal::none;
    const bool solver_allowed = observed && observed->solver_dialog_allowed();
    const bool countdown = retreat_view && retreat_view->state.phase == RetreatPhase::countdown
        && retreat_view->state.player == binding->player;
    const bool escaping = retreat_view && (retreat_view->state.phase == RetreatPhase::escaping || retreat_view->state.phase == RetreatPhase::escaped);
    if (frame.battle->outcome() || !retreat_view || escaping || !eligible || countdown || !solver_allowed)
        retreat_dialog_open = false; // WAR-65: pending victory/withdrawal closes the dialog.
    if (retreat_view && retreat_view->state.phase == RetreatPhase::escaping) {
        // FS-R08/09: the in-engine cinematic plays through the child battle's camera when it can (an entry marker, the
        // Unlocked camera data, In_Game_Cinematics). The simulation owns its time (FC-R02); the reports below only tell
        // it whether playback exists (WR-50) and when it ended.
        const auto& state = retreat_view->state;
        const bool playable = tactical && tactical->retreat_cinematic_ready();
        if (playable) tactical->update_retreat_cinematic(state, frame.battle->completed_tick());
        const bool playing = playable && tactical->retreat_cinematic_active();
        if (playing && options.space_cycle_proof == "retreat") capture_retreat_cinematic_frame(state.cinematic_frame);
        // #4444: retail plays the cinematic for whichever side retreats; only the retreating local player's own reports
        // are commands of that player (an AI side's seat setting already says its playback exists, and the simulation's
        // timer ends it, FC-R02).
        if (state.player == binding->player && !retreat_completion_sent) if (const auto feed = game->battle_feed(conflict, sim::SeatId{galaxy->local.seat})) {
            if (!playing) {
                // WR-50: no playback in this battle. Publish that fact once.
                feed->submit({binding->player, {}, RetreatCinematicPayload{false, 0}, {}});
                retreat_completion_sent = true;
            } else {
                if (!retreat_playback_started) {
                    feed->submit({binding->player, {}, RetreatCinematicPayload{true, 0}, {}});
                    retreat_playback_started = true;
                }
                // The simulation completes the retreat on the frame that reaches the duration and refuses reports after
                // it, so the end is reported with the last playing frame.
                if (state.cinematic_frame + 1 >= state.cinematic_duration) {
                    feed->submit({binding->player, {}, RetreatCinematicPayload{false, 0}, {}});
                    retreat_completion_sent = true;
                }
            }
        }
    } else if (tactical && tactical->retreat_cinematic_active()) {
        tactical->end_retreat_cinematic();
    }
    if (tactical && retreat_view)
        tactical->update_retreat_countdown(retreat_view->state, frame.battle->completed_tick(),
            retreat_view->state.playback_playing || tactical->retreat_cinematic_active());
    if (retreat_hud) retreat_hud->set_retreat(presentation::ui::retreat_button_view(
        retreat_view, frame.battle->completed_tick(), frame.battle->outcome().has_value()));
    retreat_dialog->frame->set_visible(retreat_dialog_open);
    retreat_root->set_mouse_filter(retreat_dialog_open ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
    for (auto& gadget : retreat_dialog->gadgets) {
        if (auto* button = Object::cast_to<BaseButton>(gadget.control)) {
            if (gadget.id == "IDC_RETREAT") button->set_disabled(!eligible || countdown || escaping);
            if (gadget.id == "IDC_AUTORESOLVE") button->set_disabled(!solver_allowed || escaping);
        }
        if (changed && gadget.id == "IDC_RETREAT_HEADER") if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) {
            std::string message = battle_text("TEXT_RETREAT_DIALOG");
            if (countdown) {
                const auto tick = frame.battle->completed_tick();
                const auto remaining = retreat_view->state.deadline > tick ? retreat_view->state.deadline - tick : 0;
                message += " " + std::to_string((remaining + sim::tactical::logical_frames_per_second - 1)
                    / sim::tactical::logical_frames_per_second);
            } else if (!eligible) message += " (retreat unavailable)";
            label->set_text(String(message.c_str()));
        }
    }
}
// SK-ARR-01: the campaign space battle opens with the arrival cinematic. It is refused in a multiplayer battle (the debug
// build builds no cinematic there), for a story-linked battle (SK-ARR-13: the held-arrival mode is the story engine's and is
// not published yet) and for a battle that is not a space battle. The simulation never hears of it: the camera is a function
// of the presented tick (SK-ARR-14).
void GalacticMode::State::service_arrival_cinematic() {
    if (!tactical || !game || !galaxy || arrival_cinematic_done) return;
    const auto seat = sim::SeatId{galaxy->local.seat};
    const auto frame = game->frame(seat);
    if (!frame.battle || !frame.conflict || frame.conflict->id != conflict) return;
    if (!arrival_gate_checked) {
        // SK-ARR-01: a battle restored from a save past its start plays no arrival; one ready at tick 0 does.
        arrival_gate_checked = true;
        const bool admitted = presentation::camera::arrival_admits_battle_ready_at(child_ready_tick);
        arrival_gate_log.emplace_back(child_ready_tick, admitted);
        if (!admitted) { arrival_cinematic_done = true; return; }
    }
    // The battle's loading screen (and its Begin button) comes first: the cinematic starts when the battle does.
    if (loading && loading->shown()) return;
    const auto tick = frame.battle->completed_tick();
    if (tick >= presentation::camera::arrival_rules::duration_frames) {
        tactical->end_arrival_cinematic();
        arrival_cinematic_done = true;
        return;
    }
    const auto observed = platform::campaign_tactical_view_for_observer(frame, seat);
    const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [&](const auto& candidate) { return candidate.seat == seat; });
    const bool refused = frame.conflict->layer != campaign::BattleLayer::space || !observed
        || (observed->policy && observed->policy->multiplayer) || !frame.conflict->linked_mission_options.empty()
        || binding == frame.conflict->seats.end();
    if (refused) {
        arrival_cinematic_done = true;
        return;
    }
    // The battle's map view and camera may not be drawn yet, and a map without an entry marker never will be: look again
    // next frame until the cinematic window closes.
    if (!tactical->arrival_cinematic_ready()) return;
    MapMode::ArrivalRequest request;
    request.attacker = binding->player == frame.conflict->invader;
    request.defender = binding->player; // read only when the local player defends
    if (options.arrival_proof == "defender") {
        // Eye-check proof only: show the defender shot from the proof's attacker seat, looking at the opponent's star base.
        request.attacker = false;
        for (const auto& unit : frame.battle->instances()) {
            if (unit.owner == binding->player || !sim::tactical::players_hostile(frame.battle->players(), binding->player, unit.owner)) continue;
            request.defender = unit.owner;
            break;
        }
    }
    // SK-ARR-09: a presentation-only seed; both ends of one battle derive the same shot.
    request.seed = static_cast<std::uint64_t>(frame.conflict->id) * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(frame.conflict->planet);
    tactical->update_arrival_cinematic(request, tick);
    if (!arrival_counted && tactical->arrival_cinematic_active()) { arrival_counted = true; ++arrival_cinematic_starts; }
    if (tactical->arrival_focus_losses() > arrival_focus_losses) arrival_focus_losses = tactical->arrival_focus_losses();
    if (!options.arrival_proof.empty()) if (const auto shown = tactical->arrival_cinematic_frame()) capture_arrival_cinematic_frame(*shown);
}

// Eye-check proof only (--eawr-galactic-arrival-proof): one lit frame per presented arrival frame, named by its frame
// number, beside the proof's own captures. The image is the last frame drawn, so it can lag the name by one.
void GalacticMode::State::capture_arrival_cinematic_frame(const std::uint32_t frame) {
    if (proof_arrival_frame && *proof_arrival_frame == frame) return;
    proof_arrival_frame = frame;
    const auto directory = options.report_path.parent_path() / "space-cycle" / ("arrival-" + options.arrival_proof);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { failure = "Cannot create the arrival cinematic capture directory"; return; }
    std::string name = std::to_string(frame);
    name.insert(0, name.size() < 4 ? 4 - name.size() : 0, '0');
    const auto image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->save_png(String((directory / ("k" + name + ".png")).generic_string().c_str())) != OK)
        failure = "Cannot capture arrival cinematic frame " + std::to_string(frame);
}
} // namespace eawr::presentation::godot_backend
