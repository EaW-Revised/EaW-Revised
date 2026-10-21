#include "lan_lobby_screen_internal.hpp"

namespace eawr::presentation::godot_backend {
void EawrLanLobby::fill_host_seats(const std::uint32_t requested_count) {
    auto* seats = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO");
    if (!seats) return;
    const bool was_loading = state_->loading;
    state_->loading = true;
    seats->clear();
    const skirmish::SetupMap* map = nullptr;
    if (impl_->map_index >= 0 && static_cast<std::size_t>(impl_->map_index) < resources_->maps.size())
        map = &resources_->maps[static_cast<std::size_t>(impl_->map_index)];
    if (!map || impl_->host_options.game_type != galactic_game_type) {
        // Reserve the largest loaded transport capacity; rows choose actual players.
        for (const auto& candidate : resources_->maps) {
            if (candidate.metadata.custom != impl_->custom_maps || !candidate.unavailable.empty()) continue;
            if (!map || network_row_count(candidate) > network_row_count(*map)) map = &candidate;
        }
    }
    // Transport capacity is an upper bound. The staging rows choose actual players (LLB-18).
    // Campaign hosting retains its separate human-side configuration.
    const auto seat_limit = map ? network_row_count(*map) : 0U;
    if (map && impl_->map_index < 0 && page_ == Page::host_options)
        impl_->map_index = static_cast<int>(map - resources_->maps.data());
    if (impl_->host_options.game_type != galactic_game_type) {
        if (page_ == Page::host_options) {
            impl_->human_owners.resize(seat_limit);
            std::iota(impl_->human_owners.begin(), impl_->human_owners.end(), 1U);
        }
        seats->hide();
        if (auto* label = EAWR_CONTROL(EawrUiLabel, "IDC_TEXT_HOST_CLAN")) label->hide();
    } else {
        seats->show();
        if (auto* label = EAWR_CONTROL(EawrUiLabel, "IDC_TEXT_HOST_CLAN")) label->show();
        for (std::uint32_t humans = 2; humans <= seat_limit; ++humans)
            seats->add_item(text(std::to_string(humans)), static_cast<int>(humans));
        if (seat_limit >= 2) {
            const auto count = std::clamp(requested_count == 0 ? static_cast<std::uint32_t>(impl_->human_owners.size()) : requested_count, 2U, seat_limit);
            seats->select(static_cast<int>(count) - 2);
            impl_->human_owners.resize(count);
            std::iota(impl_->human_owners.begin(), impl_->human_owners.end(), 1U);
        }
        seats->set_disabled(seat_limit < 2);
    }
    state_->loading = was_loading;
}

void EawrLanLobby::prepare_host_rows(const skirmish::SetupMap& map) {
    std::erase_if(impl_->selection.open_slots, [&](const auto slot) { return slot > network_row_count(map); });
    if (match_->lobby_state()) return; // Explicit Closed rows survive subsequent map edits.
    for (std::uint32_t slot = 2; slot <= network_row_count(map); ++slot) {
        if (std::none_of(impl_->selection.slots.begin(), impl_->selection.slots.end(), [&](const auto& row) { return row.slot == slot; })
            && std::find(impl_->selection.open_slots.begin(), impl_->selection.open_slots.end(), slot) == impl_->selection.open_slots.end())
            impl_->selection.open_slots.push_back(slot);
    }
    std::sort(impl_->selection.open_slots.begin(), impl_->selection.open_slots.end());
}

void EawrLanLobby::begin_hosting() {
    Impl& impl = *impl_;
    const auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT");
    const std::optional<std::string> cleaned = model::clean_game_name(name != nullptr ? utf8(name->get_text()) : "");
    if (!cleaned) {
        impl.host_password.clear(); impl.host_options.password.clear();
        // Retail puts the focus back on the name edit when it is empty.
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT")) edit->grab_focus();
        return;
    }
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online && !impl.online_rooms) {
        impl.host_password.clear(); impl.host_options.password.clear();
        show_failure(std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
        return;
    }
    impl.host_options.game_name = *cleaned;
    // The room password is read here, handed to the source and kept only as long as the room is listed.
    const auto* password = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_PASSWORD_EDIT");
    impl.host_options.password = online && password != nullptr ? model::clamp_room_password(utf8(password->get_text())) : std::string{};
    impl.host_password = impl.host_options.password;
    impl.last_online_notice.clear();
    if (online && !configure_online_match()) {
        impl.host_password.clear(); impl.host_options.password.clear();
        show_failure("Set the online service in Network Options first."); return;
    }
    if (online && !impl.online_rooms->set_host_password(impl.host_password)) {
        impl.host_password.clear(); impl.host_options.password.clear();
        show_failure(std::string(model::online_failure_text(model::OnlineFailure::password_locked)));
        return;
    }
    match_->configure_seats(impl.human_owners);
    match_->configure_staging_capacity(impl.host_options.game_type != galactic_game_type);
    match_->set_port_mapping(state_->port_mapping && state_->port_mapping->is_pressed());
    configure_staging();
    if (online) match_->set_public_invitation(impl.host_password.empty());
    match_->host(options_.game_port);
    // The host owns the game mode: the Campaign game type opens a Galactic Conquest room (GCMP-07), anything else a skirmish room.
    if (impl.host_options.game_type == galactic_game_type) {
        if (const auto refusal = host_galactic_game()) {
            match_->cancel();
            show_failure(*refusal);
            return;
        }
    }
    open_staging(true, *cleaned, {});
}

// ---- Staging ----------------------------------------------------------------------------------

bool EawrLanLobby::publish_staging_selection() {
    if (!impl_->host || !match_->lobby_state()) return true;
    return match_->lobby_game(staging_options(impl_->selection));
}

void EawrLanLobby::configure_staging() {
    const auto content = resources_->content;
    configure_galactic_content();
    match_->configure_factions(scene::faction_order(*content->catalog));
    match_->configure_lobby(impl_->name, impl_->colour, resources_->maps,
        [content](const skirmish::FixtureOptions& options) -> core::Result<sim::tactical::TacticalSetup> {
            auto fixture = skirmish::fixture_from_options(options, *content->filesystem, *content->catalog);
            if (!fixture) return core::Result<sim::tactical::TacticalSetup>::failure(fixture.error());
            auto prepared = prepare_skirmish(fixture.value(), *content->filesystem, *content->catalog);
            if (!prepared) return core::Result<sim::tactical::TacticalSetup>::failure(prepared.error());
            return core::Result<sim::tactical::TacticalSetup>::success(lan_recording_setup(fixture.value(), prepared.value().start));
        });
}

void EawrLanLobby::open_staging(const bool host, const std::string& game, const std::string& address) {
    Impl& impl = *impl_;
    impl.host = host;
    impl.game = game;
    impl.address = address;
    impl.locked = false;
    impl.launch.reset();
    impl.offered.reset();
    impl.preview_path.clear();
    impl.rows_signature.clear();
    impl.last_message.clear();
    impl.staging_chat = model::ChatLog();
    impl.room_chat_sequence = impl.room_revision = 0; impl.room_notice.clear();
    impl.online_connections.clear();
    impl.refused = false;
    page_ = Page::staging;
    build_page(staging_dialog);
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.loading = true;
    const BuiltDialog& dialog = *state.dialog;

    std::vector<std::string> ids;
    for (const BuiltGadget& gadget : dialog.gadgets) ids.push_back(gadget.id);
    state.dialog_rows = model::staging_row_count(ids);

    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) {
        chat->set_max_length(static_cast<int>(model::lan_chat_max));
        chat->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_LAN_CHAT_EDIT")));
        chat->grab_focus();
    }
    // Both peers can review the selected map before accepting the setup (#3759).
    if (const BuiltGadget* image = find_gadget(dialog, "IDC_IMAGE"); image != nullptr) {
        state.preview = memnew(TextureRect);
        state.preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        state.preview->set_stretch_mode(TextureRect::STRETCH_SCALE);
        state.preview->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        const Rect2 rect = local_rect(dialog, *image);
        state.preview->set_position(rect.position);
        state.preview->set_size(rect.size);
        dialog.frame->add_child(state.preview);
    }
    // The accepted and status icons are image gadgets too: a small square per row.
    for (std::size_t row = 0; row < state.dialog_rows; ++row) {
        const std::string id = "IDC_ACCEPT_PLAYER_" + std::to_string(row);
        if (const BuiltGadget* gadget = find_gadget(dialog, id)) {
            auto* icon = memnew(ColorRect);
            const Rect2 rect = local_rect(dialog, *gadget);
            icon->set_position(rect.position + Vector2(2, 2));
            icon->set_size(rect.size - Vector2(4, 4));
            icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            icon->set_color(Color(0.1F, 0.1F, 0.1F, 0.9F));
            dialog.frame->add_child(icon);
            state.accept_icons[id] = icon;
        }
    }
    // Rows: a combo each for the name, the faction, the colour and the team.
    for (std::size_t row = 0; row < state.dialog_rows; ++row) {
        const std::string suffix = std::to_string(row);
        for (const char* field : {"IDC_PLAYER_NAME_", "IDC_PLAYER_FACTION_", "IDC_PLAYER_COLOR_", "IDC_PLAYER_TEAM_"}) {
            const std::string id = field + suffix;
            if (auto* combo = EAWR_CONTROL(EawrUiCombo, id)) {
                combo->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(text(id)));
            }
        }
    }
    for (const BuiltGadget& gadget : dialog.gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    if (auto* load = EAWR_CONTROL(EawrUiButton, "IDC_LOAD_GAME")) {
        load->set_visible(host);
        load->set_text(resolved(resources.text, "TEXT_BUTTON_ACCEPT"));
        load->set_disabled(true);
    }
    // #4041, MNU-22: the host edits the advanced options; a joiner's button is hidden below and the
    // summary list shows the host's values read-only.
    if (auto* options = EAWR_CONTROL(EawrUiButton, "IDC_SELECT_OPTIONS")) {
        if (host) build_options_overlay();
        options->set_disabled(!host || state.options_panel == nullptr);
        if (host && state.options_panel == nullptr)
            options->set_tooltip_text("The advanced options dialog is not in the loaded data.");
    }
    // The client option summary shares the host's map-source button area in the resource.
    // A host shows the map controls there, not the client's option/setting headings.
    if (host) {
        for (const char* id : {"IDC_GAME_OPTIONS_CLIENT_DISPLAY_LEFT", "IDC_GAME_OPTIONS_CLIENT_DISPLAY_RIGHT"}) {
            if (auto* control = EAWR_CONTROL(Control, id)) control->hide();
        }
    }
    if (auto* start = EAWR_CONTROL(EawrUiButton, "IDC_LAN_START_GAME_BUTTON")) {
        start->set_text(resolved(resources.text, host ? "TEXT_BUTTON_START_GAME" : "TEXT_BUTTON_ACCEPT"));
        start->set_disabled(true);
    }
    if (auto* title = EAWR_CONTROL(EawrUiLabel, "IDC_INTERNET_DIALOG_HEADER")) {
        title->set_text(resolved(resources.text, host ? "TEXT_INTERNET_HOST_GAME_TITLE" : "TEXT_INTERNET_JOIN_GAME_TITLE"));
    }
    // Staging keeps its loaded rectangles: fit the skin and caption inside each one.
    for (const auto& gadget : dialog.gadgets) {
        if (gadget.kind != "button" || !gadget.control) continue;
        auto* button = cast<EawrUiButton>(gadget.control);
        if (!button) continue;
        const Rect2 authored = local_rect(dialog, gadget);
        button->set_fit_to_rect(true);
        fit_control_caption(*button, "Push_Button", authored.size);
        button->set_size(authored.size);
    }
    const auto* cancel_gadget = find_gadget(dialog, "IDC_LAN_CANCEL_BUTTON");
    const auto* start_gadget = find_gadget(dialog, "IDC_LAN_START_GAME_BUTTON");
    if (cancel_gadget && start_gadget) {
        const Rect2 cancel_rect = local_rect(dialog, *cancel_gadget);
        const Rect2 start_rect = local_rect(dialog, *start_gadget);
        const float gap = cancel_rect.size.y * 0.5F;
        const float row_left = cancel_rect.get_end().x + gap;
        const float row_width = start_rect.position.x - gap - row_left;
        const Vector2 connection_size(row_width * 0.48F, cancel_rect.size.y);
        state.connections_size = connection_size;
        if (host && impl.join_selection.source() == model::LobbySourceKind::online && impl.host_password.empty()) {
            auto* invitation = memnew(EawrUiLabel); invitation->set_name("EAWR_ONLINE_PUBLIC_INVITATION");
            invitation->set_position(Vector2(row_left + row_width * 0.52F, cancel_rect.position.y));
            invitation->set_size(connection_size);
            invitation->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); dialog.frame->add_child(invitation);
            state.controls["EAWR_ONLINE_PUBLIC_INVITATION"] = invitation;
        }
        if (impl.join_selection.source() == model::LobbySourceKind::online) {
            auto* connections = memnew(EawrUiButton);
            connections->set_name("EAWR_ONLINE_CONNECTIONS");
            connections->set_fit_to_rect(true);
            if (auto* cancel = EAWR_CONTROL(Control, "IDC_LAN_CANCEL_BUTTON"))
                connections->set_theme_type_variation(cancel->get_theme_type_variation());
            connections->set_text("My connections");
            connections->set_position(Vector2(row_left, cancel_rect.position.y));
            connections->set_size(connection_size);
            connections->set_tooltip_text("View each human seat and your connection to it. Start waits for every player's connections.");
            dialog.frame->add_child(connections); state.controls["EAWR_ONLINE_CONNECTIONS"] = connections;
            fit_control_caption(*connections, "Push_Button", state.connections_size);
            connections->set_size(state.connections_size);
            if (auto* invitation = EAWR_CONTROL(Control, "EAWR_ONLINE_PUBLIC_INVITATION"))
                invitation->set_size(state.connections_size);
            connections->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String("EAWR_ONLINE_CONNECTIONS")));
            auto* list = memnew(EawrUiList);
            list->set_name("EAWR_ONLINE_CONNECTIONS_LIST");
            list->set_position(Vector2(dialog.frame->get_size().x * 0.08F, dialog.frame->get_size().y * 0.18F));
            list->set_size(Vector2(dialog.frame->get_size().x * 0.84F, dialog.frame->get_size().y * 0.62F));
            list->set_item_selectable(list->add_item("Waiting for the room roster"), false);
            list->set_z_index(20); list->hide();
            dialog.frame->add_child(list); state.controls["EAWR_ONLINE_CONNECTIONS_LIST"] = list;
        }
    } else if (impl.join_selection.source() == model::LobbySourceKind::online) {
        notes_.push_back("online staging: connections controls require the loaded Cancel and Start row");
    }
    // The joiner never edits the host's map; the host's list is filled below.
    if (!host) {
        for (const char* hidden : {"IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_CUSTOM_MAPS", "IDC_SELECT_OPTIONS",
                                   "IDC_GAME_OPTIONS_CLIENT_DISPLAY_RIGHT"}) {
            if (auto* control = EAWR_CONTROL(Control, hidden)) control->set_visible(false);
        }
        // Reuse the host map list as a read-only settings summary. No map-selection
        // signal is connected for a joiner, and every summary item is disabled.
        if (auto* heading = EAWR_CONTROL(EawrUiLabel, "IDC_GAME_OPTIONS_CLIENT_DISPLAY_LEFT")) {
            if (const auto* area = find_gadget(dialog, "IDC_SELECT_OFFICIAL_MAPS")) {
                const Rect2 rect = local_rect(dialog, *area);
                heading->set_position(rect.position); heading->set_size(rect.size);
            }
            heading->set_alignment(HORIZONTAL_ALIGNMENT_LEFT);
            heading->set_text("Game settings");
        }
        if (const auto* area = find_gadget(dialog, "IDC_SELECT_OPTIONS")) {
            auto* name = memnew(EawrUiLabel); name->set_theme(resources.theme);
            const Rect2 rect = local_rect(dialog, *area);
            name->set_position(rect.position); name->set_size(rect.size);
            name->set_text("Waiting for host setup");
            name->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            dialog.frame->add_child(name); state.controls["EAWR_JOINER_MAP_NAME"] = name;
        }
    } else {
        impl.selection = skirmish::SetupSelection{};
        impl.selection.slots.resize(1); // The host starts alone; other rows are Open.
        impl.selection.match = resources.defaults;
        // Host options bounded the human count against this map; keep it for the new room.
        impl.remote_colour_applied = false;
        // The host's row takes the player's profile colour.
        if (!impl.selection.slots.empty() && impl.colour < resources.palette.size()) {
            static_cast<void>(skirmish::select_setup_colour(impl.selection, 1, impl.colour, resources.palette.size()));
        }
    }
    state.loading = false;
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST"); list != nullptr && host) {
        list->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_STAGING_MAP_LIST")));
    }
    if (host && !match_->galactic_room()) fill_map_list();
    announce();
    refresh_staging();
}

// MNU-22 (#4041): the catalogue's advanced options dialog, built hidden over the host's staging. Heroes,
// starting units, super weapons and the win condition reach the match through the lobby game blob;
// the credits slider only displays the data's value, as the skirmish setup does (WSS-25).
void EawrLanLobby::fill_map_list() {
    Impl& impl = *impl_;
    Resources& resources = *resources_;
    auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST");
    if (list == nullptr) return;
    const std::string previous = impl.map_index >= 0 ? resources.maps[static_cast<std::size_t>(impl.map_index)].path
                                                     : skirmish::m2_fixture().map;
    state_->loading = true;
    list->clear();
    impl.listed.clear();
    int initial = 0;
    for (std::size_t index = 0; index < resources.maps.size(); ++index) {
        const skirmish::SetupMap& map = resources.maps[index];
        if (map.metadata.custom != impl.custom_maps) continue;
        impl.listed.push_back(index);
        list->add_item(text(map.name + (map.unavailable.empty() ? "" : " (unavailable)")));
        list->set_item_tooltip(list->get_item_count() - 1, text(map.unavailable.empty() ? map.path : map.unavailable));
        list->set_item_disabled(list->get_item_count() - 1, !lan_map_supports_rows(map, impl.selection.slots));
        list->set_item_selectable(list->get_item_count() - 1, lan_map_supports_rows(map, impl.selection.slots));
        if (map.path == previous) initial = list->get_item_count() - 1;
    }
    impl.map_index = -1;
    if (!impl.listed.empty()) {
        if (!lan_map_supports_rows(resources.maps[impl.listed[static_cast<std::size_t>(initial)]], impl.selection.slots)) {
            const auto fitting = std::find_if(impl.listed.begin(), impl.listed.end(), [&](const auto index) {
                return resources.maps[index].unavailable.empty() && lan_map_supports_rows(resources.maps[index], impl.selection.slots);
            });
            initial = fitting == impl.listed.end() ? -1 : static_cast<int>(fitting - impl.listed.begin());
        }
        if (initial >= 0) {
            list->select(initial);
            impl.map_index = static_cast<int>(impl.listed[static_cast<std::size_t>(initial)]);
            skirmish::repair_setup_selection(impl.selection, resources.maps[static_cast<std::size_t>(impl.map_index)], false);
            prepare_host_rows(resources.maps[static_cast<std::size_t>(impl.map_index)]);
        }
    }
    fill_host_seats();
    state_->loading = false;
    impl.rows_signature.clear();
}

void EawrLanLobby::leave_staging() {
    cancel_join();
    impl_->host_password.clear();
    if (match_) { match_->lobby_leave(); match_->cancel(); }
    impl_->launch.reset();
    impl_->offered.reset();
    impl_->locked = false;
    open_lobby();
}

// ---- Frame ------------------------------------------------------------------------------------

} // namespace eawr::presentation::godot_backend
