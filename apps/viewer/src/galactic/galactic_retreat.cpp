#include "galactic_mode_internal.hpp"

#include <godot_cpp/core/binder_common.hpp>

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
    const auto frame = game->frame();
    const auto observed = platform::campaign_tactical_view_for_observer(frame, sim::SeatId{galaxy->local.seat});
    if (!observed || !frame.battle || !frame.conflict || frame.conflict->id != conflict) {
        retreat_view.reset(); retreat_snapshot.reset(); retreat_dialog_open = false;
        if (tactical) tactical->update_retreat_countdown({}, 0, false);
        retreat_dialog->frame->hide(); if (retreat_hud) retreat_hud->set_retreat({});
        retreat_root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        return;
    }
    const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [&](const auto& seat) { return seat.seat == sim::SeatId{galaxy->local.seat}; });
    if (binding == frame.conflict->seats.end()) {
        retreat_view.reset(); retreat_snapshot.reset(); retreat_dialog_open = false;
        if (tactical) tactical->update_retreat_countdown({}, 0, false);
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
    if (retreat_view && retreat_view->state.phase == RetreatPhase::escaping && retreat_view->state.player == binding->player
        && !retreat_completion_sent) {
        // WR-50: no playback is installed in this route. Publish that fact
        // once; the drawn flight/station cinematic remains an explicit seam.
        if (const auto feed = game->battle_feed(conflict, sim::SeatId{galaxy->local.seat})) {
            feed->submit({binding->player, {}, RetreatCinematicPayload{false, 0}, {}});
            retreat_completion_sent = true;
        }
    }
    if (tactical && retreat_view)
        tactical->update_retreat_countdown(retreat_view->state, frame.battle->completed_tick(), retreat_view->state.playback_playing);
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
} // namespace eawr::presentation::godot_backend
