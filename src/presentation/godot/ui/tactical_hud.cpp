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

void TacticalHud::prepare_command_bar_movie(const vfs::Vfs& filesystem, const std::string_view movie, BinkSourceFactory decoder_factory,
    std::function<void(std::string, std::string, std::string)> failure_sink) {
    if (state_->movie_bone && state_->hud) {
        state_->hud->set_meta("eawr_movie_ready", false);
        state_->command_bar_movie.prepare(filesystem, movie, *state_->movie_bone, *state_->hud, std::move(decoder_factory), std::move(failure_sink));
        state_->hud->set_movie_warmup([state = state_.get()] {
            if (!state->command_bar_movie.warmup()) return false;
            state->hud->set_meta("eawr_movie_ready", true);
            return true;
        });
    }
}
bool TacticalHud::play_command_bar_movie(const std::string_view speech) {
    return state_->hud && state_->command_bar_movie.start(*state_->hud, speech);
}
void TacticalHud::service_command_bar_movie(const bool speech_present, const bool paused) {
    if (state_->hud) state_->command_bar_movie.service(speech_present, paused, state_->hud->placement());
}
bool TacticalHud::command_bar_movie_playing() const { return state_->command_bar_movie.playing(); }
bool TacticalHud::command_bar_movie_active() const { return state_->command_bar_movie.active(); }
std::string_view TacticalHud::command_bar_movie_speech() const { return state_->command_bar_movie.speech(); }
void TacticalHud::write_command_bar_movie_report(std::ostream& output) const { state_->command_bar_movie.write_report(output); }

std::string TacticalHud::localized_text(std::string_view key, std::string_view fallback) const {
    if (state_->text_database)
        if (const auto* entry = state_->text_database->find(key)) return data::ui::to_utf8(entry->value);
    return std::string(fallback);
}

void TacticalHud::set_time_handlers(TimeHandlers handlers) { state_->time_handlers = std::move(handlers); }
void TacticalHud::set_movie_handler(std::function<void()> handler) { state_->movie_handler = std::move(handler); }
void TacticalHud::set_options_handler(std::function<void()> handler) { state_->options_handler = std::move(handler); }
void TacticalHud::set_beacon_handler(std::function<void()> handler) { state_->beacon_handler = std::move(handler); }
void TacticalHud::set_gift_handler(std::function<void(sim::SeatId)> handler) { state_->gift_handler = std::move(handler); }

void TacticalHud::set_gift(const bool policy, const bool enabled, const std::vector<model::ObserverOwner>& recipients) {
    auto& state = *state_;
    state.gift_policy = policy;
    const bool changed = state.gift_recipients != recipients;
    state.gift_enabled = policy && enabled && !recipients.empty();
    if (state.gift_open && (recipients.size() != state.gift_recipients.size()
        || !std::equal(recipients.begin(), recipients.end(), state.gift_recipients.begin(),
            [](const auto& a, const auto& b) { return a.seat == b.seat; }))) state.gift_open = false;
    if (changed) state.gift_recipients = recipients;
    if (!state.gift_enabled) state.gift_open = false;
    const auto definition = state.project_hud.find("gift");
    if (!policy || !state.hud || definition == state.project_hud.end() || !definition->second.enabled) {
        if (state.gift_button) state.gift_button->hide();
        if (state.gift_scroll) state.gift_scroll->hide();
        for (auto& cell : state.gift_cells) cell.button->hide();
        state.gift_viewport = Vector2(-1, -1);
        return;
    }
    const auto& look = definition->second;
    if (look.stock_art && state.shell.order_buttons.empty()) {
        if (!state.gift_dock_warning) state.warn("EAWR-UI-0790", "gift control stock art is unavailable");
        state.gift_dock_warning = true;
        if (state.gift_button) state.gift_button->hide();
        if (state.gift_scroll) state.gift_scroll->hide();
        return;
    }
    const auto& normal_name = look.stock_art ? state.shell.order_buttons.front().blank : look.normal;
    if (state.command_texture(normal_name, "gift normal", true).is_null()) {
        if (!state.gift_dock_warning) state.warn("EAWR-UI-0790", "gift control normal texture is unavailable", normal_name);
        state.gift_dock_warning = true;
        state.gift_open = false;
        if (state.gift_button) state.gift_button->hide();
        if (state.gift_scroll) state.gift_scroll->hide();
        return;
    }
    const auto previous_viewport = state.gift_viewport;
    const bool previous_enabled = state.gift_drawn_enabled;
    const bool previous_open = state.gift_drawn_open;
    state.gift_viewport = state.hud->get_size();
    const auto make_button = [&](Control* parent) {
        auto* button = memnew(EawrHudButton);
        if (look.stock_art && !state.shell.order_buttons.empty()) {
            const auto& stock = state.shell.order_buttons.front();
            const auto blank = state.command_texture(stock.blank, "gift command blank");
            button->setup_art({.normal = blank, .hover = {}, .pressed = {}, .disabled = blank,
                .portrait = {}, .overlays = HudButtonArt::Overlays{state.command_texture(stock.mouse_over, "gift command hover"),
                    state.command_texture(stock.pressed, "gift command pressed"), {}, !stock.selected_alpha}});
        } else {
            button->setup_art({.normal = state.command_texture(look.normal, "gift normal"),
                .hover = state.command_texture(look.hover, "gift hover"),
                .pressed = state.command_texture(look.pressed, "gift pressed"),
                .disabled = state.command_texture(look.disabled, "gift disabled"), .portrait = {}, .overlays = {}});
        }
        parent->add_child(button);
        auto* label = memnew(Label);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        label->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
        label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
        label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
        label->set_clip_text(true);
        label->add_theme_color_override("font_color", Color(1, 1, 1));
        label->add_theme_color_override("font_outline_color", Color(0, 0, 0));
        label->add_theme_constant_override("outline_size", 2);
        if (state.fonts) label->add_theme_font_override("font", state.fonts->font(state.card_face));
        button->add_child(label);
        return State::GiftCell{button, label, {}};
    };
    if (!state.gift_button) {
        auto cell = make_button(state.hud);
        state.gift_button = cell.button;
        state.gift_button->set_name("gift");
        const data::ui::TextDatabase empty;
        data::ui::TextLookup lookup(state.text_database ? *state.text_database : empty);
        cell.label->set_text(text(data::ui::to_utf8(lookup.text(look.caption))));
        state.gift_button->set_tooltip_text(text(data::ui::to_utf8(lookup.text(look.tooltip))));
        state.gift_button->set_action([this] {
            auto& current = *state_;
            if (!current.gift_enabled) return;
            ++current.gift_presses;
            current.gift_open = !current.gift_open;
            set_gift(true, current.gift_enabled, current.gift_recipients);
        });
        state.gift_scroll = memnew(ScrollContainer);
        state.gift_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
        state.gift_scroll->set_z_index(1);
        state.hud->add_child(state.gift_scroll);
        state.gift_list = memnew(Control);
        state.gift_list->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        state.gift_scroll->add_child(state.gift_list);
    }
    const float scale = static_cast<float>(state.hud->space().scale);
    std::map<std::string, model::PixelRect> anchors;
    std::vector<model::PixelRect> occupied;
    for (const auto& component : state.project_hud_anchors) {
        if (!component.visible) continue;
        const auto r = model::shell_to_screen(component.rect, state.hud->placement());
        anchors.emplace(component.name, r);
        occupied.push_back(r);
    }
    const Vector2 hud_origin = state.hud->get_global_position();
    const auto occupy_screen_rect = [&](const Rect2& r, const bool input_wrapper = false) {
        model::record_project_hud_occupancy(occupied,
            {r.position.x - hud_origin.x, r.position.y - hud_origin.y, r.size.x, r.size.y}, input_wrapper);
    };
    // Hero and production rects are already hud-local (shell_to_screen), unlike get_global_rect().
    const auto occupy_hud_rect = [&](const Rect2& r) {
        occupied.push_back({r.position.x, r.position.y, r.size.x, r.size.y});
    };
    if (state.heroes && state.heroes->is_visible_in_tree()) {
        for (const auto tray : {EawrHeroTray::Tray::friendly, EawrHeroTray::Tray::hostile}) {
            for (std::size_t slot = 0; slot < state.heroes->icons(tray).size(); ++slot) {
                if (const auto rect = state.heroes->slot_rect(tray, slot)) occupy_hud_rect(*rect);
            }
        }
    }
    if (state.production && state.production->is_visible_in_tree() && state.production->pane_open()) {
        if (const auto rect = state.production->control_rect("r_close")) occupy_hud_rect(*rect);
        for (std::size_t row = 0; row < model::pool_row_limit; ++row) {
            for (std::size_t column = 0; column < model::pool_columns; ++column) {
                const auto name = "r_0" + std::to_string(row) + "0" + std::to_string(column);
                if (const auto rect = state.production->control_rect(name)) occupy_hud_rect(*rect);
            }
        }
    }
    for (const auto& [name, button] : state.order_buttons) {
        if (!button->is_visible_in_tree()) continue;
        const auto r = button->get_global_rect();
        anchors[name] = {r.position.x - hud_origin.x, r.position.y - hud_origin.y, r.size.x, r.size.y};
    }
    const auto collect = [&](const auto& self, Node* node) -> void {
        if (node == state.gift_button || node == state.gift_scroll) return;
        // These custom controls span the viewport but handle input only over their drawn content.
        const bool input_wrapper = node == state.cards || node == state.heroes || node == state.abilities
            || node == state.production || node == state.minimap;
        if (auto* control = Object::cast_to<Control>(node); control && control != state.hud
            && control->is_visible_in_tree() && control->get_mouse_filter() == Control::MOUSE_FILTER_STOP) {
            occupy_screen_rect(control->get_global_rect(), input_wrapper);
        }
        for (int i = 0; i < node->get_child_count(); ++i) self(self, node->get_child(i));
    };
    collect(collect,state.hud);
    const auto placed = model::dock_project_hud(look, {0,0,state.gift_viewport.x,state.gift_viewport.y}, scale,
        anchors, occupied, state.gift_recipients.size());
    if (!placed) {
        if (!state.gift_dock_warning) state.warn("EAWR-UI-0790", "gift control has no safe docking candidate");
        state.gift_dock_warning = true;
        state.gift_open = false;
        state.gift_button->hide(); state.gift_scroll->hide();
        for (auto& cell : state.gift_cells) cell.button->hide();
        return;
    }
    state.gift_dock_warning = false;
    const Vector2 size(static_cast<float>(placed->width), static_cast<float>(placed->height));
    const Vector2 position(static_cast<float>(placed->x), static_cast<float>(placed->y));
    if (!changed && previous_viewport == state.gift_viewport && previous_enabled == state.gift_enabled
        && previous_open == state.gift_open && state.gift_button->is_visible()
        && state.gift_button->get_position() == position && state.gift_button->get_size() == size) return;
    state.gift_drawn_enabled = state.gift_enabled;
    state.gift_drawn_open = state.gift_open;
    state.gift_button->set_position(position);
    state.gift_button->set_size(size);
    state.gift_button->set_hit_rect({{}, size});
    if (look.stock_art) state.gift_button->set_special_disabled(!state.gift_enabled);
    else state.gift_button->set_disabled(!state.gift_enabled);
    state.gift_button->set_modulate(state.gift_enabled ? Color(1, 1, 1) : Color(0.5F, 0.5F, 0.5F));
    state.gift_button->show();
    const Vector2 recipient_size(static_cast<float>(look.list_width) * scale, static_cast<float>(look.row_height) * scale);
    // The dock check guarantees the whole list (rows and gap) fits above the button, so it needs no height clamp.
    const float row_gap = static_cast<float>(look.row_gap) * scale;
    const float row_pitch = recipient_size.y + row_gap;
    const float rows_height = static_cast<float>(state.gift_recipients.size()) * row_pitch;
    const auto list = model::project_hud_list_rect(look, *placed, scale, state.gift_recipients.size());
    state.gift_scroll->set_position({static_cast<float>(list.x), static_cast<float>(list.y)});
    state.gift_scroll->set_size({static_cast<float>(list.width), static_cast<float>(list.height)});
    state.gift_list->set_custom_minimum_size({recipient_size.x, rows_height});
    state.gift_scroll->set_visible(state.gift_open);
    for (std::size_t i = 0; i < state.gift_recipients.size(); ++i) {
        if (i == state.gift_cells.size()) {
            auto cell = make_button(state.gift_list);
            cell.button->set_action([this, i] {
                auto& current = *state_;
                if (!current.gift_open || !current.gift_enabled || i >= current.gift_recipients.size()) return;
                const auto seat = current.gift_cells[i].seat;
                if (current.gift_recipients[i].seat != seat) return;
                current.gift_open = false;
                ++current.gift_submissions;
                current.gift_scroll->hide();
                for (auto& row : current.gift_cells) row.button->hide();
                if (current.gift_handler) current.gift_handler(seat);
            });
            state.gift_cells.push_back(cell);
        }
        auto& cell = state.gift_cells[i];
        cell.seat = state.gift_recipients[i].seat;
        const String name = text(state.gift_recipients[i].name);
        if (cell.label->get_text() != name) cell.label->set_text(name);
        cell.button->set_tooltip_text(name);
        cell.button->set_name(String("gift_to_") + String::num_uint64(cell.seat.value));
        cell.button->set_position({0, static_cast<float>(i) * row_pitch});
        cell.button->set_size(recipient_size);
        cell.button->set_hit_rect({{}, recipient_size});
        cell.button->set_visible(state.gift_open);
    }
    for (std::size_t i = state.gift_recipients.size(); i < state.gift_cells.size(); ++i) state.gift_cells[i].button->hide();
}
void TacticalHud::refresh_gift_layout() {
    auto& state = *state_;
    if (!state.gift_policy || !state.hud) return;
    // Runs every frame: sample only the few values that move the docking result (no names, no
    // allocation once the buffers are warm) and re-dock when they change. The open production
    // pane's rectangles are fixed by the hud placement and the open flag.
    auto& watch = state.gift_layout;
    watch.begin();
    const auto size = state.hud->get_size();
    const auto placement = state.hud->placement();
    watch.add(size.x, size.y, placement.left, placement.bottom);
    watch.add(placement.scale);
    if (state.heroes && state.heroes->is_visible_in_tree()) {
        for (const auto tray : {EawrHeroTray::Tray::friendly, EawrHeroTray::Tray::hostile}) {
            const auto count = state.heroes->icons(tray).size();
            watch.add(static_cast<double>(count));
            for (std::size_t slot = 0; slot < count; ++slot) {
                if (const auto rect = state.heroes->slot_rect(tray, slot))
                    watch.add(rect->position.x, rect->position.y, rect->size.x, rect->size.y);
            }
        }
    }
    watch.add(state.production && state.production->is_visible_in_tree() && state.production->pane_open() ? 1.0 : 0.0);
    for (const auto& [name, button] : state.order_buttons) {
        if (!button->is_visible_in_tree()) continue;
        const auto r = button->get_global_rect();
        watch.add(r.position.x, r.position.y, r.size.x, r.size.y);
    }
    if (!watch.commit()) return;
    set_gift(state.gift_policy, state.gift_enabled, state.gift_recipients);
}
void TacticalHud::set_beacon(const bool enabled, const bool targeting) {
    if (auto* button = state_->beacon_button) {
        button->set_special_disabled(!enabled);
        button->set_order_selected(targeting);
    }
}
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

void TacticalHud::set_order_shortcut(const std::string_view order, const std::string_view caption) {
    const auto found = state_->order_buttons.find(order);
    if (found == state_->order_buttons.end()) return;
    const String tooltip = String::utf8(caption.data(), static_cast<int64_t>(caption.size()));
    if (found->second->get_tooltip_text() != tooltip) found->second->set_tooltip_text(tooltip);
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

void TacticalHud::set_pause_notice(const std::string& text) {
    if (state_->overlay) state_->overlay->set_pause_notice(String::utf8(text.c_str()));
}

void TacticalHud::set_time_view(const TimeView& view) {
    State& state = *state_;
    state.time_view = view;
    state.time_view_set = true;
    const String network_help = "Any active human may pause or resume; these do not count against the speed budget (1 s double-press debounce). Shared speed: "
        + String::num_int64(view.speed_step) + ". Use = / - to request a change (six per 45 seconds).";
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
        state.fast_forward_button->set_tooltip_text(view.multiplayer ? "Fast-forward is unavailable in multiplayer. " + network_help : String());
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

void TacticalHud::set_replay_writer(const bool allowed, ReplayWriter writer, ReplayWriter session_writer) {
    state_->replay_writer = std::move(writer);
    state_->session_replay_writer = std::move(session_writer);
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
    if (name == "gift") return button_centre(state.gift_button);
    for (const auto& cell : state.gift_cells)
        if (name == "gift_to_" + std::to_string(cell.seat.value)) return button_centre(cell.button);
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

EawrHeroTray* TacticalHud::hero_tray() const noexcept { return state_->heroes; }

void TacticalHud::set_hero_tray(std::vector<EawrHeroTray::Icon> friendly, std::vector<EawrHeroTray::Icon> hostile) {
    if (state_->heroes != nullptr) state_->heroes->show(std::move(friendly), std::move(hostile));
}
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
    state_->cards->show_units(layout, units);
}


} // namespace eawr::presentation::godot_backend
