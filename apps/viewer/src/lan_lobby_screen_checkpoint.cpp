#include "lan_lobby_screen_internal.hpp"

#include "checkpoint_lobby_model.hpp"
#include "coop_campaign_tab.hpp"
#include "saved_games.hpp"
#include "viewer_mounted_mods.hpp"
#include "eawr/net/lan_directory.hpp"

namespace eawr::presentation::godot_backend {
namespace {
using viewer::checkpoint_work;
// How many times in a row a restore ended by a real room change is applied again without the host pressing Apply (#7878).
constexpr unsigned max_checkpoint_reapplies = 3;
sim::galactic::MpRoster display_roster(const GalacticLobbyBackend::CheckpointCandidate& candidate) {
    auto roster = candidate.original_roster;
    roster.seats = candidate.saved.seats;
    return roster;
}
// #7869: the factions a human may join in the loaded save: every faction that has saved humans, and the playable factions the
// campaign data lets a human take over while the save holds fewer than two human sides.
std::vector<sim::galactic::MpRosterPlayer> join_choices(const GalacticLobbyBackend& backend, const sim::galactic::MpRoster& roster) {
    const auto& list = backend.campaigns();
    const auto campaign = std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.name == roster.campaign; });
    return viewer::checkpoint_join_choices(roster, [&](const sim::galactic::MpRosterPlayer& player) {
        return campaign != list.end() && std::any_of(campaign->factions.begin(), campaign->factions.end(), [&](const auto& faction) {
            return faction.player == player.player && faction.playable; });
    });
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
        current.checkpoint_status = "Reading saved game";
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
                impl.checkpoint_problem = "The saved game selection changed while the save was restoring. Apply the saved seats again.";
                (void)checkpoint_work([discard = std::move(prepared).value()] { static_cast<void>(discard); });
            } else {
                if (prepared) {
                    prepared.value().manifest.description = net::sanitize_lan_menu_text(impl.checkpoint_description, net::save_description_limit);
                    prepared.value().manifest.date = net::sanitize_lan_menu_text(impl.checkpoint_date, net::save_date_limit);
                }
                const bool was_prepared = static_cast<bool>(prepared);   // read before the move below
                auto result = prepared ? impl.galactic_backend->commit_checkpoint(std::move(prepared).value(), *snapshot)
                                       : core::Result<void>::failure(prepared.error());
                impl.checkpoint_applied = static_cast<bool>(result);
                impl.checkpoint_problem = result ? std::string{} : result.error().message;
                impl.checkpoint_wait_started = std::chrono::steady_clock::now();   // the guests' restores are waited for from here
                // The refusal shows in the panel; the log gets it too, in full (the panel row is cut at the screen edge).
                if (!result) {
                    const auto line = std::string("EAWR checkpoint refused: ") + (was_prepared ? "commit: " : "restore: ")
                        + result.error().code + " " + result.error().message;
                    UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
                }
            }
            impl.rows_signature.clear(); impl.checkpoint_signature.clear();
        } else if (polled.state == State::room_changed) {
            // #7878: only a real change (players, seats, settings) gets here. Say what it was; re-apply by itself while the
            // assignment can still be fitted to the room, and stop after a few rounds so a room that keeps changing is not
            // restored forever.
            const auto* before = impl.checkpoint_prepare.submitted_room();
            impl.checkpoint_room_change = before && snapshot ? GalacticLobbyBackend::describe_checkpoint_room_change(*before, *snapshot) : std::string{};
            if (impl.checkpoint_request != impl.checkpoint_auto_request) impl.checkpoint_reapplies = 0;
            const std::string change = impl.checkpoint_room_change.empty() ? std::string("The room changed while the save was restoring.")
                                                                           : impl.checkpoint_room_change;
            if (snapshot && impl.checkpoint_reapplies < max_checkpoint_reapplies) {
                ++impl.checkpoint_reapplies;
                impl.checkpoint_reapply_request = impl.checkpoint_request;
                note_chat(model::ChatKind::notice, change + " The saved game is being restored again.");
            } else {
                // The seat assignment is fitted to the new room in the next lobby refresh, which clears the panel line; the
                // sentence is kept there (checkpoint_kept_problem) so the host still sees what changed.
                impl.checkpoint_problem = change + " Apply the saved seats again.";
                impl.checkpoint_kept_problem = impl.checkpoint_problem;
                note_chat(model::ChatKind::notice, impl.checkpoint_problem);
            }
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
    if (!read) { impl.checkpoint_problem = read.error().message; impl.checkpoint_status.clear(); return; }
    auto result = std::move(read).value();
    impl.checkpoint_candidate = std::move(result.candidate); impl.checkpoint_bytes = std::move(result.bytes);
    impl.checkpoint_problem.clear(); impl.checkpoint_status.clear(); impl.checkpoint_bindings.clear();
    impl.checkpoint_signature.clear(); impl.rows_signature.clear();
    const auto room = match_->lobby_state();
    impl.checkpoint_bound_peers.clear();
    if (room) {
        for (const auto& seat : room->snapshot.seats) impl.checkpoint_bound_peers.push_back(seat.peer);
        impl.checkpoint_bindings = viewer::reconcile_checkpoint_bindings(
            display_roster(*impl.checkpoint_candidate), {}, {}, impl.checkpoint_bound_peers);
    }
}

void EawrLanLobby::new_campaign() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging || !impl.galactic_backend) return;
    ++impl.checkpoint_request; impl.checkpoint_store.reset();
    impl.galactic_backend->clear_checkpoint(); impl.checkpoint_candidate.reset(); impl.checkpoint_bytes.reset();
    impl.checkpoint_bindings.clear(); impl.checkpoint_problem.clear(); impl.checkpoint_status.clear(); impl.checkpoint_description.clear();
    impl.checkpoint_date.clear(); impl.checkpoint_applied = false; impl.checkpoint_signature.clear(); impl.rows_signature.clear();
}

// #7858: the host stops waiting for a restore that is taking long. The room and the chosen save stay; the guests see the restore was
// cancelled, and Apply saved seats starts it again.
void EawrLanLobby::cancel_checkpoint_restore() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging || !impl.galactic_backend || !impl.checkpoint_candidate) return;
    ++impl.checkpoint_request;
    impl.checkpoint_prepare.cancel();
    impl.galactic_backend->clear_checkpoint(); impl.checkpoint_applied = false;
    impl.checkpoint_problem.clear();
    impl.checkpoint_status = "Restore cancelled. Apply the saved seats to try again.";
    impl.checkpoint_signature.clear(); impl.rows_signature.clear();
}

// Fixture for the GPU test (--eawr-lan-do restore_fixture:<seconds>): a host with a one-human save chosen and a restore that has
// already run for <seconds> and never ends by itself, so the waiting state (busy line, slow mark, Cancel restore) can be looked at
// without a campaign that takes minutes to load. Cancel restore and Apply then run the real code.
bool EawrLanLobby::restore_fixture(const int seconds) {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging || !impl.galactic_backend || impl.checkpoint_prepare.active()) return false;
    GalacticLobbyBackend::CheckpointCandidate candidate;
    candidate.original_roster.campaign = "Fixture campaign"; candidate.original_roster.difficulty = "Normal";
    sim::galactic::MpRosterPlayer player; player.player = {1}; player.faction = "REBEL";
    candidate.original_roster.players.push_back(player);
    sim::galactic::Seat seat; seat.seat_id = {1}; seat.player = {1}; seat.human_side_id = {1};
    candidate.saved.seats.push_back(seat); candidate.saved.completed_tick = 3600;
    impl.checkpoint_candidate = std::move(candidate);
    impl.checkpoint_bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1});
    impl.checkpoint_description = "Fixture save"; impl.checkpoint_date = "2026-11-18";
    impl.checkpoint_bound_peers.clear();
    for (const auto& held : room->snapshot.seats) impl.checkpoint_bound_peers.push_back(held.peer);
    impl.checkpoint_bindings = viewer::reconcile_checkpoint_bindings(display_roster(*impl.checkpoint_candidate), {}, {}, impl.checkpoint_bound_peers);
    impl.checkpoint_applied = false; impl.checkpoint_problem.clear(); impl.checkpoint_status.clear();
    const auto request = ++impl.checkpoint_request;
    auto never = std::make_shared<std::promise<void>>();   // the worker waits on it for good (the process ends first)
    auto gate = never->get_future().share();
    using Result = core::Result<GalacticLobbyBackend::PreparedCheckpoint>;
    (void)impl.checkpoint_prepare.submit(request, room->snapshot, std::chrono::steady_clock::now() - std::chrono::seconds(seconds),
        [gate, never](const std::atomic<bool>& cancel) mutable {
            while (!cancel.load(std::memory_order_acquire) && gate.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {}
            core::Diagnostic stop; stop.message = "fixture"; return Result::failure(std::move(stop)); });
    impl.rows_signature.clear(); impl.checkpoint_signature.clear();
    return true;
}

// The Cancel restore button as a click reaches it (the panel's own button, the real signal path).
bool EawrLanLobby::press_cancel_restore() {
    auto* panel = EAWR_CONTROL(Control, "EAWR_GC_CHECKPOINT");
    if (panel == nullptr) return false;
    for (int i = 0; i < panel->get_child_count(); ++i) {
        auto* button = Object::cast_to<EawrUiButton>(panel->get_child(i));
        if (button != nullptr && utf8(button->get_text()) == "Cancel restore" && !button->is_disabled()) { button->emit_signal("pressed"); return true; }
    }
    return false;
}

void EawrLanLobby::select_checkpoint_seat(const int64_t index, const int64_t peer) {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging || !impl.checkpoint_candidate
        || index < 0 || peer < 0 || static_cast<std::size_t>(peer) >= impl.checkpoint_bindings.size()) return;
    const auto roster = display_roster(*impl.checkpoint_candidate);
    auto humans = viewer::checkpoint_humans(roster);
    for (const auto& seat : viewer::checkpoint_departed(roster)) humans.push_back(seat); // #7876: departed seats follow the active ones
    const auto joins = join_choices(*impl.galactic_backend, roster);
    auto& binding = impl.checkpoint_bindings[static_cast<std::size_t>(peer)];
    // Entries 0..humans-1 are the saved seats (active, then departed); the entries after them are the factions to join (#7869).
    if (static_cast<std::size_t>(index) >= humans.size() + joins.size()) return;
    if (static_cast<std::size_t>(index) >= humans.size()) {
        const auto& join = joins[static_cast<std::size_t>(index) - humans.size()];
        if (binding.seat.value == 0 && binding.player == join.player) return;
        binding.seat = sim::SeatId{}; binding.player = join.player;
    } else {
        const auto& saved = humans[static_cast<std::size_t>(index)];
        if (binding.seat == saved.seat_id) return;
        binding.seat = saved.seat_id; binding.player = saved.player;
    }
    ++impl.checkpoint_request;
    impl.galactic_backend->clear_checkpoint(); impl.checkpoint_applied = false;
    impl.checkpoint_problem.clear(); impl.checkpoint_status.clear(); impl.checkpoint_signature.clear();
}

void EawrLanLobby::apply_checkpoint_selection() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!impl.host || !room || room->snapshot.phase != net::LobbyPhase::staging
        || !impl.checkpoint_candidate || !impl.checkpoint_bytes || !impl.galactic_options || impl.checkpoint_prepare.active() || impl.checkpoint_prepare.finishing()) return;
    const auto& candidate = *impl.checkpoint_candidate;
    const auto& roster = candidate.original_roster;
    const auto saved_roster = display_roster(candidate);
    const auto humans = viewer::checkpoint_humans(saved_roster);
    const auto departed = viewer::checkpoint_departed(saved_roster);
    const auto joins = join_choices(*impl.galactic_backend, saved_roster);
    impl.checkpoint_problem.clear(); impl.checkpoint_status.clear();
    // A peer without a saved seat joins the save (#7869): the room may hold more peers than the save has humans, never fewer.
    if (impl.checkpoint_bindings.size() != room->snapshot.seats.size() || humans.size() > room->snapshot.seats.size()) {
        impl.checkpoint_problem = "checkpoint: missing_human_seat";
        return;
    }
    std::set<sim::SeatId> assigned;
    std::set<sim::galactic::PlayerId> joined;
    std::size_t active_mapped = 0;
    for (const auto& binding : impl.checkpoint_bindings) {
        if (binding.seat.value == 0) {
            if (std::none_of(joins.begin(), joins.end(), [&](const auto& join) { return join.player == binding.player; })) {
                const bool held = std::any_of(saved_roster.seats.begin(), saved_roster.seats.end(), [&](const auto& seat) {
                    return seat.kind == sim::SeatKind::human && seat.player == binding.player; });
                const auto known = std::any_of(saved_roster.players.begin(), saved_roster.players.end(), [&](const auto& row) {
                    return row.player == binding.player; });
                impl.checkpoint_problem = held || !known ? "checkpoint: extra_seat_faction_unknown" : "checkpoint: third_human_side";
                return;
            }
            joined.insert(binding.player);
            continue;
        }
        if (!assigned.insert(binding.seat).second) { impl.checkpoint_problem = "checkpoint: duplicate_saved_seat"; return; }
        if (std::any_of(departed.begin(), departed.end(), [&](const auto& seat) { return seat.seat_id == binding.seat && seat.player == binding.player; })) {
            joined.insert(binding.player); // a departed seat taken back (#7876): its faction has a human again
            continue;
        }
        if (std::none_of(humans.begin(), humans.end(), [&](const auto& seat) {
            return seat.seat_id == binding.seat && seat.player == binding.player;
        })) { impl.checkpoint_problem = "checkpoint: unknown_saved_human_seat"; return; }
        ++active_mapped;
    }
    if (active_mapped != humans.size()) { impl.checkpoint_problem = "checkpoint: missing_human_seat"; return; }
    auto options = *impl.galactic_options;
    options.campaign = roster.campaign; options.difficulty = roster.difficulty; options.seed = candidate.seed;
    options.custom = roster.custom; options.credits = roster.credits; options.starting_tech = roster.starting_tech;
    options.maximum_tech = roster.maximum_tech; options.allow_heroes = roster.allow_heroes; options.rows.clear();
    // #6156: the save's own battle-vote timeout (zero: a save that recorded none keeps the room's value).
    if (roster.vote_timeout_seconds) options.vote_timeout = roster.vote_timeout_seconds;
    for (const auto& player : roster.players) {
        net::GalacticLobbyRow row{}; row.player = player.player; row.faction_id = player.faction_id;
        row.flags = player.flags; row.faction = player.faction; row.dropout = player.dropout;
        row.human = std::any_of(humans.begin(), humans.end(), [&](const auto& seat) { return seat.player == player.player; })
            || joined.contains(player.player);
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
    // The restore line itself (with the elapsed time) comes from the running job; nothing refuses a restore that is still running.
    const auto request = ++impl.checkpoint_request;
    (void)impl.checkpoint_prepare.submit(request, configured->snapshot, std::chrono::steady_clock::now(),
        [backend = impl.galactic_backend, snapshot = configured->snapshot,
        roster, bindings = impl.checkpoint_bindings, bytes = impl.checkpoint_bytes,
        epoch = impl.galactic_backend->selection_epoch() + 1](const std::atomic<bool>& cancel) mutable {
        return backend->prepare_checkpoint(snapshot, std::move(roster), std::move(bindings), std::move(bytes), epoch, &cancel);
    }, &GalacticLobbyBackend::same_checkpoint_room);
    impl.rows_signature.clear(); impl.checkpoint_signature.clear();
}

void EawrLanLobby::refresh_checkpoint_staging() {
    Impl& impl = *impl_;
    const auto room = match_->lobby_state();
    if (!room || !state_->dialog || !impl.galactic_backend) return;
    const bool staging = room->snapshot.phase == net::LobbyPhase::staging;
    // #6121: a guest who joins, leaves or leaves and rejoins after Load Game changes which peers hold the room's seats. The assignment
    // is fitted to the room again, keeping the host's choices, so every human has a saved seat and the combo, Apply and Retry all
    // work; a restore already applied is void (the backend refuses a changed room) and Apply is needed again. A duplicate the host
    // picks on purpose is left alone: Apply names it.
    std::vector<net::PeerIndex> room_peers;
    for (const auto& seat : room->snapshot.seats) room_peers.push_back(seat.peer);
    if (impl.host && staging && impl.checkpoint_candidate && !impl.checkpoint_prepare.active()
        && (impl.checkpoint_bindings.size() != room_peers.size() || impl.checkpoint_bound_peers != room_peers)) {
        impl.checkpoint_bindings = viewer::reconcile_checkpoint_bindings(display_roster(*impl.checkpoint_candidate),
            impl.checkpoint_bindings, impl.checkpoint_bound_peers, room_peers);
        impl.checkpoint_bound_peers = std::move(room_peers);
        impl.galactic_backend->clear_checkpoint(); impl.checkpoint_applied = false;
        impl.checkpoint_problem.clear(); impl.checkpoint_status.clear(); impl.checkpoint_signature.clear(); impl.rows_signature.clear();
    }
    // #7878: the restore ended because the room really changed: apply the saved seats again, now fitted to the new room. When
    // they no longer fit (a human is missing or extra) the refusal names the change and what is needed.
    if (impl.checkpoint_reapply_request != 0) {
        const bool current = impl.checkpoint_reapply_request == impl.checkpoint_request;
        impl.checkpoint_reapply_request = 0;
        if (current && impl.host && staging && impl.checkpoint_candidate && impl.checkpoint_bytes && !impl.checkpoint_prepare.active()) {
            const auto before = impl.checkpoint_problem;
            apply_checkpoint_selection();
            const std::string change = impl.checkpoint_room_change.empty() ? std::string("The room changed while the save was restoring.")
                                                                           : impl.checkpoint_room_change;
            if (impl.checkpoint_prepare.active()) impl.checkpoint_auto_request = impl.checkpoint_request;
            else if (impl.checkpoint_problem == before) impl.checkpoint_problem = change + " Apply the saved seats again.";
            else if (!impl.checkpoint_problem.empty())
                impl.checkpoint_problem = change + " The saved seats no longer fit the room (" + impl.checkpoint_problem + ").";
            impl.checkpoint_signature.clear();
        }
    }
    // The reconcile above clears the panel line; a room-change sentence that was not re-applied survives it (#7878).
    if (!impl.checkpoint_kept_problem.empty()) {
        if (impl.checkpoint_candidate && impl.checkpoint_problem.empty() && !impl.checkpoint_prepare.active())
            impl.checkpoint_problem = impl.checkpoint_kept_problem;
        impl.checkpoint_kept_problem.clear(); impl.checkpoint_signature.clear();
    }
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
    // #7858: a restore in progress is a busy line with its elapsed time, on the host (own restore, then the guests') and on a guest.
    const auto now = std::chrono::steady_clock::now();
    std::string status = impl.host ? impl.checkpoint_status : std::string{};
    std::chrono::steady_clock::duration restoring{};
    bool restoring_now = false;
    if (impl.host && impl.checkpoint_prepare.active()) {
        restoring = now - impl.checkpoint_prepare.started(); restoring_now = true;
        status = viewer::checkpoint_restoring_text(restoring);
    } else if (impl.host && impl.checkpoint_prepare.finishing()) {
        // The cancelled worker is still ending: no new restore starts beside it (#7858).
        status = viewer::checkpoint_finishing_text();
    } else if (impl.host && checkpoint && impl.checkpoint_applied && !transfer.agreed && !transfer.refusal) {
        restoring = now - impl.checkpoint_wait_started; restoring_now = true;
        status = viewer::checkpoint_restoring_text(restoring, true);
    } else if (!impl.host && !transfer.restored && !transfer.refusal) {
        if (const auto elapsed = impl.galactic_backend->restore_elapsed()) {
            restoring = *elapsed; restoring_now = true;
            status = viewer::checkpoint_restoring_text(restoring);
        } else if (impl.galactic_backend->restore_finishing()) {
            status = viewer::checkpoint_finishing_text();
        }
    }
    // Past the slow mark the host may stop waiting: the Apply button turns into Cancel restore.
    const bool cancel_offered = impl.host && restoring_now && restoring >= viewer::checkpoint_restore_slow_after;
    const bool pending = impl.checkpoint_read.valid() || !problem.empty() || !status.empty()
        || (impl.checkpoint_candidate && !impl.checkpoint_applied);
    if (auto* label = EAWR_CONTROL(EawrUiLabel, "IDC_INTERNET_DIALOG_HEADER")) {
        const auto title = utf8(resolved(resources_->text, impl.host ? "TEXT_INTERNET_HOST_GAME_TITLE" : "TEXT_INTERNET_JOIN_GAME_TITLE"));
        const auto header_status = impl.checkpoint_listing.valid() && impl.checkpoint_listing_request == impl.checkpoint_request
            ? std::string("Checking saved games")
            : !checkpoint && !impl.checkpoint_candidate ? (!problem.empty() ? problem : !status.empty() ? status : impl.checkpoint_list_note) : std::string{};
        label->set_text(text(viewer::checkpoint_staging_title(title, header_status)));
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
        const auto manifest = impl.galactic_backend->checkpoint_manifest();
        const auto description = impl.host ? net::sanitize_lan_menu_text(impl.checkpoint_description, net::save_description_limit)
            : manifest ? manifest->description : std::string{};
        const auto date = impl.host ? net::sanitize_lan_menu_text(impl.checkpoint_date, net::save_date_limit)
            : manifest ? manifest->date : std::string{};
        const auto faction_name = [this](const std::string& faction) {
            return resources_->text ? model::faction_display_name(*resources_->content->catalog, &*resources_->text, faction) : faction;
        };
        const auto model = viewer::checkpoint_lobby_model(impl.host, staging, roster, tick, bindings, peers,
            match_->view().seat, transfer, description, date,
            problem, faction_name, status);
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
        signature += cancel_offered ? "X" : "-";   // the bottom button's job (Apply or Cancel restore) changes with it
        for (const auto& seat : room->snapshot.seats) signature += "|" + std::to_string(seat.peer) + seat.selection.name;
        for (const auto& binding : bindings) signature += ":" + std::to_string(binding.seat.value) + "/" + std::to_string(binding.player);
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
                auto humans = viewer::checkpoint_humans(roster);
                const auto active_humans = humans.size();
                for (const auto& seat : viewer::checkpoint_departed(roster)) humans.push_back(seat); // #7876
                const auto joins = join_choices(*impl.galactic_backend, roster);
                for (std::size_t i = 0; i < room->snapshot.seats.size(); ++i) {
                    auto* combo = memnew(EawrUiCombo); combo->set_fit_to_longest_item(false); combo->set_clip_text(true);
                    combo->set_position(Vector2(0, summary_height + row_height * static_cast<float>(i)));
                    combo->set_size(Vector2(rect.size.x, row_height)); panel->add_child(combo);
                    for (std::size_t j = 0; j < humans.size(); ++j) {
                        const auto& saved = humans[j];
                        const auto player = std::find_if(roster.players.begin(), roster.players.end(), [&](const auto& row) { return row.player == saved.player; });
                        combo->add_item(text(room->snapshot.seats[i].selection.name + " -> seat " + std::to_string(saved.seat_id.value)
                            + " / " + (player == roster.players.end() ? "unknown" : faction_name(player->faction))
                            + (j >= active_humans ? " (left, take it back)" : "")));
                        if (i < bindings.size() && bindings[i].seat == saved.seat_id) combo->select(static_cast<int>(j));
                    }
                    // #7869: a peer beyond the save's humans joins the save: after the saved seats the combo offers "Join <faction>" for every
                    // faction it may play (a shared seat beside the saved players, or an AI faction it takes over), never a refusal.
                    for (std::size_t j = 0; j < joins.size(); ++j) {
                        const bool held = std::any_of(roster.seats.begin(), roster.seats.end(), [&](const auto& seat) {
                            return seat.kind == sim::SeatKind::human && seat.player == joins[j].player; });
                        combo->add_item(text(room->snapshot.seats[i].selection.name + " -> Join " + faction_name(joins[j].faction)
                            + (held ? " (shared)" : " (takes over the AI)")));
                        if (i < bindings.size() && bindings[i].seat.value == 0 && bindings[i].player == joins[j].player)
                            combo->select(static_cast<int>(humans.size() + j));
                    }
                    combo->set_disabled(!model.can_assign || impl.checkpoint_prepare.active() || impl.checkpoint_prepare.finishing());
                    combo->connect("item_selected", callable_mp(this, &EawrLanLobby::select_checkpoint_seat).bind(static_cast<int64_t>(i)));
                }
                auto* apply = memnew(EawrUiButton); apply->set_text(cancel_offered ? "Cancel restore" : "Apply saved seats / Retry"); apply->set_fit_to_rect(true);
                apply->set_position(Vector2(0, rect.size.y - row_height)); apply->set_size(Vector2(rect.size.x, row_height));
                apply->set_disabled(!model.can_assign || (!cancel_offered && (!impl.checkpoint_bytes || impl.checkpoint_prepare.active() || impl.checkpoint_prepare.finishing()))); panel->add_child(apply);
                apply->connect("pressed", callable_mp(this, cancel_offered ? &EawrLanLobby::cancel_checkpoint_restore : &EawrLanLobby::apply_checkpoint_selection));
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
    // The host waits for every guest's restore (`agreed`); a guest only for its own (#6128: `agreed` is never set on a guest).
    const bool blocked = viewer::checkpoint_start_blocked(impl.host, pending, transfer.refusal.has_value(), checkpoint.has_value(), transfer);
    if (blocked) {
        impl.start_enabled = false;
        for (const char* id : {"IDC_LOAD_GAME", "IDC_LAN_START_GAME_BUTTON"})
            if (auto* accept = EAWR_CONTROL(EawrUiButton, id)) {
                accept->set_disabled(true); accept->set_tooltip_text("Apply the saved seats and wait for every private restore to agree.");
            }
    }
}
} // namespace eawr::presentation::godot_backend
