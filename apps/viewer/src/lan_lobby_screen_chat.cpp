#include "lan_lobby_screen_internal.hpp"

namespace eawr::presentation::godot_backend {
void EawrLanLobby::draw_chat(EawrUiList& list, const model::ChatLog& log) {
    PageState& state = *state_;
    const auto& lines = log.lines();
    if (state.chat_scroll) {
        if (VScrollBar* bar = list.get_v_scroll_bar()) bar->set_value(bar->get_max());
        state.chat_scroll = false;
    }
    // chat_drawn counts lines ever drawn, so a full log (oldest line dropped per add) still shows new lines.
    const std::size_t first_kept = log.total() - lines.size();
    if (state.chat_drawn < first_kept) state.chat_drawn = first_kept;
    if (state.chat_drawn > log.total()) state.chat_drawn = log.total();
    for (; state.chat_drawn < log.total(); ++state.chat_drawn) {
        const model::ChatLine& line = lines[state.chat_drawn - first_kept];
        const bool is_key = line.sender.empty() && line.text.rfind("TEXT_", 0) == 0;
        const int index = list.add_item(is_key ? resolved(resources_->text, line.text) : text(line.text));
        list.set_item_selectable(index, false);
        if (line.kind == model::ChatKind::error) list.set_item_custom_fg_color(index, Color(1.0F, 0.2F, 0.2F));
        else if (line.kind == model::ChatKind::notice) list.set_item_custom_fg_color(index, Color(1.0F, 0.9F, 0.4F));
        state.chat_scroll = true;
        // The list keeps what the log keeps: the oldest line goes when it is full.
        while (static_cast<std::size_t>(list.get_item_count()) > log.capacity()) list.remove_item(0);
    }
}

void EawrLanLobby::commit_name_from_edit() {
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) commit_name(utf8(edit->get_text()));
}

void EawrLanLobby::note_chat(const model::ChatKind kind, const std::string& line, const std::string& sender) {
    (page_ == Page::staging ? impl_->staging_chat : impl_->lobby_chat).add(kind, line, sender);
    impl_->dirty = true; // the lobby redraws its lists on a timer otherwise
}

void EawrLanLobby::play_sound(const std::string& wanted) {
    // The sound the debug build names is looked up in the effective data; a name the data spells
    // differently resolves to the one event within one edit of it (docs/ui/menu-screens.md).
    std::string found;
    for (const std::string& id : resources_->sfx_ids) {
        if (same_ci(id, wanted)) {
            found = id;
            break;
        }
    }
    if (found.empty()) {
        std::size_t matches = 0;
        for (const std::string& id : resources_->sfx_ids) {
            if (within_one_edit(id, wanted)) {
                found = id;
                ++matches;
            }
        }
        if (matches != 1) found.clear();
    }
    if (found.empty()) return;
    if (sounds_.size() < 64) sounds_.push_back(found);
    if (!options_.silent) emit_signal("sound_requested", text(found));
}

void EawrLanLobby::commit_name(const std::string& typed) {
    const std::string name = model::clamp_player_name(typed);
    if (name.empty() || name == impl_->name) {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) edit->set_text(text(impl_->name));
        return;
    }
    impl_->name = name;
    emit_signal("profile_changed", text(impl_->name), static_cast<int64_t>(impl_->colour));
    announce();
    impl_->dirty = true;
    refresh_lobby();
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) chat->grab_focus();
}

void EawrLanLobby::commit_colour(const std::uint32_t colour) {
    if (colour == impl_->colour) return;
    impl_->colour = colour;
    emit_signal("profile_changed", text(impl_->name), static_cast<int64_t>(impl_->colour));
    announce();
    impl_->dirty = true;
}

void EawrLanLobby::send_chat(const std::string& typed) {
    const std::optional<std::string> line = model::chat_line_to_send(typed);
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) {
        edit->set_text("");
        edit->grab_focus();
    }
    if (!line) return;
    if (impl_->join_selection.source() == model::LobbySourceKind::online && page_ == Page::lobby) {
        // The service carries chat for the room the player is in, else the browser's room-less channel (#3758); the
        // service sends the line back, so there is no local echo. When neither can carry it the window says why.
        auto members = impl_->online_rooms ? impl_->online_rooms->players() : std::vector<model::LobbyPlayer>{};
        std::erase_if(members, [](const model::LobbyPlayer& player) { return player.place == model::LobbyPlace::lobby; });
        if (members.empty()) {
            const model::OnlineMenuChat state = impl_->online_rooms ? impl_->online_rooms->menu_state() : model::OnlineMenuChat::idle;
            if (state != model::OnlineMenuChat::active) {
                note_chat(model::ChatKind::notice, std::string(model::online_menu_chat_notice(state)));
                return;
            }
            impl_->online_rooms->send_chat(model::LobbyChatScope::lobby, {}, impl_->name, *line);
            play_sound("GUI_Common_Chat_Send");
            return;
        }
        impl_->online_rooms->send_chat(model::LobbyChatScope::game, members.front().game, impl_->name, *line);
        play_sound("GUI_Common_Chat_Send");
        return;
    }
    if (page_ == Page::staging && match_->lobby_enabled()) {
        if (match_->lobby_chat(*line)) play_sound("GUI_Common_Chat_Send");
        else if (!match_->lobby_state()) note_chat(model::ChatKind::notice, "TEXT_WAITING");
        return;
    }
    if (impl_->source) {
        if (!impl_->source->try_send_chat(page_ == Page::staging ? model::LobbyChatScope::game : model::LobbyChatScope::lobby,
                                        page_ == Page::staging ? impl_->game : std::string{}, impl_->name, *line)) {
            if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) edit->set_text(text(typed));
            return;
        }
    } else if (options_.lan_source_factory) {
        // Discovery could not open: do not claim a local-only line was sent to the LAN.
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) edit->set_text(text(typed));
        return;
    }
    note_chat(model::ChatKind::normal, impl_->name + ": " + *line, impl_->name);
    play_sound("GUI_Common_Chat_Send");
}

void EawrLanLobby::announce() {
    if (!impl_->source) return;
    model::LobbyPresence presence;
    presence.name = impl_->name;
    presence.colour = impl_->colour;
    if (page_ == Page::staging && impl_->host && match_->view().phase != LanPhase::ended) {
        presence.place = model::LobbyPlace::hosting;
        presence.game = impl_->game;
        presence.map = match_->galactic_room() ? impl_->galactic_campaign
            : impl_->map_index >= 0 ? resources_->maps[static_cast<std::size_t>(impl_->map_index)].name : std::string{};
        presence.game_type = impl_->host_options.game_type;
        presence.tcp_port = match_ ? match_->view().port : 0;
        presence.max_players = static_cast<std::uint32_t>(impl_->human_owners.size());
        presence.passworded = impl_->join_selection.source() == model::LobbySourceKind::online
            && !impl_->host_password.empty();
        presence.locked = impl_->locked;
    } else if (page_ == Page::staging && match_->view().phase != LanPhase::ended) {
        presence.place = model::LobbyPlace::joining;
        presence.game = impl_->game;
    }
    impl_->source->announce(presence);
}

} // namespace eawr::presentation::godot_backend
