#include "galactic_mode_internal.hpp"
#include "galactic_lan_hud.hpp"
#include "ui/tactical_hud_internal.hpp"
#include <godot_cpp/classes/time.hpp>

namespace eawr::presentation::godot_backend {

void GalacticMode::State::service_lan_hud() {
    if (!options.lan_match || !host) return;
    const auto view = options.lan_match->view();
    for (const auto& refusal : view.clock_refusals) {
        if (refusal.seat == options.lan_match->local_seat() && refusal.completed_tick > lan_clock_refusal_seen) {
            lan_clock_refusal_seen = refusal.completed_tick;
            pause_guard = galactic_view::PauseToggleGuard{};
        }
    }
    if (galactic_lan_gameplay_enabled(view) && game && !game->frame(sim::SeatId{galaxy->local.seat}).battle
        && galactic_view::lan_releases_departed_pause(view, options.lan_players, options.lan_match->local_seat())) request_pause(false);
    auto text = lan_notice_model.notice(view, options.lan_players,
        godot::Time::get_singleton()->get_ticks_msec());
    if (!lan_notice_label) {
        auto* layer = memnew(godot::CanvasLayer);
        layer->set_layer(90);
        layer->set_name("GalacticLanNoticeLayer");
        lan_notice_label = memnew(godot::Label);
        lan_notice_label->set_name("GalacticLanNotice");
        lan_notice_label->set_position({20, 80});
        lan_notice_label->add_theme_color_override("font_shadow_color", godot::Color(0, 0, 0, 1));
        lan_notice_label->add_theme_constant_override("shadow_offset_x", 1);
        lan_notice_label->add_theme_constant_override("shadow_offset_y", 1);
        lan_notice_label->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
        layer->add_child(lan_notice_label);
        host->add_child(layer);
    }
    if (lan_notice_label) {
        if (text != lan_notice_text) {
            lan_notice_text = text;
            lan_notice_label->set_text(godot::String::utf8(text.c_str()));
        }
        lan_notice_label->set_visible(!text.empty());
    }
}

} // namespace eawr::presentation::godot_backend
