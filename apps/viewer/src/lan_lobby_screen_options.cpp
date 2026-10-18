#include "lan_lobby_screen_internal.hpp"

namespace eawr::presentation::godot_backend {
void EawrLanLobby::open_host_options() {
    if (resources_->dialogs->script.find(host_dialog) == nullptr) return;
    page_ = Page::host_options;
    build_page(host_dialog);
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.loading = true;
    if (auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT")) {
        name->set_text(text(impl_->host_options.game_name));
        name->set_max_length(static_cast<int>(model::lan_game_name_max));
        name->connect("text_submitted",
                      callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_INTERNET_GAME_NAME_EDIT")));
        name->grab_focus();
    }
    if (auto* password = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_PASSWORD_EDIT")) {
        // Decision D-b: password games are internet-lobby features, disabled with a reason.
        password->set_secret(true);
        password->set_max_length(static_cast<int>(model::lan_password_max));
        // Passwords belong to online rooms: empty makes a public room, a password a private one.
        password->set_editable(impl_->join_selection.source() == model::LobbySourceKind::online && impl_->online_rooms != nullptr);
        password->set_tooltip_text(password->is_editable()
            ? "Leave empty for a public room. A password makes it a private room."
            : "Passwords are not supported in this build yet.");
    }
    if (auto* seats = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO")) {
        seats->set_fit_to_longest_item(false);
        seats->set_clip_text(true);
        seats->set_retail_list(true);
    }
    fill_host_seats();
    if (auto* seats = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO"))
        seats->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_INTERNET_HOST_CLAN_COMBO")));
    if (auto* label = EAWR_CONTROL(EawrUiLabel, "IDC_TEXT_HOST_CLAN")) label->set_text("Human players");
    // Human players use the clan row; the LAN password row remains available for mapping.
    if (auto* password = EAWR_CONTROL(Control, "IDC_INTERNET_GAME_PASSWORD_EDIT");
        password != nullptr && impl_->join_selection.source() == model::LobbySourceKind::lan) {
        password->set_visible(false);
        auto position = password->get_position(); auto size = password->get_size();
        const auto right = position.x + size.x;
        for (const BuiltGadget& gadget : state.dialog->gadgets) {
            if (gadget.kind != "label" || gadget.control == nullptr) continue;
            const auto label_position = gadget.control->get_position();
            const auto label_size = gadget.control->get_size();
            if (label_position.x < position.x && label_position.y < position.y + size.y
                && label_position.y + label_size.y > position.y) {
                position.x = std::min(position.x, label_position.x);
                gadget.control->set_visible(false);
            }
        }
        size.x = right - position.x;
        state.port_mapping = memnew(EawrUiCheck);
        state.port_mapping->set_text("Map router TCP port for Internet play");
        state.port_mapping->set_tooltip_text("Ask your router for a temporary port mapping. This cannot bypass upstream NAT or CGNAT.");
        state.dialog->frame->add_child(state.port_mapping);
        if (const auto* clan = find_gadget(*state.dialog, "IDC_TEXT_HOST_CLAN")) {
            const Rect2 next_row = local_rect(*state.dialog, *clan);
            const float gap = next_row.size.y / 6.0F;
            if (auto* game_name = EAWR_CONTROL(Control, "IDC_INTERNET_GAME_NAME_EDIT"))
                position.y = std::max(position.y, game_name->get_position().y + game_name->get_size().y + gap);
            size.y = std::max(0.0F, next_row.position.y - gap - position.y);
            fit_control_caption(*state.port_mapping, "Global_Default", size);
        }
        state.port_mapping->set_position(position); state.port_mapping->set_size(size);
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_GAME_TYPE_LIST")) {
        int chosen = 0;
        for (std::size_t index = 0; index < std::size(game_types); ++index) {
            const int item = list->add_item(resolved(resources.text, game_types[index].name));
            if (!game_types[index].available) {
                list->set_item_disabled(item, true);
                list->set_item_tooltip(item, "This game type needs the land game or the campaign, which are not built yet.");
            } else if (impl_->host_options.game_type.empty() || impl_->host_options.game_type == game_types[index].name) {
                chosen = item;
            }
        }
        list->select(chosen);
        impl_->host_options.game_type = game_types[chosen].name;
        list->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_GAME_TYPE_LIST")));
        if (auto* description = EAWR_CONTROL(EawrUiLabel, "IDC_GAME_TYPE_DESCRIPTION_TEXT")) {
            description->set_text(resolved(resources.text, game_types[chosen].description));
        }
    }
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    state.loading = false;
}

void EawrLanLobby::build_options_overlay() {
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.options_panel = nullptr;
    const data::Dialog* resource = resources.dialogs->script.find(std::string(model::skirmish_options_dialog));
    if (resource == nullptr) return;
    state.options_dialog = build_dialog(*resource, *resources.theme_model, resources.text ? &*resources.text : nullptr,
                                        model::Placement::centre, true);
    const BuiltDialog& built = *state.options_dialog;
    const auto control_of = [&](const char* id) -> Control* {
        const BuiltGadget* gadget = find_gadget(built, id);
        return gadget != nullptr ? gadget->control : nullptr;
    };
    state.opt_heroes = cast<EawrUiCheck>(control_of("IDC_ALLOW_HEROES"));
    state.opt_units = cast<EawrUiCheck>(control_of("IDC_ALLOW_STARTING_UNIT_PURCHASE"));
    state.opt_super = cast<EawrUiCheck>(control_of("IDC_ALLOW_SUPER_WEAPONS"));
    state.opt_accept = cast<EawrUiButton>(control_of("IDC_BUTTON_ACCEPT"));
    state.opt_cancel = cast<EawrUiButton>(control_of("IDC_CANCEL_BUTTON"));
    state.opt_defaults = cast<EawrUiButton>(control_of("IDC_DEFAULTS_BUTTON"));
    if (!state.opt_heroes || !state.opt_units || !state.opt_super || !state.opt_accept || !state.opt_cancel || !state.opt_defaults) {
        state.options_dialog.reset();
        notes_.push_back("lobby options dialog lacks a required control; the Advanced Options button stays disabled");
        return;
    }
    for (const BuiltGadget& gadget : built.gadgets) {
        // A group box is its rim alone: the dialog's own shade shows inside it, as in the retail dialog.
        if (gadget.kind == "group" && gadget.control != nullptr) {
            built.frame->move_child(gadget.control, 0);
            if (auto* rim = cast<EawrUiFrame>(gadget.control)) rim->set_rim_only(true);
        }
    }
    // WSS-27: the pre-built base is hidden in space.
    if (auto* base = control_of("IDC_PRE_BUILT_BASE")) base->set_visible(false);
    for (const auto& [button, name] : {std::pair{state.opt_accept, "EAWR_OPT_ACCEPT"}, std::pair{state.opt_cancel, "EAWR_OPT_CANCEL"},
                                       std::pair{state.opt_defaults, "EAWR_OPT_DEFAULTS"}})
        button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(name)));
    state.opt_win = cast<EawrUiCombo>(control_of("IDC_WIN_CONDITION_COMBO"));
    if (state.opt_win != nullptr) {
        state.opt_win->set_focus_mode(Control::FOCUS_ALL);
        for (const auto& choice : model::setup_win_conditions())
            state.opt_win->add_item(resolved(resources.text, std::string(choice.text_key)));
        state.opt_win->select(-1);
    }
    auto* slider = cast<EawrUiSlider>(control_of("IDC_SKIRMISH_CREDITS_SLIDER"));
    const bool credits_shown = slider != nullptr && resources.credits_bounds.has_value();
    for (const char* id : {"IDC_SKIRMISH_CREDITS_TEXT", "IDC_SKIRMISH_CREDITS_SLIDER", "IDC_SKIRMISH_CREDITS_VALUE"})
        if (auto* control = control_of(id)) control->set_visible(credits_shown);
    if (credits_shown) {
        const String reason = text(std::string(model::setup_notice(model::SetupNotice::credits_not_applied)));
        slider->set_min(static_cast<double>(resources.credits_bounds->minimum));
        slider->set_max(static_cast<double>(resources.credits_bounds->maximum));
        slider->set_step(0.0);
        slider->set_forced_states(KitState::disabled, KitState::disabled);
        slider->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        slider->set_tooltip_text(reason);
        auto* cover = memnew(Control);
        cover->set_position(slider->get_position());
        cover->set_size(slider->get_size());
        cover->set_mouse_filter(Control::MOUSE_FILTER_STOP);
        cover->set_tooltip_text(reason);
        built.frame->add_child(cover);
    }
    // The transparent panel only takes the pointer, so the staging behind stays inert while the dialog is up.
    auto* panel = memnew(ColorRect);
    panel->set_name("EAWR_LOBBY_OPTIONS");
    panel->set_color(Color(0.0F, 0.0F, 0.0F, 0.0F));
    panel->set_size(state.canvas->get_size());
    panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    panel->add_child(built.frame);
    panel->hide();
    state.canvas->add_child(panel);
    state.options_panel = panel;
}

void EawrLanLobby::open_options() {
    PageState& state = *state_;
    Impl& impl = *impl_;
    if (!impl.host || state.options_panel == nullptr || state.editing) return;
    state.editing = skirmish::begin_setup_options(impl.selection, resources_->defaults);
    show_options_values();
    state.options_panel->move_to_front();
    state.options_panel->show();
}

// Writes every dialog control from the edited copy (open and Defaults).
void EawrLanLobby::show_options_values() {
    PageState& state = *state_;
    const auto& value = state.editing->value;
    state.opt_heroes->set_pressed_no_signal(value.allow_heroes);
    state.opt_units->set_pressed_no_signal(value.free_starting_units);
    state.opt_super->set_pressed_no_signal(value.allow_superweapons);
    if (state.opt_win != nullptr) {
        const auto index = model::setup_win_condition_index(value.space_win_condition);
        state.opt_win->select(index ? static_cast<int>(*index) : -1);
    }
    if (const auto* gadget = find_gadget(*state.options_dialog, "IDC_SKIRMISH_CREDITS_SLIDER");
        gadget != nullptr && resources_->credits_bounds) {
        const auto shown = model::setup_credits_clamp({resources_->credits_bounds->minimum, resources_->credits_bounds->maximum},
                                                      value.credits.trunc_to_integer());
        if (auto* slider = cast<EawrUiSlider>(gadget->control)) slider->set_value_no_signal(static_cast<double>(shown));
        if (const auto* label = find_gadget(*state.options_dialog, "IDC_SKIRMISH_CREDITS_VALUE"))
            if (auto* caption = cast<EawrUiLabel>(label->control)) caption->set_text(text(std::to_string(shown)));
    }
}

void EawrLanLobby::close_options(const bool accept) {
    PageState& state = *state_;
    if (!state.editing) return;
    if (accept) {
        // The host never runs ahead of the room: a refused publish (room starting) restores the selection.
        const auto before = impl_->selection;
        skirmish::accept_setup_options(impl_->selection, *state.editing);
        if (!publish_staging_selection()) impl_->selection = before;
    }
    state.editing.reset();
    state.options_panel->hide();
}

// Polls the dialog's checks and combo into the edited copy; a change marks the options custom.
void EawrLanLobby::process_options() {
    PageState& state = *state_;
    if (!state.editing) return;
    auto& edit = *state.editing;
    if (state.opt_heroes->is_pressed() != edit.value.allow_heroes || state.opt_units->is_pressed() != edit.value.free_starting_units
        || state.opt_super->is_pressed() != edit.value.allow_superweapons) edit.custom = true;
    edit.value.allow_heroes = state.opt_heroes->is_pressed();
    edit.value.free_starting_units = state.opt_units->is_pressed();
    edit.value.allow_superweapons = state.opt_super->is_pressed();
    if (state.opt_win != nullptr && state.opt_win->is_visible_in_tree()) {
        const int picked = state.opt_win->get_selected();
        const auto choices = model::setup_win_conditions();
        const auto current = model::setup_win_condition_index(edit.value.space_win_condition);
        if (picked >= 0 && static_cast<std::size_t>(picked) < choices.size() && (!current || static_cast<int>(*current) != picked)) {
            edit.value.space_win_condition = std::string(choices[static_cast<std::size_t>(picked)].name);
            edit.custom = true;
        }
    }
    state.opt_win_list_was_open = state.opt_win != nullptr && state.opt_win->get_popup()->is_visible();
}

// The host's map list for the official or the custom maps; keeps the current map when it is listed.
void EawrLanLobby::open_filter() {
    if (resources_->dialogs->script.find(filter_dialog) == nullptr) return;
    page_ = Page::filter;
    build_page(filter_dialog);
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.loading = true;
    const model::LobbyFilter& filter = impl_->filter;
    const auto fill = [&](const char* gadget, const std::vector<std::pair<std::string, int>>& items, int selected_id) {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
            int selected = 0;
            for (const auto& [label, id] : items) {
                combo->add_item(resolved(resources.text, label), id);
                if (id == selected_id) selected = combo->get_item_count() - 1;
            }
            combo->select(selected);
        }
    };
    std::vector<std::pair<std::string, int>> types{{"TEXT_GAME_TYPE_FILTER_ALL", 0}};
    int type_id = 0;
    for (std::size_t index = 0; index < std::size(game_types); ++index) {
        types.emplace_back(game_types[index].name, static_cast<int>(index) + 1);
        if (filter.game_type && *filter.game_type == game_types[index].name) type_id = static_cast<int>(index) + 1;
    }
    fill("IDC_GAME_TYPE_FILTER_COMBO", types, type_id);
    // 2 to 9 players, as retail lists; the first entry shows no limit.
    std::vector<std::pair<std::string, int>> players{{"TEXT_GAME_TYPE_FILTER_ALL", 0}};
    for (int count = 2; count < 10; ++count) players.emplace_back(std::to_string(count), count);
    fill("IDC_MIN_PLAYERS_FILTER_COMBO", players, filter.min_players ? static_cast<int>(*filter.min_players) : 0);
    fill("IDC_MAX_PLAYERS_FILTER_COMBO", players, filter.max_players ? static_cast<int>(*filter.max_players) : 0);
    std::vector<std::pair<std::string, int>> pings{{"TEXT_ANY_PING_FILTER_TIME", 0}};
    for (int ping = 100; ping <= 1000; ping += 100) pings.emplace_back(std::to_string(ping), ping);
    fill("IDC_MAX_PING_FILTER_COMBO", pings, filter.max_ping_ms ? static_cast<int>(*filter.max_ping_ms) : 0);
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    state.loading = false;
}

void EawrLanLobby::open_direct_connect() {
    if (resources_->dialogs->script.find(direct_dialog) == nullptr) return;
    page_ = Page::direct_connect;
    build_page(direct_dialog);
    PageState& state = *state_;
    state.loading = true;
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) {
        edit->set_placeholder("127.0.0.1:47624");
        edit->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_EDIT_IP")));
        edit->grab_focus();
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_IP_LIST")) {
        for (const std::string& address : impl_->direct_addresses) list->add_item(text(address));
        auto* seats = memnew(EawrUiCombo);
        seats->set_name("EAWR_LAN_DIRECT_SEATS");
        seats->set_position(list->get_position()); seats->set_size(Vector2(list->get_size().x, 26));
        for (int count = 2; count <= static_cast<int>(net::max_match_seats); ++count) seats->add_item(text(std::to_string(count) + " human seats"), count);
        seats->select(static_cast<int>(impl_->direct_seats) - 2);
        seats->set_tooltip_text("Choose the same human seat count as the hosted game.");
        list->set_position(list->get_position() + Vector2(0, 31));
        list->set_size(Vector2(list->get_size().x, std::max(0.0F, list->get_size().y - 31)));
        state.dialog->frame->add_child(seats); state.controls["EAWR_LAN_DIRECT_SEATS"] = seats;
        seats->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("EAWR_LAN_DIRECT_SEATS")));
    }
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    state.loading = false;
}

// ---- The staging's frame ----------------------------------------------------------------------

} // namespace eawr::presentation::godot_backend
