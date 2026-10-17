#include "tactical_hud_internal.hpp"

namespace eawr::presentation::godot_backend {
using tactical_hud_detail::text;
using tactical_hud_detail::rect2;

TacticalHud::State::State(Options value) : options(std::move(value)) {}

void TacticalHud::State::warn(std::string code, std::string message, std::string path) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::move(code);
        diagnostic.severity = core::Severity::warning;
        diagnostic.message = std::move(message);
        diagnostic.logical_path = std::move(path);
        diagnostics.push_back(std::move(diagnostic));
    }

void TacticalHud::world_input(const Ref<InputEvent>& event) {
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button != nullptr && button->is_pressed() && button->get_button_index() == MOUSE_BUTTON_LEFT) {
        ++state_->world_presses;
    }
}

TacticalHud::TacticalHud(Options options) : state_(std::make_unique<State>(std::move(options))) {}
TacticalHud::~TacticalHud() = default;

const std::string& TacticalHud::failure() const noexcept { return state_->failure; }

int TacticalHud::options_presses() const noexcept {
    return state_->options_button != nullptr ? state_->options_button->presses() : 0;
}
EawrTacticalHud* TacticalHud::hud() const noexcept { return state_->hud; }

std::string TacticalHud::localized_text(std::string_view key, std::string_view fallback) const {
    if (state_->text_database)
        if (const auto* entry = state_->text_database->find(key)) return data::ui::to_utf8(entry->value);
    return std::string(fallback);
}

void TacticalHud::set_time_handlers(TimeHandlers handlers) { state_->time_handlers = std::move(handlers); }
void TacticalHud::set_movie_handler(std::function<void()> handler) { state_->movie_handler = std::move(handler); }
void TacticalHud::set_options_handler(std::function<void()> handler) { state_->options_handler = std::move(handler); }
void TacticalHud::set_beacon_handler(std::function<void()> handler) { state_->beacon_handler = std::move(handler); }
void TacticalHud::set_beacon(const bool enabled, const bool targeting) {
    if (auto* button = state_->beacon_button) {
        button->set_special_disabled(!enabled);
        button->set_order_selected(targeting);
    }
}
bool TacticalHud::movie_mode() const { return state_->movie; }
void TacticalHud::set_movie_mode(const bool on) {
    auto& state = *state_;
    state.movie = on && state.movie_height.has_value();
    if (state.mask) state.mask->set_visible(!state.overview && !state.movie);
    if (state.movie_bands) {
        // CanvasLayer supplies no parent Control rectangle for full-rect anchors.
        // Keep the band's parent at the current viewport size, including resizes.
        state.movie_bands->set_position(Vector2{});
        state.movie_bands->set_size(state.movie_bands->get_viewport()->get_visible_rect().size);
        state.movie_bands->set_visible(state.movie);
    }
    if (state.overlay) state.overlay->set_visible(!state.movie);
}
void TacticalHud::set_order_handler(std::function<void(std::string_view)> handler) {
    state_->order_handler = std::move(handler);
}
void TacticalHud::set_order_mode(const std::string_view active) {
    for (const auto& [name, button] : state_->order_buttons) {
        button->set_order_selected(name == active);
    }
}

void TacticalHud::set_special_weapon_handler(std::function<void()> handler) { state_->special_handler = std::move(handler); }
void TacticalHud::set_special_weapon(const model::SpecialWeaponButton& view) {
    auto& state = *state_;
    state.special_view = view;
    if (!state.special_button) return;
    state.special_button->set_special_disabled(!view.enabled());
    state.special_button->set_order_selected(view.targeting);
    if (state.special_dial && state.special_dial->completion != view.dial()) {
        state.special_dial->completion = view.dial();
        state.special_dial->queue_redraw();
    }
}

void TacticalHud::set_retreat_handler(std::function<void()> handler) { state_->retreat_handler = std::move(handler); }
const model::RetreatButtonView& TacticalHud::retreat() const noexcept { return state_->retreat_view; }
void TacticalHud::set_retreat(const model::RetreatButtonView& view) {
    auto& state = *state_;
    if (state.retreat_view == view) return;
    state.retreat_view = view;
    if (!state.retreat_button) return;
    state.retreat_button->set_special_disabled(!view.enabled);
    state.retreat_button->set_flashing(view.flashing);
    if (state.retreat_dial && state.retreat_dial->completion != view.dial) {
        state.retreat_dial->completion = view.dial;
        state.retreat_dial->queue_redraw();
    }
}

void TacticalHud::set_time_view(const TimeView& view) {
    State& state = *state_;
    state.time_view = view;
    state.time_view_set = true;
    const String network_help = "Pause and fast-forward are unavailable in multiplayer. Shared speed: "
        + String::num_int64(view.speed_step) + ". Use = / - to request a change (two per 45 seconds).";
    if (state.pause_button != nullptr) {
        // TM-08: pausing starts an additive flash; it does not hold the selected state.
        state.pause_button->set_pressed_no_signal(false);
        state.pause_button->set_disabled(!view.pause_enabled);
        state.pause_button->set_tooltip_text(view.multiplayer ? network_help : String());
        state.pause_button->set_flashing(view.paused && view.pause_enabled);
    }
    if (state.fast_forward_button != nullptr) {
        state.fast_forward_button->set_pressed_no_signal(view.fast_forward);
        state.fast_forward_button->set_disabled(!view.fast_forward_enabled);
        state.fast_forward_button->set_tooltip_text(view.multiplayer ? network_help : String());
        // TM-08: tinted grey (128) while disabled.
        const float tint = view.fast_forward_enabled ? 1.0F : 128.0F / 255.0F;
        state.fast_forward_button->set_self_modulate(Color(tint, tint, tint, 1.0F));
    }
    if (state.overlay != nullptr) state.overlay->show_paused(view.paused && !state.overview);
}

void TacticalHud::set_overview(const bool on) {
    State& state = *state_;
    if (state.overview == on) return;
    state.overview = on;
    // V-5b: the hit mask carries the shell and everything placed on it; hidden, it takes no input.
    if (state.mask != nullptr) state.mask->set_visible(!on && !state.movie);
    if (state.overlay != nullptr) state.overlay->show_paused(state.time_view.paused && !on);
}

bool TacticalHud::shell_shown() const { return state_->mask != nullptr && state_->mask->is_visible(); }

bool TacticalHud::pause_banner_shown() const { return state_->overlay != nullptr && state_->overlay->paused_shown(); }

void TacticalHud::set_battle(const std::optional<bool> won, const bool ended) {
    if (state_->overlay == nullptr) return;
    state_->overlay->show_message(won);
    state_->overlay->show_end(ended ? won : std::nullopt);
}

void TacticalHud::set_retreat_countdown(const std::string& line, const std::array<std::uint8_t, 4>& colour) {
    if (state_->overlay == nullptr) return;
    const auto& looks = state_->message_looks;
    state_->overlay->show_countdown(tactical_hud_detail::text(line), Color(colour[0] / 255.0F, colour[1] / 255.0F, colour[2] / 255.0F, colour[3] / 255.0F),
        looks.countdown_x, looks.countdown_y);
}

void TacticalHud::set_begin(const bool ready) {
    if (state_->overlay != nullptr) state_->overlay->show_begin(ready);
}

void TacticalHud::set_results(const presentation::ui::BattleResults& results) {
    if (state_->overlay != nullptr) state_->overlay->set_results(results);
}

void TacticalHud::set_replay_writer(const bool allowed, ReplayWriter writer) {
    state_->replay_writer = std::move(writer);
    if (state_->overlay) state_->overlay->allow_save_replay(allowed);
}

std::optional<std::array<float, 2>> TacticalHud::control_point(const std::string& name) const {
    const State& state = *state_;
    const auto centre = [](const Rect2& rect) {
        const Vector2 point = rect.get_center();
        return std::array<float, 2>{point.x, point.y};
    };
    const auto button_centre = [&](const EawrHudButton* button) -> std::optional<std::array<float, 2>> {
        if (button == nullptr || !button->is_visible_in_tree()) return std::nullopt;
        return centre(Rect2(button->get_global_position() + button->hit_rect().position, button->hit_rect().size));
    };
    if (name == "pause") return button_centre(state.pause_button);
    if (name == "options" || name == "b_option_t") return button_centre(state.options_button);
    if (name == "movie_camera" || name == "b_camera_t") return button_centre(state.movie_button);
    if (name == "beacon" || name == "b_special_weapon2") return button_centre(state.beacon_button);
    if (name == "special_weapon" || name == "b_special_weapon") return button_centre(state.special_button);
    if (name == "retreat" || name == "b_retreat") return button_centre(state.retreat_button);
    if (const auto found = state.order_buttons.find(name); found != state.order_buttons.end()) return button_centre(found->second);
    if (state.production != nullptr) {
        if (const auto rect = state.production->control_rect(name)) return centre(*rect);
    }
    if (name == "fast_forward") return button_centre(state.fast_forward_button);
    if (state.overlay != nullptr) {
        if (const auto rect = state.overlay->control_rect(name)) return centre(*rect);
    }
    return std::nullopt;
}

EawrUnitCards* TacticalHud::unit_cards() const noexcept { return state_->cards; }
Ref<Font> TacticalHud::world_group_font(const bool squadron) const { return state_->world_group_fonts[squadron ? 0 : 1]; }

EawrProductionPanel* TacticalHud::production() const noexcept { return state_->production; }

std::optional<std::int64_t> TacticalHud::listed_build_cost(const std::string& type) const {
    return model::unit_card_looks(type, state_->objects, nullptr).build_cost;
}

EawrMinimap* TacticalHud::minimap() const noexcept { return state_->minimap; }

EawrAbilityButtons* TacticalHud::ability_buttons() const noexcept { return state_->abilities; }

void TacticalHud::set_ability_bar(const model::AbilityBar& bar) {
    if (state_->abilities != nullptr) state_->abilities->show(bar);
}

void TacticalHud::set_minimap(const MinimapView& view) {
    State& state = *state_;
    if (state.minimap == nullptr) return;
    state.minimap_extents = view.extents;
    const auto looks = [this](const std::string_view type) -> const model::MinimapTypeLooks& { return minimap_looks(type); };
    const auto [width, height] = state.minimap->pixel_size();
    state.minimap->set_hazards(view.hazards, view.extents, state.minimap_settings);
    state.minimap_fog.resize(width, height);
    const bool advanced = view.cells
        ? state.minimap_fog.advance(view.extents, *view.cells, state.minimap_settings.fog, view.fog)
        : state.minimap_fog.advance(view.extents, view.revealers, state.minimap_settings.fog, view.fog);
    if (advanced) {
        state.minimap->set_fog(state.minimap_fog.texels(), state.minimap_fog.width(), state.minimap_fog.height(),
                               state.minimap_fog.passes());
    }
    EawrMinimap::Frame frame;
    for (const auto& warning : view.warnings) {
        frame.warnings.push_back({model::minimap_point(view.extents, warning.x, warning.y), warning.age});
    }
    frame.blips = model::minimap_blips(view.units, looks, view.extents, state.minimap_settings, view.memories);
    if (view.ground) frame.guide = model::minimap_guide(*view.ground, view.extents, state.minimap_settings.guide_rectangle);
    state.minimap->show(std::move(frame));
}

void TacticalHud::set_minimap_handlers(std::function<void(double, double)> look, std::function<void(double, double, bool)> move) {
    state_->minimap_look = std::move(look);
    state_->minimap_move = std::move(move);
}


std::optional<std::array<float, 2>> TacticalHud::minimap_point(const double x, const double y) const {
    const EawrMinimap* minimap = state_->minimap;
    if (minimap == nullptr || !minimap->is_visible_in_tree() || !minimap->minimap_rect().has_area()) return std::nullopt;
    const Vector2 at = minimap->to_screen({x, y});
    return std::array<float, 2>{at.x, at.y};
}

void TacticalHud::set_unit_cards(const model::CardLayout& layout, const std::span<const model::CardUnit> units) {
    if (state_->cards == nullptr) return;
    std::vector<EawrUnitCards::Card> cards;
    cards.reserve(layout.cards.size());
    for (const model::UnitCard& card : layout.cards) {
        if (card.unit >= units.size()) continue;
        cards.push_back({card.slot, units[card.unit].type, card.count, card.stacked, card.health_level, card.shield});
    }
    state_->cards->show(std::move(cards), layout.borders);
}


} // namespace eawr::presentation::godot_backend
