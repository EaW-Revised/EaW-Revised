#include "lan_lobby_screen_internal.hpp"

#include "checkpoint_lobby_model.hpp"
#include "coop_campaign_tab.hpp"
#include "saved_games.hpp"
#include "viewer_mounted_mods.hpp"
#include "eawr/net/lan_directory.hpp"

namespace eawr::presentation::godot_backend {
namespace {
using viewer::checkpoint_work;
sim::galactic::MpRoster display_roster(const GalacticLobbyBackend::CheckpointCandidate& candidate) {
    auto roster = candidate.original_roster;
    roster.seats = candidate.saved.seats;
    return roster;
}
} // namespace

void EawrLanLobby::checkpoint_closed() { close_checkpoint_list(); }

void EawrLanLobby::close_checkpoint_list() {
    Impl& impl = *impl_;
    // A list that closed itself (Cancel, a chosen save) is already queued for freeing; any other exit frees it here.
    if (auto* list = impl.list(); list != nullptr && !list->is_queued_for_deletion()) list->queue_free();
    impl.checkpoint_list = nullptr; impl.checkpoint_list_instance = 0; impl.checkpoint_store.reset();
    // The list screen shares the lobby's layer while it is open (it is the lobby's child); the lobby steps down so the
    // two never tie, and steps back up here.
    set_layer(screen_layer);
}

void EawrLanLobby::open_checkpoint_list() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging
        || !impl.galactic_backend || impl.checkpoint_list || impl.checkpoint_read.valid() || impl.checkpoint_listing.valid()) return;
    impl.checkpoint_list_note.clear();
    LoadGameContext context;
    context.boxes = MessageBoxContext{&*resources_->dialogs, &*resources_->theme_model,
        resources_->text ? &*resources_->text : nullptr, resources_->theme, resources_->theme_size};
    auto directory = options_.save_directory.empty()
        ? ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "saves" : options_.save_directory;
    if (options_.save_directory.empty()) {
        const auto arguments = OS::get_singleton()->get_cmdline_user_args();
        for (int64_t i = 0; i + 1 < arguments.size(); ++i)
            if (arguments[i] == "--eawr-save-dir") directory = ViewerPath{utf8(arguments[i + 1])}.native();
    }
    if (!impl.checkpoint_store) {
        prime_mounted_mods(options_.game_root);
        impl.checkpoint_listing_request = ++impl.checkpoint_request;
        impl.checkpoint_listing_started = std::chrono::steady_clock::now();
        auto source = std::make_shared<viewer::DirectorySavedGameStore>(directory,
            viewer::local_saved_game_clock(), viewer::SavedGameFilter::multiplayer);
        impl.checkpoint_listing = checkpoint_work([source, backend = impl.galactic_backend, hang = options_.fault_listing_hang] {
            // Fixture fault: the worker never returns, as a save folder that blocks forever would (the lobby gives up on it).
            while (hang) std::this_thread::sleep_for(std::chrono::hours(1));
            std::map<std::string, std::optional<mods::ModDescription>> details;
            auto listing = viewer::checked_saved_game_listing(*source, [backend](std::span<const std::uint8_t> bytes) {
                const auto inspected = backend->inspect_checkpoint(bytes);
                return inspected ? std::string{} : inspected.error().message;
            }, [&details](std::string_view name, std::span<const std::uint8_t> bytes) {
                details.emplace(name, viewer::saved_game_mod_description(bytes));
            });
            return std::make_shared<viewer::SnapshotSavedGameStore>(source, std::move(listing), std::move(details));
        });
        return;
    }
    context.saves = impl.checkpoint_store;
    context.save_details = [store = impl.checkpoint_store](std::string_view name) {
        return store->details(name, mounted_mod_description().value_or(mods::ModDescription{}));
    };
    context.launcher = [this](const model::SavedGameRequest& request) -> std::string {
        Impl& current = *impl_;
        const auto state = match_->lobby_state();
        if (!current.host || !state || state->snapshot.phase != net::LobbyPhase::staging)
            return "Only the staging host can select a saved game.";
        current.galactic_backend->clear_checkpoint();
        current.checkpoint_applied = false; current.checkpoint_candidate.reset();
        current.checkpoint_bytes.reset(); current.checkpoint_bindings.clear();
        current.checkpoint_description = request.name; current.checkpoint_date.clear();
        current.checkpoint_problem = "Reading saved game";
        current.checkpoint_request_active = ++current.checkpoint_request;
        if (current.list() != nullptr) {
            const auto* row = current.list()->model().selected_entry();
            if (row && row->file_name == request.name) {
                current.checkpoint_date = row->date_text;
                current.checkpoint_description = row->text;
            }
        }
        current.checkpoint_read = checkpoint_work([request, backend = current.galactic_backend] {
            using Result = core::Result<Impl::CheckpointRead>;
            auto read = platform::read_saved_game_bytes(ViewerPath::utf8(request.directory), request.name);
            if (!read) return Result::failure(read.error());
            auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(read).value());
            auto candidate = backend->inspect_checkpoint(*bytes);
            if (!candidate) return Result::failure(candidate.error());
            return Result::success(Impl::CheckpointRead{std::move(bytes), std::move(candidate).value()});
        });
        return {};
    };
    auto* screen = memnew(EawrLoadGameScreen);
    if (!screen->open(*this, context)) {
        note_chat(model::ChatKind::error, screen->failure()); memdelete(screen); return;
    }
    impl.checkpoint_list = screen;
    impl.checkpoint_list_instance = static_cast<std::uint64_t>(screen->get_instance_id());
    // This shared screen normally sits under the lobby; raise its existing modal over staging. It is a child canvas layer
    // of the lobby, and two canvas layers with the same index draw and hit-test in an order nobody chose (#5512): the
    // lobby steps below the list for as long as the list is open, so the dialog is always the one on top.
    set_layer(screen_layer - 2);
    screen->set_layer(screen_layer);
    // The lobby's 1920x1080 reference space is letterboxed into the window; the list is built for that space too, so it
    // takes the same scale and offset as the lobby's message boxes (#5512: it was drawn off-screen at 1280x720).
    layout_page();
    screen->connect("closed", callable_mp(this, &EawrLanLobby::checkpoint_closed));
    screen->connect("save_chosen", callable_mp(this, &EawrLanLobby::checkpoint_chosen));
    if (auto* recordings = screen->gadget("IDC_CHECK_RECORDINGS")) recordings->hide();
}

void EawrLanLobby::checkpoint_chosen(String, String) { checkpoint_closed(); }

void EawrLanLobby::process_checkpoint_read() {
    Impl& impl = *impl_;
    if (impl.checkpoint_prepare.active()) {
        const auto room = match_->lobby_state();
        const auto* snapshot = impl.host && room && match_->galactic_room() ? &room->snapshot : nullptr;
        auto polled = impl.checkpoint_prepare.poll(impl.checkpoint_request, snapshot, std::chrono::steady_clock::now());
        using State = viewer::CheckpointRestoreState;
        if (polled.state == State::ready) {
            auto prepared = std::move(*polled.value);
            if (prepared && prepared.value().manifest.epoch <= impl.galactic_backend->selection_epoch()) {
                impl.checkpoint_problem = "The room changed while the save was restoring. Apply the saved seats again.";
                (void)checkpoint_work([discard = std::move(prepared).value()] { static_cast<void>(discard); });
            } else {
                if (prepared) {
                    prepared.value().manifest.description = net::sanitize_lan_menu_text(impl.checkpoint_description, net::save_description_limit);
                    prepared.value().manifest.date = net::sanitize_lan_menu_text(impl.checkpoint_date, net::save_date_limit);
                }
                auto result = prepared ? impl.galactic_backend->commit_checkpoint(std::move(prepared).value(), *snapshot)
                                       : core::Result<void>::failure(prepared.error());
                impl.checkpoint_applied = static_cast<bool>(result);
                impl.checkpoint_problem = result ? std::string{} : result.error().message;
            }
            impl.rows_signature.clear(); impl.checkpoint_signature.clear();
        } else if (polled.state == State::timed_out) {
            impl.checkpoint_problem = "Restoring the saved game timed out. Return to the lobby or try again when it finishes.";
            impl.checkpoint_signature.clear();
        } else if (polled.state == State::room_changed) {
            impl.checkpoint_problem = "The room changed while the save was restoring. Apply the saved seats again.";
            impl.checkpoint_signature.clear();
        } else if (polled.state == State::failed) {
            impl.checkpoint_problem = "The saved game could not be restored.";
            impl.checkpoint_signature.clear();
        } else if (polled.state == State::stale) {
            impl.checkpoint_signature.clear();
        }
    }
    // A list screen the engine freed without telling the lobby must not leave Load Game dead.
    if (impl.checkpoint_list != nullptr && impl.list() == nullptr) checkpoint_closed();
    if (impl.checkpoint_listing.valid()) {
        const bool current = impl.checkpoint_listing_request == impl.checkpoint_request && impl.host && match_->galactic_room();
        if (impl.checkpoint_listing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto store = impl.checkpoint_listing.get();
                if (current) {
                    impl.checkpoint_store = std::move(store);
                    open_checkpoint_list();
                } else if (impl.host) {
                    impl.checkpoint_list_note = "The room changed while the saved games were listed. Press Load Game again.";
                }
            } catch (...) {
                if (current) impl.checkpoint_list_note = "Saved games could not be listed.";
            }
            impl.checkpoint_signature.clear();
        } else if (viewer::checkpoint_listing_expired(impl.checkpoint_listing_started, std::chrono::steady_clock::now(),
                       options_.listing_timeout_ms > 0 ? std::chrono::milliseconds(options_.listing_timeout_ms) : viewer::checkpoint_listing_limit)) {
            // The worker keeps its own copy of everything; dropping the future never waits for it.
            impl.checkpoint_listing = {};
            if (current) impl.checkpoint_list_note = "Checking the saved games took too long. Press Load Game to try again.";
            impl.checkpoint_signature.clear();
        }
    }
    if (!impl.checkpoint_read.valid()
        || impl.checkpoint_read.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    core::Result<Impl::CheckpointRead> read = [&]() {
        try { return impl.checkpoint_read.get(); }
        catch (...) {
            core::Diagnostic error; error.message = "The saved game could not be read.";
            return core::Result<Impl::CheckpointRead>::failure(std::move(error));
        }
    }();
    if (impl.checkpoint_request_active != impl.checkpoint_request || !impl.host || !match_->galactic_room()) return;
    if (!read) { impl.checkpoint_problem = read.error().message; return; }
    auto result = std::move(read).value();
    impl.checkpoint_candidate = std::move(result.candidate); impl.checkpoint_bytes = std::move(result.bytes);
    impl.checkpoint_problem.clear(); impl.checkpoint_bindings.clear();
    impl.checkpoint_signature.clear(); impl.rows_signature.clear();
    const auto room = match_->lobby_state();
    const auto humans = viewer::checkpoint_humans(display_roster(*impl.checkpoint_candidate));
    if (room) for (std::size_t i = 0; i < room->snapshot.seats.size(); ++i) {
        net::SetupSeat binding{}; binding.peer = static_cast<net::PeerIndex>(i);
        if (i < humans.size()) { binding.seat = humans[i].seat_id; binding.player = humans[i].player; }
        impl.checkpoint_bindings.push_back(binding);
    }
}

void EawrLanLobby::new_campaign() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging || !impl.galactic_backend) return;
    ++impl.checkpoint_request; impl.checkpoint_store.reset();
    impl.galactic_backend->clear_checkpoint(); impl.checkpoint_candidate.reset(); impl.checkpoint_bytes.reset();
    impl.checkpoint_bindings.clear(); impl.checkpoint_problem.clear(); impl.checkpoint_description.clear();
    impl.checkpoint_date.clear(); impl.checkpoint_applied = false; impl.checkpoint_signature.clear(); impl.rows_signature.clear();
}

void EawrLanLobby::select_checkpoint_seat(const int64_t index, const int64_t peer) {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging || !impl.checkpoint_candidate
        || index < 0 || peer < 0 || static_cast<std::size_t>(peer) >= impl.checkpoint_bindings.size()) return;
    const auto humans = viewer::checkpoint_humans(display_roster(*impl.checkpoint_candidate));
    if (static_cast<std::size_t>(index) >= humans.size()) return;
    auto& binding = impl.checkpoint_bindings[static_cast<std::size_t>(peer)];
    const auto& saved = humans[static_cast<std::size_t>(index)];
    if (binding.seat == saved.seat_id) return;
    ++impl.checkpoint_request;
    binding.seat = saved.seat_id; binding.player = saved.player;
    impl.galactic_backend->clear_checkpoint(); impl.checkpoint_applied = false;
    impl.checkpoint_problem.clear(); impl.checkpoint_signature.clear();
}

void EawrLanLobby::apply_checkpoint_selection() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging
        || !impl.checkpoint_candidate || !impl.checkpoint_bytes || !impl.galactic_options || impl.checkpoint_prepare.active()) return;
    const auto& candidate = *impl.checkpoint_candidate;
    const auto& roster = candidate.original_roster;
    const auto humans = viewer::checkpoint_humans(display_roster(candidate));
    impl.checkpoint_problem.clear();
    if (humans.size() != room->snapshot.seats.size() || impl.checkpoint_bindings.size() != humans.size()) {
        impl.checkpoint_problem = humans.size() > room->snapshot.seats.size() ? "checkpoint: missing_human_seat" : "checkpoint: extra_human_seat";
        return;
    }
    std::set<sim::SeatId> assigned;
    for (const auto& binding : impl.checkpoint_bindings) {
        if (!assigned.insert(binding.seat).second) { impl.checkpoint_problem = "checkpoint: duplicate_saved_seat"; return; }
        if (std::none_of(humans.begin(), humans.end(), [&](const auto& seat) {
            return seat.seat_id == binding.seat && seat.player == binding.player;
        })) { impl.checkpoint_problem = "checkpoint: unknown_saved_human_seat"; return; }
    }
    auto options = *impl.galactic_options;
    options.campaign = roster.campaign; options.difficulty = roster.difficulty; options.seed = candidate.seed;
    options.custom = roster.custom; options.credits = roster.credits; options.starting_tech = roster.starting_tech;
    options.maximum_tech = roster.maximum_tech; options.allow_heroes = roster.allow_heroes; options.rows.clear();
    for (const auto& player : roster.players) {
        net::GalacticLobbyRow row{}; row.player = player.player; row.faction_id = player.faction_id;
        row.flags = player.flags; row.faction = player.faction; row.dropout = player.dropout;
        row.human = std::any_of(humans.begin(), humans.end(), [&](const auto& seat) { return seat.player == player.player; });
        options.rows.push_back(std::move(row));
    }
    // The options and every faction owner change together, before the backend captures the exact room.
    if (!match_->lobby_galactic_checkpoint(options, impl.checkpoint_bindings)) {
        const auto current = match_->lobby_state();
        impl.checkpoint_problem = current && current->notice ? current->notice->message : "Saved faction assignment refused";
        return;
    }
    const auto configured = match_->lobby_state();
    if (!configured) return;
    // Restore refusal retains these visible room choices for Apply/Retry. The pending
    // candidate keeps Start disabled until successful selection or explicit New Campaign.
    impl.checkpoint_applied = false;
    impl.checkpoint_problem = "Restoring saved game...";
    const auto request = ++impl.checkpoint_request;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    (void)impl.checkpoint_prepare.submit(request, configured->snapshot, deadline,
        [backend = impl.galactic_backend, snapshot = configured->snapshot,
        roster, bindings = impl.checkpoint_bindings, bytes = impl.checkpoint_bytes,
        epoch = impl.galactic_backend->selection_epoch() + 1]() mutable {
        return backend->prepare_checkpoint(snapshot, std::move(roster), std::move(bindings), std::move(bytes), epoch);
    });
    impl.rows_signature.clear(); impl.checkpoint_signature.clear();
}

void EawrLanLobby::refresh_checkpoint_staging() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!room || !state_->dialog || !impl.galactic_backend) return;
    const bool staging = room->snapshot.phase == net::LobbyPhase::staging;
    auto button = [&](const char* id, const char* caption, const char* anchor) {
        auto* found = EAWR_CONTROL(EawrUiButton, id);
        if (!found) {
            const auto* area = find_gadget(*state_->dialog, anchor);
            if (!area) return;
            found = memnew(EawrUiButton); found->set_text(caption); found->set_fit_to_rect(true);
            const auto rect = local_rect(*state_->dialog, *area);
            found->set_position(rect.position); found->set_size(rect.size);
            state_->dialog->frame->add_child(found); state_->controls[id] = found;
            found->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
        }
        found->set_visible(impl.host);
        found->set_disabled(!staging || (std::string_view(id) == "EAWR_GC_LOAD_GAME"
            && (impl.checkpoint_read.valid() || impl.checkpoint_listing.valid())));
    };
    button("EAWR_GC_LOAD_GAME", "Load Game", "IDC_SELECT_OFFICIAL_MAPS");
    button("EAWR_GC_NEW_CAMPAIGN", "New Campaign", "IDC_SELECT_CUSTOM_MAPS");
    // Keep the campaign tabs within the list's own rectangle, leaving the save controls in place.
    const auto* list_area = find_gadget(*state_->dialog, "IDC_STAGING_MAP_LIST");
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST"); list && impl.host && list_area) {
        // The list's authored rectangle: the tabs take its top strip, a small gap follows, the list gets the rest.
        const Rect2 rect = local_rect(*state_->dialog, *list_area);
        constexpr float tab_height = 28.0F;
        EawrUiButton* tabs[2]{};
        for (int i = 0; i != 2; ++i) {
            const char* id = i == 0 ? "EAWR_GC_TAB_MP" : "EAWR_GC_TAB_COOP";
            auto* tab = EAWR_CONTROL(EawrUiButton, id);
            if (!tab) {
                tab = memnew(EawrUiButton);
                tab->set_fit_to_rect(true);
                state_->dialog->frame->add_child(tab);
                state_->controls[id] = tab;
                tab->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
            }
            const char* key = i == 0 ? "TEXT_MULTIPLAYER" : "TEXT_COOP";
            tab->set_text(resources_->text && resources_->text->contains(key)
                ? resolved(resources_->text, key) : String(i == 0 ? "Multiplayer" : "Co-op"));
            tab->set_visible(i == 0 ? impl.tab_multiplayer_shown : impl.tab_coop_shown);
            tab->set_disabled(!staging || impl.checkpoint_candidate.has_value() || impl.galactic_backend->checkpoint_view().has_value());
            tabs[i] = tab;
        }
        // Each caption gets the width it needs, so a long one ("MULTIPLAYER") is not cut at the tab's edge.
        auto widths = viewer::campaign_tab_widths(rect.size.x, tabs[0]->get_combined_minimum_size().x, tabs[1]->get_combined_minimum_size().x);
        if (!impl.tab_multiplayer_shown) widths = {0.0F, rect.size.x};
        else if (!impl.tab_coop_shown) widths = {rect.size.x, 0.0F};
        tabs[0]->set_position(rect.position);
        tabs[0]->set_size(Vector2(widths.multiplayer, tab_height));
        tabs[1]->set_position(rect.position + Vector2(widths.multiplayer, 0));
        tabs[1]->set_size(Vector2(widths.coop, tab_height));
        const float header = tab_height + viewer::campaign_tab_gap;
        list->set_position(rect.position + Vector2(0, header));
        list->set_size(rect.size - Vector2(0, header));
    }

    if (auto* description = EAWR_CONTROL(Control, "EAWR_CAMPAIGN_DESCRIPTION")) description->hide();
    const auto checkpoint = impl.galactic_backend->checkpoint_view();
    const auto transfer = impl.galactic_backend->checkpoint_progress();
    const std::string problem = impl.host ? impl.checkpoint_problem
        : transfer.refusal ? transfer.refusal->message : std::string{};
    const bool pending = impl.checkpoint_read.valid() || !problem.empty()
        || (impl.checkpoint_candidate && !impl.checkpoint_applied);
    if (auto* label = EAWR_CONTROL(EawrUiLabel, "IDC_INTERNET_DIALOG_HEADER")) {
        const auto title = utf8(resolved(resources_->text, impl.host ? "TEXT_INTERNET_HOST_GAME_TITLE" : "TEXT_INTERNET_JOIN_GAME_TITLE"));
        const auto status = impl.checkpoint_listing.valid() && impl.checkpoint_listing_request == impl.checkpoint_request
            ? std::string("Checking saved games")
            : !checkpoint && !impl.checkpoint_candidate ? (problem.empty() ? impl.checkpoint_list_note : problem) : std::string{};
        label->set_text(text(viewer::checkpoint_staging_title(title, status)));
    }
    if (!checkpoint && !impl.checkpoint_candidate) {
        if (auto* panel = EAWR_CONTROL(Control, "EAWR_GC_CHECKPOINT")) panel->hide();
    } else {
        const auto roster = checkpoint ? checkpoint->roster : display_roster(*impl.checkpoint_candidate);
        const auto tick = checkpoint ? checkpoint->completed_tick : impl.checkpoint_candidate->saved.completed_tick;
        const auto& bindings = impl.host ? impl.checkpoint_bindings : checkpoint->bindings;
        std::vector<net::PeerIndex> peers;
        if (checkpoint) peers = checkpoint->peers;
        else for (const auto& seat : room->snapshot.seats) peers.push_back(seat.peer);
        const auto* manifest = impl.galactic_backend->checkpoint_manifest();
        const auto description = impl.host ? net::sanitize_lan_menu_text(impl.checkpoint_description, net::save_description_limit)
            : manifest ? manifest->description : std::string{};
        const auto date = impl.host ? net::sanitize_lan_menu_text(impl.checkpoint_date, net::save_date_limit)
            : manifest ? manifest->date : std::string{};
        const auto model = viewer::checkpoint_lobby_model(impl.host, staging, roster, tick, bindings, peers,
            match_->view().seat, transfer, description, date,
            problem);
        if (auto* maps = EAWR_CONTROL(Control, "IDC_STAGING_MAP_LIST")) maps->hide();
        auto* panel = EAWR_CONTROL(Control, "EAWR_GC_CHECKPOINT");
        const auto* area = find_gadget(*state_->dialog, "IDC_STAGING_MAP_LIST");
        if (!area) return;
        const auto rect = local_rect(*state_->dialog, *area);
        if (!panel) {
            panel = memnew(Control); panel->set_position(rect.position); panel->set_size(rect.size);
            state_->dialog->frame->add_child(panel); state_->controls["EAWR_GC_CHECKPOINT"] = panel;
        }
        panel->show();
        std::string signature = impl.host ? "host" : "joiner";
        signature += roster.campaign + std::to_string(tick);
        signature += staging ? "S" : "F";
        for (const auto& seat : room->snapshot.seats) signature += "|" + std::to_string(seat.peer) + seat.selection.name;
        for (const auto& binding : bindings) signature += ":" + std::to_string(binding.seat.value);
        if (signature != impl.checkpoint_signature) {
            impl.checkpoint_signature = signature;
            while (panel->get_child_count()) { auto* child = panel->get_child(0); panel->remove_child(child); memdelete(child); }
            auto* summary = memnew(EawrUiList);
            summary->set_name("CheckpointSummary");
            const float row_height = std::min(30.0F, rect.size.y / static_cast<float>(room->snapshot.seats.size() + 3U));
            const float summary_height = impl.host ? std::max(row_height, rect.size.y - row_height * static_cast<float>(room->snapshot.seats.size() + 1U)) : rect.size.y;
            summary->set_size(Vector2(rect.size.x, summary_height)); panel->add_child(summary);
            impl.checkpoint_status_signature.clear();
            if (impl.host) {
                const auto humans = viewer::checkpoint_humans(roster);
                for (std::size_t i = 0; i < room->snapshot.seats.size(); ++i) {
                    auto* combo = memnew(EawrUiCombo); combo->set_fit_to_longest_item(false); combo->set_clip_text(true);
                    combo->set_position(Vector2(0, summary_height + row_height * static_cast<float>(i)));
                    combo->set_size(Vector2(rect.size.x, row_height)); panel->add_child(combo);
                    for (std::size_t j = 0; j < humans.size(); ++j) {
                        const auto& saved = humans[j];
                        const auto player = std::find_if(roster.players.begin(), roster.players.end(), [&](const auto& row) { return row.player == saved.player; });
                        combo->add_item(text(room->snapshot.seats[i].selection.name + " -> seat " + std::to_string(saved.seat_id.value)
                            + " / " + (player == roster.players.end() ? "unknown" : resources_->text
                                ? model::faction_display_name(*resources_->content->catalog, &*resources_->text, player->faction)
                                : player->faction)));
                        if (i < bindings.size() && bindings[i].seat == saved.seat_id) combo->select(static_cast<int>(j));
                    }
                    combo->set_disabled(!model.can_assign || impl.checkpoint_prepare.active());
                    combo->connect("item_selected", callable_mp(this, &EawrLanLobby::select_checkpoint_seat).bind(static_cast<int64_t>(i)));
                }
                auto* apply = memnew(EawrUiButton); apply->set_text("Apply saved seats / Retry"); apply->set_fit_to_rect(true);
                apply->set_position(Vector2(0, rect.size.y - row_height)); apply->set_size(Vector2(rect.size.x, row_height));
                apply->set_disabled(!model.can_assign || !impl.checkpoint_bytes || impl.checkpoint_prepare.active()); panel->add_child(apply);
                apply->connect("pressed", callable_mp(this, &EawrLanLobby::apply_checkpoint_selection));
            }
        }
        std::string status_signature;
        for (const auto& line : model.rows) status_signature += "|" + line;
        if (impl.checkpoint_status_signature != status_signature) {
            impl.checkpoint_status_signature = std::move(status_signature);
            auto* summary = Object::cast_to<EawrUiList>(panel->get_child(0));
            if (summary) {
                summary->clear();
                for (const auto& line : model.rows) summary->add_item(text(line), Ref<Texture2D>(), false);
            }
        }
    }
    if (!checkpoint && !impl.checkpoint_candidate) {
        if (auto* maps = EAWR_CONTROL(Control, "IDC_STAGING_MAP_LIST")) maps->show();
    }
    const bool blocked = pending || transfer.refusal || (checkpoint && !transfer.agreed);
    if (blocked) {
        impl.start_enabled = false;
        for (const char* id : {"IDC_LOAD_GAME", "IDC_LAN_START_GAME_BUTTON"})
            if (auto* accept = EAWR_CONTROL(EawrUiButton, id)) {
                accept->set_disabled(true); accept->set_tooltip_text("Apply the saved seats and wait for every private restore to agree.");
            }
    }
}
} // namespace eawr::presentation::godot_backend
