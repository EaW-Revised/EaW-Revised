#include "lan_match_internal.hpp"
#include "eawr/core/build_identity.hpp"
#include <cctype>

namespace eawr::presentation::godot_backend::lan_detail {

bool TcpLanMatch::lobby_enabled() const  { return static_cast<bool>(lobby_builder_); }

void TcpLanMatch::configure_lobby(std::string name, std::uint32_t colour, std::vector<skirmish::SetupMap> maps, LanLobbyBuilder builder)  {
        // This staging slice has no recovery provisioning; preserve explicit caller policies for typed refusal.
        if (default_credentials_) set_recovery_policy({});
        lobby_name_ = net::sanitize_lobby_text(name, name.size()); lobby_colour_ = colour; lobby_maps_ = std::move(maps); lobby_builder_ = std::move(builder);
    }

bool TcpLanMatch::lobby_select(net::LobbySelection selection)  {
        if (!lobby_mutable()) return false;
        selection.name = net::sanitize_lobby_text(selection.name, selection.name.size());
        const auto local = std::find_if(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == io_->transport.self(); });
        if (local != lobby_->snapshot().seats.end() && selection.slot != local->selection.slot) {
            auto options = decode_lan_lobby_game(lobby_->snapshot()); if (!options) return false;
            const auto row = std::find_if(options.value().slots->begin(), options.value().slots->end(), [&](const auto& slot) { return slot.slot == selection.slot; });
            if (row == options.value().slots->end()) return false;
            selection.faction = row->faction; selection.team = row->team; selection.colour = row->colour_index.value_or(row->slot - 1);
        }
        if (local != lobby_->snapshot().seats.end() && selection.name == local->selection.name && selection.name != lobby_name_) selection.name = lobby_name_;
        const auto requested_name = selection.name;
        auto applied = lobby_->request(net::LobbyAction::select, lobby_->snapshot().revision, std::move(selection));
        if (applied) lobby_name_ = requested_name;
        return lobby_feedback(std::move(applied));
    }

bool TcpLanMatch::lobby_assign(net::PeerIndex peer, net::LobbySelection selection)  {
        return lobby_mutable() && lobby_feedback(lobby_->assign(peer, selection, selection.slot, lobby_->snapshot().revision));
    }

bool TcpLanMatch::lobby_game(const skirmish::FixtureOptions& options)  {
        if (!lobby_mutable() || !view().host) return false;
        auto configured = options;
        const std::set<sim::tactical::PlayerId> planned(owners_.begin(), owners_.end());
        if (configured.slots) for (auto& row : *configured.slots) row.human = planned.contains(row.slot);
        auto encoded = encode_lan_lobby_game(configured, input_delay_);
        if (!encoded) return lobby_feedback(core::Result<void>::failure(encoded.error()));
        auto assignments = lobby_->snapshot().seats;
        for (auto& seat : assignments) {
            const auto row = std::find_if(options.slots->begin(), options.slots->end(), [&](const auto& slot) { return slot.slot == seat.selection.slot; });
            // #4041: a row holding a human (host or guest) keeps its control; the host removes a guest with the kick action.
            if (row == options.slots->end() || !planned.contains(row->slot))
                return lobby_feedback(lobby_fail<void>(net::LobbyReason::invalid_choice, "A slot held by a human cannot be switched to AI, Closed or Open; kick the player first."));
            seat.selection.faction = row->faction; seat.selection.team = row->team;
            seat.selection.colour = row->colour_index.value_or(row->slot - 1); seat.player = row->slot;
        }
        return lobby_feedback(lobby_->configure_game(std::move(encoded).value(), std::move(assignments), lobby_->snapshot().revision));
    }

bool TcpLanMatch::lobby_ready(bool ready)  {
        if (!lobby_mutable()) return false;
        if (ready && lobby_state()->identity_pending) return lobby_feedback(lobby_fail<void>(net::LobbyReason::not_ready, "TEXT_WAITING"));
        if (!ready) {
            ready_pending_ = false;
            return lobby_feedback(lobby_->request(net::LobbyAction::unready, lobby_->snapshot().revision));
        }
        auto options = decode_lan_lobby_game(lobby_->snapshot());
        if (!options) return lobby_feedback(core::Result<void>::failure(options.error()));
        if (auto valid = validate_lobby(lobby_->snapshot(), true); !valid) return lobby_feedback(std::move(valid));
        if (preparing_.valid()) return false;
        auto canonical = encode_lan_lobby_game(options.value()); if (!canonical) return false;
        preparing_game_ = std::move(canonical).value();
        ready_pending_ = true; preparing_revision_ = lobby_->snapshot().revision;
        auto work = std::make_shared<std::packaged_task<core::Result<tactical::TacticalSetup>()>>(
            [builder = lobby_builder_, selected = std::move(options).value()] { return builder(selected); });
        preparing_ = work->get_future();
        drain_workers().start(std::function<void()>([work] { (*work)(); }));
        return true;
    }

bool TcpLanMatch::lobby_chat(std::string text)  {
        return lobby_mutable() && lobby_feedback(lobby_->chat(std::move(text)));
    }

bool TcpLanMatch::lobby_kick(net::PeerIndex peer)  {
        return lobby_mutable() && lobby_feedback(lobby_->kick(peer, lobby_->snapshot().revision));
    }

void TcpLanMatch::lobby_leave()  {
        if (!lobby_mutable()) return;
        (void)(view().host ? lobby_->close() : lobby_->request(net::LobbyAction::leave, lobby_->snapshot().revision));
        // One nonblocking pump flushes ordered notice/leave before cancellation.
        (void)io_->transport.tcp->poll();
    }

std::optional<LanLobbyState> TcpLanMatch::lobby_state() const  {
        if (handed_over_) return last_lobby_state_;
        if (!lobby_) return {};
        LanLobbyState state; state.snapshot = lobby_->snapshot(); state.chat = lobby_->chat_history(); state.notice = lobby_->notice() ? lobby_->notice() : lobby_notice_;
        state.preparing = preparing_.valid();
        const auto local = std::find_if(state.snapshot.seats.begin(), state.snapshot.seats.end(), [&](const auto& seat) { return seat.peer == lobby_self_; });
        state.local_ready = local != state.snapshot.seats.end() && local->ready;
        state.identity_pending = local != state.snapshot.seats.end() && local->selection.name != lobby_name_;
        const auto valid = validate_lobby(state.snapshot, true);
        if (!valid) state.ready_reason = valid.error().message;
        const bool hosting = view().host;
        const bool mesh_complete = !io_->transport.tcp || io_->transport.tcp->mesh_complete();
        const bool all_ready = std::all_of(state.snapshot.seats.begin(), state.snapshot.seats.end(), [](const auto& seat) { return seat.ready; });
        state.connections_pending = hosting && online_options_.has_value() && !mesh_complete;
        state.start_reason = state.ready_reason;
        if (state.start_reason.empty() && state.connections_pending)
            state.start_reason = "Waiting for connections between players. Every pair must connect before Start; My connections shows only your own links.";
        else if (state.start_reason.empty() && !all_ready) state.start_reason = "Waiting for every player to accept the current setup.";
        state.ready_enabled = !state.preparing && !state.identity_pending && local != state.snapshot.seats.end()
            && state.snapshot.phase == net::LobbyPhase::staging && (state.local_ready || valid);
        // An online room starts only complete: every seat present and every pair verified (docs/net-online-npeer.md, section 3).
        state.start_enabled = hosting && state.snapshot.seats.size() >= 2 && state.snapshot.phase == net::LobbyPhase::staging
            && valid
            && mesh_complete && all_ready;
        return state;
    }

bool TcpLanMatch::lobby_mutable() const { return !handed_over_ && view().phase != LanPhase::ended && lobby_ && io_->transport.tcp; }

void TcpLanMatch::flush_lobby_closes() {
        if (io_->transport.tcp) for (const auto peer : closing_peers_) {
            // A room mesh has no per-peer stream to close: the host frees the seat in the room, which closes its pairs.
            if (io_->transport.tcp->mesh()) { if (const auto room = online_endpoint_.lock(); room && view().host) (void)room->release(peer); }
            else (void)io_->transport.tcp->close_peer(peer);
        }
        closing_peers_.clear();
    }

bool TcpLanMatch::lobby_feedback(core::Result<void> result) {
        if (!result && lobby_) {
            const auto reason = lan_lobby_reason(result.error());
            lobby_notice_ = net::LobbyNotice{lobby_->snapshot().room, lobby_->snapshot().revision, reason, result.error().message};
        }
        return static_cast<bool>(result);
    }

core::Result<void> TcpLanMatch::validate_lobby(const net::LobbySnapshot& state, bool starting) const {
        auto options = decode_lan_lobby_game(state);
        if (!options) return core::Result<void>::failure(options.error());
        const auto map = std::find_if(lobby_maps_.begin(), lobby_maps_.end(), [&](const auto& value) { return value.path == state.game.map; });
        if (map == lobby_maps_.end() || !map->unavailable.empty()) return lobby_fail<void>(net::LobbyReason::invalid_choice, "TEXT_MAP_NOT_AVAILABLE_TO_PLAY");
        // WSS-17/18 gate Ready/Start, not intermediate edits or staging snapshots.
        if (auto slots = skirmish::validate_setup_slots(*options.value().slots, map->metadata, false, starting); !slots)
            return lobby_fail<void>(net::LobbyReason::invalid_choice, slots.error().message);
        auto planned_state = state; planned_state.seats.clear();
        auto planned = decode_lan_lobby_game(planned_state);
        if (!planned) return core::Result<void>::failure(planned.error());
        if (view().host) for (const auto owner : owners_) {
            if (std::none_of(planned.value().slots->begin(), planned.value().slots->end(), [&](const auto& row) { return row.slot == owner; }))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "A planned human owner has no loaded map row.");
        }
        const auto owner_rows = skirmish::setup_row_count(*map, false);
        for (const auto& row : *planned.value().slots) if (row.slot > owner_rows)
            return lobby_fail<void>(net::LobbyReason::invalid_choice, "An owner row is outside the loaded map capacity.");
        std::set<std::uint32_t> colours;
        for (const auto& slot : *options.value().slots) {
            const auto colour = slot.colour_index.value_or(slot.slot - 1);
            if (colour >= skirmish::multiplayer_colour_count || !colours.insert(colour).second)
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Choose distinct colours from the authored multiplayer palette.");
        }
        std::set<std::string> names;
        std::set<sim::tactical::PlayerId> human_rows;
        for (const auto& seat : state.seats) {
            std::string name = seat.selection.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.empty() || name.size() > presentation::ui::lan_name_max || !names.insert(name).second
                || seat.selection.colour >= skirmish::multiplayer_colour_count || seat.selection.slot == 0 || std::none_of(options.value().slots->begin(), options.value().slots->end(), [&](const auto& row) { return row.slot == seat.selection.slot; }))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Invalid or duplicate player choice.");
            if (std::none_of(planned.value().slots->begin(), planned.value().slots->end(), [&](const auto& row) { return row.slot == seat.selection.slot && row.human; }))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "This owner row is reserved for AI by the host.");
            // Defense in depth: the host roster already limits its planned human rows.
            if (!human_rows.insert(seat.selection.slot).second)
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Each human needs a separate owner row.");
            if (human_rows.size() > sim::max_match_human_sides)
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Too many human owner rows.");
            const auto team = std::find_if(map->teams.begin(), map->teams.end(), [&](const auto& value) { return value.team == seat.selection.team; });
            if (team == map->teams.end() || std::find(team->factions.begin(), team->factions.end(), seat.selection.faction) == team->factions.end())
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Faction is unavailable on this team.");
            if (starting && seat.player != seat.selection.slot) return lobby_fail<void>(net::LobbyReason::invalid_choice, "Owner assignment is not committed.");
        }
        if (starting) {
            skirmish::SetupSelection selected; selected.map = *options.value().map; selected.slots = *options.value().slots; selected.match = options.value().match;
            auto built = validated_lan_options(selected, lobby_maps_);
            if (!built) return lobby_fail<void>(net::LobbyReason::invalid_choice, built.error().message);
        }
        return core::Result<void>::success();
    }

void TcpLanMatch::clear_mesh_repair_material() {
        mesh_repair_auth_.reset(); mesh_material_revision_.reset(); mesh_retired_ = 0;
        credential_state_->clear();
    }

bool TcpLanMatch::current_mesh_credentials(const net::Received& frame) {
        if (!lobby_) return true;
        const auto revision = lobby_->snapshot().revision;
        if (frame.frame.front() == net::lan_credentials_kind && frame.from == 0 && io_->transport.self() != 0) {
            // Host snapshots and credentials share one ordered pair stream. The
            // preceding Ready snapshot supplies this frozen attempt's revision.
            if (mesh_material_revision_ && *mesh_material_revision_ != revision) clear_mesh_repair_material();
            mesh_material_revision_ = revision;
        } else if (frame.frame.front() == net::lan_credentials_ack_kind && io_->transport.self() == 0) {
            if (mesh_material_revision_ && lobby_->snapshot().phase == net::LobbyPhase::staging
                && *mesh_material_revision_ != revision) clear_mesh_repair_material();
            // A new Start requires every guest's fresh Ready on this same ordered
            // stream, so an old attempt's ACK cannot overtake that Ready barrier.
            if (!mesh_material_revision_) return false; // Late ACK from an already cancelled attempt.
        }
        return true;
    }

core::Result<void> TcpLanMatch::provision_mesh_repair(const net::SetupOffer& offer, std::uint64_t revision) {
        if (!online_options_ || offer.seats.size() < 3) return core::Result<void>::success();
        if (mesh_material_revision_ && *mesh_material_revision_ != revision) clear_mesh_repair_material();
        auto keys = credential_state_->provide_mesh_repair(offer, io_->transport);
        if (!keys) return core::Result<void>::failure(keys.error());
        auto encoded = net::encode_setup_offer(offer);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        net::TcpRejoinAuth auth;
        auth.match_identity = core::sha256(encoded.value()); auth.seat_tokens = std::move(keys).value();
        mesh_repair_auth_ = std::move(auth); mesh_material_revision_ = revision;
        return core::Result<void>::success();
    }

net::LobbyHooks TcpLanMatch::lobby_hooks() {
        net::LobbyHooks hooks;
        hooks.validate = [this](const net::LobbySnapshot& state, bool starting) { return validate_lobby(state, starting); };
        hooks.offer = [this](const net::LobbySnapshot& state, const std::vector<net::SetupSeat>& seats) -> core::Result<net::SetupOffer> {
            std::uint64_t delay{};
            auto selected = decode_lan_lobby_game(state, &delay);
            auto canonical = selected ? encode_lan_lobby_game(selected.value()) : core::Result<net::LobbyGame>::failure(selected.error());
            if (!prepared_setup_ || !prepared_setup_->match_identity || !canonical || canonical.value() != prepared_game_) return lobby_fail<net::SetupOffer>(net::LobbyReason::setup_failed, "Current staging choices have not been prepared.");
            auto bytes = net::encode_skirmish_setup(*prepared_setup_); if (!bytes) return core::Result<net::SetupOffer>::failure(bytes.error());
            net::SetupOffer offer; offer.setup = std::move(bytes).value(); offer.seed = prepared_setup_->seed;
            offer.identity = {std::string(core::build_identity()), *prepared_setup_->match_identity}; offer.seats = seats; offer.input_delay = delay;
            // The default LAN lobby does not provision confidential recovery. Existing legacy setup retains that path.
            if (recovery_policy_.reconnect || recovery_policy_.resync) return lobby_fail<net::SetupOffer>(net::LobbyReason::recovery_unavailable, "Recovery provisioning for staging is unavailable.");
            if (auto provisioned = provision_mesh_repair(offer, state.frozen_revision); !provisioned)
                return core::Result<net::SetupOffer>::failure(provisioned.error());
            return core::Result<net::SetupOffer>::success(std::move(offer));
        };
        hooks.setup = [this](net::MessageTransport& stream, const net::LobbySnapshot& staged) -> core::Result<net::LobbySetupContext> {
            lobby_stream_ = &stream;
            net::LobbySetupContext context; context.options.auto_accept = false; context.options.auto_start = false;
            context.options.timeout_ms = 180'000; context.options.session.keep_admission_tick_open = true;
            if (online_options_ && staged.seats.size() >= 3) {
                // Three or more seats share one room mesh: a lost pair is decided by one seat, not by each end (N4). The setup
                // stream is dense (host first, roster order); the pair diagnostics belong to the transport slots.
                net::MeshIncidentOptions mesh;
                std::vector<net::PeerIndex> slots;
                for (const auto& seat : staged.seats) slots.push_back(seat.peer);
                mesh.relay_failed = [this, slots](net::PeerIndex dense) {
                    return dense >= slots.size() || !io_->transport.tcp || io_->transport.tcp->pair(slots[dense]).relay_failed;
                };
                context.options.session.mesh = std::move(mesh);
                context.providers.configure_session = [this, revision = staged.frozen_revision](net::SessionOptions&, const net::SetupOffer& offer) {
                    return provision_mesh_repair(offer, revision);
                };
                context.providers.credentials_delivered = [this] { return credential_state_->delivered(); };
            }
            context.options.session.pending_input_epoch = [this] { return pending_input_epoch(); };
            context.providers.identify = [this](const net::SetupOffer&) {
                if (!prepared_setup_ || !prepared_setup_->match_identity) return lobby_fail<sim::MatchAgreement>(net::LobbyReason::setup_failed, "Local staging match identity is unavailable.");
                return core::Result<sim::MatchAgreement>::success({std::string(core::build_identity()), *prepared_setup_->match_identity});
            };
            context.providers.build = [this](const net::SetupOffer& offer) -> core::Result<std::unique_ptr<net::SessionAdapter>> {
                if (!view().host) return deferred_providers_.build(offer);
                auto adapter = std::make_unique<LiveAdapter>(*prepared_setup_, offer.seats, trace_);
                adapter_ = adapter.get();
                return core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
            };
            context.providers.monitor = &gate_;
            context.providers.tick_zero_agreed = [this] { return io_->monitor && io_->monitor->setup_agreed(); };
            context.providers.tick_zero_mismatch = [this]() -> std::optional<net::TickZeroMismatch> {
                if (!io_->monitor || !io_->monitor->report()) return {};
                const auto& report = *io_->monitor->report();
                if (report.first_divergent_tick != 0) return {};
                return net::TickZeroMismatch{report.local_hash, report.remote_hash, report.channel};
            };
            return core::Result<net::LobbySetupContext>::success(std::move(context));
        };
        hooks.remove_peer = [this](net::PeerIndex peer, net::LobbyReason) { closing_peers_.insert(peer); removed_peers_.insert(peer); };
        if (online_options_) {
            hooks.mesh_complete = [this]() -> core::Result<void> {
                if (io_->transport.tcp && io_->transport.tcp->mesh_complete()) return core::Result<void>::success();
                return lobby_fail<void>(net::LobbyReason::mesh_incomplete, "The room mesh is incomplete.");
            };
            hooks.lock_room = [this]() -> core::Result<void> {
                return io_->transport.tcp ? io_->transport.tcp->lock_room() : lobby_fail<void>(net::LobbyReason::mesh_incomplete, "The room mesh is gone.");
            };
        }
        return hooks;
    }

void TcpLanMatch::reset_lobby() {
        gate_.reset();
        lobby_.reset(); lobby_result_.reset(); lobby_stream_ = nullptr; prepared_setup_.reset(); ready_pending_ = false; lobby_setup_accepted_ = false; lobby_room_committed_ = false;
        removed_peers_.clear(); known_edges_.clear(); closing_peers_.clear(); lobby_notice_.reset(); last_lobby_state_.reset(); identity_retry_at_ms_ = 0;
    }

void TcpLanMatch::start_lobby() {
        const auto state = lobby_state();
        if (!state || !state->start_enabled || !prepared_setup_ || handed_over_) return;
        auto started = lobby_->start(state->snapshot.revision);
        if (!started) {
            if (online_options_) handle_online_start_failure(started.error(),
                [this](const core::Diagnostic& error) { (void)lobby_feedback(core::Result<void>::failure(error)); },
                [this](const core::Diagnostic& error) { set_failure(error); });
            else (void)lobby_feedback(started);
            return;
        }
        auto raw = io_->transport.tcp->poll();
        if (raw) { io_->transport.pending_bytes += raw->frame.size(); io_->transport.pending.push_back(std::move(*raw)); }
        last_lobby_state_ = lobby_state();
        auto options = decode_lan_lobby_game(lobby_->snapshot());
        if (options) pending_ = std::move(options).value();
    }

void TcpLanMatch::pump_lobby(const skirmish::FixtureOptions& selected) {
        if (handed_over_ || !io_->transport.tcp || view().phase == LanPhase::ended) return;
        lobby_clock_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (online_options_ && !drain_mesh_events()) return;
        sample_online_route();
        if (lobby_ && lobby_->snapshot().phase == net::LobbyPhase::starting && view().host) {
            auto raw = io_->transport.tcp->poll();
            if (raw) {
                constexpr auto max_pending_bytes = 8U * 1024U * 1024U;
                if (io_->transport.pending.size() >= 256 || io_->transport.pending_bytes > max_pending_bytes
                    || raw->frame.size() > max_pending_bytes - io_->transport.pending_bytes) {
                    set_failure(net::lobby_diagnostic(net::LobbyReason::resource_limit, "Ordered setup input exceeds the staging handover budget."));
                    return;
                }
                io_->transport.pending_bytes += raw->frame.size(); io_->transport.pending.push_back(std::move(*raw));
            }
            return; // No core pump or tick-zero publication before live bind.
        }
        if (!lobby_) {
            if (view().host) {
                auto game = encode_lan_lobby_game(selected, input_delay_);
                if (!game || !selected.slots || selected.slots->empty()) {
                    if (online_options_) set_failure(diagnostic("EAWR-NET-1535", "Online staging requires valid game options and player slots."));
                    return;
                }
                auto configured = selected;
                for (auto& slot : *configured.slots) slot.human = std::find(owners_.begin(), owners_.end(), slot.slot) != owners_.end();
                configured.slots->front().colour_index = lobby_colour_;
                std::set<std::uint32_t> used_colours{lobby_colour_};
                for (auto& slot : *configured.slots) {
                    if (slot.slot == configured.slots->front().slot) continue;
                    auto colour = slot.colour_index.value_or(slot.slot - 1);
                    if (used_colours.contains(colour)) {
                        colour = 0;
                        while (colour < skirmish::multiplayer_colour_count && used_colours.contains(colour)) ++colour;
                    }
                    slot.colour_index = colour; used_colours.insert(colour);
                }
                game = encode_lan_lobby_game(configured, input_delay_);
                if (!game) {
                    if (online_options_) set_failure(diagnostic("EAWR-NET-1535", "Online staging options could not be encoded."));
                    return;
                }
                const auto& row = configured.slots->front();
                const auto room_seed = lobby_name_ + std::to_string(lobby_clock_);
                const Bytes room_bytes(room_seed.begin(), room_seed.end());
                auto created = net::LobbySession::host(io_->transport, "lan-" + core::sha256_hex(room_bytes).substr(0, 60),
                    static_cast<std::uint32_t>(owners_.size()), {lobby_name_, row.faction, lobby_colour_, row.team, row.slot}, row.slot, std::move(game).value(), lobby_hooks());
                if (!created) { set_failure(created.error()); return; }
                lobby_ = std::make_shared<net::LobbySession>(std::move(created).value()); io_->room_lifetime = lobby_; lobby_self_ = io_->transport.self();
            } else {
                auto raw = io_->transport.tcp->poll();
                if (!raw) {
                    // The wait for the host's first snapshot starts once the host pair is verified: ICE has its own deadline.
                    if (!online_snapshot_deadline_ && online_options_ && io_->transport.link(0) == net::LinkState::open)
                        online_snapshot_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    if (online_snapshot_deadline_ && std::chrono::steady_clock::now() >= *online_snapshot_deadline_)
                        set_failure(diagnostic("EAWR-NET-1536", "Online staging snapshot timed out."));
                    return;
                }
                auto decoded = net::decode_lobby_frame(raw->frame);
                const auto* snapshot = decoded ? std::get_if<net::LobbySnapshot>(&decoded.value()) : nullptr;
                if (raw->from == 0 && decoded) if (const auto* refused = std::get_if<net::LobbyRejected>(&decoded.value())) {
                    set_failure(net::lobby_diagnostic(refused->reason, refused->message)); return;
                }
                if (!snapshot || raw->from != 0) { set_failure(net::lobby_diagnostic(net::LobbyReason::malformed, "Missing authoritative admission snapshot.")); return; }
                auto created = net::LobbySession::join(io_->transport, snapshot->room, 0, lobby_hooks());
                if (!created) { set_failure(created.error()); return; }
                lobby_ = std::make_shared<net::LobbySession>(std::move(created).value()); io_->room_lifetime = lobby_; lobby_self_ = io_->transport.self();
                online_snapshot_deadline_.reset();
                io_->transport.pending_bytes += raw->frame.size(); io_->transport.pending.push_back(std::move(*raw));
            }
        }
        // Closing is deferred until the core has queued its typed notice.
        flush_lobby_closes();
        if (view().host) {
            if (io_->transport.peer_count() == 2 && lobby_->snapshot().phase == net::LobbyPhase::staging && lobby_->snapshot().seats.size() == 1)
                (void)lobby_feedback(io_->transport.tcp->reopen_staging_listener());
            // Polling advances admission even when there is no application frame.
            auto frame = io_->transport.tcp->poll();
            if (frame) { io_->transport.pending_bytes += frame->frame.size(); io_->transport.pending.push_back(std::move(*frame)); }
            const auto& snapshot = lobby_->snapshot();
            auto options = decode_lan_lobby_game(snapshot);
            if (!options) { set_failure(options.error()); return; }
            for (net::PeerIndex peer = 1; peer < io_->transport.peer_count(); ++peer) {
                if (io_->transport.link(peer) != net::LinkState::open) {
                    // A room slot keeps its roster identity when its occupant returns, so a removal cannot outlive the closed pair
                    // (a leave, a refusal or a retry would otherwise be answered "removed from the room"). The service's own
                    // denylist covers a kicked player.
                    if (io_->transport.tcp->mesh()) { removed_peers_.erase(peer); known_edges_.erase(peer); }
                    continue;
                }
                if (std::any_of(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == peer; })) continue;
                const auto edge = room_edge(peer);
                if (known_edges_[peer] != edge) { known_edges_[peer] = edge; removed_peers_.erase(peer); }
                if (removed_peers_.contains(peer)) {
                    auto rejected = net::encode_lobby_frame(net::LobbyRejected{{lobby_->snapshot().room, lobby_->snapshot().revision, net::LobbyReason::denied, "This connection was removed from the room."}});
                    if (rejected) (void)io_->transport.send(peer, rejected.value());
                    closing_peers_.insert(peer); continue;
                }
                auto row = options.value().slots->end();
                for (auto candidate = options.value().slots->begin(); candidate != options.value().slots->end(); ++candidate) {
                    if (std::find(owners_.begin(), owners_.end(), candidate->slot) == owners_.end()
                        || std::any_of(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.selection.slot == candidate->slot; })) continue;
                    if (row == options.value().slots->end() || candidate->slot < row->slot) row = candidate;
                }
                if (row == options.value().slots->end()) {
                    if (online_options_) { set_failure(diagnostic("EAWR-NET-1535", "Online staging has no vacant player row for this seat.")); return; }
                    continue;
                }
                auto admitted = lobby_->admit(peer, admission_identity(peer, edge),
                    {lan_admission_name(lobby_->snapshot(), peer), row->faction, row->colour_index.value_or(row->slot - 1), row->team, row->slot}, row->slot, lobby_->snapshot().revision);
                if (!admitted) {
                    const auto reason = lan_lobby_reason(admitted.error());
                    auto rejected = net::encode_lobby_frame(net::LobbyRejected{{lobby_->snapshot().room, lobby_->snapshot().revision, reason, admitted.error().message}});
                    if (rejected) (void)io_->transport.send(peer, rejected.value());
                    closing_peers_.insert(peer); removed_peers_.insert(peer);
                    (void)lobby_feedback(std::move(admitted));
                }
            }
        }
        auto pumped = lobby_->pump(lobby_clock_); flush_lobby_closes(); if (!pumped) { set_failure(pumped.error()); return; }
        if (online_options_ && mesh_material_revision_ && lobby_->snapshot().phase == net::LobbyPhase::staging
            && *mesh_material_revision_ != lobby_->snapshot().revision) clear_mesh_repair_material();
        if (!view().host && lobby_->snapshot().phase == net::LobbyPhase::staging && lobby_clock_ >= identity_retry_at_ms_) {
            const auto local = std::find_if(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == io_->transport.self(); });
            if (local != lobby_->snapshot().seats.end() && local->selection.name != lobby_name_) {
                auto choice = local->selection; choice.name = lobby_name_;
                const bool shared = std::any_of(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& other) { return other.peer != local->peer && other.selection.slot == local->selection.slot; });
                if (!shared && lobby_colour_ < skirmish::multiplayer_colour_count) {
                    const auto choices = decode_lan_lobby_game(lobby_->snapshot());
                    if (choices && std::none_of(choices.value().slots->begin(), choices.value().slots->end(), [&](const auto& row) {
                        return row.slot != choice.slot && row.colour_index.value_or(row.slot - 1) == lobby_colour_;
                    })) choice.colour = lobby_colour_;
                }
                // A queued name request is not an acknowledgement: simultaneous guests can lose a revision race.
                // Retry against fresh authoritative choices at most once per core request-budget window.
                identity_retry_at_ms_ = lobby_clock_ + net::LobbyLimits{}.chat_window_ms;
                (void)lobby_select(std::move(choice));
            }
        }
        if (view().host && lobby_->snapshot().phase == net::LobbyPhase::staging) {
            // Commit requested owner mapping after the core validates the player's own slot request.
            const auto seats = lobby_->snapshot().seats;
            for (const auto& seat : seats) if (seat.player != seat.selection.slot)
                (void)lobby_feedback(lobby_->assign(seat.peer, seat.selection, seat.selection.slot, lobby_->snapshot().revision));
        }
        if (preparing_.valid() && preparing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto prepared = preparing_.get();
            if (prepared && preparing_revision_ == lobby_->snapshot().revision && ready_pending_) {
                prepared_setup_ = std::move(prepared).value(); prepared_revision_ = preparing_revision_; prepared_game_ = preparing_game_;
                (void)lobby_feedback(lobby_->request(net::LobbyAction::ready, prepared_revision_));
            } else if (!prepared) (void)lobby_feedback(lobby_fail<void>(net::LobbyReason::setup_failed, prepared.error().message));
            ready_pending_ = false;
        }
        std::uint64_t delay{};
        auto options = decode_lan_lobby_game(lobby_->snapshot(), &delay);
        if (options) {
            offered_delay_ = delay;
            auto canonical = encode_lan_lobby_game(options.value());
            if (prepared_setup_ && canonical && canonical.value() == prepared_game_) prepared_revision_ = lobby_->snapshot().revision;
            else if (prepared_setup_) prepared_setup_.reset();
            offered_ = std::move(options).value();
        }
        const auto state = lobby_state();
        if (!state) return;
        last_lobby_state_ = state;
        const auto local = std::find_if(state->snapshot.seats.begin(), state->snapshot.seats.end(), [&](const auto& seat) { return seat.peer == io_->transport.self(); });
        { const std::lock_guard lock(mutex_);
            view_.seat = io_->transport.self(); if (local != state->snapshot.seats.end()) view_.player = local->player;
            view_.connected_seats = static_cast<std::uint32_t>(state->snapshot.seats.size()); view_.connected = state->snapshot.seats.size() > 1;
            if (online_options_ && view_.connected && (view_.message == "Player connecting..." || view_.message == "Waiting for a player to join...")) view_.message.clear();
            if (online_options_ && view_.host) {
                // The room code stays on show while a slot is free; a full room has no use for it.
                const bool full = state->snapshot.seats.size() >= state->snapshot.capacity;
                view_.invitation_code = full ? std::string{} : online_alias_; view_.invitation_consumed = full;
            }
            view_.human_owners.clear(); for (const auto& seat : state->snapshot.seats) view_.human_owners.push_back(seat.player);
            view_.phase = LanPhase::setup; view_.start_enabled = state->start_enabled; view_.accept_enabled = state->ready_enabled;
            if (state->snapshot.phase == net::LobbyPhase::closed) { view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left; }
        }
        if (state->snapshot.phase == net::LobbyPhase::closed) {
            if (state->notice && state->notice->reason == net::LobbyReason::pair_unreachable) {
                set_failure(net::lobby_diagnostic(state->notice->reason, state->notice->message)); return;
            }
            erase_credentials();
            io_->transport.tcp->close(); (void)io_->transport.tcp->poll();
        }
        if (state->snapshot.phase == net::LobbyPhase::starting && !view().host && offered_) { accepted_ = true; pending_ = offered_; }
        if (io_->transport.tcp->diagnostic() && !view().connected) set_failure(*io_->transport.tcp->diagnostic());
    }

} // namespace eawr::presentation::godot_backend::lan_detail
