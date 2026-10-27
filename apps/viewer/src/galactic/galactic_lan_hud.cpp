#include "galactic_mode_internal.hpp"
#include "galactic_lan_hud.hpp"
#include "ui/battle_overlay.hpp"
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

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
    const auto text = lan_notice_model.notice(view, options.lan_players,
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
        const std::array<const char*, 4> captions{"Pause / Resume", "0.5x", "1x", "2x"};
        for (std::size_t i = 0; i < captions.size(); ++i) {
            auto* action = memnew(EawrOverlayButton);
            action->set_position({20.0F + static_cast<float>(i) * 120.0F, 40.0F});
            action->set_size({112, 32});
            action->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
            action->set_action([this, i] {
                if (!game || game->frame(sim::SeatId{galaxy->local.seat}).battle || !options.lan_match || !galactic_lan_gameplay_enabled(options.lan_match->view())
                    || map_input.gates.tutorial_locked || !map_input.gates.world_allowed()) return;
                if (i == 0) request_pause(!paused);
                else {
                    const auto raw = sim::math::Fixed::scale;
                    submit_strategic_speed(sim::math::Fixed::from_raw(i == 1 ? raw / 2 : (i == 2 ? raw : raw * 2)));
                }
            });
            auto* button = memnew(godot::Button);
            button->set_text(captions[i]);
            button->set_size({112, 32});
            button->set_focus_mode(godot::Control::FOCUS_NONE);
            button->set_tooltip_text("Shared clock: pause and resume do not count against the speed budget (1 s double-press debounce); speed changes are limited to 6 per 45 session seconds per player");
            button->connect("pressed", callable_mp(action, &EawrOverlayButton::activate));
            action->add_child(button);
            layer->add_child(action);
            lan_clock_buttons[i] = button;
        }
    }
    const bool strategic = game && !game->frame(sim::SeatId{galaxy->local.seat}).battle;
    for (auto* button : lan_clock_buttons) {
        if (!button) continue;
        button->set_visible(strategic);
        button->set_disabled(!strategic || !galactic_lan_gameplay_enabled(view)
            || map_input.gates.tutorial_locked || !map_input.gates.world_allowed());
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
