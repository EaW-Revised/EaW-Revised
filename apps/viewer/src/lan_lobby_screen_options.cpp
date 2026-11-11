#include "lan_lobby_screen_internal.hpp"

#include "galactic_lobby_backend.hpp"

#include <charconv>
#include <cctype>
#include <tuple>

namespace eawr::presentation::godot_backend {
// Scripted GUI contract: edit the actual dialog controls and use the normal modal buttons.
bool EawrLanLobby::action_options(const std::string& argument,
                                 bool (*number)(const std::string&, std::uint32_t&)) {
    if (!impl_->host || state_->options_panel == nullptr) return false;
    if (argument == "open") { handle_button("IDC_SELECT_OPTIONS"); return state_->editing.has_value() || state_->galactic_editing.has_value(); }
    if (state_->galactic_editing) {
        const auto equal = argument.find('=');
        if (equal != std::string::npos) {
            const auto key = argument.substr(0, equal), value = argument.substr(equal + 1);
            if (key == "custom") state_->gc_custom->set_pressed_no_signal(value == "on");
            else if (key == "heroes") state_->gc_heroes->set_pressed_no_signal(value == "on");
            else if (key == "credits") state_->gc_credits->set_text(text(value));
            else if (key == "start_tech") state_->gc_start_tech->set_text(text(value));
            else if (key == "max_tech") state_->gc_max_tech->set_text(text(value));
            else if (key == "vote_timeout") state_->gc_vote_timeout->set_text(text(value));
            else if (key == "difficulty") {
                int picked = -1;
                for (int item = 0; item < state_->gc_difficulty->get_item_count(); ++item)
                    if (utf8(state_->gc_difficulty->get_item_text(item)) == value || utf8(String(state_->gc_difficulty->get_item_metadata(item))) == value) picked = item;
                if (picked < 0) return false;
                state_->gc_difficulty->select(picked);
            }
            else if (key == "auto_resolve") {
                std::uint32_t picked{};
                if (!number(value, picked) || picked > net::galactic_lobby_max_auto_resolve) return false;
                state_->gc_auto_resolve->select(static_cast<int>(picked));
            } else return false;
            process_options();
            return true;
        }
        if (argument == "accept") handle_button("EAWR_OPT_ACCEPT");
        else if (argument == "cancel") handle_button("EAWR_OPT_CANCEL");
        else if (argument == "defaults") handle_button("EAWR_OPT_DEFAULTS");
        else return false;
        return true;
    }
    if (!state_->editing) return false;
    if (argument.starts_with("credits=")) {
        std::uint32_t value{};
        const auto* gadget = find_gadget(*state_->options_dialog, "IDC_SKIRMISH_CREDITS_SLIDER");
        auto* slider = gadget != nullptr ? cast<EawrUiSlider>(gadget->control) : nullptr;
        if (slider == nullptr || !number(argument.substr(8), value)) return false;
        slider->set_value(static_cast<double>(value));
        process_options();
        return true;
    }
    process_options();
    if (argument == "accept") handle_button("EAWR_OPT_ACCEPT");
    else if (argument == "cancel") handle_button("EAWR_OPT_CANCEL");
    else if (argument == "defaults") handle_button("EAWR_OPT_DEFAULTS");
    else return false;
    return true;
}

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
    if (match_->galactic_room()) {
        build_galactic_options_overlay();
        return;
    }
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
        slider->set_min(static_cast<double>(resources.credits_bounds->minimum));
        slider->set_max(static_cast<double>(resources.credits_bounds->maximum));
        slider->set_step(0.0);
        slider->set_button_step(model::setup_credits_button_step({resources.credits_bounds->minimum, resources.credits_bounds->maximum}));
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

// The skirmish dialog has no GC tech, difficulty or auto-resolve controls. Keep the GC
// editor separate, using the same themed widgets and modal Accept/Cancel contract.
void EawrLanLobby::build_galactic_options_overlay() {
    auto& state = *state_;
    auto* panel = memnew(ColorRect);
    panel->set_name("EAWR_GALACTIC_OPTIONS");
    panel->set_color(Color(0.0F, 0.0F, 0.0F, 0.65F));
    panel->set_size(state.canvas->get_size());
    panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    state.canvas->add_child(panel);
    auto* frame = memnew(ColorRect);
    frame->set_color(Color(0.04F, 0.07F, 0.11F, 1.0F));
    frame->set_size(Vector2(920, 650));
    frame->set_position((panel->get_size() - frame->get_size()) * 0.5F);
    panel->add_child(frame);
    const auto place = [frame](Control* control, const char* name, float x, float y, float width, float height) {
        control->set_name(name);
        frame->add_child(control);
        control->set_position(Vector2(x, y));
        control->set_size(Vector2(width, height));
    };
    const auto label = [&](const char* name, const char* caption, float y) {
        auto* control = memnew(EawrUiLabel);
        control->set_text(caption);
        place(control, name, 20, y, 420, 38);
        return control;
    };
    label("EAWR_GC_TITLE", "Galactic Conquest options", 16);
    state.gc_custom = memnew(EawrUiCheck);
    state.gc_custom->set_text("Custom campaign settings");
    place(state.gc_custom, "EAWR_GC_CUSTOM", 20, 70, 880, 38);
    const auto edit = [&](const char* name, const char* caption, float y) {
        label((std::string(name) + "_LABEL").c_str(), caption, y);
        auto* control = memnew(EawrUiEdit);
        place(control, name, 460, y, 440, 38);
        return control;
    };
    state.gc_credits = edit("EAWR_GC_CREDITS", "Starting credits", 125);
    state.gc_start_tech = edit("EAWR_GC_START_TECH", "Starting tech level", 180);
    state.gc_max_tech = edit("EAWR_GC_MAX_TECH", "Maximum tech level", 235);
    state.gc_heroes = memnew(EawrUiCheck);
    state.gc_heroes->set_text("Allow heroes");
    place(state.gc_heroes, "EAWR_GC_HEROES", 20, 290, 880, 38);
    label("EAWR_GC_DIFFICULTY_LABEL", "Difficulty", 345);
    state.gc_difficulty = memnew(EawrUiCombo);
    place(state.gc_difficulty, "EAWR_GC_DIFFICULTY", 460, 345, 440, 38);
    // The choices are the selected campaign's own (show_galactic_options_values), not every entry of the difficulty document.
    label("EAWR_GC_AUTO_LABEL", "Auto-resolve", 400);
    state.gc_auto_resolve = memnew(EawrUiCombo);
    place(state.gc_auto_resolve, "EAWR_GC_AUTO_RESOLVE", 460, 400, 440, 38);
    // WAR-01: the four protocol values, shared with GalacticLobbyOptions.
    for (const char* caption : {"Always", "Never", "Ask", "Overwhelming odds"}) state.gc_auto_resolve->add_item(caption);
    // #6156: the battle-vote timeout between different human sides; a whole number of seconds the host edits, clamped on Accept.
    state.gc_vote_timeout = edit("EAWR_GC_VOTE_TIMEOUT", "Battle vote timeout (seconds)", 455);
    state.gc_error = label("EAWR_GC_ERROR", "", 510);
    state.gc_error->set_size(Vector2(880, 70));
    for (const auto& [caption, id, x] : {
            std::tuple{"Accept", "EAWR_OPT_ACCEPT", 20.0F},
            std::tuple{"Cancel", "EAWR_OPT_CANCEL", 320.0F},
            std::tuple{"Defaults", "EAWR_OPT_DEFAULTS", 620.0F}}) {
        auto* button = memnew(EawrUiButton);
        button->set_text(caption);
        button->set_fit_to_rect(true);
        place(button, id, x, 590, 280, 40);
        button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
    }
    panel->hide();
    state.options_panel = panel;
}

void EawrLanLobby::show_galactic_options_values() {
    auto& state = *state_;
    const auto& edit = *state.galactic_editing;
    state.gc_custom->set_pressed_no_signal(edit.custom);
    state.gc_credits->set_text(text(std::to_string(edit.credits)));
    state.gc_start_tech->set_text(text(std::to_string(edit.starting_tech)));
    state.gc_max_tech->set_text(text(std::to_string(edit.maximum_tech)));
    state.gc_heroes->set_pressed_no_signal(edit.allow_heroes);
    const auto& campaigns = impl_->galactic_backend->campaigns();
    const auto campaign = std::find_if(campaigns.begin(), campaigns.end(), [&](const auto& value) { return value.name == edit.campaign; });
    // Only the difficulties the campaign's AI types author, under the stock AI captions like single player's levels; a
    // campaign that states none shows the room's current one.
    state.gc_difficulty->clear();
    const auto add_choice = [&](const std::string& entry) {
        state.gc_difficulty->add_item(resolved(resources_->text, ai_seat_caption(entry)));
        state.gc_difficulty->set_item_metadata(state.gc_difficulty->get_item_count() - 1, text(entry));
        return state.gc_difficulty->get_item_count() - 1;
    };
    int selected = -1, fallback = -1;
    if (campaign != campaigns.end()) {
        const std::string normal = sim::galactic::lobby_default_difficulty(*campaign, edit.difficulty);
        for (const auto& entry : campaign->difficulties) {
            if (entry.empty()) continue;
            const int item = add_choice(entry);
            if (entry == edit.difficulty) selected = item;
            if (entry == normal) fallback = item;
        }
    }
    if (state.gc_difficulty->get_item_count() == 0) selected = add_choice(edit.difficulty);
    state.gc_difficulty->select(selected >= 0 ? selected : fallback >= 0 ? fallback : 0);
    state.gc_auto_resolve->select(static_cast<int>(edit.auto_resolve));
    state.gc_vote_timeout->set_text(text(std::to_string(edit.vote_timeout)));
    state.gc_vote_timeout->set_tooltip_text(text("Seconds a battle vote waits between different human sides, "
        + std::to_string(net::galactic_lobby_min_vote_timeout) + " to " + std::to_string(net::galactic_lobby_max_vote_timeout)
        + ". Players on one side never vote against a clock."));
    state.gc_error->set_text("");
    const bool supported = campaign != campaigns.end() && campaign->supports_custom_settings;
    if (supported && !edit.custom) {
        const auto minimum = campaign->minimum_custom_tech.value_or(0);
        state.gc_start_tech->set_text(text(std::to_string(minimum)));
        state.gc_max_tech->set_text(text(std::to_string(campaign->maximum_custom_tech.value_or(minimum))));
    }
    state.gc_custom->set_disabled(!supported);
    std::string hint = supported ? "Custom credits and tech must fit the selected campaign." : "This campaign uses its authored settings.";
    if (supported && campaign->minimum_custom_tech) hint += " Minimum tech: " + std::to_string(*campaign->minimum_custom_tech) + ".";
    if (supported && campaign->maximum_custom_tech) hint += " Maximum tech: " + std::to_string(*campaign->maximum_custom_tech) + ".";
    state.gc_custom->set_tooltip_text(text(hint));
    state.gc_start_tech->set_tooltip_text(text(hint));
    state.gc_max_tech->set_tooltip_text(text(hint));
    process_options();
}

void EawrLanLobby::reset_galactic_options() {
    auto& edit = *state_->galactic_editing;
    // Reset settings only: faction/seat plans, the seed and network delay remain the room's choices.
    edit.custom = false;
    edit.credits = edit.starting_tech = edit.maximum_tech = 0;
    edit.allow_heroes = true;
    // SK-42/WGEP-16, as when opening the room: the campaign's Normal entry.
    const auto& campaigns = impl_->galactic_backend->campaigns();
    const auto campaign = std::find_if(campaigns.begin(), campaigns.end(), [&](const auto& value) { return value.name == edit.campaign; });
    edit.difficulty = campaign != campaigns.end() ? sim::galactic::lobby_default_difficulty(*campaign, "Normal_Default") : "Normal_Default";
    edit.auto_resolve = net::galactic_lobby_default_auto_resolve;
    // As when the room opened: the campaign's own constant clamped to the host range, else the retail 15.
    edit.vote_timeout = campaign != campaigns.end() && campaign->vote_timeout_seconds
        ? net::galactic_lobby_clamp_vote_timeout(*campaign->vote_timeout_seconds) : net::galactic_lobby_default_vote_timeout;
    show_galactic_options_values();
}

void EawrLanLobby::close_galactic_options(const bool accept) {
    auto& state = *state_;
    if (!state.galactic_editing) return;
    if (accept) {
        auto edited = *state.galactic_editing;
        edited.custom = state.gc_custom->is_pressed();
        edited.allow_heroes = !edited.custom || state.gc_heroes->is_pressed();
        edited.difficulty = utf8(String(state.gc_difficulty->get_item_metadata(state.gc_difficulty->get_selected())));
        edited.auto_resolve = static_cast<std::uint32_t>(state.gc_auto_resolve->get_selected());
        const auto integer = [](EawrUiEdit* control, std::int64_t& value) {
            const auto typed = utf8(control->get_text());
            const auto parsed = std::from_chars(typed.data(), typed.data() + typed.size(), value);
            return !typed.empty() && parsed.ec == std::errc{} && parsed.ptr == typed.data() + typed.size();
        };
        if (edited.custom && (!integer(state.gc_credits, edited.credits)
            || !integer(state.gc_start_tech, edited.starting_tech) || !integer(state.gc_max_tech, edited.maximum_tech))) {
            state.gc_error->set_text("Enter whole numbers for credits and tech levels.");
            return;
        }
        std::int64_t timeout{};
        if (!integer(state.gc_vote_timeout, timeout)) {
            state.gc_error->set_text("Enter a whole number of seconds for the battle vote timeout.");
            return;
        }
        edited.vote_timeout = net::galactic_lobby_clamp_vote_timeout(timeout);
        if (!edited.custom) edited.credits = edited.starting_tech = edited.maximum_tech = 0;
        // The modal edits settings, not rows. A guest can move to a free faction while it is open (#6134), which reshapes the planned
        // rows, so the rows are taken from the room as it is now; the stale copy would plan the guest's new row as AI.
        if (const auto room = match_->lobby_state())
            if (auto current = net::decode_galactic_lobby_game(room->snapshot.game); current && current.value().campaign == edited.campaign)
                edited.rows = std::move(current).value().rows;
        // Publish through the normal validator; a refused edit leaves the room and modal copy intact.
        if (!match_->lobby_galactic_game(edited)) {
            const auto room = match_->lobby_state();
            state.gc_error->set_text(text(room && room->notice ? room->notice->message : "The room is no longer editable."));
            return;
        }
        impl_->rows_signature.clear();
    }
    state.galactic_editing.reset();
    state.options_panel->hide();
}

void EawrLanLobby::open_options() {
    if (match_->galactic_room() && (impl_->checkpoint_candidate || impl_->checkpoint_read.valid()
        || (impl_->galactic_backend && impl_->galactic_backend->checkpoint_view()))) return;
    PageState& state = *state_;
    Impl& impl = *impl_;
    if (!impl.host || state.options_panel == nullptr || state.editing || state.galactic_editing) return;
    if (match_->galactic_room()) {
        const auto room = match_->lobby_state();
        if (!room || room->snapshot.phase != net::LobbyPhase::staging) return;
        auto decoded = net::decode_galactic_lobby_game(room->snapshot.game);
        if (!decoded) return;
        state.galactic_editing = std::move(decoded).value();
        show_galactic_options_values();
        state.options_panel->move_to_front();
        state.options_panel->show();
        state.gc_custom->grab_focus();
        return;
    }
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
        const auto index = model::setup_win_condition_index(value.tactical_win_condition);
        state.opt_win->select(index ? static_cast<int>(*index) : -1);
    }
    if (const auto* gadget = find_gadget(*state.options_dialog, "IDC_SKIRMISH_CREDITS_SLIDER");
        gadget != nullptr && resources_->credits_bounds) {
        const auto shown = model::setup_credits_clamp({resources_->credits_bounds->minimum, resources_->credits_bounds->maximum},
                                                      value.credits.trunc_to_integer());
        if (auto* slider = cast<EawrUiSlider>(gadget->control)) {
            slider->set_value_no_signal(static_cast<double>(shown));
            state.shown_credits = slider->get_value();
            state.credits_revision = slider->edit_revision();
            state.editing->value.credits = sim::math::Fixed::from_integer(shown).value();
        }
        if (const auto* label = find_gadget(*state.options_dialog, "IDC_SKIRMISH_CREDITS_VALUE"))
            if (auto* caption = cast<EawrUiLabel>(label->control)) caption->set_text(text(std::to_string(shown)));
    }
}

void EawrLanLobby::close_options(const bool accept) {
    if (state_->galactic_editing) { close_galactic_options(accept); return; }
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
    if (state_->galactic_editing) {
        const bool custom = state_->gc_custom->is_pressed();
        state_->gc_credits->set_editable(custom);
        state_->gc_start_tech->set_editable(custom);
        state_->gc_max_tech->set_editable(custom);
        state_->gc_heroes->set_disabled(!custom);
        return;
    }
    PageState& state = *state_;
    if (!state.editing) return;
    auto& edit = *state.editing;
    if (state.opt_heroes->is_pressed() != edit.value.allow_heroes || state.opt_units->is_pressed() != edit.value.free_starting_units
        || state.opt_super->is_pressed() != edit.value.allow_superweapons) edit.custom = true;
    edit.value.allow_heroes = state.opt_heroes->is_pressed();
    edit.value.free_starting_units = state.opt_units->is_pressed();
    edit.value.allow_superweapons = state.opt_super->is_pressed();
    if (const auto* gadget = find_gadget(*state.options_dialog, "IDC_SKIRMISH_CREDITS_SLIDER");
        gadget != nullptr && resources_->credits_bounds) {
        if (auto* slider = cast<EawrUiSlider>(gadget->control); slider != nullptr
            && (slider->get_value() != state.shown_credits || slider->edit_revision() != state.credits_revision)) {
            state.shown_credits = slider->get_value();
            state.credits_revision = slider->edit_revision();
            const auto& bounds = *resources_->credits_bounds;
            const auto chosen = model::setup_credits_edit({bounds.minimum, bounds.maximum}, state.shown_credits);
            edit.value.credits = sim::math::Fixed::from_integer(chosen).value();
            edit.custom = true;
            if (const auto* label = find_gadget(*state.options_dialog, "IDC_SKIRMISH_CREDITS_VALUE"))
                if (auto* caption = cast<EawrUiLabel>(label->control)) caption->set_text(text(std::to_string(chosen)));
        }
    }
    if (state.opt_win != nullptr && state.opt_win->is_visible_in_tree()) {
        const int picked = state.opt_win->get_selected();
        const auto choices = model::setup_win_conditions();
        const auto current = model::setup_win_condition_index(edit.value.tactical_win_condition);
        if (picked >= 0 && static_cast<std::size_t>(picked) < choices.size() && (!current || static_cast<int>(*current) != picked)) {
            edit.value.tactical_win_condition = std::string(choices[static_cast<std::size_t>(picked)].name);
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
