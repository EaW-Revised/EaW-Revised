#include "lan_lobby_screen_internal.hpp"

namespace eawr::presentation::godot_backend {
EawrLanLobby::EawrLanLobby() {
    set_layer(screen_layer);
    set_visible(false);
}

EawrLanLobby::~EawrLanLobby() = default;

void EawrLanLobby::_bind_methods() {
    ADD_SIGNAL(MethodInfo("closed"));
    ADD_SIGNAL(MethodInfo("started"));
    ADD_SIGNAL(MethodInfo("profile_changed", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::INT, "colour")));
    ADD_SIGNAL(MethodInfo("sound_requested", PropertyInfo(Variant::STRING, "event")));
}

void register_lan_lobby_classes() {
    GDREGISTER_CLASS(EawrLobbyGameList);
    GDREGISTER_CLASS(EawrLanLobby);
}

// ---- Loading ----------------------------------------------------------------------------------

bool EawrLanLobby::build(Options options, const skirmish::SetupMapCache* shared_maps) {
    options_ = std::move(options);
    resources_ = std::make_unique<Resources>();
    impl_ = std::make_unique<Impl>();
    Resources& resources = *resources_;
    impl_->name = model::clamp_player_name(options_.player_name);
    if (impl_->name.empty()) impl_->name = "Player";
    impl_->colour = options_.player_colour;
    impl_->lobby_chat = model::ChatLog();
    impl_->online_rooms = options_.online_rooms;
    if (!options_.online_settings) {
        if (const auto* saved = ActiveSettings::get()) options_.online_settings = saved->network;
        else options_.online_settings = SettingsStore{}.load().settings.network;
    }

    resources.content = options_.content;
    if (!resources.content) {
        if (options_.game_root.empty()) {
            failure_ = "the LAN screens need the game root or the content a menu loaded";
            return false;
        }
        const std::filesystem::path base = options_.game_root / "GameData" / "Data";
        const std::filesystem::path expansion = options_.game_root / "corruption" / "Data";
        if (!std::filesystem::is_directory(base) || !std::filesystem::is_directory(expansion)) {
            failure_ = "the LAN screens need an installation with GameData/Data and corruption/Data";
            return false;
        }
        const std::string profile = options_.profile.empty() ? (options_.mod_root.empty() ? "foc" : "remake")
                                                             : options_.profile;
        if (profile != "foc" && profile != "remake") {
            failure_ = "the LAN screens need the FoC or mod profile";
            return false;
        }
        std::vector<vfs::LayerRoot> roots;
        if (profile == "remake") {
            if (options_.mod_root.empty()) {
                failure_ = "the mod profile needs a mod root";
                return false;
            }
            roots = vfs::mod_chain_roots(options_.mod_root);
        }
        roots.emplace_back("expansion", expansion);
        roots.emplace_back("base", base);
        auto chain = vfs::resolve_manifest_chain(roots);
        if (!chain) {
            failure_ = core::format_diagnostic(chain.error());
            return false;
        }
        std::vector<vfs::MountSpec> specs;
        std::vector<std::string> layers;
        for (auto& manifest : chain.value()) {
            layers.push_back(manifest.mount.layer_id);
            specs.push_back(std::move(manifest.mount));
        }
        auto mounted = vfs::Vfs::mount(specs);
        if (!mounted) {
            failure_ = core::format_diagnostic(mounted.error());
            return false;
        }
        auto filesystem = std::make_shared<const vfs::Vfs>(std::move(mounted).value());
        auto objects = eawr::data::load_catalog_strict(*filesystem, profile == "foc" ? eawr::data::Profile::foc
                                                                               : eawr::data::Profile::remake);
        if (!objects) {
            failure_ = core::format_diagnostic(objects.error());
            return false;
        }
        auto catalog = std::make_shared<const eawr::data::Catalog>(std::move(objects.value().catalog));
        resources.content = std::make_shared<const BattleContent>(
            BattleContent{std::move(filesystem), std::move(catalog), profile, std::move(layers)});
    }
    const vfs::Vfs& filesystem = *resources.content->filesystem;

    auto colours = skirmish::read_lobby_colours(filesystem);
    if (!colours) {
        failure_ = colours.error().message;
        return false;
    }
    resources.palette = std::move(colours).value();
    auto defaults = skirmish::read_match_defaults(filesystem);
    if (!defaults) {
        failure_ = defaults.error().message;
        return false;
    }
    resources.defaults = std::move(defaults).value();
    if (auto bounds = skirmish::read_credits_bounds(filesystem)) resources.credits_bounds = bounds.value();
    skirmish::SetupMapQuery query;
    query.custom.reset();
    const auto* shared = shared_maps ? shared_maps->find(filesystem, *resources.content->catalog, query) : nullptr;
    if (shared) resources.maps = *shared;
    else {
        auto maps = skirmish::setup_maps(filesystem, *resources.content->catalog, query);
        if (!maps) {
            failure_ = maps.error().message;
            return false;
        }
        resources.maps = std::move(maps).value();
    }

    auto dialogs = data::load_dialog_catalog(filesystem);
    if (!dialogs) {
        failure_ = core::format_diagnostic(dialogs.error());
        return false;
    }
    resources.dialogs.emplace(std::move(dialogs.value()));
    // A mod that removes a dialog removes its entry, with one note, never a crash.
    for (const char* required : {lobby_dialog, staging_dialog}) {
        if (resources.dialogs->script.find(required) == nullptr) {
            failure_ = std::string("the data has no dialog ") + required;
            return false;
        }
    }
    for (const char* optional : {host_dialog, filter_dialog, direct_dialog}) {
        if (resources.dialogs->script.find(optional) == nullptr) {
            notes_.push_back(std::string("dialog ") + optional + " is not in the data; its entry is disabled");
        }
    }
    auto atlas = assets::load_mega_texture_atlas(filesystem,
                                                 "data/art/textures/" + resources.dialogs->skin.texture_file + ".mtd");
    if (!atlas) {
        failure_ = core::format_diagnostic(atlas.error());
        return false;
    }
    resources.atlas.emplace(std::move(atlas.value()));
    resources.textures = std::make_unique<UiTextures>(*resources.atlas, filesystem);
    if (!resources.textures->page_failure().empty()) {
        failure_ = "atlas page: " + resources.textures->page_failure();
        return false;
    }
    if (auto database = data::load_language_text_database(filesystem, options_.language)) {
        resources.text_database.emplace(std::move(database.value()));
        resources.text.emplace(*resources.text_database);
    } else {
        notes_.push_back("text database: " + core::format_diagnostic(database.error()) + "; captions show their keys");
    }
    const std::filesystem::path cache = default_font_cache(options_.font_cache);
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = cache,
                               .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    if (font_mount) resources.font_filesystem.emplace(std::move(font_mount.value()));
    const vfs::Vfs unmounted;
    resources.fonts = std::make_unique<FontProvider>(model::load_font_cache(
        resources.font_filesystem ? *resources.font_filesystem : unmounted, ViewerPath::utf8(cache)));

    for (const eawr::data::Definition& definition : resources.content->catalog->definitions()) {
        if (definition.category == eawr::data::Category::sfx && definition.active) {
            resources.sfx_ids.push_back(definition.id);
        }
    }

    match_ = options_.match;
    if (match_) match_->configure_factions(scene::faction_order(*resources_->content->catalog));
    if (!match_) {
        const std::filesystem::path diagnostics = options_.diagnostic_root.empty()
            ? ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "lan-desync"
            : options_.diagnostic_root;
        match_ = make_lan_match(diagnostics, options_.input_delay);
        match_->configure_factions(scene::faction_order(*resources_->content->catalog));
    }
    built_ = true;
    lan_match_ = match_;
    match_->set_diagnostic_logger([](std::string_view line) {
        UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
    });
    return true;
}

// ---- Showing and closing ----------------------------------------------------------------------

void EawrLanLobby::show(Node& parent, std::optional<model::LobbySourceKind> initial_source) {
    if (!built_ || shown_) return;
    parent.add_child(this);
    set_visible(true);
    shown_ = true;
    set_process(true);
    set_process_input(true);

    impl_->source = impl_->join_selection.source() == model::LobbySourceKind::lan
                    && initial_source != model::LobbySourceKind::online ? lan_source()
                  : std::shared_ptr<model::LobbyGameSource>(impl_->online_rooms);
    open_lobby();
    if (impl_->online_rooms) impl_->online_rooms->set_menu_visible(impl_->join_selection.source() == model::LobbySourceKind::online);
    if (initial_source) select_source(*initial_source);
}

void EawrLanLobby::clear_page() {
    if (!state_) return;
    if (state_->root != nullptr) {
        remove_child(state_->root);
        state_->root->queue_free();
    }
    state_.reset();
}

void EawrLanLobby::close_screen(const bool emit) {
    if (!shown_) return;
    cancel_join();
    if (impl_->online_rooms) impl_->online_rooms->set_menu_visible(false);
    shown_ = false;
    set_process(false);
    set_process_input(false);
    clear_page();
    impl_->source.reset();
    owned_lan_source_.reset();
    set_visible(false);
    if (Node* parent = get_parent()) parent->remove_child(this);
    if (emit) emit_signal("closed");
}

void EawrLanLobby::dismiss() { close_screen(false); }

std::optional<skirmish::FixtureOptions> EawrLanLobby::take_start() { return std::exchange(impl_->launch, std::nullopt); }

// ---- Page building ----------------------------------------------------------------------------

void EawrLanLobby::build_page(const char* dialog_name) {
    clear_page();
    Resources& resources = *resources_;
    state_ = std::make_unique<PageState>();
    PageState& state = *state_;
    const Vector2 size(lobby_width, lobby_height);
    if (resources.theme.is_null() || resources.theme_size != size) {
        model::ThemeSources sources;
        sources.catalog = &*resources.dialogs;
        sources.atlas = &resources.atlas->directory;
        sources.standalone = model::vfs_standalone_textures(*resources.content->filesystem);
        sources.fonts = &resources.fonts->cache();
        const std::vector<std::string>& families = resources.fonts->system_families();
        sources.system = [&families](const std::string_view face) {
            return model::match_system_face(face, families).has_value();
        };
        resources.theme_model.emplace(model::build_theme_model(
            sources, model::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)},
                                            options_.rules)));
        resources.theme = build_theme(*resources.theme_model, *resources.textures, *resources.fonts);
        resources.theme_size = size;
    }
    auto* root = memnew(EawrUiModalLayer);
    root->set_name("EawrLanLobby");
    root->set_position(Vector2());
    root->set_size(size);
    root->set_theme(resources.theme);
    add_child(root);
    state.root = root;
    // The lobby and the staging are full-screen opaque dialogs: nothing of the world shows behind.
    auto* backdrop = memnew(ColorRect);
    backdrop->set_color(Color(0.0F, 0.0F, 0.0F, 1.0F));
    backdrop->set_size(size);
    backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root->add_child(backdrop);
    state.backdrop = backdrop;
    state.canvas = memnew(Control);
    state.canvas->set_size(size);
    state.canvas->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    state.canvas->set_oversampling_with_scale(CanvasItem::OVERSAMPLING_WITH_SCALE_ENABLED);
    root->add_child(state.canvas);

    const data::Dialog* dialog = resources.dialogs->script.find(dialog_name);
    state.dialog = build_dialog(*dialog, *resources.theme_model, resources.text ? &*resources.text : nullptr,
                                model::Placement::centre, std::string_view(dialog_name) == staging_dialog);
    state.canvas->add_child(state.dialog->frame);
    layout_page();
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.control != nullptr) state.controls[gadget.id] = gadget.control;
    }
}

void EawrLanLobby::layout_page() {
    if (!state_ || !state_->canvas) return;
    const Vector2 size = get_viewport()->get_visible_rect().size;
    if (size.x <= 0.0F || size.y <= 0.0F) return;
    state_->root->set_size(size);
    state_->backdrop->set_size(size);
    const auto layout = fit_letterbox_canvas(*state_->canvas, size, Vector2(lobby_width, lobby_height));
    if (impl_->box) {
        impl_->box->set_scale(Vector2(layout.scale, layout.scale));
        impl_->box->set_offset(layout.offset);
    }
}

void EawrLanLobby::on_pressed(String id) { handle_button(utf8(id)); }

void EawrLanLobby::on_text_submitted(String typed, String id) {
    const std::string gadget = utf8(id);
    const std::string value = utf8(typed);
    if (gadget == "IDC_LAN_NAME_EDIT") commit_name(value);
    else if (gadget.starts_with("EAWR_STAGING_NAME_")) static_cast<void>(run_action("name:" + value));
    else if (gadget == "IDC_LAN_CHAT_EDIT") send_chat(value);
    else if (gadget == "IDC_INTERNET_GAME_NAME_EDIT" || gadget == "IDC_INTERNET_GAME_PASSWORD_EDIT") handle_button("IDC_INTERNET_CREATE_GAME_BUTTON");
    else if (gadget == "IDC_EDIT_IP") handle_button("IDC_ADD_IP_BUTTON");
    else if (gadget == invitation_edit) join_code(value);
    else if (gadget == prompt_edit) submit_password(value);
}

void EawrLanLobby::on_box_result(const int64_t) {
    impl_->box = nullptr;
    // A refused or dropped connection leaves the staging for the lobby once the player read it.
    if (impl_->refused) {
        impl_->refused = false;
        leave_staging();
    }
}

// ---- Lobby ------------------------------------------------------------------------------------

// Fit captions to a bounded authored row without changing the loaded skin.

void EawrLanLobby::on_item_selected(const int64_t index, String id) {
    if (state_ && state_->loading) return;
    const std::string gadget = utf8(id);
    if (gadget == "IDC_INTERNET_HOST_CLAN_COMBO") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
            const auto count = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
            fill_host_seats(count);
        }
        return;
    }
    if (gadget == "EAWR_LAN_DIRECT_SEATS") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget))
            impl_->direct_seats = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        return;
    }
    if (gadget == "IDC_LAN_COLOR_SELECT") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_LAN_COLOR_SELECT")) {
            commit_colour(static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index))));
        }
        return;
    }
    if (gadget == "IDC_GAME_TYPE_LIST") {
        if (index >= 0 && static_cast<std::size_t>(index) < std::size(game_types)) {
            impl_->host_options.game_type = game_types[index].name;
            if (auto* description = EAWR_CONTROL(EawrUiLabel, "IDC_GAME_TYPE_DESCRIPTION_TEXT")) {
                description->set_text(resolved(resources_->text, game_types[index].description));
            }
        }
        return;
    }
    if (page_ != Page::staging) return;
    if (!impl_->host && match_->lobby_state()) {
        const auto room = *match_->lobby_state();
        auto local = std::find_if(room.snapshot.seats.begin(), room.snapshot.seats.end(), [&](const auto& seat) { return seat.peer == match_->view().seat; });
        if (local == room.snapshot.seats.end()) return;
        const auto suffix = std::to_string(local->selection.slot - 1);
        auto choice = local->selection;
        auto* combo = EAWR_CONTROL(EawrUiCombo, gadget); if (!combo) return;
        if (gadget == "IDC_PLAYER_FACTION_" + suffix) choice.faction = utf8(combo->get_item_text(static_cast<int>(index)));
        else if (gadget == "IDC_PLAYER_COLOR_" + suffix) choice.colour = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        else if (gadget == "IDC_PLAYER_TEAM_" + suffix) choice.team = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        else if (gadget == "IDC_PLAYER_NAME_" + suffix) choice.slot = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        else return;
        (void)match_->lobby_select(std::move(choice)); return;
    }
    if (!impl_->host) return;
    Impl& impl = *impl_;
    Resources& resources = *resources_;
    if (gadget == "IDC_STAGING_MAP_LIST") {
        if (index >= 0 && static_cast<std::size_t>(index) < impl.listed.size()) {
            const auto chosen = impl.listed[static_cast<std::size_t>(index)];
            if (network_row_count(resources.maps[chosen]) < impl.human_owners.size()) {
                note_chat(model::ChatKind::error, "Choose a map with a player row for every human.");
                if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST")) {
                    const auto previous = std::find(impl.listed.begin(), impl.listed.end(), static_cast<std::size_t>(impl.map_index));
                    if (previous != impl.listed.end()) list->select(static_cast<int>(previous - impl.listed.begin()));
                }
                return;
            }
            impl.map_index = static_cast<int>(chosen);
            skirmish::repair_setup_selection(impl.selection, resources.maps[static_cast<std::size_t>(impl.map_index)], false);
            prepare_host_rows(resources.maps[static_cast<std::size_t>(impl.map_index)]);
            impl.rows_signature.clear();
            fill_host_seats();
            static_cast<void>(publish_staging_selection());
            announce();
        }
        return;
    }
    if (impl.map_index < 0) return;
    const skirmish::SetupMap& map = resources.maps[static_cast<std::size_t>(impl.map_index)];
    // The row gadgets are IDC_PLAYER_<FIELD>_<row>.
    const auto split = gadget.rfind('_');
    if (split == std::string::npos) return;
    const std::string field = gadget.substr(0, split);
    std::uint32_t row{};
    const auto parsed = std::from_chars(gadget.data() + split + 1, gadget.data() + gadget.size(), row);
    if (parsed.ec != std::errc{}) return;
    const std::uint32_t slot = row + 1;
    if (slot > network_row_count(map)) {
        note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
            "This player row is outside the map's loaded capacity.")));
        impl.rows_signature.clear();
        return;
    }
    auto* combo = EAWR_CONTROL(EawrUiCombo, gadget);
    if (combo == nullptr) return;
    const int id_value = combo->get_item_id(static_cast<int>(index));
    auto record = std::find_if(impl.selection.slots.begin(), impl.selection.slots.end(),
                               [slot](const skirmish::LobbySlot& value) { return value.slot == slot; });
    if (field == "IDC_PLAYER_NAME") {
        if (id_value >= 10000 && match_->lobby_state()) { (void)match_->lobby_kick(static_cast<net::PeerIndex>(id_value - 10000)); return; }
        // #4041: a row held by a human keeps its control for everyone; only the kick item (above) removes the player.
        if (const auto lobby = match_->lobby_state(); lobby && std::any_of(lobby->snapshot.seats.begin(), lobby->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == slot; })) {
            note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
                "A slot held by a human cannot be switched to AI, Closed or Open; kick the player first.")));
            impl.rows_signature.clear();
            return;
        }
        if (slot < 3) return;
        if (id_value == 0) {
            if (record != impl.selection.slots.end()) impl.selection.slots.erase(record);
        } else if (id_value == 3 && record == impl.selection.slots.end()) {
            if (auto added = append_lan_ai_row(impl.selection, map, slot, resources.palette.size()); !added) {
                note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice, added.error().message)));
                impl.rows_signature.clear();
                return;
            }
        }
    } else if (record != impl.selection.slots.end()) {
        if (field == "IDC_PLAYER_FACTION") {
            record->faction = utf8(combo->get_item_text(static_cast<int>(index)));
        } else if (field == "IDC_PLAYER_TEAM") {
            record->team = static_cast<std::uint32_t>(id_value);
            // Keep a faction the new team can start with.
            const auto side = std::find_if(map.teams.begin(), map.teams.end(),
                                           [&](const skirmish::SetupTeam& t) { return t.team == record->team; });
            if (side != map.teams.end() && !side->factions.empty()
                && std::find(side->factions.begin(), side->factions.end(), record->faction) == side->factions.end()) {
                record->faction = side->factions.front();
            }
        } else if (field == "IDC_PLAYER_COLOR") {
            static_cast<void>(skirmish::select_setup_colour(impl.selection, slot, static_cast<std::uint32_t>(id_value),
                                                            resources.palette.size()));
        }
    }
    static_cast<void>(publish_staging_selection());
    impl.rows_signature.clear();
}

void EawrLanLobby::_process(const double) {
    if (!shown_ || !impl_) return;
    layout_page();
    Impl& impl = *impl_;
    bool changed = false;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online) {
        pump_online();
    }
    if (impl.source) {
        changed = impl.source->pump() || changed;
        for (const model::LobbyChatMessage& chat : impl.source->take_chat()) {
            // The online window shows the service's room chat; the LAN one shows lobby chat.
            const bool for_lobby = page_ != Page::staging
                && (chat.scope == model::LobbyChatScope::lobby || (online && chat.scope == model::LobbyChatScope::game));
            const bool for_game =
                chat.scope == model::LobbyChatScope::game && page_ == Page::staging && chat.game == impl.game
                && (online || !match_->lobby_enabled());
            if (!for_lobby && !for_game) continue;
            note_chat(model::ChatKind::normal, chat.name + ": " + chat.text, chat.name);
            play_sound("GUI_Common_Chat_Receive");
            changed = true;
        }
    }
    if (page_ == Page::lobby) {
        // Retail rebuilds the list once a second.
        if (changed || impl.dirty || now_ms() - impl.last_list_ms >= model::lan_game_list_refresh_ms) refresh_lobby();
    } else if (page_ == Page::staging) {
        process_options();
        refresh_staging();
    }
}

void EawrLanLobby::_input(const Ref<InputEvent>& event) {
    if (!shown_ || impl_->box != nullptr) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (key == nullptr || !key->is_pressed() || key->is_echo() || key->get_keycode() != KEY_ESCAPE) return;
    if (Viewport* viewport = get_viewport()) viewport->set_input_as_handled();
    if (page_ == Page::staging) {
        // Escape cancels the options copy, except the press that closes the win-condition list.
        if (state_->editing) {
            if (!state_->opt_win_list_was_open) close_options(false);
            return;
        }
        if (auto* list = EAWR_CONTROL(EawrUiList, "EAWR_ONLINE_CONNECTIONS_LIST"); list && list->is_visible()) {
            list->hide(); return;
        }
    }
    switch (page_) {
    case Page::lobby: handle_button(impl_->prompt ? prompt_cancel_button : "IDC_LAN_CANCEL_BUTTON"); break;
    case Page::staging: leave_staging(); break;
    case Page::host_options:
    case Page::direct_connect:
    case Page::filter: open_lobby(); break;
    }
}

void EawrLanLobby::show_failure(const std::string& message) {
    Impl& impl = *impl_;
    if (impl.box != nullptr) return;
    model::MessageBoxRequest request;
    request.caption = "TEXT_LAN_LOBBY_TITLE";
    request.lines[0] = message;
    request.buttons.push_back({"TEXT_BUTTON_OK", 1, true});
    Resources& resources = *resources_;
    const MessageBoxContext context{&*resources.dialogs, &*resources.theme_model,
                                    resources.text ? &*resources.text : nullptr, resources.theme, resources.theme_size};
    auto* box = memnew(EawrMessageBox);
    if (!box->open(*this, context, request)) {
        notes_.push_back("message box: " + box->failure());
        memdelete(box);
        return;
    }
    impl.box = box;
    layout_page();
    box->connect("result_chosen", callable_mp(this, &EawrLanLobby::on_box_result));
}

void EawrLanLobby::handle_button(const std::string& id) {
    if (impl_->box != nullptr) return;
    if (id == "EAWR_ONLINE_CONNECTIONS" && page_ == Page::staging) {
        if (auto* list = EAWR_CONTROL(EawrUiList, "EAWR_ONLINE_CONNECTIONS_LIST")) list->set_visible(!list->is_visible());
        return;
    }
    if (page_ == Page::staging && impl_->host && state_->options_panel != nullptr) {
        if (id == "IDC_SELECT_OPTIONS") { open_options(); return; }
        if (state_->editing) {
            if (id == "EAWR_OPT_ACCEPT") close_options(true);
            else if (id == "EAWR_OPT_CANCEL") close_options(false);
            else if (id == "EAWR_OPT_DEFAULTS") {
                skirmish::reset_setup_options(*state_->editing, resources_->defaults);
                show_options_values();
            }
            return; // the dialog is modal: nothing behind it acts
        }
    }
    Impl& impl = *impl_;
    if (impl.box != nullptr) return;
    // The password prompt is modal: only its own buttons act while it is up.
    if (impl.prompt && id != prompt_ok_button && id != prompt_cancel_button) return;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (id == prompt_ok_button) {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) submit_password(utf8(edit->get_text()));
    } else if (id == prompt_cancel_button) {
        cancel_join();
        close_password_prompt();
        note_chat(model::ChatKind::notice, "Join cancelled.");
        refresh_lobby();
    } else if (id == code_button) {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, invitation_edit)) join_code(utf8(edit->get_text()));
    } else if (id == refresh_button) {
        refresh_online_rooms();
    } else if (id == prev_button || id == next_button) {
        if (id == prev_button && impl.online_page > 0) --impl.online_page;
        if (id == next_button) ++impl.online_page; // refresh_lobby clamps it to the last page
        refresh_lobby();
    } else if (id == lan_source_button || id == online_source_button) {
        select_source(id == lan_source_button ? model::LobbySourceKind::lan : model::LobbySourceKind::online);
    } else if (id == cancel_join_button) {
        cancel_join();
        note_chat(model::ChatKind::notice, "Join cancelled.");
        refresh_lobby();
    } else if (id == "IDC_LAN_CANCEL_BUTTON") {
        if (page_ == Page::staging) {
            leave_staging();
            return;
        }
        // Retail shuts the LAN session down and returns to the main menu.
        if (match_) match_->cancel();
        close_screen(true);
    } else if (id == "IDC_LAN_HOST_GAME_BUTTON") {
        if (!online || impl.online_rooms) open_host_options();
    } else if (id == "IDC_LAN_JOIN_GAME_BUTTON") {
        if (online) join_online_selected();
        else join_selected();
    } else if (id == "IDC_SET_FILTER_BUTTON") {
        if (impl.join_selection.source() == model::LobbySourceKind::lan) open_filter();
    } else if (id == direct_button) {
        if (impl.join_selection.source() == model::LobbySourceKind::lan) open_direct_connect();
    } else if (id == "IDC_INTERNET_SERVER_NAME_BUTTON") {
        impl.sort = model::LobbySort::game_name;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_GAME_TYPE_BUTTON") {
        impl.sort = online ? model::LobbySort::host : model::LobbySort::game_type;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_PLAYERS_BUTTON") {
        impl.sort = model::LobbySort::players;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_MAP_NAME_BUTTON") {
        impl.sort = model::LobbySort::map_name;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_PING_BUTTON") {
        impl.sort = model::LobbySort::ping;
        impl.dirty = true;
    } else if (id == "IDC_CANCEL_BUTTON") {
        // The host options, the filter and the direct-connect pages all return to the lobby.
        open_lobby();
    } else if (id == "IDC_INTERNET_CREATE_GAME_BUTTON") {
        begin_hosting();
    } else if (id == "IDC_ADD_IP_BUTTON") {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) {
            const std::string typed = utf8(edit->get_text());
            if (!typed.empty() && std::find(impl.direct_addresses.begin(), impl.direct_addresses.end(), typed) == impl.direct_addresses.end()) {
                impl.direct_addresses.push_back(typed);
                if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_IP_LIST")) list->add_item(text(typed));
            }
        }
    } else if (id == "IDC_IP_CONNECT_BUTTON") {
        std::string address;
        if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_IP_LIST")) {
            const PackedInt32Array chosen = list->get_selected_items();
            if (chosen.size() > 0) address = utf8(list->get_item_text(chosen[0]));
        }
        if (address.empty()) {
            if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) address = utf8(edit->get_text());
        }
        if (!address.empty()) join_address(address, "");
    } else if (id == "IDC_LOAD_GAME" && page_ == Page::staging && impl.host && match_->lobby_state()) {
        const auto state = *match_->lobby_state(); (void)match_->lobby_ready(!state.local_ready);
    } else if (id.starts_with("IDC_BUTTON_KICK_PLAYER") && page_ == Page::staging && impl.host) {
        std::uint32_t row{};
        const auto suffix = id.substr(std::string("IDC_BUTTON_KICK_PLAYER").size());
        if (std::from_chars(suffix.data(), suffix.data() + suffix.size(), row).ec == std::errc{}) {
            if (auto room = match_->lobby_state()) for (const auto& seat : room->snapshot.seats)
                if (seat.selection.slot == row + 1 && seat.peer != room->snapshot.host) { (void)match_->lobby_kick(seat.peer); break; }
        }
    } else if (id == "IDC_LAN_START_GAME_BUTTON") {
        // Start (host) or Accept (joiner); the checks run in refresh_staging's enabled state and here.
        if (page_ != Page::staging) return;
        static_cast<void>(run_action(impl.host ? "start" : "accept"));
    } else if (id == "IDC_BUTTON_ACCEPT") {
        // The filter page's Accept: the combos are the player's filter.
        auto value = [&](const char* gadget) -> std::optional<std::int64_t> {
            if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
                const int selected = combo->get_selected();
                if (selected >= 0 && combo->get_item_id(selected) != 0) return combo->get_item_id(selected);
            }
            return std::nullopt;
        };
        model::LobbyFilter filter;
        if (const auto type = value("IDC_GAME_TYPE_FILTER_COMBO"); type && *type >= 1 && *type <= 4) {
            filter.game_type = game_types[*type - 1].name;
        }
        if (const auto min = value("IDC_MIN_PLAYERS_FILTER_COMBO")) filter.min_players = static_cast<std::uint32_t>(*min);
        if (const auto max = value("IDC_MAX_PLAYERS_FILTER_COMBO")) filter.max_players = static_cast<std::uint32_t>(*max);
        if (const auto ping = value("IDC_MAX_PING_FILTER_COMBO")) filter.max_ping_ms = static_cast<std::uint32_t>(*ping);
        impl.filter = filter;
        impl.dirty = true;
        open_lobby();
    } else if (id == "IDC_DEFAULTS_BUTTON") {
        // Clear filters: the combos go back to their first entry.
        for (const char* gadget : {"IDC_GAME_TYPE_FILTER_COMBO", "IDC_MIN_PLAYERS_FILTER_COMBO", "IDC_MAX_PLAYERS_FILTER_COMBO",
                                   "IDC_MAX_PING_FILTER_COMBO"}) {
            if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) combo->select(0);
        }
        impl.filter.clear();
    } else if (id == "IDC_SELECT_OFFICIAL_MAPS" || id == "IDC_SELECT_CUSTOM_MAPS") {
        impl.custom_maps = id == "IDC_SELECT_CUSTOM_MAPS";
        fill_map_list();
        static_cast<void>(publish_staging_selection());
        announce();
    }
}

// ---- The filter and direct connect pages ------------------------------------------------------

std::string EawrLanLobby::player_name() const { return impl_ ? impl_->name : std::string{}; }

std::uint32_t EawrLanLobby::player_colour() const { return impl_ ? impl_->colour : 0; }

EawrLanLobby::Snapshot EawrLanLobby::snapshot() const {
    Snapshot shot;
    if (!impl_) return shot;
    const Impl& impl = *impl_;
    static constexpr const char* pages[] = {"lobby", "host_options", "staging", "direct_connect", "filter"};
    shot.page = pages[static_cast<std::size_t>(page_)];
    shot.name = impl.name;
    shot.colour = impl.colour;
    for (const model::LobbyGame& game : impl.shown) {
        const model::LobbyRowText row = impl.join_selection.source() == model::LobbySourceKind::online
            ? model::online_row_text(game) : model::lobby_row_text(game);
        shot.games.push_back(row.name + " | " + row.type + " | " + row.players + " | " + row.map + " | " + row.ping
                             + (game.version_matches ? "" : " | other version") + (game.passworded ? " | password" : ""));
    }
    shot.players.push_back(impl.name);
    for (const model::LobbyPlayer& player : impl.players) {
        if (player.place == model::LobbyPlace::lobby) shot.players.push_back(player.name);
    }
    const auto& lines = (page_ == Page::staging ? impl.staging_chat : impl.lobby_chat).lines();
    for (const model::ChatLine& line : lines) shot.chat.push_back(line.text);
    // The page is gone once the screen closed: the report then has the model's lines only.
    if (const auto* list = state_ ? EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST") : nullptr) {
        shot.chat_shown = static_cast<std::size_t>(list->get_item_count());
        if (shot.chat_shown > 0) shot.chat_last_shown = utf8(list->get_item_text(list->get_item_count() - 1));
    }
    shot.rows = impl.row_states;
    if (state_ && page_ == Page::staging) {
        if (const auto* list = EAWR_CONTROL(EawrUiList, "EAWR_ONLINE_CONNECTIONS_LIST")) {
            shot.connections_open = list->is_visible();
            for (int item = 0; item < list->get_item_count(); ++item) shot.connections.push_back(utf8(list->get_item_text(item)));
        }
    }
    shot.status = impl.status;
    if (state_ && state_->dialog) shot.rectangles = dialog_rectangles(*state_->dialog);
    if (impl.host && impl.map_index >= 0) shot.map = resources_->maps[static_cast<std::size_t>(impl.map_index)].name;
    else if (impl.offered && impl.offered->map) shot.map = *impl.offered->map;
    if (state_ && page_ == Page::staging && !impl.host) {
        if (const auto* name = EAWR_CONTROL(EawrUiLabel, "EAWR_JOINER_MAP_NAME")) shot.drawn_map = utf8(name->get_text());
        shot.preview_visible = state_->preview && state_->preview->is_visible() && state_->preview->get_texture().is_valid();
        if (const auto* summary = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST")) {
            shot.settings_read_only = summary->is_visible() && summary->get_item_count() > 0;
            for (int i = 0; i < summary->get_item_count(); ++i) {
                shot.settings.push_back(utf8(summary->get_item_text(i)));
                shot.settings_read_only = shot.settings_read_only && summary->is_item_disabled(i) && !summary->is_item_selectable(i);
            }
        }
    }
    shot.start_caption = impl.start_caption;
    shot.start_reason = impl.start_reason;
    shot.start_enabled = impl.start_enabled;
    shot.host = impl.host;
    if (const auto room = match_->lobby_state()) {
        shot.room_revision = room->snapshot.revision; shot.local_ready = room->local_ready; shot.preparing = room->preparing;
        shot.ready_seats = static_cast<std::size_t>(std::count_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; }));
    }
    shot.dialog_rows = state_ ? state_->dialog_rows : 0;
    shot.box_open = impl.box != nullptr;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    shot.source = online ? "online" : "lan";
    shot.chat_open = !online && static_cast<bool>(impl.source);
    if (online) {
        const model::OnlinePageView view = model::online_page(impl.online_rooms_total, impl.online_page);
        shot.room_page = view.page;
        shot.room_pages = view.pages;
        shot.rooms_total = impl.online_rooms_total;
        shot.prompt_open = impl.prompt.has_value();
        shot.prompt_error = impl.prompt ? impl.prompt->error : std::string{};
        shot.hosting = std::string(model::online_hosting_name(impl.hosting));
        shot.room_notice = impl.list_notice;
        shot.chat_open = impl.chat_open;
        shot.room_private = !impl.host_password.empty();
        for (const model::LobbyGame& game : impl.shown) if (game.passworded) shot.locked.push_back(game.name);
    }
    if (state_ && page_ == Page::staging) {
        for (const char* id : {"IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_CUSTOM_MAPS", "IDC_STAGING_MAP_LIST", "IDC_SELECT_OPTIONS"}) {
            if (const auto* control = EAWR_CONTROL(Control, id); control != nullptr && !control->is_visible()) shot.hidden.emplace_back(id);
        }
        // The preview is a texture rect shared by both staging roles.
        if (state_->preview == nullptr) shot.hidden.emplace_back("IDC_IMAGE");
    }
    return shot;
}

} // namespace eawr::presentation::godot_backend
