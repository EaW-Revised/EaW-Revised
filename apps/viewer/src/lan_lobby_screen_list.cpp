#include "lan_lobby_screen_internal.hpp"

namespace eawr::presentation::godot_backend {
void EawrLanLobby::open_lobby() {
    page_ = Page::lobby;
    impl_->prompt.reset();
    build_page(lobby_dialog);
    PageState& state = *state_;
    const BuiltDialog& dialog = *state.dialog;
    Resources& resources = *resources_;
    state.loading = true;

    // The retail dialog's second short list has no traced use: it stays hidden.
    if (auto* extra = EAWR_CONTROL(Control, "IDC_GAME_LIST2")) extra->set_visible(false);
    if (const BuiltGadget* gadget = find_gadget(dialog, "IDC_GAME_LIST")) {
        state.controls["IDC_GAME_LIST"]->set_visible(false);
        state.controls["IDC_GAME_LIST"]->set_name("EAWR_AUTHORED_GAME_LIST");
        state.game_list = memnew(EawrLobbyGameList);
        state.game_list->set_name("IDC_GAME_LIST");
        Rect2 rect = local_rect(dialog, *gadget);
        // Use the loaded list width and action-row height for the source selector.
        const BuiltGadget* spare = find_gadget(dialog, "IDC_GAME_LIST2");
        const Rect2 selector = spare != nullptr ? local_rect(dialog, *spare) : rect;
        const auto* anchor = find_gadget(dialog, "IDC_SET_FILTER_BUTTON");
        const float selector_height = anchor ? local_rect(dialog, *anchor).size.y : selector.size.y / 2.0F;
        const float gap = selector_height / 6.0F;
        // The spare list shares the sort-header row. Use the main list's top for the source tabs.
        const Vector2 selector_position(rect.position.x, rect.position.y);
        for (const auto& [id, caption] : std::array<std::pair<const char*, const char*>, 2>{{
                 {lan_source_button, "LAN"}, {online_source_button, "Online"}}}) {
            auto* button = memnew(EawrUiButton);
            button->set_name(id);
            button->set_text(caption);
            const bool lan = std::string_view(id) == lan_source_button;
            const float x = lan ? 0.0F : selector.size.x / 2.0F;
            button->set_position(selector_position + Vector2(x, 0.0F));
            button->set_size(Vector2(selector.size.x / 2.0F, selector_height));
            button->set_disabled(lan ==
                (impl_->join_selection.source() == model::LobbySourceKind::lan));
            dialog.frame->add_child(button);
            button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
            state.controls[id] = button;
        }
        rect.position.y += selector_height + gap;
        rect.size.y = std::max(0.0F, rect.size.y - selector_height - gap);
        const bool online = impl_->join_selection.source() == model::LobbySourceKind::online;
        const float row_height = selector_height;
        // Anchor the new bar to the authored action row, reserving three separate lines.
        const float second = anchor ? local_rect(dialog, *anchor).position.y - row_height - gap
                                    : rect.position.y + rect.size.y - row_height;
        const float top = second - row_height - gap;
        const float footer = online ? std::max(0.0F, rect.position.y + rect.size.y - top + gap) : 0.0F;
        const Rect2 list_rect(rect.position, Vector2(rect.size.x, std::max(0.0F, rect.size.y - footer)));
        state.game_list->set_position(list_rect.position);
        state.game_list->set_size(list_rect.size);
        dialog.frame->add_child(state.game_list);
        state.game_list->connect("row_chosen", callable_mp(this, &EawrLanLobby::on_rows_chosen));
        if (online) {
            const float width = rect.size.x;
            const auto add_button = [&](const char* id, const char* caption, const float x, const float y, const float w) {
                auto* button = memnew(EawrUiButton);
                button->set_name(id);
                button->set_text(caption);
                button->set_position(Vector2(rect.position.x + x * width, y));
                button->set_size(Vector2(w * width, row_height));
                dialog.frame->add_child(button);
                button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
                state.controls[id] = button;
            };
            auto* notice = memnew(EawrUiLabel);
            notice->set_name(online_notice_label);
            notice->set_position(list_rect.position + Vector2(8.0F, 40.0F));
            notice->set_size(Vector2(std::max(0.0F, list_rect.size.x - 16.0F), 48.0F));
            notice->set_wrap(true);
            notice->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            dialog.frame->add_child(notice);
            state.controls[online_notice_label] = notice;
            auto* code = memnew(EawrUiEdit);
            code->set_name(invitation_edit);
            code->set_placeholder("Room code");
            code->set_text(text(impl_->entered_code));
            code->set_position(Vector2(rect.position.x, top));
            code->set_size(Vector2(0.44F * width, row_height));
            code->set_max_length(128);
            dialog.frame->add_child(code);
            code->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String(invitation_edit)));
            state.controls[invitation_edit] = code;
            add_button(code_button, "Join code", 0.46F, top, 0.24F);
            add_button(cancel_join_button, "Cancel join", 0.72F, top, 0.28F);
            add_button(refresh_button, "Refresh", 0.0F, second, 0.2F);
            add_button(prev_button, "<", 0.22F, second, 0.1F);
            add_button(next_button, ">", 0.34F, second, 0.1F);
            auto* pages = memnew(EawrUiLabel);
            pages->set_name(page_label);
            pages->set_position(Vector2(rect.position.x, second + row_height + gap));
            pages->set_size(Vector2(width, row_height));
            pages->set_wrap(false);
            pages->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            dialog.frame->add_child(pages);
            state.controls[page_label] = pages;
        }
    }
    if (auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) {
        name->set_text(text(impl_->name));
        name->set_max_length(static_cast<int>(model::lan_name_max));
        name->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_LAN_NAME_EDIT")));
        name->connect("focus_exited", callable_mp(this, &EawrLanLobby::commit_name_from_edit));
    }
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) {
        chat->set_max_length(static_cast<int>(model::lan_chat_max));
        chat->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_LAN_CHAT_EDIT")));
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST")) list->set_select_mode(ItemList::SELECT_SINGLE);
    if (auto* colour = EAWR_CONTROL(EawrUiCombo, "IDC_LAN_COLOR_SELECT")) {
        colour->set_fit_to_longest_item(false);
        colour->set_clip_text(true);
        colour->set_retail_list(true);
        for (std::size_t index = 0; index < resources.palette.size(); ++index) {
            const auto& rgb = resources.palette[index].rgb;
            Ref<Image> swatch = Image::create_empty(12, 12, false, Image::FORMAT_RGBA8);
            swatch->fill(Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
            colour->add_icon_item(ImageTexture::create_from_image(swatch), text(colour_name(resources.palette[index].constant)),
                                  static_cast<int>(index));
        }
        const int count = colour->get_item_count();
        if (count > 0) colour->select(std::min<int>(static_cast<int>(impl_->colour), count - 1));
        colour->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_LAN_COLOR_SELECT")));
    }
    for (const BuiltGadget& gadget : dialog.gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    if (resources.dialogs->script.find(host_dialog) == nullptr) {
        if (auto* host = EAWR_CONTROL(EawrUiButton, "IDC_LAN_HOST_GAME_BUTTON")) {
            host->set_disabled(true);
            host->set_tooltip_text("The host options dialog is not in the loaded data.");
        }
    }
    if (resources.dialogs->script.find(filter_dialog) == nullptr) {
        if (auto* filter = EAWR_CONTROL(EawrUiButton, "IDC_SET_FILTER_BUTTON")) filter->set_disabled(true);
    }
    // Our extra: a typed address, for networks the broadcast does not cross.
    if (resources.dialogs->script.find(direct_dialog) != nullptr) {
        if (const BuiltGadget* filter = find_gadget(dialog, "IDC_SET_FILTER_BUTTON")) {
            const Rect2 set = local_rect(dialog, *filter);
            if (const BuiltGadget* host = find_gadget(dialog, "IDC_LAN_HOST_GAME_BUTTON")) {
                const Rect2 hosted = local_rect(dialog, *host);
                auto* direct = memnew(EawrUiButton);
                direct->set_name(direct_button);
                direct->set_text(resolved(resources.text, "TEXT_IP"));
                const float left = set.position.x + set.size.x + 8.0F;
                direct->set_position(Vector2(left, set.position.y));
                const float width = std::max(0.0F, std::min(hosted.position.x - 8.0F,
                    dialog.frame->get_size().x) - left);
                direct->set_size(Vector2(width, set.size.y));
                direct->set_visible(width > 0.0F);
                direct->set_tooltip_text(resolved(resources.text, "TEXT_IP_DIRECT_CONNECT"));
                dialog.frame->add_child(direct);
                direct->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(direct_button)));
                state.controls[direct_button] = direct;
            }
        }
    }
    if (impl_->join_selection.source() == model::LobbySourceKind::online) {
        // The online list keeps the sort headers; the type column shows the host. The filter and the
        // typed address are LAN features.
        for (const char* id : {"IDC_SET_FILTER_BUTTON", direct_button}) {
            if (auto* button = EAWR_CONTROL(EawrUiButton, id)) button->set_disabled(true);
        }
        if (auto* host_column = EAWR_CONTROL(EawrUiButton, "IDC_INTERNET_GAME_TYPE_BUTTON")) host_column->set_text("Host");
        if (auto* host = EAWR_CONTROL(EawrUiButton, "IDC_LAN_HOST_GAME_BUTTON")) {
            host->set_text("Host Online");
            host->set_disabled(!impl_->online_rooms || resources.dialogs->script.find(host_dialog) == nullptr);
            host->set_tooltip_text(impl_->online_rooms
                ? "Host a public room, or a private one with a password."
                : "Online play is not available in this build or service configuration.");
        }
        if (auto* join = EAWR_CONTROL(EawrUiButton, "IDC_LAN_JOIN_GAME_BUTTON")) join->set_disabled(!impl_->online_rooms);
        // Measure the loaded skin and captions before assigning the shared bar. Godot buttons
        // and edits can be taller/wider than their requested rectangles because of minimum sizes.
        if (auto* list = state.game_list) {
            const std::array<const char*, 7> ids{{refresh_button, prev_button, next_button, direct_button,
                "IDC_SET_FILTER_BUTTON", "IDC_LAN_HOST_GAME_BUTTON", "IDC_LAN_JOIN_GAME_BUTTON"}};
            const auto* anchor = find_gadget(dialog, "IDC_SET_FILTER_BUTTON");
            if (anchor) {
                const Rect2 authored = local_rect(dialog, *anchor);
                const float width = list->get_size().x;
                float height = authored.size.y;
                if (auto* code = EAWR_CONTROL(Control, invitation_edit))
                    height = std::max(height, code->get_combined_minimum_size().y);
                for (const char* id : ids) if (auto* button = EAWR_CONTROL(Control, id))
                    height = std::max(height, button->get_combined_minimum_size().y);
                const float gap = height / 6.0F;
                float count = 0;
                for (const char* id : ids) if (EAWR_CONTROL(Control, id)) ++count;
                const float available = width - gap * (count - 1);
                const auto minimum_width = [&]() {
                    float total = 0;
                    for (const char* id : ids) if (auto* button = EAWR_CONTROL(Control, id))
                        total += button->get_combined_minimum_size().x;
                    return total;
                };
                // The extra online actions share one row. Fit their captions uniformly if the
                // loaded font needs more room than the authored list provides.
                auto* sample = EAWR_CONTROL(Control, refresh_button);
                const KitTextStyle style = kit_text_style(*sample, "Push_Button");
                for (int size = style.size - 1; minimum_width() > available && size > 0; --size) {
                    for (const char* id : ids) if (auto* button = EAWR_CONTROL(Control, id)) {
                        button->add_theme_font_size_override("Push_Button", size);
                        button->add_theme_constant_override("Push_Button_cell_ascent", style.cell_ascent * size / style.size);
                        button->add_theme_constant_override("Push_Button_cell_descent", style.cell_descent * size / style.size);
                        button->update_minimum_size();
                    }
                }
                const float second = authored.position.y - height - gap;
                const float first = second - height - gap;
                float x = list->get_position().x;
                const float extra = std::max(0.0F, available - minimum_width()) / count;
                for (const char* id : ids) if (auto* button = EAWR_CONTROL(Control, id)) {
                    const float button_width = button->get_combined_minimum_size().x + extra;
                    button->set_position(Vector2(x, second));
                    button->set_size(Vector2(button_width, height));
                    x += button_width + gap;
                }
                const std::array<const char*, 3> code_ids{{invitation_edit, code_button, cancel_join_button}};
                float code_minimum = 0;
                for (const char* id : code_ids) if (auto* control = EAWR_CONTROL(Control, id))
                    code_minimum += control->get_combined_minimum_size().x;
                const float code_extra = std::max(0.0F, width - 2 * gap - code_minimum);
                x = list->get_position().x;
                for (const char* id : code_ids) if (auto* control = EAWR_CONTROL(Control, id)) {
                    const float share = std::string_view(id) == invitation_edit ? 0.6F : 0.2F;
                    const float control_width = control->get_combined_minimum_size().x + code_extra * share;
                    control->set_position(Vector2(x, first));
                    control->set_size(Vector2(control_width, height));
                    x += control_width + gap;
                }
                if (auto* pages = EAWR_CONTROL(Control, page_label)) {
                    pages->set_position(Vector2(list->get_position().x, second + height + gap));
                    pages->set_size(Vector2(width, height));
                }
                list->set_size(Vector2(width, std::max(0.0F, first - gap - list->get_position().y)));
            }
        }
    }
    // Respect measured header and tab heights at smaller viewports. Captions can make
    // a button wider than its resource rectangle, while the skin can make it taller.
    if (state.game_list) {
        float header_bottom = 0.0F;
        for (const auto& gadget : dialog.gadgets) {
            if (!gadget.control || !gadget.control->is_visible()
                || !gadget.id.starts_with("IDC_INTERNET_") || gadget.kind != "button") continue;
            const Rect2 authored = local_rect(dialog, gadget);
            fit_control_caption(*gadget.control, "Push_Button", Vector2(authored.size.x, gadget.control->get_size().y));
            gadget.control->set_size(Vector2(authored.size.x, gadget.control->get_size().y));
            header_bottom = std::max(header_bottom, gadget.control->get_position().y + gadget.control->get_size().y);
        }
        auto* lan = EAWR_CONTROL(Control, lan_source_button);
        auto* online_tab = EAWR_CONTROL(Control, online_source_button);
        if (lan && online_tab) {
            const float tab_height = std::max(lan->get_combined_minimum_size().y, online_tab->get_combined_minimum_size().y);
            const float gap = tab_height / 6.0F;
            const float tab_top = std::max(lan->get_position().y, header_bottom + gap);
            for (auto* tab : {lan, online_tab}) {
                tab->set_position(Vector2(tab->get_position().x, tab_top));
                tab->set_size(Vector2(tab->get_size().x, tab_height));
            }
            const float bottom = state.game_list->get_position().y + state.game_list->get_size().y;
            const float top = tab_top + tab_height + gap;
            state.game_list->set_position(Vector2(state.game_list->get_position().x, top));
            state.game_list->set_size(Vector2(state.game_list->get_size().x, std::max(0.0F, bottom - top)));
        }
    }
    // The native edit minimum can exceed its authored height. Keep the following
    // player heading and list below it, preserving the list's bottom anchor.
    if (auto* name = EAWR_CONTROL(Control, "IDC_LAN_NAME_EDIT")) {
        if (auto* players = EAWR_CONTROL(Control, "IDC_PLAYER_LIST")) {
            const float bottom = players->get_position().y + players->get_size().y;
            float top = players->get_position().y;
            for (const auto& gadget : dialog.gadgets) {
                if (gadget.kind != "label" || !gadget.control) continue;
                const Rect2 authored = local_rect(dialog, gadget);
                if (authored.position.y <= name->get_position().y || authored.position.y >= players->get_position().y) continue;
                const float gap = authored.size.y / 6.0F;
                const float heading_top = std::max(authored.position.y, name->get_position().y + name->get_size().y + gap);
                gadget.control->set_position(Vector2(authored.position.x, heading_top));
                top = std::max(top, heading_top + gadget.control->get_size().y + gap);
            }
            players->set_position(Vector2(players->get_position().x, top));
            players->set_size(Vector2(players->get_size().x, std::max(0.0F, bottom - top)));
        }
    }
    state.loading = false;
    // The player list shows who is in the lobby; the first line is the player.
    impl_->dirty = true;
    announce();
    refresh_lobby();
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) chat->grab_focus();
    if (impl_->name == "Player") {
        // Retail focuses the name edit while the profile still has the default name.
        if (auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) name->grab_focus();
    }
}

// Adds the lines the list has not shown yet. A key or a literal notice shows through the text
// database; a player's line shows as typed. Retail's chat list is not selectable.
void EawrLanLobby::refresh_lobby() {
    if (page_ != Page::lobby || !state_) return;
    Impl& impl = *impl_;
    Resources& resources = *resources_;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online) {
        const bool busy = impl.join_selection.attempt().has_value() || impl.prompt.has_value();
        if (auto* join = EAWR_CONTROL(EawrUiButton, "IDC_LAN_JOIN_GAME_BUTTON")) join->set_disabled(busy || !impl.online_rooms);
        if (auto* code = EAWR_CONTROL(EawrUiButton, code_button)) code->set_disabled(busy);
        if (auto* cancel = EAWR_CONTROL(EawrUiButton, cancel_join_button)) cancel->set_disabled(!impl.join_selection.attempt());
    }
    // The name edit is read as retail reads it: a changed text is the player's name.
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) {
        if (!edit->has_focus()) {
            const std::string typed = utf8(edit->get_text());
            if (typed != impl.name) edit->set_text(text(impl.name));
        }
    }
    impl.games.clear();
    impl.players.clear();
    if (impl.source) {
        impl.games = impl.source->games();
        impl.players = impl.source->players();
    }
    // Online ping stays unknown ("?"): the service round trip is not the game's.
    if (match_ && !online) apply_lan_lobby_ping(impl.games, match_->view());
    impl.shown = model::arrange_games(impl.games, impl.filter, impl.sort);
    if (online) {
        // One page of the filtered rooms is drawn; `shown` is that page, so a row index maps straight to it.
        impl.online_rooms_total = impl.shown.size();
        const model::OnlinePageView view = model::online_page(impl.shown.size(), impl.online_page);
        impl.online_page = view.page;
        impl.shown = std::vector<model::LobbyGame>(impl.shown.begin() + static_cast<std::ptrdiff_t>(view.first),
                                                   impl.shown.begin() + static_cast<std::ptrdiff_t>(view.first + view.count));
        impl.list_notice = impl.online_rooms
            ? std::string(model::online_list_notice(impl.online_rooms->last_error().has_value(), impl.online_rooms_total))
            : std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured));
        if (!impl.online_rooms && options_.online_settings) {
            if (const auto error = model::validate_online_settings(*options_.online_settings, true)) impl.list_notice = error->message;
        } else if (impl.online_rooms) {
            if (const auto error = impl.online_rooms->last_error(); error && (error->code == "EAWR-NET-1519" || error->code == "EAWR-NET-1528"))
                impl.list_notice = "Could not reach or trust the online service. Check Network Options.";
        }
        impl.chat_open = impl.online_rooms
            && (impl.online_rooms->menu_state() == model::OnlineMenuChat::active
                || std::any_of(impl.players.begin(), impl.players.end(),
                               [](const model::LobbyPlayer& player) { return player.place != model::LobbyPlace::lobby; }));
        impl.status = impl.list_notice;
        if (auto* notice = EAWR_CONTROL(EawrUiLabel, online_notice_label)) {
            notice->set_text(text(impl.list_notice));
            notice->set_visible(!impl.list_notice.empty());
        }
        if (auto* pages = EAWR_CONTROL(EawrUiLabel, page_label)) {
            pages->set_text(text("Page " + std::to_string(view.page + 1) + "/" + std::to_string(view.pages) + " - "
                                 + std::to_string(impl.online_rooms_total) + (impl.online_rooms_total == 1 ? " room" : " rooms")));
        }
        if (auto* prev = EAWR_CONTROL(EawrUiButton, prev_button)) prev->set_disabled(view.page == 0);
        if (auto* next = EAWR_CONTROL(EawrUiButton, next_button)) next->set_disabled(view.page + 1 >= view.pages);
        if (auto* refresh = EAWR_CONTROL(EawrUiButton, refresh_button)) refresh->set_disabled(!impl.online_rooms);
    }
    int selected = -1;
    std::vector<EawrLobbyGameList::Row> rows;
    for (std::size_t index = 0; index < impl.shown.size(); ++index) {
        const model::LobbyGame& game = impl.shown[index];
        if (game.name + "|" + game.address == impl.selected_key) selected = static_cast<int>(index);
        const model::LobbyRowText row = online ? model::online_row_text(game) : model::lobby_row_text(game);
        EawrLobbyGameList::Row drawn;
        drawn.locked = online && game.passworded;
        drawn.cells = {text(row.name), online ? text(row.type) : resolved(resources.text, row.type), text(row.players),
                       text(row.map), text(row.ping)};
        // Retail colours: password games yellow, another version dark orange, the rest cyan.
        drawn.colour = game.passworded ? Color(1.0F, 0.9F, 0.2F) : !game.version_matches ? Color(0.75F, 0.4F, 0.05F)
                                                                                         : Color(0.35F, 0.95F, 1.0F);
        rows.push_back(std::move(drawn));
    }
    if (state_->game_list != nullptr) state_->game_list->set_rows(std::move(rows), selected);

    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_PLAYER_LIST")) {
        list->clear();
        const auto add_player = [&](const std::string& name, const std::uint32_t colour) {
            const int index = list->add_item(text(name));
            if (colour < resources.palette.size()) {
                const auto& rgb = resources.palette[colour].rgb;
                list->set_item_custom_fg_color(index, Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
            }
        };
        add_player(impl.name, impl.colour);
        for (const model::LobbyPlayer& player : impl.players) {
            // Online lists the admitted members of the room the player is in, minus the player.
            if (online ? !same_ci(player.name, impl.name) : player.place == model::LobbyPlace::lobby)
                add_player(player.name, player.colour);
        }
    }
    // The chat list: only appended lines are added, so a read position is kept.
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST")) draw_chat(*list, impl.lobby_chat);
    impl.last_list_ms = now_ms();
    impl.dirty = false;
}

void EawrLanLobby::join_selected() {
    if (impl_->join_selection.source() != model::LobbySourceKind::lan) return;
    Impl& impl = *impl_;
    const model::LobbyGame* selected = nullptr;
    for (const model::LobbyGame& game : impl.shown) {
        if (game.name + "|" + game.address == impl.selected_key) selected = &game;
    }
    const model::JoinVerdict verdict = model::check_join(selected, impl.name);
    if (!verdict.ok()) {
        note_chat(model::ChatKind::error, verdict.key.empty() ? verdict.literal : verdict.key);
        refresh_lobby();
        return;
    }
    impl.direct_seats = selected->max_players;
    if (impl.direct_seats < 2 || impl.direct_seats > net::max_match_seats) {
        show_failure("Choose a hosted game with two to nine human seats."); return;
    }
    const auto attempt = impl.join_selection.begin(model::LanDiscoveryTarget{*selected});
    if (!attempt) return;
    std::vector<sim::tactical::PlayerId> owners(impl.direct_seats);
    std::iota(owners.begin(), owners.end(), 1U);
    match_->configure_seats(std::move(owners));
    configure_staging();
    match_->join(selected->address);
    open_staging(false, selected->name, selected->address);
}

void EawrLanLobby::join_address(const std::string& address, const std::string& game) {
    if (impl_->join_selection.source() != model::LobbySourceKind::lan) return;
    if (impl_->direct_seats < 2 || impl_->direct_seats > net::max_match_seats) {
        show_failure("Choose a hosted game with two to nine human seats."); return;
    }
    if (!impl_->join_selection.begin(model::LanAddressTarget{address})) return;
    std::vector<sim::tactical::PlayerId> owners(impl_->direct_seats);
    std::iota(owners.begin(), owners.end(), 1U);
    match_->configure_seats(std::move(owners));
    configure_staging();
    match_->join(address);
    open_staging(false, game, address);
}

void EawrLanLobby::cancel_join() {
    if (impl_->online_rooms && impl_->join_selection.attempt()
        && impl_->join_selection.attempt()->source == model::LobbySourceKind::online) {
        impl_->online_rooms->cancel(*impl_->join_selection.attempt());
    }
    impl_->join_selection.cancel();
}

void EawrLanLobby::refresh_online_rooms() {
    if (impl_->online_rooms) impl_->online_rooms->refresh();
    impl_->dirty = true;
}

std::shared_ptr<model::LobbyGameSource> EawrLanLobby::lan_source() {
    if (options_.source) return options_.source;
    if (!owned_lan_source_ && options_.lan_source_factory) {
        std::string failure;
        owned_lan_source_ = options_.lan_source_factory(failure);
        if (!owned_lan_source_ && !failure.empty()) note_chat(model::ChatKind::error, failure);
    }
    return owned_lan_source_;
}

void EawrLanLobby::select_source(const model::LobbySourceKind source) {
    if (page_ != Page::lobby || source == impl_->join_selection.source()) return;
    cancel_join();
    impl_->join_selection.select_source(source);
    if (impl_->online_rooms) impl_->online_rooms->set_menu_visible(source == model::LobbySourceKind::online);
    if (source == model::LobbySourceKind::online) {
        if (!options_.online_rooms && !impl_->online_rooms && options_.online_settings) {
            auto configured = online_match_options(*options_.online_settings);
            if (configured) impl_->online_rooms = make_live_online_source(configured.value());
        }
        static_cast<void>(configure_online_match());
    } else {
        if (match_ && match_ != lan_match_) match_->cancel();
        match_ = lan_match_;
    }
    // No old source rows, queued chat, selection or invitation survives a switch.
    if (impl_->source) static_cast<void>(impl_->source->take_chat());
    impl_->lobby_chat.clear();
    if (source == model::LobbySourceKind::online) owned_lan_source_.reset();
    impl_->source = source == model::LobbySourceKind::lan ? lan_source()
                                                          : std::shared_ptr<model::LobbyGameSource>(impl_->online_rooms);
    impl_->online_page = 0;
    impl_->prompt.reset();
    impl_->join_game.clear();
    impl_->join_room.clear();
    if (impl_->source) {
        // A freshly opened LAN source must announce this profile before its first pump.
        if (source == model::LobbySourceKind::lan) announce();
        // Drain arrivals accumulated while this source was inactive before drawing its new page.
        static_cast<void>(impl_->source->pump());
        static_cast<void>(impl_->source->take_chat());
    }
    impl_->games.clear();
    impl_->shown.clear();
    impl_->players.clear();
    impl_->selected_key.clear();
    impl_->entered_code.clear();
    impl_->last_message.clear();
    impl_->status.clear();
    open_lobby();
    if (source == model::LobbySourceKind::online && !impl_->online_rooms) {
        if (const auto error = model::validate_online_settings(*options_.online_settings, true)) {
            note_chat(model::ChatKind::error, error->message); impl_->list_notice = error->message; refresh_lobby();
        }
    }
}

bool EawrLanLobby::configure_online_match() {
    if (options_.match) { match_ = options_.match; return true; }
    if (online_match_) { match_ = online_match_; return true; }
    auto configured = online_match_options(*options_.online_settings);
    if (!configured) return false;
    // The host's match opens the signalling scope of the room this source published.
    if (impl_->online_rooms) bind_online_room_source(configured.value(), impl_->online_rooms);
    if (match_ && match_ != lan_match_) match_->cancel();
    match_ = make_online_match(std::move(configured).value(), options_.diagnostic_root, options_.input_delay);
    match_->set_diagnostic_logger([](std::string_view line) {
        UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
    });
    match_->configure_factions(scene::faction_order(*resources_->content->catalog));
    online_match_ = match_;
    return true;
}

// The room code field: a four-character code attaches the room's signalling scope straight away, the same way a picked
// room does after its directory admission.
void EawrLanLobby::join_code(const std::string& code) {
    if (page_ != Page::lobby || impl_->join_selection.source() != model::LobbySourceKind::online
        || impl_->join_selection.attempt() || impl_->prompt) return;
    impl_->entered_code = code;
    std::optional<std::string> canonical = model::normalize_short_code(code);
    if (!canonical) canonical = model::normalize_invitation_code(code);
    if (!canonical) {
        note_chat(model::ChatKind::error, "Enter four letters or digits.");
    } else if (!impl_->online_rooms || !configure_online_match()) {
        note_chat(model::ChatKind::error, std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
    } else if (impl_->join_selection.begin(model::OnlineInvitationTarget{"online", *canonical})) {
        note_chat(model::ChatKind::notice, "Finding game…");
        // Code admission learns its capacity from the attached room roster, before creating any pair transport.
        configure_staging();
        match_->join(*canonical);
        open_staging(false, {}, {});
        return;
    } else {
        note_chat(model::ChatKind::error, std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
    }
    refresh_lobby();
}

// A picked online room: a passworded one asks for its password first.
void EawrLanLobby::join_online_selected() {
    Impl& impl = *impl_;
    if (impl.join_selection.source() != model::LobbySourceKind::online || page_ != Page::lobby || impl.prompt
        || impl.join_selection.attempt()) return;
    if (!impl.online_rooms) {
        note_chat(model::ChatKind::error, std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
        refresh_lobby();
        return;
    }
    const model::LobbyGame* selected = nullptr;
    for (const model::LobbyGame& game : impl.shown) {
        if (game.name + "|" + game.address == impl.selected_key) selected = &game;
    }
    const model::JoinVerdict verdict = model::check_join(selected, impl.name, true);
    if (!verdict.ok()) {
        note_chat(model::ChatKind::error, verdict.key.empty() ? verdict.literal : verdict.key);
        refresh_lobby();
        return;
    }
    if (selected->passworded) open_password_prompt(*selected);
    else begin_online_join(selected->name, selected->address, {});
}

void EawrLanLobby::begin_online_join(const std::string& game, const std::string& room, const std::string& password) {
    Impl& impl = *impl_;
    const auto attempt = impl.join_selection.begin(model::OnlineInvitationTarget{"online", room});
    if (!attempt) {
        note_chat(model::ChatKind::error, std::string(model::online_failure_text(model::OnlineFailure::unavailable)));
        refresh_lobby();
        return;
    }
    impl.join_game = game;
    impl.join_room = room;
    impl.last_online_notice.clear();
    impl.online_rooms->begin_join(*attempt, room, password, impl.name);
    note_chat(model::ChatKind::notice, "Finding game…");
    refresh_lobby();
}

void EawrLanLobby::open_password_prompt(const model::LobbyGame& game) {
    Impl& impl = *impl_;
    if (!state_ || page_ != Page::lobby || state_->dialog->frame == nullptr) return;
    model::OnlinePasswordPrompt prompt;
    prompt.game = game.name;
    prompt.room = game.address;
    impl.prompt = prompt;
    PageState& state = *state_;
    Control* frame = state.dialog->frame;
    const Vector2 area = frame->get_size();
    const Vector2 size(std::min(380.0F, area.x - 40.0F), 150.0F);
    auto* panel = memnew(ColorRect);
    panel->set_name(prompt_panel);
    panel->set_color(Color(0.02F, 0.05F, 0.1F, 0.96F));
    panel->set_position(Vector2((area.x - size.x) / 2.0F, (area.y - size.y) / 2.0F));
    panel->set_size(size);
    frame->add_child(panel);
    state.controls[prompt_panel] = panel;
    const float inner = size.x - 24.0F;
    auto* title = memnew(EawrUiLabel);
    title->set_name(prompt_title_label);
    title->set_text(text("Password for " + game.name));
    title->set_position(Vector2(12.0F, 10.0F));
    title->set_size(Vector2(inner, 24.0F));
    title->set_wrap(false);
    title->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    panel->add_child(title);
    auto* edit = memnew(EawrUiEdit);
    edit->set_name(prompt_edit);
    edit->set_secret(true);
    edit->set_max_length(static_cast<int>(model::lan_password_max));
    edit->set_position(Vector2(12.0F, 40.0F));
    edit->set_size(Vector2(inner, 30.0F));
    panel->add_child(edit);
    edit->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String(prompt_edit)));
    state.controls[prompt_edit] = edit;
    auto* error = memnew(EawrUiLabel);
    error->set_name(prompt_error_label);
    error->set_position(Vector2(12.0F, 74.0F));
    error->set_size(Vector2(inner, 28.0F));
    error->set_wrap(true);
    error->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    panel->add_child(error);
    state.controls[prompt_error_label] = error;
    for (const auto& [id, caption, x] : std::array<std::tuple<const char*, const char*, float>, 2>{{
             {prompt_ok_button, "OK", 12.0F}, {prompt_cancel_button, "Cancel", 12.0F + inner / 2.0F + 4.0F}}}) {
        auto* button = memnew(EawrUiButton);
        button->set_name(id);
        button->set_text(caption);
        button->set_position(Vector2(x, size.y - 40.0F));
        button->set_size(Vector2(inner / 2.0F - 4.0F, 30.0F));
        panel->add_child(button);
        button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
        state.controls[id] = button;
    }
    edit->grab_focus();
    refresh_lobby();
}

void EawrLanLobby::close_password_prompt() {
    Impl& impl = *impl_;
    impl.prompt.reset();
    if (!state_) return;
    if (auto* panel = EAWR_CONTROL(Control, prompt_panel)) {
        panel->get_parent()->remove_child(panel);
        panel->queue_free();
    }
    for (const char* id : {prompt_panel, prompt_edit, prompt_title_label, prompt_error_label, prompt_ok_button,
                           prompt_cancel_button}) {
        state_->controls.erase(id);
    }
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) chat->grab_focus();
}

void EawrLanLobby::submit_password(const std::string& typed) {
    Impl& impl = *impl_;
    if (!impl.prompt || !impl.online_rooms) return;
    const std::string password = model::clamp_room_password(typed);
    if (!model::can_submit_password(*impl.prompt, password)) return;
    const std::string game = impl.prompt->game, room = impl.prompt->room;
    const auto attempt = impl.join_selection.begin(model::OnlineInvitationTarget{"online", room});
    if (!attempt) return;
    impl.prompt->waiting = true;
    impl.prompt->error.clear();
    if (auto* error = EAWR_CONTROL(EawrUiLabel, prompt_error_label)) error->set_text("");
    impl.join_game = game;
    impl.join_room = room;
    impl.last_online_notice.clear();
    // The password goes to the source and nowhere else: not to the chat, the report or a log.
    impl.online_rooms->begin_join(*attempt, room, password, impl.name);
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) edit->set_text("");
    note_chat(model::ChatKind::notice, "Finding game…");
}

// A refused service call: the curated text, never the diagnostic's own words.
void EawrLanLobby::online_failure(const core::Diagnostic& diagnostic) {
    const std::string message(model::online_failure_text(model::classify_online_failure(diagnostic.code)));
    if (message == impl_->last_online_notice) return;
    impl_->last_online_notice = message;
    note_chat(model::ChatKind::error, message);
}

// One frame of the online half: the join answers, the service notices and the hosting state.
void EawrLanLobby::pump_online() {
    Impl& impl = *impl_;
    if (!impl.online_rooms) return;
    for (const net::OnlineLobbyJoinCompletion& result : impl.online_rooms->take_join_results()) {
        if (!impl.join_selection.complete(result.attempt)) continue;
        if (result.admission) {
            // Admitted: the reserved seat and its member capability attach the room's signalling scope, then the shared staging.
            // This directory connection stays with the source, so the seat reservation outlives the attach.
            const std::string game = impl.join_game, room = impl.join_room;
            close_password_prompt();
            if (!configure_online_match() || (!options_.match && result.admission->member_capability.empty())) {
                impl.online_rooms->cancel(result.attempt);
                core::Diagnostic error; error.code = "EAWR-NET-1752"; error.message = "The game is no longer available.";
                online_failure(error);
                continue;
            }
            // The admission reserves a room slot; its roster supplies the authoritative two-to-nine seat count.
            configure_staging();
            match_->join_admitted(*result.admission);
            open_staging(false, game, {});
            return;
        }
        const model::OnlineFailure failure = result.refusal ? model::classify_online_failure(result.refusal->code)
                                                            : model::OnlineFailure::other;
        const std::string message(model::online_failure_text(failure));
        if (impl.prompt && model::online_failure_keeps_prompt(failure)) {
            impl.prompt->waiting = false;
            impl.prompt->error = message;
            if (auto* error = EAWR_CONTROL(EawrUiLabel, prompt_error_label)) error->set_text(text(message));
            if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) edit->grab_focus();
        } else if (impl.prompt) {
            close_password_prompt();
        }
        // The room and the name stay as they were; only the attempt is over.
        note_chat(model::ChatKind::error, message);
    }
    for (const core::Diagnostic& notice : impl.online_rooms->take_notices()) {
        // A join refusal was shown above, the room list failure shows in the list's own line; the
        // rest (publishing, room membership) shows here.
        const model::OnlineFailure failure = model::classify_online_failure(notice.code);
        if (failure == model::OnlineFailure::wrong_password || failure == model::OnlineFailure::cooldown
            || failure == model::OnlineFailure::full || failure == model::OnlineFailure::unavailable
            || failure == model::OnlineFailure::duplicate_name) continue;
        // On the lobby page the room list has its own line; chat refusals and removal still show.
        if (page_ == Page::lobby && failure != model::OnlineFailure::busy && failure != model::OnlineFailure::removed) continue;
        online_failure(notice);
    }
    // A service that predates the room-less chat (#3758) gets one neutral line in the chat window, then nothing more.
    const model::OnlineMenuChat menu = impl.online_rooms->menu_state();
    if (menu == model::OnlineMenuChat::unavailable && impl.menu_chat != menu && page_ == Page::lobby)
        note_chat(model::ChatKind::notice, std::string(model::online_menu_chat_notice(menu)));
    if (menu != model::OnlineMenuChat::unavailable || page_ == Page::lobby) impl.menu_chat = menu;
    const net::OnlineHostingState state = impl.online_rooms->hosting_state();
    const model::OnlineHostingPhase phase = state == net::OnlineHostingState::publishing ? model::OnlineHostingPhase::publishing
        : state == net::OnlineHostingState::hosted ? model::OnlineHostingPhase::hosted
        : state == net::OnlineHostingState::retrying ? model::OnlineHostingPhase::retrying : model::OnlineHostingPhase::idle;
    if (phase != impl.hosting) {
        impl.hosting = phase;
        if (page_ == Page::staging && impl.host && impl.join_selection.source() == model::LobbySourceKind::online) {
            const std::string_view line = model::online_hosting_notice(phase);
            if (!line.empty()) {
                note_chat(phase == model::OnlineHostingPhase::retrying ? model::ChatKind::error : model::ChatKind::notice,
                          std::string(line));
            }
        }
    }
}

// ---- Host options -----------------------------------------------------------------------------

void EawrLanLobby::on_rows_chosen(const int64_t index, const bool activated) {
    if (index < 0 || static_cast<std::size_t>(index) >= impl_->shown.size()) return;
    const model::LobbyGame& game = impl_->shown[static_cast<std::size_t>(index)];
    impl_->selected_key = game.name + "|" + game.address;
    if (activated) {
        if (impl_->join_selection.source() == model::LobbySourceKind::online) join_online_selected();
        else join_selected();
    }
}

} // namespace eawr::presentation::godot_backend
