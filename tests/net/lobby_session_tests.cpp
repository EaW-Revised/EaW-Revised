#include "eawr/net/lobby_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "fake_transport.hpp"
#include "setup_test_support.hpp"

#include <iostream>
#include <set>

namespace {
using namespace setup_test;
using eawr::core::Result;
net::LobbySelection selection(unsigned peer) {
    return {"guest-" + std::to_string(peer), "mod-side-" + std::to_string(peer % 3), peer, peer % 3 + 1, peer};
}
struct ProviderOwner {
    std::shared_ptr<eawr::platform::ThreadWorkerAdapter> executor;
    net::DesyncMonitor monitor;
    ProviderOwner(std::shared_ptr<eawr::platform::ThreadWorkerAdapter> workers, net::MessageTransport& transport)
        : executor(std::move(workers)), monitor(transport) {}
};
struct Room {
    eawr::testing::FakeNetwork network;
    std::shared_ptr<eawr::platform::ThreadWorkerAdapter> executor;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<net::LobbySession> sessions;
    std::vector<net::MessageTransport*> streams;
    std::vector<std::pair<net::PeerIndex, net::LobbyReason>> removed;
    bool mismatch{}, perturb{}, invalid_start{}, fail_provider{};
    unsigned contexts{}, builds{};
    explicit Room(unsigned count = 2, std::size_t workers = 1, net::LobbyLimits limits = {})
        : network(count), executor(std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers)), streams(count, nullptr) {
        for (unsigned peer = 0; peer < count; ++peer) transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
        sessions.push_back(require(net::LobbySession::host(*transports[0], "test-room", std::min(count, net::max_lobby_seats),
            selection(0), 1, {"data/art/maps/test.ted", {0}}, hooks(0), limits)));
        for (unsigned peer = 1; peer < count; ++peer) sessions.push_back(require(net::LobbySession::join(*transports[peer], "test-room", 0, hooks(peer), limits)));
        for (unsigned peer = 1; peer < std::min(count, net::max_lobby_seats); ++peer)
            require(host().admit(peer, "identity-" + std::to_string(peer), selection(peer), peer % 3 + 1, revision()));
        rounds(3);
    }
    net::LobbySession& host() { return sessions[0]; }
    net::LobbySession& client() { return sessions[1]; }
    std::uint64_t revision() { return host().snapshot().revision; }
    net::LobbyHooks hooks(unsigned peer) {
        net::LobbyHooks value;
        value.validate = [this](const net::LobbySnapshot& snapshot, bool starting) {
            if (snapshot.game.map != "data/art/maps/test.ted" || snapshot.game.options.size() != 1
                || snapshot.game.options[0] > 5 || (starting && invalid_start))
                return Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "synthetic map/options/team validation"));
            for (const auto& seat : snapshot.seats) if (seat.selection.name.empty() || seat.selection.faction == "unavailable"
                || seat.selection.colour > 100 || seat.selection.team > 100 || seat.selection.slot > 100)
                return Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "synthetic loaded choice validation"));
            return Result<void>::success();
        };
        value.offer = [](const net::LobbySnapshot& snapshot, const std::vector<net::SetupSeat>& roster) {
            auto initial = setup(); initial.seed += snapshot.game.options[0];
            for (auto& slot : initial.skirmish->slots) slot.human = std::any_of(roster.begin(), roster.end(), [&](const auto& seat) { return seat.player == slot.player; });
            for (const auto& seat : snapshot.seats) {
                auto& owner = initial.players[seat.player - 1]; owner.team_id = seat.selection.team;
                initial.skirmish->slots[seat.player - 1].colour_index = seat.selection.colour;
            }
            auto selected = offer(); selected.seed = initial.seed;
            selected.setup = require(net::encode_skirmish_setup(initial)); selected.seats = roster;
            return Result<net::SetupOffer>::success(std::move(selected));
        };
        value.setup = [this, peer](net::MessageTransport& transport, const net::LobbySnapshot&) {
            ++contexts;
            streams[peer] = &transport;
            if (fail_provider && peer == 1) return Result<net::LobbySetupContext>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "provider failed"));
            auto owner = std::make_shared<ProviderOwner>(executor, transport);
            auto local = identity(); if (mismatch && peer == 1) local.build = "different-build";
            auto supplied = providers(owner->monitor, *executor, std::move(local), perturb && peer == 1);
            auto build = supplied.build;
            supplied.build = [this, build = std::move(build)](const auto& selected) { ++builds; return build(selected); };
            net::LobbySetupContext context; context.providers = std::move(supplied); context.lifetime = std::move(owner);
            return Result<net::LobbySetupContext>::success(std::move(context));
        };
        value.remove_peer = [this](auto who, auto reason) { removed.emplace_back(who, reason); };
        return value;
    }
    void round(std::uint64_t delta = 1) {
        network.round += delta;
        for (auto& session : sessions) require(session.pump(network.round));
    }
    void rounds(unsigned count) { for (unsigned i = 0; i < count; ++i) round(); }
    void all_ready() {
        round(2000); // Human-paced actions begin a fresh bounded request window.
        for (auto& session : sessions) require(session.request(net::LobbyAction::ready, revision()));
        rounds(3);
        for (const auto& seat : host().snapshot().seats) expect(seat.ready, "every seat explicitly readies same revision");
    }
    void propose() { all_ready(); require(host().start(revision())); rounds(3); }
    void commit() {
        propose();
        for (unsigned peer = 1; peer < sessions.size(); ++peer) require(sessions[peer].accept_setup());
        for (unsigned i = 0; i < 50 && client().snapshot().phase != net::LobbyPhase::committed; ++i) round();
        expect(host().snapshot().phase == net::LobbyPhase::committed && client().snapshot().phase == net::LobbyPhase::committed, "host commits only after setup agreement");
    }
    void inject(unsigned from, unsigned to, net::LobbyFrame frame) { require(transports[from]->send(to, require(net::encode_lobby_frame(frame)))); }
};
void codec() {
    expect(net::sanitize_lobby_text("a\xe2\x80\x8e" "b\xe2\x80\x8f" "c\xd8\x9c" "d", 128) == "abcd", "all directional marks are stripped from room labels/chat");
    expect(net::lobby_diagnostic(net::LobbyReason::recovery_unavailable, "").code == "EAWR-NET-1615", "coordinator-reserved recovery refusal reason");
    Room room;
    const auto snapshot = room.host().snapshot();
    const std::vector<net::LobbyFrame> frames{
        snapshot, net::LobbyRequest{"test-room", snapshot.revision, net::LobbyAction::select, selection(1)},
        net::LobbyChatRequest{"test-room", "hello"}, net::LobbyChat{"test-room", 1, sim::SeatId{1}, "guest-0", "hello"},
        net::LobbySetupFrame{"test-room", snapshot.revision, {net::setup_start_kind}},
        net::LobbyCancel{{"test-room", snapshot.revision, net::LobbyReason::not_ready, "not ready"}},
        net::LobbyClosed{{"test-room", snapshot.revision, net::LobbyReason::host_closed, "closed"}},
        net::LobbyRejected{{"test-room", snapshot.revision, net::LobbyReason::stale_revision, "stale"}}
    };
    for (const auto& frame : frames) {
        auto bytes = require(net::encode_lobby_frame(frame));
        expect(require(net::encode_lobby_frame(require(net::decode_lobby_frame(bytes)))) == bytes, "canonical room frame round trip");
        for (std::size_t size = 0; size < bytes.size(); ++size) expect(!net::decode_lobby_frame(std::span(bytes).first(size)), "truncated room frame refused");
        bytes.push_back(0); expect(!net::decode_lobby_frame(bytes), "trailing room bytes refused");
    }
    expect(!net::decode_lobby_frame(Bytes(net::max_lobby_frame_bytes + 1)), "oversized frame rejected before allocation");
    auto bytes = require(net::encode_lobby_frame(snapshot)); bytes[1] = 2;
    expect(!net::decode_lobby_frame(bytes), "unsupported frame version");
    bytes = require(net::encode_lobby_frame(snapshot)); std::fill_n(bytes.begin() + 5, 4, std::uint8_t{255});
    expect(!net::decode_lobby_frame(bytes), "forged text length rejected");
    auto invalid = snapshot; invalid.seats[1].seat = invalid.seats[0].seat;
    expect(!net::encode_lobby_frame(invalid), "duplicate seat rejected");
    invalid = snapshot; invalid.seats[1].peer = invalid.host;
    expect(!net::encode_lobby_frame(invalid), "duplicate peer rejected");
    invalid = snapshot; invalid.phase = net::LobbyPhase::starting;
    expect(!net::encode_lobby_frame(invalid), "Start snapshot requires exact frozen revision and readiness");
    expect(net::sanitize_lobby_text("a\n\t\x1b" "b\xE2\x80\xAE" "c", 64) == "a   bc", "control and directional override sanitization");
    auto chat = require(net::encode_lobby_frame(net::LobbyChatRequest{"test-room", "a b"}));
    chat[chat.size() - 2] = 0;
    const auto repaired = std::get<net::LobbyChatRequest>(require(net::decode_lobby_frame(chat)));
    expect(repaired.text.find('\0') == std::string::npos, "embedded NUL text is sanitized at intake");
    invalid = snapshot; invalid.seats[1].selection.name = "GuEsT-0";
    expect(!net::encode_lobby_frame(invalid), "case-insensitive duplicate guest names refused");
}
void authority_and_revisions() {
    Room room;
    expect(room.host().snapshot() == room.client().snapshot(), "host owns authoritative room snapshots");
    expect(!room.client().set_game({"data/art/maps/test.ted", {1}}, room.revision())
        && !room.client().assign(0, selection(0), 1, room.revision()) && !room.client().start(room.revision())
        && !room.client().kick(0, room.revision()) && !room.client().close(), "host-only controls denied locally");
    room.all_ready(); const auto old_revision = room.revision();
    auto name = selection(1); name.name = "another guest";
    require(room.client().request(net::LobbyAction::select, old_revision, name)); room.rounds(3);
    expect(room.revision() > old_revision && room.host().snapshot().seats[0].ready && room.host().snapshot().seats[1].ready,
        "name-only change preserves all ready flags");
    require(room.client().chat("hello")); room.rounds(3);
    expect(room.host().snapshot().seats[1].ready, "chat preserves readiness");
    room.inject(1, 0, net::LobbyRequest{"test-room", old_revision, net::LobbyAction::unready, {}}); room.rounds(3);
    expect(room.host().snapshot().seats[1].ready && room.client().notice()->reason == net::LobbyReason::stale_revision, "stale requests cannot change room state");
    for (unsigned field = 0; field < 5; ++field) {
        room.all_ready(); const auto before = room.revision(); auto changed = room.host().snapshot().seats[1].selection;
        if (field == 0) ++changed.colour;
        if (field == 1) ++changed.team;
        if (field == 2) changed.faction = "mod-extra-side";
        if (field == 3) ++changed.slot;
        require(room.host().assign(1, changed, field == 4 ? 1 : 2, before)); room.rounds(3);
        expect(!room.host().snapshot().seats[0].ready && !room.host().snapshot().seats[1].ready && room.revision() > before,
            "colour/team/faction/slot/owner edits clear every ready flag");
    }
    room.all_ready(); require(room.host().set_game({"data/art/maps/test.ted", {1}}, room.revision())); room.rounds(3);
    expect(!room.client().snapshot().seats[1].ready, "advanced option changes clear readiness");
    const auto before = room.host().snapshot();
    expect(!room.host().set_game({"missing-map", {0}}, room.revision()), "loaded map validator rejects missing map");
    auto invalid = selection(1); invalid.faction = "unavailable";
    require(room.client().request(net::LobbyAction::select, room.revision(), invalid)); room.rounds(3);
    expect(room.host().snapshot() == before, "invalid choice leaves snapshot unchanged");
    auto duplicate = selection(1); duplicate.name = "GUEST-0";
    require(room.client().request(net::LobbyAction::select, room.revision(), duplicate)); room.rounds(3);
    expect(room.host().snapshot() == before, "duplicate guest name never changes authoritative state");
    room.inject(1, 0, room.host().snapshot()); room.rounds(3);
    expect(room.host().snapshot() == before, "joiner cannot forge authoritative snapshots");
    room.inject(1, 0, net::LobbyRequest{"another-room", room.revision(), net::LobbyAction::leave, {}}); room.rounds(3);
    expect(room.host().snapshot() == before, "room identity isolates requests");
    require(room.transports[1]->send(0, Bytes(net::max_lobby_frame_bytes + 1))); room.rounds(3);
    expect(room.host().snapshot() == before, "oversized sender frame never mutates room");
    expect(!room.host().pump(room.network.round - 1), "caller clock must be monotonic");
}
void chat_contracts() {
    net::LobbyLimits limits; limits.chat_history = 3;
    Room room(3, 1, limits);
    for (unsigned i = 0; i < 4; ++i) require(room.client().chat("line-" + std::to_string(i)));
    room.rounds(3);
    expect(room.host().chat_history().size() == 3 && room.client().chat_history() == room.host().chat_history()
        && room.sessions[2].chat_history() == room.host().chat_history(), "bounded host-sequenced chat is identical at every peer");
    expect(room.host().chat_history().front().sequence == 2 && room.host().chat_history().back().sequence == 4, "chat sequence survives history eviction");
    require(room.client().chat("rate-limited")); room.rounds(3);
    expect(room.host().chat_history().back().sequence == 4 && room.client().notice()->reason == net::LobbyReason::chat_limit, "host bounds per-peer chat rate");
    require(room.sessions[2].chat("independent budget")); room.rounds(3);
    expect(room.host().chat_history().back().sequence == 5, "different seats have independent chat budgets");
    std::string unicode;
    for (unsigned i = 0; i < 200; ++i) unicode += "\xE2\x82\xAC";
    room.round(2000); require(room.client().chat(unicode)); room.rounds(3);
    expect(room.host().chat_history().back().text.size() == 510, "local oversized chat truncates safely at a three-byte character boundary");
    room.inject(1, 0, net::LobbyChat{"test-room", 999, sim::SeatId{1}, "guest-0", "forged"}); room.rounds(3);
    expect(room.host().chat_history().back().sequence == 6, "joiner cannot assign chat sequence or sender");
    room.inject(0, 1, net::LobbyChat{"test-room", 8, sim::SeatId{1}, "guest-0", "gap"});
    room.rounds(4);
    expect(room.client().snapshot() == room.host().snapshot(), "chat gap requests and adopts a fresh authoritative cursor");
    require(room.host().chat("after resync")); room.rounds(3);
    expect(room.client().chat_history().back().sequence == 7, "chat continues in order after snapshot resync");
}

void recovery_and_budgets() {
    Room stale;
    bool dropped_snapshot{};
    stale.network.fault = [&](auto from, auto to, std::span<const std::uint8_t> bytes) {
        eawr::testing::FakeDelivery fault;
        if (!dropped_snapshot && from == 0 && to == 1 && bytes.front() == net::lobby_snapshot_kind) { fault.drop = true; dropped_snapshot = true; }
        return fault;
    };
    auto renamed = selection(0); renamed.name = "host renamed";
    require(stale.host().assign(0, renamed, 1, stale.revision())); stale.rounds(3);
    expect(stale.client().snapshot().revision < stale.revision(), "fixture withheld one snapshot");
    require(stale.client().request(net::LobbyAction::ready, stale.client().snapshot().revision)); stale.rounds(4);
    expect(stale.client().notice()->reason == net::LobbyReason::stale_revision && stale.client().snapshot() == stale.host().snapshot(),
        "stale rejection carries a fresh snapshot instead of stranding the client");

    Room gap;
    bool dropped_chat{};
    gap.network.fault = [&](auto from, auto to, std::span<const std::uint8_t> bytes) {
        eawr::testing::FakeDelivery fault;
        if (!dropped_chat && from == 0 && to == 1 && bytes.front() == net::lobby_chat_kind) { fault.drop = true; dropped_chat = true; }
        return fault;
    };
    require(gap.host().chat("missed")); require(gap.host().chat("gap")); gap.rounds(5);
    expect(gap.client().snapshot().chat_sequence == 2, "chat gap resynchronizes to the authoritative cursor");
    require(gap.host().chat("third")); gap.rounds(3);
    expect(gap.client().chat_history().back().sequence == 3, "resynchronized client receives subsequent chat");

    Room cursor;
    cursor.network.fault = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
        eawr::testing::FakeDelivery fault;
        fault.drop = from == 0 && to == 1 && bytes.front() == net::lobby_chat_kind; return fault;
    };
    require(cursor.host().chat("withheld")); require(cursor.host().assign(0, renamed, 1, cursor.revision())); cursor.rounds(5);
    expect(cursor.client().snapshot() == cursor.host().snapshot(), "snapshot cursor mismatch also initiates bounded resynchronization");

    Room no_op; no_op.all_ready();
    unsigned snapshots{};
    no_op.network.before_send = [&](auto from, auto, std::span<const std::uint8_t> bytes) { if (from == 0 && bytes.front() == net::lobby_snapshot_kind) ++snapshots; };
    require(no_op.client().request(net::LobbyAction::ready, no_op.revision())); no_op.rounds(3);
    expect(snapshots == 0, "no-op Ready never broadcasts a roster");
    no_op.round(2000);
    require(no_op.client().request(net::LobbyAction::unready, no_op.revision())); no_op.rounds(3);
    expect(snapshots == 1, "changed Unready broadcasts exactly once");
    snapshots = 0;
    require(no_op.client().request(net::LobbyAction::unready, no_op.revision())); no_op.rounds(3);
    expect(snapshots == 0, "no-op Unready never broadcasts a roster");
    no_op.round(2000);
    for (unsigned i = 0; i < 64; ++i) no_op.inject(1, 0, net::LobbyRequest{"test-room", no_op.revision(), net::LobbyAction::unready, {}});
    no_op.rounds(3);
    expect(snapshots == 0 && no_op.client().notice()->reason == net::LobbyReason::resource_limit, "request surplus has a typed budget refusal and no roster amplification");

    Room peers(3);
    peers.inject(2, 1, peers.host().snapshot()); peers.inject(2, 1, net::LobbyRequest{"test-room", peers.revision(), net::LobbyAction::leave, {}});
    peers.rounds(3);
    expect(peers.client().dropped_frames() == 2 && peers.client().snapshot() == peers.host().snapshot(), "joiner drops and counts peer-forged authority frames without failing pump");
}

void send_failures_and_envelopes() {
    Room snapshot_loss(4);
    bool failed{};
    snapshot_loss.network.fail_send = [&](auto from, auto to, std::span<const std::uint8_t> bytes) {
        if (!failed && from == 0 && to == 1 && bytes.front() == net::lobby_snapshot_kind) { failed = true; return true; } return false;
    };
    auto name = selection(0); name.name = "new host name";
    expect(!snapshot_loss.host().assign(0, name, 1, snapshot_loss.revision()), "injected snapshot send failure is reported");
    snapshot_loss.rounds(4);
    expect(snapshot_loss.host().snapshot().seats.size() == 3 && snapshot_loss.sessions[2].snapshot() == snapshot_loss.host().snapshot()
        && snapshot_loss.sessions[3].snapshot() == snapshot_loss.host().snapshot(), "partial snapshot broadcast removes the failed seat and reaches every survivor");

    Room chat_loss(3);
    chat_loss.network.fail_send = [](auto from, auto to, std::span<const std::uint8_t> bytes) { return from == 0 && to == 1 && bytes.front() == net::lobby_chat_kind; };
    expect(!chat_loss.host().chat("send failure"), "injected chat send failure reported"); chat_loss.rounds(3);
    require(chat_loss.host().chat("survivor message")); chat_loss.rounds(3);
    expect(chat_loss.sessions[2].chat_history().back().sequence == 2 && chat_loss.sessions[2].snapshot() == chat_loss.host().snapshot(), "chat failure does not strand a later survivor behind a cursor gap");

    Room kick;
    kick.network.fail_send = [](auto from, auto to, std::span<const std::uint8_t> bytes) { return from == 0 && to == 1 && bytes.front() == net::lobby_closed_kind; };
    require(kick.host().kick(1, kick.revision()));
    expect(kick.host().snapshot().seats.size() == 1 && kick.removed.back().second == net::LobbyReason::kicked
        && !kick.host().admit(1, "identity-1", selection(1), 2, kick.revision()), "failed kick notice cannot retain the seat, identity or reservation");

    Room start(3);
    auto coop = selection(2); coop.team = 1;
    require(start.host().assign(2, coop, 1, start.revision())); start.rounds(3); start.all_ready();
    start.network.fail_send = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
        auto decoded = net::decode_lobby_frame(bytes);
        const auto* snapshot = decoded ? std::get_if<net::LobbySnapshot>(&decoded.value()) : nullptr;
        return from == 0 && to == 1 && snapshot && snapshot->phase == net::LobbyPhase::starting;
    };
    expect(!start.host().start(start.revision()), "failed Start announcement reports the transport failure"); start.rounds(4);
    expect(start.host().snapshot().phase == net::LobbyPhase::staging && !start.host().setup_phase()
        && start.sessions[2].snapshot() == start.host().snapshot(), "failed Start announcement aborts and returns all survivors to staging");
    start.round(2000); require(start.host().request(net::LobbyAction::ready, start.revision()));
    require(start.sessions[2].request(net::LobbyAction::ready, start.revision())); start.rounds(3);
    require(start.host().start(start.revision()));
    expect(start.host().snapshot().phase == net::LobbyPhase::starting, "host can retry Start after announcement failure");

    Room commitment(3);
    require(commitment.host().assign(2, coop, 1, commitment.revision())); commitment.rounds(3); commitment.propose();
    unsigned commit_sends{};
    commitment.network.fail_send = [&](auto from, auto to, std::span<const std::uint8_t> bytes) {
        auto decoded = net::decode_lobby_frame(bytes);
        const auto* snapshot = decoded ? std::get_if<net::LobbySnapshot>(&decoded.value()) : nullptr;
        const bool committing = from == 0 && snapshot && snapshot->phase == net::LobbyPhase::committed;
        if (committing) ++commit_sends;
        return from == 0 && to == 1 && (committing || bytes.front() == net::lobby_closed_kind);
    };
    require(commitment.client().accept_setup()); require(commitment.sessions[2].accept_setup());
    bool commit_failed{};
    for (unsigned i = 0; i < 50 && commitment.sessions[2].snapshot().phase != net::LobbyPhase::closed; ++i) {
        ++commitment.network.round;
        if (!commitment.host().pump(commitment.network.round)) commit_failed = true;
        require(commitment.client().pump(commitment.network.round)); require(commitment.sessions[2].pump(commitment.network.round));
    }
    expect(commit_failed && commit_sends == 2 && commitment.host().snapshot().phase == net::LobbyPhase::closed
        && commitment.sessions[2].snapshot().phase == net::LobbyPhase::closed && !commitment.host().take_result(), "commit failure closes all reachable survivors even when the failed seat also rejects Closed");

    Room envelope; envelope.commit();
    auto host_result = require(envelope.host().take_result()), client_result = require(envelope.client().take_result());
    static_cast<void>(host_result); static_cast<void>(client_result);
    const auto batch = require(net::encode_batch({0, 1, 1, {{{1, sim::SeatId{1}, 0}, Bytes(net::max_batch_bytes - 41, 0xA5)}}}));
    expect(batch.size() == net::max_batch_bytes && net::decode_batch(batch).has_value(), "fixture is a maximal legal sealed batch");
    const auto before = envelope.network.inbox[1].size();
    require(envelope.streams[0]->send(1, batch));
    auto wrapped = require(net::decode_lobby_frame(envelope.network.inbox[1].back().received.frame));
    expect(envelope.network.inbox[1].size() == before + 1 && std::get<net::LobbySetupFrame>(wrapped).frame == batch,
        "maximal legal lockstep batch survives the post-transfer wrapper unchanged");
    require(envelope.streams[0]->send(1, Bytes(net::max_batch_bytes + 5, 0xA5)));
    expect(!envelope.streams[0]->send(1, Bytes(net::max_lobby_payload_bytes + 1, 0xA5)), "wrapper accepts the lockstep inbound bound and rejects excess payload");
}
void admission_kick_close() {
    Room room(3);
    expect(!room.host().admit(1, "extra", selection(1), 2, room.revision()), "occupied stream cannot get a second seat");
    expect(!room.host().kick(0, room.revision()), "host cannot kick itself");
    room.all_ready(); const auto departed_id = room.host().snapshot().seats[1].seat;
    require(room.host().kick(1, room.revision())); room.rounds(3);
    expect(room.client().snapshot().phase == net::LobbyPhase::closed && room.client().notice()->reason == net::LobbyReason::kicked
        && room.removed.back() == std::pair{net::PeerIndex{1}, net::LobbyReason::kicked}, "typed kick closes seat and invokes stream/reservation removal");
    expect(!room.host().snapshot().seats[0].ready && !room.host().snapshot().seats[1].ready, "membership changes clear ready");
    expect(!room.host().admit(1, "identity-1", selection(1), 2, room.revision()), "kicked admitted identity denied for room lifetime");
    require(room.host().admit(1, "new-identity", selection(1), 2, room.revision()));
    expect(room.host().snapshot().seats.back().seat != departed_id, "vacated seat cannot silently become another command identity");
    // New identity requires a fresh local room client, just as the bridge does.
    room.sessions[1] = require(net::LobbySession::join(*room.transports[1], "test-room", 0, room.hooks(1)));
    room.rounds(3); require(room.host().close()); room.rounds(3);
    expect(room.sessions[1].snapshot().phase == net::LobbyPhase::closed && room.sessions[2].snapshot().phase == net::LobbyPhase::closed,
        "host close ends staging for every admitted peer");
    Room leave;
    require(leave.client().request(net::LobbyAction::leave, leave.revision())); leave.rounds(3);
    expect(leave.host().snapshot().seats.size() == 1 && leave.client().notice()->reason == net::LobbyReason::left, "voluntary leave frees seat");
    Room lost; lost.network.open[0] = false; lost.rounds(2);
    expect(lost.client().snapshot().phase == net::LobbyPhase::closed && lost.client().notice()->reason == net::LobbyReason::host_closed, "host loss closes staging without election");
    Room lost_seat; lost_seat.network.open[1] = false; lost_seat.rounds(2);
    expect(lost_seat.host().snapshot().seats.size() == 1, "closed joiner stream frees its stable seat");
}
std::string nine_seats_three_sides(std::size_t workers) {
    Room room(9, workers);
    expect(room.host().snapshot().capacity == 9 && room.host().snapshot().seats.size() == 9, "staging supports nine seats");
    std::set<sim::tactical::PlayerId> owners;
    for (const auto& seat : room.host().snapshot().seats) owners.insert(seat.player);
    expect(owners.size() == 3, "three independently controlled human sides in 3v3v3 roster");
    room.commit();
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) {
        expect(session.snapshot().phase == net::LobbyPhase::committed, "nine-seat Start commits only after setup and tick-zero agreement");
        auto result = require(session.take_result());
        expect(result.setup().offer().seats.size() == 9, "each transferred setup retains all nine command seats");
        results.push_back(std::move(result));
    }
    for (std::size_t peer = 0; peer < results.size(); ++peer) {
        auto& result = results[peer];
        const auto& seat = result.setup().offer().seats[peer];
        const auto binding = tactical::SeatBinding{seat.seat, seat.player, true, false};
        require(result.setup().session().submit([binding](sim::SeatCommandKey key) {
            return net::encode_tactical_input(key, binding, {{binding.player}, tactical::AttackPayload{binding.player % 3 + 1}});
        }));
    }
    for (unsigned round = 0; round < 200; ++round) {
        ++room.network.round;
        for (auto& result : results) if (result.setup().adapter().completed_tick() < 12)
            require(result.setup().session().pump(room.network.round));
    }
    const auto hash = results.front().setup().adapter().state_hash();
    for (auto& result : results) expect(result.setup().adapter().completed_tick() == 12
        && result.setup().adapter().state_hash() == hash, "nine committed command seats advance identical battles");
    Room full(10);
    expect(!full.host().admit(9, "identity-9", selection(9), 1, full.revision()), "fixed connected room capacity bounds admission");
    require(full.host().kick(1, full.revision())); full.rounds(3);
    expect(!full.host().admit(9, "identity-1", selection(9), 1, full.revision()), "kick denial survives replacement transport identity");
    Room vacant(9, workers); vacant.all_ready();
    require(vacant.sessions[8].request(net::LobbyAction::leave, vacant.revision())); vacant.rounds(3);
    expect(vacant.host().snapshot().seats.size() == 8 && !vacant.host().start(vacant.revision()), "leaving ninth seat clears readiness");
    require(vacant.host().admit(8, "replacement", selection(8), 3, vacant.revision())); vacant.rounds(3);
    expect(vacant.host().snapshot().seats.size() == 9 && !vacant.host().snapshot().seats.back().ready, "vacant ninth seat admits a fresh unready controller");
    for (const bool kick : {false, true}) {
        Room race(9, workers); race.propose();
        if (kick) require(race.host().kick(8, race.revision()));
        else require(race.sessions[8].request(net::LobbyAction::leave, race.revision()));
        race.rounds(4);
        expect(race.host().snapshot().phase == net::LobbyPhase::staging && race.host().snapshot().seats.size() == 8,
            "ninth-seat kick or leave cancels Start before commitment");
        for (const auto& seat : race.host().snapshot().seats) expect(!seat.ready, "nine-seat Start race clears every controller's readiness");
    }
    return hash;
}
void start_races() {
    Room room;
    expect(!room.host().start(room.revision()), "host readiness required before Start");
    room.all_ready(); room.invalid_start = true;
    expect(!room.host().start(room.revision()), "loaded data/teams validated at Start"); room.invalid_start = false;
    require(room.host().start(room.revision())); room.rounds(3);
    expect(room.client().setup_phase() == net::SetupPhase::reviewing_offer && room.builds == 1 && !room.client().take_result(),
        "UI Ready does not accept/build client setup or permit stepping");
    expect(!room.host().set_game({"data/art/maps/test.ted", {1}}, room.revision()), "frozen offer is immutable");
    const auto frozen = room.revision();
    require(room.client().request(net::LobbyAction::unready, frozen)); room.rounds(3);
    expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.client().snapshot() == room.host().snapshot()
        && room.revision() > frozen && !room.client().setup_phase(), "Unready aborts Start and clears readiness at new revision");
    // Delayed old setup traffic cannot poison a second attempt.
    room.inject(1, 0, net::LobbySetupFrame{"test-room", frozen, {net::setup_accept_kind}});
    room.inject(0, 1, net::LobbySetupFrame{"test-room", frozen, {net::setup_start_kind}});
    room.rounds(3); room.commit();
    expect(!room.client().request(net::LobbyAction::unready, room.revision()) && !room.host().kick(1, room.revision()) && !room.host().close(),
        "after reliable commitment controls use session departure rather than staging mutation");
    auto host_result = require(room.host().take_result()), client_result = require(room.client().take_result());
    expect(host_result.setup().adapter().completed_tick() == 0 && client_result.setup().adapter().completed_tick() == 0
        && host_result.snapshot().frozen_revision == frozen + 1, "committed result freezes exact revision and tick-zero barrier");
    expect(!room.host().take_result() && !room.host().pump(room.network.round), "result transfer gives game exclusive transport ownership");
    Room leaving; leaving.propose(); require(leaving.client().request(net::LobbyAction::leave, leaving.revision())); leaving.rounds(3);
    expect(leaving.host().snapshot().phase == net::LobbyPhase::staging && leaving.host().snapshot().seats.size() == 1, "Leave before commitment aborts Start");
    Room kicking; kicking.propose(); require(kicking.host().kick(1, kicking.revision())); kicking.rounds(3);
    expect(kicking.host().snapshot().phase == net::LobbyPhase::staging && kicking.client().notice()->reason == net::LobbyReason::kicked, "Kick before commitment aborts Start");
    Room disconnected; disconnected.propose(); disconnected.network.open[1] = false; disconnected.rounds(3);
    expect(disconnected.host().snapshot().phase == net::LobbyPhase::staging && disconnected.host().snapshot().seats.size() == 1, "disconnect before commitment aborts Start");
    Room refusal; refusal.mismatch = true; refusal.propose(); expect(!refusal.client().accept_setup(), "content/build mismatch refuses setup acceptance"); refusal.rounds(4);
    expect(refusal.host().snapshot().phase == net::LobbyPhase::staging && !refusal.host().snapshot().seats[0].ready, "setup refusal returns everyone to staging");
    refusal.mismatch = false; refusal.commit(); expect(refusal.contexts == 4, "retry creates fresh setup providers and monitors");
    Room divergence; divergence.perturb = true; divergence.propose(); require(divergence.client().accept_setup()); divergence.rounds(10);
    expect(divergence.host().snapshot().phase == net::LobbyPhase::staging, "tick-zero mismatch never commits");
    Room provider; provider.fail_provider = true; provider.propose(); provider.rounds(6);
    expect(provider.host().snapshot().phase == net::LobbyPhase::staging, "failed joiner setup provider sends refusal and cancels attempt");
    Room barrier;
    // Delay client proof so the core Start is in flight while commitment waits.
    barrier.network.fault = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
        eawr::testing::FakeDelivery delay;
        auto decoded = net::decode_lobby_frame(bytes);
        if (decoded) if (const auto* frame = std::get_if<net::LobbySetupFrame>(&decoded.value()))
            if (from == 1 && to == 0 && frame->frame.front() == net::desync_combined_kind) delay.ordered_delay = 20;
        return delay;
    };
    barrier.propose(); require(barrier.client().accept_setup()); barrier.rounds(3);
    expect(barrier.host().snapshot().phase == net::LobbyPhase::starting, "tick-zero proof prevents early room commitment");
    require(barrier.client().request(net::LobbyAction::unready, barrier.revision())); barrier.rounds(25);
    expect(barrier.host().snapshot().phase == net::LobbyPhase::staging, "ordered Unready queued with final proof wins over commitment in same pump");
    net::LobbyLimits bounded; bounded.frames_per_pump = 1;
    Room batch(2, 1, bounded); batch.propose();
    require(batch.client().request(net::LobbyAction::unready, batch.revision())); batch.rounds(12);
    expect(batch.host().snapshot().phase == net::LobbyPhase::staging, "full inbound batch defers setup commitment until cancellation drains");
}
std::string battle(std::size_t workers) {
    Room room(2, workers); room.commit();
    auto host = require(room.host().take_result()), client = require(room.client().take_result());
    // LobbyResult owns its setup stream/providers even when the lobby handles go away.
    room.sessions.clear();
    const auto binding = tactical::SeatBinding{host.setup().offer().seats[0].seat, 1, true, false};
    require(host.setup().session().submit([&](sim::SeatCommandKey key) {
        return net::encode_tactical_input(key, binding, {{1}, tactical::AttackPayload{2}});
    }));
    for (unsigned i = 0; i < 150 && (host.setup().adapter().completed_tick() < 12 || client.setup().adapter().completed_tick() < 12); ++i) {
        ++room.network.round;
        if (host.setup().adapter().completed_tick() < 12) require(host.setup().session().pump(room.network.round));
        if (client.setup().adapter().completed_tick() < 12) require(client.setup().session().pump(room.network.round));
    }
    expect(host.setup().adapter().completed_tick() == 12 && client.setup().adapter().completed_tick() == 12, "committed wrapped stream runs battle");
    expect(host.setup().adapter().state_hash() == client.setup().adapter().state_hash(), "all peers agree after battle handoff");
    return host.setup().adapter().state_hash();
}
}
void atomic_game_configuration() {
    Room room(3); room.all_ready();
    const auto before = room.host().snapshot(); auto changed = before.seats;
    changed[1].selection.team = 2; changed[1].player = 2;
    require(room.host().configure_game({before.game.map, {1}}, changed, before.revision));
    expect(room.revision() == before.revision + 1, "map and repaired choices publish one revision");
    for (const auto& seat : room.host().snapshot().seats) expect(!seat.ready, "atomic game edit clears every ready");
    room.rounds(3);
    expect(room.client().snapshot() == room.host().snapshot(), "clients see only the validated whole candidate");
    const auto committed = room.host().snapshot();
    auto spoofed = committed.seats; ++spoofed[1].seat.value;
    expect(!room.host().configure_game(committed.game, spoofed, room.revision()), "atomic edit cannot replace stable seat identity");
    auto invalid = committed.seats; invalid[1].selection.faction = "unavailable";
    expect(!room.host().configure_game({committed.game.map, {2}}, invalid, room.revision()), "invalid candidate refuses before game mutation");
    expect(room.host().snapshot() == committed, "invalid edit has no partial game or roster commit");
    expect(!room.host().configure_game(committed.game, committed.seats, before.revision), "stale atomic edit is refused");
}
int main() {
    try {
        atomic_game_configuration(); codec(); authority_and_revisions(); chat_contracts(); recovery_and_budgets(); send_failures_and_envelopes();
        admission_kick_close(); start_races();
        std::string baseline, nine_baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto nine_hash = nine_seats_three_sides(workers);
            if (nine_baseline.empty()) nine_baseline = nine_hash;
            expect(nine_hash == nine_baseline, "nine-seat battles equal at 1/2/4/8 workers");
            const auto hash = battle(workers);
            if (baseline.empty()) baseline = hash;
            expect(hash == baseline, "staging-to-setup battle identical at 1/2/4/8 workers");
        }
        std::cout << "lobby contracts passed: authority/revisions, nine seats/three sides, bounded chat, Start races, setup/content/tick-zero and 1/2/4/8 battle\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
