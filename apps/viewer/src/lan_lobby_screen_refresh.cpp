#include "lan_lobby_screen_internal.hpp"

namespace eawr::presentation::godot_backend {
void EawrLanLobby::refresh_staging() {
    if (page_ != Page::staging || !state_) return;
    // A Galactic Conquest room (the host's choice, read from the room's options) has its own roster and campaign list.
    if (match_->galactic_room()) { refresh_galactic_staging(); return; }
    Impl& impl = *impl_;
    PageState& state = *state_;
    Resources& resources = *resources_;
    const std::size_t dialog_rows = state.dialog_rows;

    // LobbySession owns editable staging; SetupSession owns the frozen final barrier.
    std::optional<skirmish::FixtureOptions> built;
    if (impl.host && impl.map_index >= 0) built = staging_options(impl.selection);
    match_->pump_setup(built ? *built : skirmish::FixtureOptions{});
    const LanView view = match_->view();
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    refresh_staging_connections(impl, view, online);
    const auto room = match_->lobby_state();
    refresh_staging_status(impl, state, resources, view, online, room);

    std::vector<model::StagingRow> rows(dialog_rows);
    const skirmish::SetupMap* map = nullptr;
    std::size_t visible = 0;
    const bool connected = view.connected && view.phase != LanPhase::ended;
    build_staging_rows(impl, resources, view, room, dialog_rows, rows, map, visible, connected);
    refresh_staging_options(impl, resources, map);
    refresh_staging_roster(impl, state, resources, room, rows, map, visible, connected, dialog_rows);
    refresh_staging_start(impl, resources, view, room, rows, online);
    finish_staging_refresh(impl, view, room);
}

void EawrLanLobby::refresh_staging_connections(Impl& impl, const LanView& view, bool online) {
    if (auto* connections = EAWR_CONTROL(EawrUiButton, "EAWR_ONLINE_CONNECTIONS")) {
        const String caption = text(view.online_capacity == 0 ? std::string("My connections")
            : "My connections (" + std::to_string(view.online_connected > 0 ? view.online_connected - 1 : 0) + "/"
                + std::to_string(view.online_capacity - 1) + ")");
        if (connections->get_text() != caption) {
            connections->set_text(caption);
            fit_control_caption(*connections, "Push_Button", state_->connections_size);
            connections->set_size(state_->connections_size);
        }
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "EAWR_ONLINE_CONNECTIONS_LIST"); list && view.online_connections != impl.online_connections) {
        impl.online_connections = view.online_connections;
        list->clear();
        for (const auto& line : impl.online_connections) {
            const int item = list->add_item(text(line));
            list->set_item_selectable(item, false); list->set_item_tooltip(item, text(line));
        }
    }
    if (online && impl.host && impl.online_rooms) {
        if (auto* invitation = EAWR_CONTROL(EawrUiLabel, "EAWR_ONLINE_PUBLIC_INVITATION")) {
            // Display public invites in their own control, never in chat/status reports.
            invitation->set_text(text(impl.host_password.empty() && view.invitation_code.size() == 4 ? "Room code: " + view.invitation_code : std::string{}));
        }
    }
    if (!impl.host) {
        if (auto offered = match_->offered_setup()) {
            if (!impl.offered || offered->map != impl.offered->map || !same_slots(offered->slots, impl.offered->slots)) {
                impl.offered = offered;
                impl.rows_signature.clear();
            }
        }
    }
}

void EawrLanLobby::refresh_staging_status(Impl& impl, PageState& state, Resources& resources,
                                          const LanView& view, bool online,
                                          const std::optional<LanLobbyState>& room) {
    // The status line is one notice per change, like a chat line.
    if (auto* options = EAWR_CONTROL(EawrUiButton, "IDC_SELECT_OPTIONS"))
        options->set_disabled(!impl.host || state.options_panel == nullptr
                              || (room && room->snapshot.phase != net::LobbyPhase::staging));
    std::string status;
    if (view.phase != LanPhase::idle && view.phase != LanPhase::running && view.phase != LanPhase::ended)
        status = online ? view.message : lan_message(view);
    if (view.phase == LanPhase::setup && view.read_only && !view.accept_enabled) status = utf8(resolved(resources.text, "TEXT_WAITING"));
    if (lan_reconnect_disabled_notice(view)) {
        if (!status.empty()) status += "\n";
        status += "Reconnect: disabled. " + view.reconnect_unavailable;
    }
    if (room && (room->preparing || room->snapshot.phase == net::LobbyPhase::starting)) status = utf8(resolved(resources.text, "TEXT_WAITING"));
    if (online && impl.host && room && room->connections_pending && room->ready_reason.empty() && view.phase == LanPhase::setup)
        status = room->start_reason;
    if (!status.empty() && status != impl.status) {
        impl.status = status;
        note_chat(model::ChatKind::notice, status);
    }
}

void EawrLanLobby::build_staging_rows(Impl& impl, Resources& resources, const LanView& view,
                                      const std::optional<LanLobbyState>& room, std::size_t dialog_rows,
                                      std::vector<model::StagingRow>& rows, const skirmish::SetupMap*& map,
                                      std::size_t& visible, bool connected) {
    if (room) {
        impl.linked = room->snapshot.seats.size() > 1;
        auto authoritative = decode_lan_lobby_game(room->snapshot);
        if (authoritative) {
            impl.offered = authoritative.value();
            if (impl.host) { impl.selection.map = *authoritative.value().map; impl.selection.slots = *authoritative.value().slots; impl.selection.match = authoritative.value().match; }
            for (std::size_t i = 0; i < resources.maps.size(); ++i) if (resources.maps[i].path == room->snapshot.game.map) {
                map = &resources.maps[i]; impl.map_index = static_cast<int>(i);
            }
            if (map) visible = model::visible_staging_rows(network_row_count(*map), dialog_rows);
            auto planned_snapshot = room->snapshot; planned_snapshot.seats.clear();
            const auto planned = decode_lan_lobby_game(planned_snapshot);
            for (const auto& slot : *authoritative.value().slots) {
                if (slot.slot == 0 || slot.slot > dialog_rows) continue;
                auto& row = rows[slot.slot - 1]; row.kind = model::RowKind::ai;
                if (planned && room->snapshot.seats.size() < room->snapshot.capacity && std::any_of(planned.value().slots->begin(), planned.value().slots->end(), [&](const auto& desired) { return desired.slot == slot.slot && desired.human; })) row.kind = model::RowKind::open;
                row.faction = slot.faction; row.team = slot.team; row.colour = slot.colour_index.value_or(slot.slot - 1); row.accepted = row.kind == model::RowKind::ai;
                bool first = true;
                for (const auto& seat : room->snapshot.seats) if (seat.selection.slot == slot.slot) {
                    row.kind = model::RowKind::human; row.name += (first ? "" : ", ") + seat.selection.name;
                    row.local = row.local || seat.peer == view.seat;
                    row.accepted = first ? seat.ready : row.accepted && seat.ready; first = false;
                }
            }
        }
        const auto local = std::find_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.peer == view.seat; });
        if (!room->identity_pending && local != room->snapshot.seats.end() && (impl.name != local->selection.name || impl.colour != local->selection.colour)) {
            impl.name = local->selection.name; impl.colour = local->selection.colour;
            emit_signal("profile_changed", text(impl.name), static_cast<int64_t>(impl.colour));
        }
        impl.room_revision = room->snapshot.revision;
        for (const auto& chat : room->chat) if (view.phase != LanPhase::ended && chat.sequence > impl.room_chat_sequence) {
            note_chat(model::ChatKind::normal, chat.name + ": " + chat.text, chat.name);
            impl.room_chat_sequence = chat.sequence;
            if (chat.name != impl.name) play_sound("GUI_Common_Chat_Receive");
        }
        if (view.phase != LanPhase::ended && room->notice && room->notice->message != impl.room_notice) {
            impl.room_notice = room->notice->message;
            const auto key = room->notice->reason == net::LobbyReason::kicked ? "TEXT_INTERNET_PLAYER_KICKED"
                : room->notice->reason == net::LobbyReason::host_closed ? "TEXT_HOST_LEFT_GAME" : "";
            if (!lan_notice_in_chat(*room->notice, room->chat)) note_chat(model::ChatKind::error, *key ? key : impl.room_notice);
        }
        if (auto* ready = EAWR_CONTROL(EawrUiButton, "IDC_LOAD_GAME")) {
            ready->set_text(resolved(resources.text, room->local_ready ? "TEXT_BUTTON_CANCEL" : "TEXT_BUTTON_ACCEPT"));
            ready->set_disabled(!room->ready_enabled);
            ready->set_tooltip_text(text(room->ready_reason));
        }
    } else {
        // Who is in the game, from the presence: the host's name for the joiner, the joiner's for the host.
        std::string remote_name;
        std::uint32_t remote_colour = 1;
        if (impl.source) {
            if (impl.host) {
                const auto joining = impl.source->joining(impl.game);
                if (!joining.empty()) {
                    remote_name = joining.front().name;
                    remote_colour = joining.front().colour;
                }
            } else {
                for (const model::LobbyGame& game : impl.source->games()) {
                    if (game.name != impl.game && game.address != impl.address) continue;
                    // A game joined by a typed address learns its name from the host's presence.
                    if (impl.game.empty()) impl.game = game.name;
                    impl.host_name = game.player_names.empty() ? std::string{} : game.player_names.front();
                    for (const model::LobbyPlayer& player : impl.source->players()) {
                        if (player.id == game.host) remote_colour = player.colour;
                    }
                    break;
                }
            }
        }
        if (remote_name.empty() && impl.host && view.connected) remote_name = view.peer_name;

        impl.linked = connected;
        if (impl.host && !connected) impl.remote_colour_applied = false;
        if (impl.host && connected && !impl.remote_colour_applied && !remote_name.empty()
            && remote_colour < resources.palette.size()) {
            // The joiner's row takes its profile colour once; the host may change it afterwards.
            impl.remote_colour_applied = true;
            static_cast<void>(skirmish::select_setup_colour(impl.selection, 2, remote_colour, resources.palette.size()));
            impl.rows_signature.clear();
        }

        // The rows as the player sees them.
        if (impl.host && impl.map_index >= 0) map = &resources.maps[static_cast<std::size_t>(impl.map_index)];
        if (!impl.host && impl.offered && impl.offered->map) {
            for (const skirmish::SetupMap& candidate : resources.maps) {
                if (candidate.path == *impl.offered->map) map = &candidate;
            }
        }
        if (map != nullptr) visible = model::visible_staging_rows(network_row_count(*map), dialog_rows);
        if (impl.host) {
            // Each planned human has its own row; pending members remain Open.
            for (const skirmish::LobbySlot& slot : impl.selection.slots) {
                if (slot.slot == 0 || slot.slot > dialog_rows) continue;
                model::StagingRow& row = rows[slot.slot - 1];
                const auto seats = std::count(impl.human_owners.begin(), impl.human_owners.end(), slot.slot);
                row.kind = seats > 0 ? (slot.slot == 1 || (slot.slot == 2 && connected) ? model::RowKind::human : model::RowKind::open) : model::RowKind::ai;
                row.name = row.kind != model::RowKind::human ? std::string{} : slot.slot == 1 ? impl.name : remote_name;
                row.faction = slot.faction;
                row.team = slot.team;
                row.colour = slot.colour_index.value_or(slot.slot - 1);
                row.local = slot.slot == 1;
                row.accepted = row.kind == model::RowKind::ai || row.kind == model::RowKind::human;
            }
            if (dialog_rows > 1 && rows[1].kind == model::RowKind::none) rows[1].kind = model::RowKind::open;
        } else {
            if (dialog_rows > 0) {
                rows[0].kind = model::RowKind::human;
                rows[0].name = impl.host_name.empty() ? "Host" : impl.host_name;
                rows[0].colour = remote_colour;
                rows[0].accepted = true;
            }
            if (dialog_rows > 1) {
                rows[1].kind = model::RowKind::human;
                rows[1].name = impl.name;
                rows[1].colour = impl.colour;
                rows[1].local = true;
                rows[1].accepted = view.phase == LanPhase::barrier || view.phase == LanPhase::running;
            }
            if (impl.offered && impl.offered->slots) {
                for (const skirmish::LobbySlot& slot : *impl.offered->slots) {
                    if (slot.slot == 0 || slot.slot > dialog_rows) continue;
                    model::StagingRow& row = rows[slot.slot - 1];
                    row.kind = slot.human ? model::RowKind::human : model::RowKind::ai;
                    row.local = slot.human && slot.slot == view.player;
                    row.accepted = !slot.human || view.phase == LanPhase::barrier || view.phase == LanPhase::running;
                    if (slot.slot == 1) row.accepted = true;
                    if (!slot.human) row.name.clear();
                    else if (row.local) row.name = impl.name;
                    else if (slot.slot == 1) row.name = impl.host_name.empty() ? "Host" : impl.host_name;
                    row.faction = slot.faction;
                    row.team = slot.team;
                    row.colour = slot.colour_index.value_or(slot.slot - 1);
                }
            }
        }
    }
    impl.rows = rows;
    impl.visible_rows = visible;
}

void EawrLanLobby::refresh_staging_options(Impl& impl, Resources& resources, const skirmish::SetupMap* map) {
    if (!impl.host && impl.offered && impl.offered->map) {
        if (auto* name = EAWR_CONTROL(EawrUiLabel, "EAWR_JOINER_MAP_NAME")) {
            name->set_text(text(map ? map->name : *impl.offered->map));
            name->set_tooltip_text(text(*impl.offered->map));
        }
        // Refresh independently of the roster: a host can change only options.
        if (auto* summary = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST"); summary && impl.offered->match) {
            const auto& options = *impl.offered->match;
            std::vector<String> lines;
            const auto flag = [&](const char* caption, bool enabled) {
                lines.push_back(resolved(resources.text, caption) + ": " + (enabled ? "On" : "Off"));
            };
            lines.push_back(resolved(resources.text, "TEXT_SKIRMISH_CREDITS") + ": "
                + text(std::to_string(options.credits.trunc_to_integer())));
            flag("TEXT_ALLOW_HEROES", options.allow_heroes);
            flag("TEXT_FREE_STARTING_UNITS", options.free_starting_units);
            flag("TEXT_ALLOW_SUPER_WEAPONS", options.allow_superweapons);
            flag("TEXT_PRE_BUILT_BASE", options.pre_built_base);
            const auto condition = model::setup_win_condition_index(options.space_win_condition);
            const auto condition_text = condition ? resolved(resources.text,
                std::string(model::setup_win_conditions()[*condition].text_key)) : text(options.space_win_condition);
            lines.push_back(resolved(resources.text, "TEXT_WIN_CONDITION") + ": " + condition_text);
            bool changed = summary->get_item_count() != static_cast<int>(lines.size());
            for (int i = 0; !changed && i < summary->get_item_count(); ++i)
                changed = summary->get_item_text(i) != lines[static_cast<std::size_t>(i)];
            if (changed) {
                summary->clear();
                for (const auto& line : lines) {
                    summary->add_item(line, Ref<Texture2D>(), false);
                    summary->set_item_disabled(summary->get_item_count() - 1, true);
                    summary->set_item_tooltip(summary->get_item_count() - 1, line);
                }
            }
        }
    }
}

void EawrLanLobby::refresh_staging_roster(Impl& impl, PageState& state, Resources& resources,
                                          const std::optional<LanLobbyState>& room,
                                          const std::vector<model::StagingRow>& rows,
                                          const skirmish::SetupMap* map, std::size_t visible, bool connected,
                                          std::size_t dialog_rows) {
    // Refill the combos only when what they show changed: a refill closes an open drop-down.
    std::string signature = std::string(impl.host ? "H" : "J") + std::to_string(impl.map_index) + "|"
        + std::to_string(visible) + "|" + (connected ? "c" : "-");
    for (const model::StagingRow& row : rows) {
        signature += "|" + std::to_string(static_cast<int>(row.kind)) + "," + row.name + "," + row.faction + ","
            + (row.team ? std::to_string(*row.team) : std::string("-")) + "," + std::to_string(row.colour) + ","
            + (row.accepted ? "a" : "p");
    }
    if (signature != impl.rows_signature) {
        impl.rows_signature = signature;
        state.loading = true;
        std::vector<std::string> factions;
        if (map != nullptr) {
            for (const skirmish::SetupTeam& team : map->teams) {
                for (const std::string& faction : team.factions) {
                    if (std::find(factions.begin(), factions.end(), faction) == factions.end()) factions.push_back(faction);
                }
            }
        }
        impl.row_states.clear();
        for (std::size_t index = 0; index < dialog_rows; ++index) {
            const model::StagingRow& row = rows[index];
            const std::string suffix = std::to_string(index);
            // The primary owner rows stay visible; the host sees as many as the map holds.
            const bool shown = index < visible || (!impl.host && index < 2);
            auto* name = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_NAME_" + suffix);
            auto* faction = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_FACTION_" + suffix);
            auto* colour = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_COLOR_" + suffix);
            auto* team = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_TEAM_" + suffix);
            const bool editable = impl.host && row.kind == model::RowKind::ai;
            const bool choices_editable = row.occupied() && (impl.host || (room && row.local))
                && (!room || room->snapshot.phase == net::LobbyPhase::staging);
            if (name != nullptr) {
                name->clear();
                name->set_visible(shown);
                if (impl.host && index >= 2 && row.kind != model::RowKind::human && row.kind != model::RowKind::open) {
                    // Unplanned rows offer none or AI; unavailable AI levels stay disabled.
                    // Planned human/open rows retain their individual name controls.
                    name->add_item(resolved(resources.text, "TEXT_NO_PLAYER"), 0);
                    name->add_item(resolved(resources.text, "TEXT_OPEN_PLAYER"), 1);
                    name->add_item(resolved(resources.text, "TEXT_EASY_AI_PLAYER"), 2);
                    name->add_item(resolved(resources.text, "TEXT_MEDIUM_AI_PLAYER"), 3);
                    name->add_item(resolved(resources.text, "TEXT_HARD_AI_PLAYER"), 4);
                    for (const int item : {1, 2, 4}) name->set_item_disabled(item, true);
                    name->set_tooltip_text("AI rows use the selected AI level. Set alliances with each row's Team.");
                    name->select(row.kind == model::RowKind::ai ? 3 : 0);
                    name->set_disabled(false);
                } else {
                    std::string label = row.name.empty()
                        ? (row.kind == model::RowKind::open ? std::string("TEXT_OPEN_PLAYER")
                            : row.kind == model::RowKind::ai ? std::string("TEXT_MEDIUM_AI_PLAYER") : std::string{}) : row.name;
                    name->add_item(resolved(resources.text, label), 0);
                    name->set_tooltip_text(text(row.name));
                    if (room && row.local && !impl.host) {
                        name->clear();
                        for (const auto& target : rows) if (target.local || target.kind == model::RowKind::open) {
                            const auto slot = static_cast<int>(&target - rows.data() + 1);
                            name->add_item(text(std::to_string(slot)), slot);
                            if (slot == static_cast<int>(index + 1)) name->select(name->get_item_count() - 1);
                        }
                        name->set_disabled(!choices_editable);
                    } else {
                        name->set_disabled(true);
                        if (room && impl.host && row.kind == model::RowKind::human) {
                            for (const auto& seat : room->snapshot.seats) if (seat.selection.slot == index + 1 && seat.peer != room->snapshot.host) {
                                name->add_item(resolved(resources.text, "TEXT_KICK") + " " + text(seat.selection.name), static_cast<int>(10000 + seat.peer));
                                name->set_disabled(!choices_editable);
                            }
                        }
                    }
                }
            }
            const std::string rename_id = "EAWR_STAGING_NAME_" + suffix;
            auto* rename = EAWR_CONTROL(EawrUiEdit, rename_id);
            if (room && row.local && shown) {
                const auto* authored = find_gadget(*state.dialog, "IDC_PLAYER_NAME_" + suffix);
                if (authored && name) {
                    const Rect2 rect = local_rect(*state.dialog, *authored);
                    if (!rename) {
                        rename = memnew(EawrUiEdit); rename->set_theme(resources.theme);
                        rename->set_max_length(static_cast<int>(model::lan_name_max));
                        rename->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(text(rename_id)));
                        state.dialog->frame->add_child(rename); state.controls[rename_id] = rename;
                    }
                    rename->set_position(rect.position); const bool shared_remote = room && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == index + 1 && seat.peer != room->snapshot.host; });
                    const bool combo_shown = !impl.host || shared_remote;
                    rename->set_size(Vector2(rect.size.x * (combo_shown ? 0.7F : 1.0F), rect.size.y));
                    rename->set_text(text(impl.name)); rename->set_editable(choices_editable); rename->set_visible(true);
                    name->set_visible(combo_shown);
                    if (combo_shown) { name->set_position(rect.position + Vector2(rect.size.x * 0.7F, 0)); name->set_size(Vector2(rect.size.x * 0.3F, rect.size.y)); }
                }
            } else if (rename) rename->set_visible(false);
            const bool filled = shown && (row.occupied() || !impl.host);
            if (faction != nullptr) {
                faction->clear();
                faction->set_visible(filled);
                for (const std::string& value : factions) faction->add_item(text(value));
                if (!row.faction.empty() && factions.empty()) faction->add_item(text(row.faction));
                for (int item = 0; item < faction->get_item_count(); ++item) {
                    if (utf8(faction->get_item_text(item)) == row.faction) faction->select(item);
                }
                faction->set_disabled(!choices_editable);
            }
            if (colour != nullptr) {
                colour->clear();
                colour->set_visible(filled);
                // Like skirmish setup, the staging colour column contains only a centred bar.
                const Vector2 box = colour->get_size();
                const int bar_width = std::max(1, static_cast<int>(std::lround(box.x * 0.27F)));
                const int bar_height = std::max(1, static_cast<int>(std::lround(box.y * 0.25F)));
                colour->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
                for (std::size_t item = 0; item < resources.palette.size(); ++item) {
                    const auto& rgb = resources.palette[item].rgb;
                    Ref<Image> swatch = Image::create_empty(bar_width, bar_height, false, Image::FORMAT_RGBA8);
                    swatch->fill(Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
                    colour->add_icon_item(ImageTexture::create_from_image(swatch),
                                          String(), static_cast<int>(item));
                }
                if (row.colour < resources.palette.size()) colour->select(static_cast<int>(row.colour));
                colour->set_disabled(!choices_editable);
            }
            if (team != nullptr) {
                team->clear();
                team->set_visible(filled);
                if (map != nullptr) {
                    for (const skirmish::SetupTeam& side : map->teams) {
                        team->add_item(text("Team " + std::to_string(side.team + 1)), static_cast<int>(side.team));
                    }
                }
                if (row.team) {
                    for (int item = 0; item < team->get_item_count(); ++item) {
                        if (team->get_item_id(item) == static_cast<int>(*row.team)) team->select(item);
                    }
                }
                team->set_disabled(!choices_editable);
            }
            if (auto* kick = EAWR_CONTROL(EawrUiButton, "IDC_BUTTON_KICK_PLAYER" + suffix)) {
                const bool remote = room && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == index + 1 && seat.peer != room->snapshot.host; });
                kick->set_visible(shown && impl.host && remote);
                kick->set_disabled(!room || room->snapshot.phase != net::LobbyPhase::staging);
            }
            // Faction choices a team has no station for stay listed and disabled, as the setup screen does.
            if (faction != nullptr && choices_editable && map != nullptr && row.team) {
                const auto side = std::find_if(map->teams.begin(), map->teams.end(),
                                               [&](const skirmish::SetupTeam& t) { return t.team == *row.team; });
                for (int item = 0; item < faction->get_item_count(); ++item) {
                    const std::string value = utf8(faction->get_item_text(item));
                    const bool allowed = side != map->teams.end()
                        && std::find(side->factions.begin(), side->factions.end(), value) != side->factions.end();
                    faction->set_item_disabled(item, !allowed);
                }
            }
            const auto icon = state.accept_icons.find("IDC_ACCEPT_PLAYER_" + suffix);
            if (icon != state.accept_icons.end()) {
                icon->second->set_visible(shown && row.occupied());
                icon->second->set_color(row.accepted ? Color(0.2F, 0.85F, 0.3F, 0.95F) : Color(0.15F, 0.15F, 0.15F, 0.9F));
            }
            RowState row_state;
            row_state.kind = row.kind == model::RowKind::human ? "human" : row.kind == model::RowKind::ai ? "ai"
                           : row.kind == model::RowKind::open ? "open" : "none";
            row_state.name = row.name;
            row_state.faction = row.faction;
            row_state.team = row.team ? std::to_string(*row.team + 1) : std::string{};
            row_state.colour = row.colour < resources.palette.size() ? colour_name(resources.palette[row.colour].constant)
                                                                     : std::string{};
            row_state.accepted = row.accepted;
            row_state.enabled = editable || choices_editable;
            row_state.visible = shown;
            impl.row_states.push_back(std::move(row_state));
        }
        // The preview follows the map.
        if (state.preview != nullptr && map != nullptr && impl.preview_path != map->path) {
            impl.preview_path = map->path;
            Ref<Image> image;
            std::string failure;
            if (!map->preview_path.empty()) {
                if (auto texture = assets::load_texture(*resources.content->filesystem, map->preview_path)) {
                    image = texture_image(texture.value(), failure);
                }
            } else if (!map->embedded_preview.empty()) {
                if (auto texture = assets::load_texture(map->embedded_preview, assets::Source{})) {
                    image = texture_image(texture.value(), failure);
                }
            }
            if (image.is_null()) {
                if (auto texture = assets::load_texture(*resources.content->filesystem, "data/art/textures/generic_space.dds")) {
                    image = texture_image(texture.value(), failure);
                }
            }
            state.preview->set_texture(image.is_valid() ? ImageTexture::create_from_image(image) : Ref<ImageTexture>());
        }
        state.loading = false;
    }
}

void EawrLanLobby::refresh_staging_start(Impl& impl, Resources& resources, const LanView& view,
                                         const std::optional<LanLobbyState>& room,
                                         const std::vector<model::StagingRow>& rows, bool online) {
    // The start (host) or accept (joiner) button.
    if (auto* start = EAWR_CONTROL(EawrUiButton, "IDC_LAN_START_GAME_BUTTON")) {
        const bool enabled = room ? (impl.host ? room->start_enabled : room->ready_enabled)
            : impl.host ? (view.start_enabled && model::host_start_enabled(rows, impl.human_owners.size())) : view.accept_enabled;
        if (room && !impl.host) start->set_text(resolved(resources.text, room->local_ready ? "TEXT_BUTTON_CANCEL" : "TEXT_BUTTON_ACCEPT"));
        start->set_disabled(!enabled || impl.launch.has_value());
        impl.start_reason = room ? (impl.host ? room->start_reason : room->ready_reason) : online ? view.message : std::string{};
        start->set_tooltip_text(text(impl.start_reason));
        impl.start_enabled = enabled;
        impl.start_caption = utf8(start->get_text());
    }
}

void EawrLanLobby::finish_staging_refresh(Impl& impl, const LanView& view,
                                          const std::optional<LanLobbyState>& room) {
    // The chat list.
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST")) draw_chat(*list, impl.staging_chat);
    announce();

    // A launch the transport handed over (the host's Start; the joiner's Accept).
    if (!impl.launch) {
        if (auto launch = match_->take_start()) {
            impl.launch = std::move(launch);
            impl.locked = true;
            announce();
            emit_signal("started");
        }
    }
    // A refused or dropped connection ends the staging once the player has read why.
    if (view.phase == LanPhase::ended && !impl.refused && !impl.launch) {
        impl.refused = true;
        if (room && room->notice && room->notice->reason == net::LobbyReason::kicked) show_failure("TEXT_INTERNET_PLAYER_KICKED");
        else if (room && room->notice && room->notice->reason == net::LobbyReason::host_closed) show_failure("TEXT_HOST_LEFT_GAME");
        else show_failure(lan_message(view));
    }
}

// ---- Scripted use -----------------------------------------------------------------------------

bool EawrLanLobby::run_action(const std::string& action) {
    if (!shown_ || !state_) return false;
    if (action == "connections") return action_connections();
    Impl& impl = *impl_;
    if (!shown_) return false;
    const auto colon = action.find(':');
    const std::string verb = action.substr(0, colon);
    const std::string argument = colon == std::string::npos ? std::string{} : action.substr(colon + 1);
    const auto number = [](const std::string& value, std::uint32_t& out) {
        return !value.empty() && std::from_chars(value.data(), value.data() + value.size(), out).ec == std::errc{};
    };
    if (verb == "source") return action_source(argument);
    if (verb == "cancel_join") return action_cancel_join();
    if (verb == "name" && page_ != Page::staging) return action_name(argument);
    if (verb == "colour" && page_ != Page::staging) return action_colour(argument, number);
    if (verb == "chat") return action_chat(argument);
    if (verb == "sort") return action_sort(impl, argument);
    if (verb == "filter") return action_filter(impl, argument, number);
    if (verb == "humans" && (page_ == Page::lobby || page_ == Page::host_options)) return action_humans(argument, number);
    if (verb == "host_dropdown" && page_ == Page::host_options) return action_host_dropdown(argument);

    if (verb == "staging_dropdown" && page_ == Page::staging) return action_staging_dropdown(argument, number);

    if (verb == "host") return action_host(impl, argument);
    if (verb == "join") return action_join(impl, argument);
    if (verb == "password") return action_password(impl, argument);
    if (verb == "prompt_cancel") return action_prompt_cancel(impl);
    if (verb == "refresh") return action_refresh(impl);
    if (verb == "rooms") return action_rooms(impl, argument);
    if (verb == "code") return action_code(impl, argument);
    if (verb == "direct") return action_direct(impl, argument);
    if (verb == "cancel") return action_cancel();
    if (verb == "leave") return action_leave();
    if (page_ != Page::staging) return false;
    if (verb == "map" && impl.host) return action_map(impl, argument);
    if (verb == "ai" && impl.host) return action_ai(argument, number);
    if (verb == "team" && page_ == Page::staging) return action_team(argument, number);
    if ((verb == "ready" || verb == "unready") && page_ == Page::staging && match_->lobby_state()) return action_ready(verb);
    if (verb == "kick" && impl.host && page_ == Page::staging) return action_kick(argument, number);
    if (page_ == Page::staging && match_->lobby_state() && (verb == "slot" || verb == "faction" || verb == "name" || verb == "colour")) return action_staging_selection(verb, argument, number);
    if (verb == "start" && impl.host) return action_start(impl);
    if (verb == "accept" && !impl.host) return action_accept(impl);
    return false;
}

bool EawrLanLobby::action_connections() {
    if (page_ != Page::staging || !EAWR_CONTROL(EawrUiList, "EAWR_ONLINE_CONNECTIONS_LIST")) return false;
    handle_button("EAWR_ONLINE_CONNECTIONS"); return true;
}

bool EawrLanLobby::action_source(const std::string& argument) {
    if (page_ != Page::lobby || (argument != "lan" && argument != "online")) return false;
    select_source(argument == "lan" ? model::LobbySourceKind::lan : model::LobbySourceKind::online);
    return true;
}

bool EawrLanLobby::action_cancel_join() {
    if (page_ != Page::lobby) return false;
    handle_button(cancel_join_button);
    return true;
}

bool EawrLanLobby::action_name(const std::string& argument) {
    if (page_ != Page::lobby) return false;
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) edit->set_text(text(argument));
    commit_name(argument);
    return true;
}

bool EawrLanLobby::action_colour(const std::string& argument, bool (*number)(const std::string&, std::uint32_t&)) {
    std::uint32_t value{};
    if (page_ != Page::lobby || !number(argument, value) || value >= resources_->palette.size()) return false;
    if (auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_LAN_COLOR_SELECT")) combo->select(static_cast<int>(value));
    commit_colour(value);
    return true;
}

bool EawrLanLobby::action_chat(const std::string& argument) {
    if (page_ != Page::lobby && page_ != Page::staging) return false;
    send_chat(argument);
    return true;
}

bool EawrLanLobby::action_sort(Impl& impl, const std::string& argument) {
    static const std::map<std::string, model::LobbySort> columns{{"name", model::LobbySort::game_name},
        {"type", model::LobbySort::game_type}, {"players", model::LobbySort::players},
        {"map", model::LobbySort::map_name}, {"ping", model::LobbySort::ping}, {"host", model::LobbySort::host}};
    const auto found = columns.find(argument);
    if (found == columns.end()) return false;
    impl.sort = found->second;
    impl.dirty = true;
    return true;
}

bool EawrLanLobby::action_filter(Impl& impl, const std::string& argument,
                                 bool (*number)(const std::string&, std::uint32_t&)) {
    // filter:clear, or filter:type=<key>, min=<n>, max=<n>, ping=<ms>
    if (argument == "clear") {
        impl.filter.clear();
    } else {
        const auto equals = argument.find('=');
        if (equals == std::string::npos) return false;
        const std::string field = argument.substr(0, equals);
        const std::string value = argument.substr(equals + 1);
        std::uint32_t parsed{};
        const bool numeric = number(value, parsed);
        if (field == "type") impl.filter.game_type = value;
        else if (field == "min" && numeric) impl.filter.min_players = parsed;
        else if (field == "max" && numeric) impl.filter.max_players = parsed;
        else if (field == "ping" && numeric) impl.filter.max_ping_ms = parsed;
        else return false;
    }
    impl.dirty = true;
    return true;
}

bool EawrLanLobby::action_humans(const std::string& argument, bool (*number)(const std::string&, std::uint32_t&)) {
    if (page_ == Page::lobby) open_host_options();
    std::uint32_t count{};
    if (!number(argument, count)) return false;
    auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO");
    if (!combo) return false;
    for (int item = 0; item < combo->get_item_count(); ++item) if (combo->get_item_id(item) == static_cast<int>(count)) {
        combo->select(item); on_item_selected(item, "IDC_INTERNET_HOST_CLAN_COMBO"); return true;
    }
    return false;
}

bool EawrLanLobby::action_host_dropdown(const std::string& argument) {
    if (argument != "humans") return false;
    if (auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO")) {
        combo->show_popup(); return true;
    }
    return false;
}

bool EawrLanLobby::action_staging_dropdown(const std::string& argument,
                                           bool (*number)(const std::string&, std::uint32_t&)) {
    const auto field_separator = argument.find(':');
    if (field_separator == std::string::npos) return false;
    const std::string field = argument.substr(0, field_separator);
    if (field != "COLOR" && field != "FACTION" && field != "TEAM") return false;
    std::uint32_t row{};
    if (!number(argument.substr(field_separator + 1), row) || row >= state_->dialog_rows) return false;
    for (const auto& entry : state_->controls) {
        if (auto* combo = cast<EawrUiCombo>(entry.second)) combo->get_popup()->hide();
    }
    if (auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_" + field + "_" + std::to_string(row));
        combo != nullptr && combo->is_visible() && !combo->is_disabled()) {
        combo->show_popup();
        return true;
    }
    return false;
}

bool EawrLanLobby::action_host(Impl& impl, const std::string& argument) {
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online && !impl.online_rooms) return false;
    if (page_ != Page::lobby && page_ != Page::host_options) return false;
    if (page_ == Page::lobby) open_host_options();
    if (page_ != Page::host_options) return false;
    // Online: host:<name>|<password> makes a private room, host:<name> a public one.
    std::string name = argument, room_password;
    if (online) {
        if (const auto bar = argument.find('|'); bar != std::string::npos) {
            name = argument.substr(0, bar);
            room_password = argument.substr(bar + 1);
        }
    }
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT")) edit->set_text(text(name));
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_PASSWORD_EDIT"); edit != nullptr && online) edit->set_text(text(room_password));
    begin_hosting();
    return page_ == Page::staging;
}

bool EawrLanLobby::action_join(Impl& impl, const std::string& argument) {
    if (page_ != Page::lobby) return false;
    refresh_lobby();
    for (const model::LobbyGame& game : impl.shown) {
        if (game.name == argument) {
            impl.selected_key = game.name + "|" + game.address;
            if (impl.join_selection.source() == model::LobbySourceKind::online) {
                join_online_selected();
                return impl.prompt.has_value() || impl.join_selection.attempt().has_value() || page_ == Page::staging;
            }
            join_selected();
            return page_ == Page::staging;
        }
    }
    return false;
}

bool EawrLanLobby::action_password(Impl& impl, const std::string& argument) {
    if (page_ != Page::lobby || !impl.prompt) return false;
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) edit->set_text(text(argument));
    submit_password(argument);
    return impl.prompt.has_value() ? impl.prompt->waiting : page_ == Page::staging;
}

bool EawrLanLobby::action_prompt_cancel(Impl& impl) {
    if (page_ != Page::lobby || !impl.prompt) return false;
    handle_button(prompt_cancel_button);
    return true;
}

bool EawrLanLobby::action_refresh(Impl& impl) {
    if (page_ != Page::lobby || impl.join_selection.source() != model::LobbySourceKind::online) return false;
    refresh_online_rooms();
    return true;
}

bool EawrLanLobby::action_rooms(Impl& impl, const std::string& argument) {
    if (page_ != Page::lobby || impl.join_selection.source() != model::LobbySourceKind::online
        || (argument != "next" && argument != "prev")) return false;
    handle_button(argument == "next" ? next_button : prev_button);
    return true;
}

bool EawrLanLobby::action_code(Impl& impl, const std::string& argument) {
    if (page_ != Page::lobby || impl.join_selection.source() != model::LobbySourceKind::online) return false;
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, invitation_edit)) edit->set_text(text(argument));
    join_code(argument);
    return true;
}

bool EawrLanLobby::action_direct(Impl& impl, const std::string& argument) {
    if (impl.join_selection.source() != model::LobbySourceKind::lan) return false;
    if (page_ != Page::lobby) return false;
    open_direct_connect();
    if (page_ != Page::direct_connect) return false;
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) edit->set_text(text(argument));
    handle_button("IDC_IP_CONNECT_BUTTON");
    return page_ == Page::staging;
}

bool EawrLanLobby::action_cancel() {
    handle_button(page_ == Page::host_options || page_ == Page::direct_connect || page_ == Page::filter
        ? "IDC_CANCEL_BUTTON" : "IDC_LAN_CANCEL_BUTTON");
    return true;
}

bool EawrLanLobby::action_leave() {
    if (page_ != Page::staging) return false;
    leave_staging();
    return true;
}

bool EawrLanLobby::action_map(Impl& impl, const std::string& argument) {
    for (std::size_t item = 0; item < impl.listed.size(); ++item) {
        if (resources_->maps[impl.listed[item]].name == argument || resources_->maps[impl.listed[item]].path == argument) {
            if (network_row_count(resources_->maps[impl.listed[item]]) < impl.human_owners.size()) return false;
            if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST")) list->select(static_cast<int>(item));
            on_item_selected(static_cast<int64_t>(item), "IDC_STAGING_MAP_LIST");
            return true;
        }
    }
    return false;
}

bool EawrLanLobby::action_ai(const std::string& argument, bool (*number)(const std::string&, std::uint32_t&)) {
    std::uint32_t row{};
    if (!number(argument, row) || row < 2) return false;
    const std::string id = "IDC_PLAYER_NAME_" + std::to_string(row);
    auto* combo = EAWR_CONTROL(EawrUiCombo, id);
    if (combo == nullptr || combo->get_item_count() < 4) return false;
    combo->select(3);
    on_item_selected(3, text(id));
    return true;
}

bool EawrLanLobby::action_team(const std::string& argument, bool (*number)(const std::string&, std::uint32_t&)) {
    const auto equals = argument.find_first_of(":=");
    if (equals == std::string::npos) return false;
    std::uint32_t row{}, team{};
    if (!number(argument.substr(0, equals), row) || !number(argument.substr(equals + 1), team) || team == 0) return false;
    const std::string id = "IDC_PLAYER_TEAM_" + std::to_string(row);
    auto* combo = EAWR_CONTROL(EawrUiCombo, id);
    if (combo == nullptr || !combo->is_visible() || combo->is_disabled()) return false;
    for (int item = 0; item < combo->get_item_count(); ++item) {
        if (combo->get_item_id(item) == static_cast<int>(team) - 1) {
            combo->select(item);
            on_item_selected(item, text(id));
            return true;
        }
    }
    return false;
}

bool EawrLanLobby::action_ready(const std::string& verb) {
    const bool result = match_->lobby_ready(verb == "ready"); refresh_staging(); return result;
}

bool EawrLanLobby::action_kick(const std::string& argument, bool (*number)(const std::string&, std::uint32_t&)) {
    std::uint32_t peer{}; return number(argument, peer) && match_->lobby_kick(peer);
}

bool EawrLanLobby::action_staging_selection(const std::string& verb, const std::string& argument,
                                            bool (*number)(const std::string&, std::uint32_t&)) {
    const auto room = *match_->lobby_state();
    const auto local = std::find_if(room.snapshot.seats.begin(), room.snapshot.seats.end(), [&](const auto& seat) { return seat.peer == match_->view().seat; });
    if (local == room.snapshot.seats.end()) return false;
    auto choice = local->selection;
    if (verb == "name") choice.name = model::clamp_player_name(argument);
    else if (verb == "faction") choice.faction = argument;
    else if (verb == "slot") { if (!number(argument, choice.slot)) return false; }
    else if (!number(argument, choice.colour)) return false;
    return match_->lobby_select(std::move(choice));
}

bool EawrLanLobby::action_start(Impl& impl) {
    refresh_staging();
    if (match_->galactic_room()) {
        // GC: the room's own validation gates Start (two humans, the human-seat limit, the campaign rules).
        match_->start(skirmish::FixtureOptions{});
        refresh_staging();
        return match_->galactic_start().has_value();
    }
    const auto room = match_->lobby_state();
    const auto humans = room ? room->snapshot.seats.size() : impl.human_owners.size();
    const model::StartVerdict verdict = model::check_host_start(impl.rows, impl.map_index >= 0, humans);
    if (!verdict.ok()) {
        if (!verdict.key.empty()) note_chat(model::ChatKind::error, verdict.key);
        return false;
    }
    auto options = validated_lan_options(impl.selection, resources_->maps);
    if (!options) {
        note_chat(model::ChatKind::error, options.error().message);
        return false;
    }
    match_->start(options.value());
    refresh_staging();
    return impl.launch.has_value();
}

bool EawrLanLobby::action_accept(Impl& impl) {
    if (const auto room = match_->lobby_state()) {
        const bool result = match_->lobby_ready(!room->local_ready); refresh_staging(); return result;
    }
    bool available = false;
    if (impl.offered && impl.offered->map) {
        for (const skirmish::SetupMap& candidate : resources_->maps) {
            if (candidate.path == *impl.offered->map && candidate.unavailable.empty()) available = true;
        }
    }
    const model::StartVerdict verdict = model::check_client_accept(impl.rows, available);
    if (!verdict.ok()) {
        note_chat(model::ChatKind::error, verdict.key);
        return false;
    }
    match_->accept();
    refresh_staging();
    return true;
}

bool EawrLanLobby::condition(const std::string& wanted) const {
    if (!impl_ || !shown_) return false;
    const Impl& impl = *impl_;
    const auto colon = wanted.find(':');
    const std::string key = wanted.substr(0, colon);
    const std::string value = colon == std::string::npos ? std::string{} : wanted.substr(colon + 1);
    const auto count_of = [&value](std::uint32_t& out) {
        return !value.empty() && std::from_chars(value.data(), value.data() + value.size(), out).ec == std::errc{};
    };
    if (key == "page") {
        static constexpr const char* names[] = {"lobby", "host", "staging", "direct", "filter"};
        return value == names[static_cast<std::size_t>(page_)];
    }
    if (key == "game") {
        return std::any_of(impl.shown.begin(), impl.shown.end(), [&](const model::LobbyGame& game) { return game.name == value; });
    }
    if (key == "players") {
        std::uint32_t count{};
        return count_of(count) && impl.players.size() >= count;
    }
    if (key == "staging") return page_ == Page::staging;
    if (key == "box") return impl.box != nullptr;
    if (key == "prompt") return impl.prompt.has_value();
    if (key == "prompt_ready") return impl.prompt && !impl.prompt->waiting;
    if (key == "prompt_error") return impl.prompt && impl.prompt->error.find(value) != std::string::npos;
    if (key == "hosting") return model::online_hosting_name(impl.hosting) == value;
    if (key == "rooms") {
        std::uint32_t count{};
        return count_of(count) && impl.online_rooms_total == count;
    }
    if (key == "source") {
        return value == (impl.join_selection.source() == model::LobbySourceKind::online ? "online" : "lan");
    }
    if (key == "room_player") {
        const auto room = match_->lobby_state(); return room && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.name == value; });
    }
    if (key == "unready") {
        const auto room = match_->lobby_state(); return room && std::none_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; });
    }
    if (key == "ready") {
        const auto room = match_->lobby_state(); if (!room) return false;
        if (value == "all") return !room->snapshot.seats.empty() && std::all_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; });
        std::uint32_t count{}; return count_of(count) && std::count_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; }) >= count;
    }
    if (key == "joined") {
        std::uint32_t count{};
        if (!count_of(count) || page_ != Page::staging || !impl.linked) return false;
        return match_->view().connected_seats >= count;
    }
    if (key == "chat") {
        const auto& lines = (page_ == Page::staging ? impl.staging_chat : impl.lobby_chat).lines();
        return std::any_of(lines.begin(), lines.end(), [&](const model::ChatLine& line) {
            return line.text.find(value) != std::string::npos;
        });
    }
    return false;
}

} // namespace eawr::presentation::godot_backend
