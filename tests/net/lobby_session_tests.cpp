#include "eawr/net/lobby_session.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "fake_transport.hpp"
#include "setup_test_support.hpp"
#include "eawr/skirmish/start.hpp"
#include "../support/temp_dir.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>

#include <iostream>
#include <map>
#include <set>

namespace {
using namespace setup_test;
using eawr::core::Result;
net::LobbySelection selection(unsigned peer) {
    return {"guest-" + std::to_string(peer), "mod-side-" + std::to_string(peer % 3), peer, peer % 3 + 1, peer + 1};
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
    bool mismatch{}, perturb{}, invalid_start{}, fail_provider{}, team_rules{}, fail_credential_offer{};
    std::optional<sim::MatchAgreement> host_identity, client_identity;
    // Online room mesh hooks (N4): the Start gate and the Lock before the commitment. Off unless `gate` is set.
    bool gate{}, mesh_ok{true}, lock_ok{true};
    unsigned contexts{}, builds{}, locks{}, lock_polls{}, lock_calls{};
    bool async_lock{}, async_confirm{}, async_refuse{}, early_lock{};
    // #6045: the offer hook answers "pending" for the peers in `held_offers` (a campaign derivation still running on a worker)
    // and, once `offer_fails` is set, fails with its own diagnostic instead.
    std::set<unsigned> held_offers;
    bool offer_fails{};
    unsigned offer_calls{};
    bool concurrent_prepare{}, bad_offer{};
    std::string bad_input_domain, bad_state_domain;
    unsigned bad_mounts{};
    bool alias_mount_names{};
    std::map<unsigned, unsigned> offers_by_peer, discarded;
    explicit Room(unsigned count = 2, std::size_t workers = 1, net::LobbyLimits limits = {}, bool early = false, bool concurrent = false)
        : network(count), executor(std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers)), streams(count, nullptr), early_lock(early) {
        concurrent_prepare = concurrent;
        for (unsigned peer = 0; peer < count; ++peer) transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
        sessions.push_back(require(net::LobbySession::host(*transports[0], "test-room", std::min(count, net::max_lobby_seats),
            selection(0), 1, {"data/art/maps/test.ted", {0}}, hooks(0), limits)));
        for (unsigned peer = 1; peer < count; ++peer) sessions.push_back(require(net::LobbySession::join(*transports[peer], "test-room", 0, hooks(peer), limits)));
        for (unsigned peer = 1; peer < std::min(count, net::max_lobby_seats); ++peer)
            require(host().admit(peer, "identity-" + std::to_string(peer), selection(peer), peer + 1, revision()));
        rounds(3);
    }
    net::LobbySession& host() { return sessions[0]; }
    net::LobbySession& client() { return sessions[1]; }
    std::uint64_t revision() { return host().snapshot().revision; }
    net::LobbyHooks hooks(unsigned peer) {
        net::LobbyHooks value;
        if (concurrent_prepare) {
            value.preparation_mounts = [this, peer](const net::LobbySnapshot&, const net::SetupOffer* prepared) {
                std::vector<eawr::core::ContentLayerIdentity> layers{
                    {"Fixture leaf", sim::identity_domain("leaf", "content A").digest},
                    {"Fixture parent", sim::identity_domain("parent", "content B").digest}};
                if (peer == 1 && alias_mount_names) { layers[0].name = "Workshop copy"; layers[1].name = "Manual folder"; }
                if (peer == 1 && ((bad_mounts == 1 && !prepared) || (bad_mounts == 2 && prepared))) layers[0].digest[0] ^= 1;
                if (peer == 1 && bad_mounts == 3) std::swap(layers[0], layers[1]);
                if (peer == 1 && bad_mounts == 4 && !prepared) return Result<std::vector<eawr::core::ContentLayerIdentity>>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, ""));
                if (peer == 1 && bad_mounts == 5 && !prepared) return Result<std::vector<eawr::core::ContentLayerIdentity>>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, " 	 "));
                return Result<std::vector<eawr::core::ContentLayerIdentity>>::success(std::move(layers));
            };
            value.preparation_inputs = [this, peer](const net::LobbySnapshot&, const std::vector<net::SetupSeat>&) {
                sim::MatchIdentity identity;
                for (const auto* domain : {"build", "mod-order", "mod-bytes", "roster", "seats", "seed", "options", "rules"})
                    identity.domains.push_back(sim::identity_domain(domain, peer == 1 && bad_input_domain == domain ? "different" : "same"));
                return Result<std::vector<sim::IdentityDomain>>::success(sim::compute_match_identity(std::move(identity)).domains);
            };
            value.preparation_domains = [this, peer](const net::SetupOffer&) {
                sim::MatchIdentity identity;
                for (const auto* domain : {"gc-ai-controllers", "gc-prepared-owner", "gc-prepared-provider", "gc-tick-zero"})
                    identity.domains.push_back(sim::identity_domain(domain, peer == 1 && bad_state_domain == domain ? "different" : "same"));
                return Result<std::vector<sim::IdentityDomain>>::success(sim::compute_match_identity(std::move(identity)).domains);
            };
            value.discard_preparation = [this, peer] { ++discarded[peer]; };
        }
        value.validate = [this](const net::LobbySnapshot& snapshot, bool starting) {
            if (snapshot.game.map != "data/art/maps/test.ted" || snapshot.game.options.size() != 1
                || snapshot.game.options[0] > 5 || (starting && invalid_start))
                return Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "synthetic map/options/team validation"));
            for (const auto& seat : snapshot.seats) if (seat.selection.name.empty() || seat.selection.faction == "unavailable"
                || seat.selection.colour > 100 || seat.selection.team > 100 || seat.selection.slot > 100)
                return Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "synthetic loaded choice validation"));
            if (starting && team_rules) {
                std::map<std::uint32_t, std::string> teams;
                for (const auto& seat : snapshot.seats) {
                    const auto [team, inserted] = teams.emplace(seat.selection.team, seat.selection.faction);
                    if (!inserted && team->second != seat.selection.faction)
                        return Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
                            "Players sharing a team must choose the same faction."));
                }
                if (teams.size() < 2) return Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
                    "Choose at least two different teams."));
            }
            return Result<void>::success();
        };
        value.offer = [this, peer](const net::LobbySnapshot& snapshot, const std::vector<net::SetupSeat>& roster) {
            ++offer_calls;
            ++offers_by_peer[peer];
            if (offer_fails) return Result<net::SetupOffer>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "synthetic derivation failure"));
            if (held_offers.contains(peer)) return Result<net::SetupOffer>::failure(net::start_pending_diagnostic("derivation running"));
            if (fail_credential_offer) {
                eawr::core::Diagnostic error; error.code = "EAWR-NET-1022"; error.message = "Partial credential row delivery failed.";
                return Result<net::SetupOffer>::failure(std::move(error));
            }
            auto initial = human_setup(static_cast<unsigned>(snapshot.capacity)); initial.seed += snapshot.game.options[0];
            for (auto& player : initial.players) player.team_id = (player.player_id - 1) % 3 + 1;
            for (auto& slot : initial.skirmish->slots) slot.human = std::any_of(roster.begin(), roster.end(), [&](const auto& seat) { return seat.player == slot.player; });
            for (const auto& seat : snapshot.seats) {
                auto& owner = initial.players[seat.player - 1]; owner.team_id = seat.selection.team;
                initial.skirmish->slots[seat.player - 1].colour_index = seat.selection.colour;
            }
            auto selected = offer(); selected.seed = initial.seed;
            if (host_identity) { selected.identity = *host_identity; initial.match_identity = host_identity->content; }
            selected.setup = require(net::encode_skirmish_setup(initial)); selected.seats = roster;
            if (bad_offer && peer == 1) ++selected.input_delay;
            return Result<net::SetupOffer>::success(std::move(selected));
        };
        value.setup = [this, peer](net::MessageTransport& transport, const net::LobbySnapshot&) {
            ++contexts;
            streams[peer] = &transport;
            if (fail_provider && peer == 1) return Result<net::LobbySetupContext>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "provider failed"));
            auto owner = std::make_shared<ProviderOwner>(executor, transport);
            auto local = peer == 0 ? host_identity.value_or(identity()) : client_identity.value_or(identity());
            if (mismatch && peer == 1) local.build = "different-build";
            auto supplied = providers(owner->monitor, *executor, std::move(local), perturb && peer == 1);
            auto build = supplied.build;
            supplied.build = [this, build = std::move(build)](const auto& selected) { ++builds; return build(selected); };
            net::LobbySetupContext context; context.providers = std::move(supplied); context.lifetime = std::move(owner);
            return Result<net::LobbySetupContext>::success(std::move(context));
        };
        value.remove_peer = [this](auto who, auto reason) { removed.emplace_back(who, reason); };
        value.mesh_complete = [this] {
            return gate && !mesh_ok ? Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::mesh_incomplete, "synthetic incomplete mesh"))
                : Result<void>::success();
        };
        value.lock_at_start = early_lock && peer == 0;
        value.lock_room_progress = [this] {
            ++lock_calls;
            if (!async_lock) return Result<bool>::success(true);
            ++lock_polls;
            if (async_refuse) {
                auto error = net::lobby_diagnostic(net::LobbyReason::mesh_incomplete, "Start raced a player joining.");
                error.code = "EAWR-NET-1774"; return Result<bool>::failure(std::move(error));
            }
            return Result<bool>::success(async_confirm);
        };
        value.lock_room = [this] {
            if (gate) ++locks;
            return gate && !lock_ok ? Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::mesh_incomplete, "synthetic lock refusal"))
                : Result<void>::success();
        };
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
void revised_layer_join() {
    const auto root = std::filesystem::temp_directory_path() / ("eawr-revised-lobby-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { eawr::testing::remove_temp_directory(root); } } cleanup{root};
    const auto base = root / "base";
    const auto revised = root / "revised";
    std::filesystem::create_directories(base / "xml");
    std::filesystem::create_directories(revised / "xml");
    std::ofstream(base / "xml" / "SelectionControls.xml") << "stock";
    std::ofstream(revised / "xml" / "SelectionControls.xml") << "presentation";
    const eawr::vfs::MountSpec stock{"base", base, "data", {}};
    const eawr::vfs::MountSpec layer{std::string(eawr::vfs::revised_layer_id), revised, "data", {}};
    const auto make_identity = [&](std::vector<eawr::vfs::MountSpec> mounts) {
        auto filesystem = require(eawr::vfs::Vfs::mount(mounts));
        auto data = require(eawr::skirmish::read_data_identity(filesystem));
        auto result = identity();
        for (auto& domain : result.content.domains) {
            if (domain.name == "ai-data") domain = sim::identity_domain("ai-data", data.ai_data_digest);
            if (domain.name == "script-manifest") domain = sim::identity_domain("script-manifest", data.script_manifest_digest);
        }
        result.content = sim::compute_match_identity(std::move(result.content));
        return result;
    };
    Room room;
    room.host_identity = make_identity({layer, stock});
    room.client_identity = make_identity({stock});
    room.commit();
    auto host = require(room.host().take_result());
    auto guest = require(room.client().take_result());
    expect(host.setup().adapter().state_hash() == guest.setup().adapter().state_hash(),
        "host with Revised and guest without agree on the simulation digest");
}
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
    auto bytes = require(net::encode_lobby_frame(snapshot)); bytes[1] = 1;
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
    invalid = snapshot; invalid.mounted_mods = {3, 1, 0, 0, 0, 0};
    const auto with_mods = require(net::encode_lobby_frame(invalid));
    expect(std::get<net::LobbySnapshot>(require(net::decode_lobby_frame(with_mods))).mounted_mods == invalid.mounted_mods,
        "mounted description survives the lobby snapshot");
    auto old_mods = with_mods;
    old_mods[old_mods.size() - 6] = 1;
    const auto old_result = net::decode_lobby_frame(old_mods);
    expect(!old_result && old_result.error().message.find("mod list version 1, this build needs version 2") != std::string::npos,
        "old lobby mod description gives a clear version diagnostic");
    auto forged_mods = with_mods;
    forged_mods[forged_mods.size() - 2] = 1;
    expect(!net::decode_lobby_frame(forged_mods), "an oversized mod count in a received snapshot is refused");
    invalid.mounted_mods = {3, 1, 1, 1, 0, 0};
    expect(!net::encode_lobby_frame(invalid), "excessive mounted mod count is refused");
    invalid.mounted_mods = {3, 1, 1, 0, 0, 0, 1, 255, 255, 255, 255};
    expect(!net::encode_lobby_frame(invalid), "excessive mounted mod string is refused");
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
    require(start.host().assign(2, coop, 3, start.revision())); start.rounds(3); start.all_ready();
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
    require(commitment.host().assign(2, coop, 3, commitment.revision())); commitment.rounds(3); commitment.propose();
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
void concurrent_staging_departures() {
    for (const auto count : {3U, 9U}) {
        Room room(count);
        const auto before = room.revision();
        for (unsigned peer = 1; peer < count; ++peer) room.network.open[peer] = false;
        require(room.host().pump(++room.network.round));
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.host().snapshot().seats.size() == 1
            && room.revision() == before + 1 && room.removed.size() == count - 1,
            "concurrent closed guest links become one vacancy revision without a host failure");
        for (unsigned peer = 1; peer < count; ++peer) {
            room.network.open[peer] = true;
            room.sessions[peer] = require(net::LobbySession::join(*room.transports[peer], "test-room", 0, room.hooks(peer)));
            require(room.host().admit(peer, "refill-" + std::to_string(peer), selection(peer), peer + 1, room.revision()));
        }
        room.rounds(3);
        room.commit();
    }
    Room starting(3);
    starting.propose();
    starting.network.open[1] = starting.network.open[2] = false;
    require(starting.host().pump(++starting.network.round));
    expect(starting.host().snapshot().phase == net::LobbyPhase::staging && !starting.host().setup_phase()
        && starting.host().snapshot().seats.size() == 1, "concurrent losses during Start still abort the frozen attempt");
}
std::string close_inside_start_drain(unsigned count, std::size_t workers) {
    Room room(count, workers);
    room.propose();
    const auto frozen = room.revision();
    bool closed{};
    room.network.before_poll = [&](auto peer) {
        if (peer == 0 && !closed) { room.network.open[count - 1] = false; closed = true; }
    };
    require(room.host().pump(++room.network.round));
    room.network.before_poll = {};
    const auto& staging = room.host().snapshot();
    expect(closed && staging.phase == net::LobbyPhase::staging && staging.revision == frozen + 1
        && staging.frozen_revision == 0 && staging.seats.size() == count - 1 && !room.host().setup_phase(),
        "a close observed inside the Start drain cancels to one fresh vacancy revision");
    expect(room.removed.size() == 1 && room.removed.front() == std::pair{net::PeerIndex{count - 1}, net::LobbyReason::link_closed},
        "the closed guest's credentials and stream are removed once");
    ++room.network.round;
    for (unsigned peer = 0; peer < count - 1; ++peer) {
        expect(!staging.seats[peer].ready, "Start cancellation clears every survivor's readiness");
        if (peer) require(room.sessions[peer].pump(room.network.round));
        expect(room.sessions[peer].snapshot() == staging && !room.sessions[peer].setup_phase(),
            "survivors receive the cancellation and authoritative vacancy snapshot");
    }
    room.network.inbox[count - 1].clear();
    room.network.open[count - 1] = true;
    room.sessions[count - 1] = require(net::LobbySession::join(*room.transports[count - 1], "test-room", 0, room.hooks(count - 1)));
    require(room.host().admit(count - 1, "replacement", selection(count - 1), count, room.revision()));
    room.rounds(3);
    room.commit();
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) results.push_back(require(session.take_result()));
    for (unsigned round = 0; round < 200; ++round) {
        ++room.network.round;
        for (auto& result : results) if (result.setup().adapter().completed_tick() < 10)
            require(result.setup().session().pump(room.network.round));
    }
    const auto hash = results.front().setup().adapter().state_hash();
    for (auto& result : results) expect(result.setup().adapter().completed_tick() == 10
        && result.setup().adapter().state_hash() == hash, "refilled Start reaches equal tick-10 hashes");
    return hash;
}
void cancel_send_failure() {
    Room room(3); room.propose();
    room.network.before_poll = [&](auto peer) { if (peer == 0) room.network.open[2] = false; };
    room.network.fail_send = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
        return from == 0 && to == 1 && bytes.front() == net::lobby_cancel_kind;
    };
    const auto pumped = room.host().pump(++room.network.round);
    expect(!pumped && pumped.error().code == "EAWR-TEST-FAKE-SEND",
        "an open survivor's cancellation send failure still propagates");
}
void mesh_refusal_and_start_gate() {
    // Owner decision 1 (#3669): a newcomer that cannot reach one player is refused, not the room.
    Room room(3);
    const auto before = room.revision();
    expect(!room.client().refuse_unreachable(2, 1, true), "only the host refuses a newcomer");
    require(room.host().refuse_unreachable(2, 1, true));
    room.rounds(3);
    expect(room.host().snapshot().seats.size() == 2 && room.host().snapshot().revision > before
        && room.client().snapshot() == room.host().snapshot(), "everyone else sees a roster revision without the newcomer");
    expect(room.removed.back() == std::pair{net::PeerIndex{2}, net::LobbyReason::pair_unreachable}, "the slot and stream are released with the typed reason");
    const std::string line = "guest-2 (Seat 3) could not connect to guest-1 (Seat 2) and was not admitted.";
    expect(!room.host().chat_history().empty() && room.host().chat_history().back().text == line
        && room.host().chat_history().back().seat == sim::SeatId{1}, "the host posts one room line naming both players");
    expect(!room.client().chat_history().empty() && room.client().chat_history().back().text == line, "the player it could not reach hears who and why");
    expect(room.host().notice() && room.host().notice()->reason == net::LobbyReason::pair_unreachable
        && room.host().notice()->message == line + " EAWR-NET-1762", "the host's notice carries the diagnostic ID");
    expect(room.sessions[2].snapshot().phase == net::LobbyPhase::closed && room.sessions[2].notice()
        && room.sessions[2].notice()->reason == net::LobbyReason::pair_unreachable
        && room.sessions[2].notice()->message == net::mesh_newcomer_refusal_text("guest-1", true), "the newcomer gets the curated refusal text");
    expect(net::lobby_diagnostic(net::LobbyReason::pair_unreachable, "").code == "EAWR-NET-1762"
        && net::lobby_diagnostic(net::LobbyReason::mesh_incomplete, "").code == "EAWR-NET-1764"
        && net::lobby_diagnostic(net::LobbyReason::recovery_unavailable, "").code == "EAWR-NET-1615", "new reasons take the 1760..1779 diagnostics");
    require(room.host().refuse_unreachable(2, 1, true)); // already gone: only the room line is posted
    expect(room.host().chat_history().back().text == "A player (Seat 3) could not connect to guest-1 (Seat 2) and was not admitted.", "a newcomer that never took a seat is still reported");
    require(room.host().admit(2, "identity-2", selection(2), 3, room.revision()));
    expect(room.host().snapshot().seats.size() == 3, "no ban: the freed slot takes the same identity again");
    expect(!room.host().refuse_unreachable(0, 1, true) && !room.host().refuse_unreachable(1, 1, true), "the host and the unreachable seat itself cannot be refused");
    Room no_relay(3);
    require(no_relay.host().refuse_unreachable(2, 1, false)); no_relay.rounds(3);
    expect(no_relay.sessions[2].notice() && no_relay.sessions[2].notice()->message.find("no relay is configured on this server") != std::string::npos,
        "the text says when the server has no relay");
    Room starting(2); starting.propose();
    require(starting.host().refuse_unreachable(1, 0, true)); starting.rounds(3);
    expect(starting.host().snapshot().phase == net::LobbyPhase::staging && starting.host().snapshot().seats.size() == 1,
        "a refusal during Start cancels Start and leaves staging");

    // Start is gated on completeness; the Lock comes with the commitment, so a cancelled Start leaves the room open.
    Room gated(2); gated.gate = true; gated.mesh_ok = false; gated.all_ready();
    const auto incomplete = gated.host().start(gated.revision());
    expect(!incomplete && incomplete.error().code == "EAWR-NET-1764" && gated.host().snapshot().phase == net::LobbyPhase::staging && gated.locks == 0,
        "Start is refused while the mesh is incomplete");
    gated.mesh_ok = true; gated.commit();
    expect(gated.locks == 1, "the room is locked exactly once, as Start commits");
    Room unlockable(2); unlockable.gate = true; unlockable.lock_ok = false; unlockable.propose();
    require(unlockable.client().accept_setup());
    for (unsigned i = 0; i < 50 && unlockable.host().snapshot().phase == net::LobbyPhase::starting; ++i) unlockable.round();
    expect(unlockable.host().snapshot().phase == net::LobbyPhase::staging && unlockable.locks == 1 && unlockable.host().notice()
        && unlockable.host().notice()->reason == net::LobbyReason::mesh_incomplete, "a refused Lock returns the room to staging");
}
std::string nine_seats_three_sides(std::size_t workers) {
    Room room(9, workers);
    expect(room.host().snapshot().capacity == 9 && room.host().snapshot().seats.size() == 9, "staging supports nine seats");
    std::set<sim::tactical::PlayerId> owners;
    for (const auto& seat : room.host().snapshot().seats) owners.insert(seat.player);
    expect(owners.size() == 9, "nine independently controlled human owners in 3v3v3 roster");
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
    require(vacant.host().admit(8, "replacement", selection(8), 9, vacant.revision())); vacant.rounds(3);
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
void editable_team_rules() {
    Room room; room.team_rules = true;
    const auto player = room.host().snapshot().seats[1].player;
    auto choice = room.host().snapshot().seats[1].selection;
    const auto opposing_team = choice.team;
    const auto shared_team = room.host().snapshot().seats.front().selection.team;
    expect(opposing_team != shared_team && choice.faction != room.host().snapshot().seats.front().selection.faction,
        "fixture begins with different factions on opposing teams");
    choice.team = shared_team;
    require(room.host().assign(1, choice, player, room.revision()));
    room.rounds(3);
    expect(room.host().snapshot() == room.client().snapshot() && !room.client().notice(),
        "mixed-faction staging edits are shared without an error event");
    choice.faction = "another-side";
    require(room.client().request(net::LobbyAction::select, room.revision(), choice)); room.rounds(3);
    expect(room.host().snapshot().seats[1].selection == choice && room.host().snapshot() == room.client().snapshot()
        && !room.client().notice(), "remote faction edit is applied despite invalid teams");
    const auto refused = room.host().request(net::LobbyAction::ready, room.revision());
    expect(!refused && refused.error().message == "Players sharing a team must choose the same faction."
        && !room.host().snapshot().seats.front().ready, "host Ready refuses with the team faction reason");
    require(room.client().request(net::LobbyAction::ready, room.revision())); room.rounds(3);
    expect(room.client().notice() && room.client().notice()->message == refused.error().message
        && !room.host().snapshot().seats[1].ready, "authoritative host refuses remote Ready with the same reason");
    const auto start = room.host().start(room.revision());
    expect(!start && start.error().message == refused.error().message, "Start reports setup reason before readiness");
    choice.faction = room.host().snapshot().seats.front().selection.faction;
    require(room.host().assign(1, choice, player, room.revision())); room.rounds(3);
    const auto one_team = room.host().request(net::LobbyAction::ready, room.revision());
    expect(!one_team && one_team.error().message == "Choose at least two different teams.", "same-faction one-team setup still blocks Ready");
    const auto one_team_start = room.host().start(room.revision());
    expect(!one_team_start && one_team_start.error().message == one_team.error().message, "one-team setup still blocks Start");
    choice.team = opposing_team;
    require(room.host().assign(1, choice, player, room.revision())); room.rounds(3);
    room.all_ready();
    expect(room.host().snapshot() == room.client().snapshot(), "corrected teams can Ready on the shared revision");
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
// #3929: a guest that sees the host's commitment before its own tick-zero barrier agrees must wait locally.
void guest_waits_for_own_barrier() {
    Room room(3);
    constexpr std::uint64_t delay_rounds = 40;
    room.network.fault = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
        eawr::testing::FakeDelivery delay;
        auto decoded = net::decode_lobby_frame(bytes);
        if (decoded) if (const auto* frame = std::get_if<net::LobbySetupFrame>(&decoded.value()))
            if (from == 1 && to == 2 && frame->frame.front() == net::desync_combined_kind) delay.ordered_delay = delay_rounds;
        return delay;
    };
    room.propose();
    for (unsigned peer = 1; peer < room.sessions.size(); ++peer) require(room.sessions[peer].accept_setup());
    auto& late = room.sessions[2];
    // The viewer's order: pump, and take the result the moment the room reads committed once the local barrier allows it.
    const auto viewer_take = [&](net::LobbySession& session) -> std::optional<net::LobbyResult> {
        if (session.snapshot().phase != net::LobbyPhase::committed || !session.result_ready()) return std::nullopt;
        return require(session.take_result());
    };
    unsigned waited = 0;
    for (unsigned i = 0; i < 30 && late.snapshot().phase != net::LobbyPhase::committed; ++i) room.round();
    expect(room.host().snapshot().phase == net::LobbyPhase::committed && late.snapshot().phase == net::LobbyPhase::committed,
        "the host commits on its own readiness while one guest's peer summary is still in flight");
    expect(late.setup_phase() != net::SetupPhase::ready && !late.result_ready() && !viewer_take(late),
        "the delayed guest's own barrier is still pending when it sees the commitment");
    // The pre-fix viewer took the result here; the setup layer's refusal is the bug's NET-0301.
    const auto premature = late.take_result();
    expect(!premature && premature.error().code == "EAWR-NET-0301" && !late.result_ready(),
        "taking the result before the local barrier is the NET-0301 race the viewer must not trigger");
    expect(late.snapshot().phase == net::LobbyPhase::committed && !late.notice(),
        "a premature transfer is refused without ending the committed room");
    std::optional<net::LobbyResult> taken;
    for (unsigned i = 0; i < 200 && !taken; ++i) { room.round(); ++waited; taken = viewer_take(late); }
    expect(taken && waited > 5, "the delayed guest transfers only after its own tick-zero barrier agrees");
    expect(taken->setup().adapter().completed_tick() == 0 && !late.notice(), "the barrier hand-off keeps tick zero and raises no notice");
}
// #3929 review F1: a committed guest whose own barrier never completes must end with the named setup code, not wait forever.
void committed_guest_barrier_failure_is_typed() {
    Room room(3);
    room.network.fault = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
        eawr::testing::FakeDelivery delay;
        auto decoded = net::decode_lobby_frame(bytes);
        if (decoded) if (const auto* frame = std::get_if<net::LobbySetupFrame>(&decoded.value()))
            if (from == 1 && to == 2 && frame->frame.front() == net::desync_combined_kind) delay.ordered_delay = 10'000'000;
        return delay;
    };
    room.propose();
    for (unsigned peer = 1; peer < room.sessions.size(); ++peer) require(room.sessions[peer].accept_setup());
    auto& late = room.sessions[2];
    for (unsigned i = 0; i < 30 && late.snapshot().phase != net::LobbyPhase::committed; ++i) room.round();
    expect(late.snapshot().phase == net::LobbyPhase::committed && !late.result_ready(),
        "the guest sees the commitment while the withheld peer summary keeps its barrier open");
    // Pump only the starved guest past the setup timeout, as its viewer driver does; the other seats stay out of this.
    room.network.round += 31'000;
    const auto pumped = late.pump(room.network.round);
    expect(!pumped && pumped.error().code == "EAWR-NET-0306", "a committed guest's barrier timeout returns the named setup code to the driver");
    expect(!late.result_ready() && !late.take_result(), "the failed barrier never hands over a result");
    expect(!late.pump(room.network.round + 1), "the failure repeats on every pump, so the driver cannot miss it");
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
void retained_transport_maps_room_peers() {
    Room room(3);
    require(room.host().kick(1, room.revision()));
    room.rounds(3);
    room.sessions.erase(room.sessions.begin() + 1);
    room.commit();
    auto host = require(room.host().take_result());
    auto guest = require(room.client().take_result());
    expect(&host.transport() == room.streams[0] && &guest.transport() == room.streams[2],
        "transferred result exposes the same stream used by its setup core");
    room.sessions.clear();
    expect(host.transport().self() == 0 && guest.transport().self() == 1
        && host.transport().peer_count() == 2, "retained stream maps sparse room peers to roster indices");
    room.network.open[2] = false;
    expect(host.transport().link(1) == net::LinkState::closed, "link state maps the roster index to the room peer");
    room.network.open[2] = true;
    const Bytes payload{0x42, 0x2a};
    require(host.transport().send(1, payload));
    ++room.network.round;
    std::optional<net::Received> received;
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        auto frame = guest.transport().poll();
        if (!frame) break;
        if (frame->frame == payload) { received = std::move(frame); break; }
    }
    expect(received && received->from == 0 && received->frame == payload,
        "the retained stream envelopes game frames and maps the receiving peer");
    require(guest.transport().send(0, payload));
    ++room.network.round;
    received.reset();
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        auto frame = host.transport().poll();
        if (!frame) break;
        if (frame->frame == payload) { received = std::move(frame); break; }
    }
    expect(received && received->from == 1 && received->frame == payload,
        "the retained stream maps the returning sender back to its roster index");
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
void credential_delivery_retry() {
    Room room(3); room.all_ready();
    const auto before = room.host().snapshot();
    room.fail_credential_offer = true;
    const auto started = room.host().start(before.revision);
    expect(!started && started.error().code == "EAWR-NET-1022", "partial credential delivery remains recoverable Start feedback");
    expect(started.error().message == "Connections are not ready. Everyone must accept the setup again before Start.",
        "the host is told that every player must accept again");
    expect(room.host().notice() && room.host().notice()->message == started.error().message,
        "the new feedback is retained as the host room notice");
    auto reset = before; ++reset.revision;
    for (auto& seat : reset.seats) seat.ready = false;
    expect(room.host().snapshot() == reset && room.removed.empty() && room.contexts == 0,
        "a failed key offer advances only revision/readiness, keeping the room and choices intact");
    room.rounds(3);
    for (const auto& session : room.sessions) expect(session.snapshot() == reset,
        "every guest receives the new staging revision that invalidates its partial key attempt");
    expect(!room.host().start(before.revision), "the failed credential revision cannot be reused");
    room.fail_credential_offer = false;
    room.commit();
    for (const auto& session : room.sessions) expect(session.snapshot().phase == net::LobbyPhase::committed,
        "the same room starts after every player accepts the fresh attempt");
}
void asynchronous_roster_lock() {
    for (const bool refuse : {false, true}) {
        Room room; room.async_lock = true; room.propose();
        require(room.client().accept_setup());
        for (unsigned i = 0; i < 50 && room.lock_polls == 0; ++i) room.round();
        expect(room.lock_polls != 0 && room.host().snapshot().phase == net::LobbyPhase::starting,
            "setup readiness alone does not commit an unconfirmed roster lock");
        room.rounds(5);
        expect(!room.host().result_ready() && !room.client().result_ready(), "pending service confirmation keeps every peer frozen");
        room.async_confirm = !refuse; room.async_refuse = refuse;
        room.rounds(30);
        if (refuse) {
            expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.client().snapshot().phase == net::LobbyPhase::staging,
                "a refused lock returns every peer to staging");
            expect(room.host().notice() && room.host().notice()->message == "Start raced a player joining.", "the refusal keeps its named race reason");
        } else expect(room.host().result_ready() && room.client().result_ready(), "only confirmed roster lock commits on all peers");
    }
}
void cancellation_during_roster_confirmation() {
    for (const bool kick : {false, true}) {
        Room room; room.async_lock = true; room.propose(); require(room.client().accept_setup());
        for (unsigned i = 0; i < 50 && room.lock_polls == 0; ++i) room.round();
        expect(room.lock_polls != 0, "the service Lock is pending before cancellation");
        if (kick) require(room.host().kick(1, room.revision()));
        else require(room.client().request(net::LobbyAction::unready, room.revision()));
        room.rounds(3);
        expect(room.host().snapshot().phase == net::LobbyPhase::closed && room.client().snapshot().phase == net::LobbyPhase::closed,
            "cancelling an uncertain remote Lock never reopens a potentially service-locked room");
        expect(room.host().notice() && room.host().notice()->message.find("create a new room") != std::string::npos,
            "late cancellation explains how to continue");
    }
}
void early_freeze_cancellation() {
    Room room(2, 1, {}, true); room.async_lock = true; room.all_ready();
    require(room.host().start(room.revision()));
    expect(room.lock_polls == 1 && room.host().snapshot().phase == net::LobbyPhase::starting,
        "Opt-in native roster freezes before publishing the Start snapshot");
    room.network.open[1] = false;
    static_cast<void>(room.host().pump(++room.network.round));
    expect(room.host().snapshot().phase == net::LobbyPhase::closed && !room.host().result_ready(),
        "A guest lost between early freeze and native mesh-open closes without half commitment");
    Room refused(2, 1, {}, true); refused.async_lock = refused.async_refuse = true; refused.all_ready();
    const auto before = refused.host().snapshot();
    const auto start = refused.host().start(refused.revision());
    expect(!start && start.error().code == "EAWR-NET-1774" && refused.host().snapshot() == before,
        "Proved admission race leaves the exact staging revision editable");
}
// #6045: the host's Start waits on an asynchronous offer hook with the room locked, and publishes when it answers.
void asynchronous_host_start() {
    {
        Room room; room.all_ready();
        room.held_offers = {0};
        const auto before = room.host().snapshot();
        require(room.host().start(room.revision()));
        expect(room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::staging,
            "#6045 a pending Start stays in staging on the wire");
        room.rounds(30);
        expect(room.host().start_pending() && room.client().snapshot().phase == net::LobbyPhase::staging && room.client().snapshot() == room.host().snapshot(),
            "#6045 every pump keeps the room serviced and the guest is not told Start before the offer exists");
        expect(room.offer_calls > 1, "#6045 each pump asks the offer hook again");
        // Locked: the host's own edits and a guest's edit are refused with the reason.
        const auto locked = room.host().set_game({"data/art/maps/test.ted", {1}}, room.revision());
        expect(!locked && locked.error().message.find("preparing") != std::string::npos, "#6045 host edits are refused while the Start is pending");
        expect(!room.host().start(room.revision()), "#6045 a second Start is refused while one is pending");
        require(room.client().request(net::LobbyAction::select, room.revision(), selection(5)));
        room.rounds(3);
        expect(room.client().notice() && room.client().notice()->message.find("preparing") != std::string::npos,
            "#6045 a guest's edit is refused and says why the room is locked");
        bool told = false;
        for (const auto& line : room.client().chat_history()) told = told || line.text.find("preparing the campaign") != std::string::npos;
        expect(told, "#6045 the room is told once that the host is preparing");
        expect(room.host().snapshot().revision == before.revision && room.host().snapshot().game == before.game, "#6045 the locked room did not change");
        // The derivation finishes: Start publishes on the next pump and the match proceeds as usual.
        room.held_offers.clear();
        room.rounds(3);
        expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::starting
            && room.client().snapshot().phase == net::LobbyPhase::starting, "#6045 the answered offer publishes Start on both peers");
        for (unsigned peer = 1; peer < room.sessions.size(); ++peer) require(room.sessions[peer].accept_setup());
        for (unsigned i = 0; i < 50 && room.client().snapshot().phase != net::LobbyPhase::committed; ++i) room.round();
        expect(room.host().snapshot().phase == net::LobbyPhase::committed && room.client().snapshot().phase == net::LobbyPhase::committed,
            "#6045 a Start that waited commits like any other");
    }
    {
        // Cancel: the room is editable again and everyone is told.
        Room room; room.all_ready();
        room.held_offers = {0};
        require(room.host().start(room.revision()));
        room.rounds(5);
        require(room.host().cancel_start());
        room.rounds(3);
        expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::staging, "#6045 Cancel ends the pending Start");
        expect(!room.host().cancel_start(), "#6045 Cancel with nothing pending is refused");
        bool told = false;
        for (const auto& line : room.client().chat_history()) told = told || line.text.find("Start was cancelled") != std::string::npos;
        expect(told, "#6045 the room is told the Start was cancelled");
        require(room.host().set_game({"data/art/maps/test.ted", {1}}, room.revision()));
        room.held_offers.clear();
        room.all_ready();
        require(room.host().start(room.revision()));
        room.rounds(3);
        expect(room.host().snapshot().phase == net::LobbyPhase::starting, "#6045 after Cancel a new Start publishes normally");
    }
    {
        // A guest leaving or withdrawing readiness cancels the wait; no Start is published for a room that changed.
        for (const bool leave : {true, false}) {
            Room room; room.all_ready();
            room.held_offers = {0};
            require(room.host().start(room.revision()));
            room.rounds(3);
            require(room.client().request(leave ? net::LobbyAction::leave : net::LobbyAction::unready, room.revision()));
            room.held_offers.clear();
            room.rounds(5);
            expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::staging,
                "#6045 a guest leaving or withdrawing readiness cancels the pending Start even when the offer is ready");
            expect(room.host().notice().has_value(), "#6045 the host keeps a notice saying why the Start was cancelled");
        }
    }
    {
        // The derivation fails: the wait ends with the failure's own message, the room stays open.
        Room room; room.all_ready();
        room.held_offers = {0};
        require(room.host().start(room.revision()));
        room.rounds(3);
        room.held_offers.clear(); room.offer_fails = true;
        room.rounds(3);
        expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::staging
            && room.host().notice() && room.host().notice()->message.find("synthetic derivation failure") != std::string::npos,
            "#6045 a failed derivation ends the wait and reports its failure");
    }
    {
        // #6117 review: a resumed Start refused by the room Lock (EAWR-NET-1774, the proved admission race) is not silent: the host
        // gets the refusal as its notice and the room is told, exactly as the synchronous Start reports it.
        Room room(2, 1, {}, true); room.async_lock = room.async_refuse = true; room.all_ready();
        room.held_offers = {0};
        require(room.host().start(room.revision()));
        expect(room.host().start_pending(), "#6045 the early-freeze room's Start waits on the offer hook");
        room.held_offers.clear();
        room.rounds(3);
        expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::staging
            && room.host().notice() && room.host().notice()->message.find("Start raced a player joining.") != std::string::npos,
            "#6045 a resumed Start refused by the Lock reports the refusal to the host");
        bool told = false;
        for (const auto& line : room.client().chat_history()) told = told || line.text.find("Start was cancelled") != std::string::npos;
        expect(told, "#6045 and the room is told the Start was cancelled");
    }
    {
        // Closing the room while waiting.
        Room room; room.all_ready();
        room.held_offers = {0};
        require(room.host().start(room.revision()));
        require(room.host().close());
        expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::closed, "#6045 closing the room ends the wait");
    }
}

// #6045: a guest's frozen setup waits on the offer hook without freezing the room, keeps the host's setup frames queued and
// proceeds when the hook answers.
void asynchronous_guest_setup() {
    Room room; room.all_ready();
    room.held_offers = {1};
    require(room.host().start(room.revision()));
    expect(!room.host().start_pending() && room.host().snapshot().phase == net::LobbyPhase::starting, "#6045 the host publishes Start (its offer is ready)");
    room.rounds(40);
    expect(room.client().snapshot().phase == net::LobbyPhase::starting && room.client().setup_pending() && !room.client().setup_phase(),
        "#6045 the guest is frozen for Start with its own setup pending, and its pump keeps returning");
    expect(room.offer_calls > 2, "#6045 the guest's pump asks the offer hook again");
    expect(!room.client().accept_setup(), "#6045 there is nothing to accept while the guest's setup is pending");
    room.held_offers.clear();
    room.rounds(3);
    expect(!room.client().setup_pending() && room.client().setup_phase() == net::SetupPhase::reviewing_offer,
        "#6045 the answered offer builds the guest's setup and the host's queued offer is reviewed");
    require(room.client().accept_setup());
    for (unsigned i = 0; i < 50 && room.client().snapshot().phase != net::LobbyPhase::committed; ++i) room.round();
    expect(room.host().snapshot().phase == net::LobbyPhase::committed && room.client().snapshot().phase == net::LobbyPhase::committed,
        "#6045 a guest that waited commits like any other");
    // The host cancelling the attempt while the guest waits clears the guest's pending setup.
    Room cancelled; cancelled.all_ready();
    cancelled.held_offers = {1};
    require(cancelled.host().start(cancelled.revision()));
    cancelled.rounds(5);
    require(cancelled.host().kick(1, cancelled.host().snapshot().revision));
    cancelled.rounds(3);
    expect(!cancelled.client().setup_pending(), "#6045 a pending guest setup ends when the room does");
}
void concurrent_campaign_preparation() {
    for (const auto workers : {1U, 8U}) for (const auto count : {2U, 3U, net::max_lobby_seats}) for (const bool host_first : {false, true}) {
        Room room(count, workers, {}, false, true);
        room.alias_mount_names = true;
        for (unsigned peer = 0; peer < count; ++peer) room.held_offers.insert(peer);
        room.network.fault = [](auto, auto, std::span<const std::uint8_t> bytes) {
            eawr::testing::FakeDelivery delivery;
            if (!bytes.empty() && bytes.front() == net::lobby_prepared_kind) {
                delivery.duplicate = true; delivery.ordered_delay = 2; delivery.duplicate_delay = 2;
            }
            return delivery;
        };
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(5);
        expect(room.host().start_pending() && room.contexts == 0 && room.builds == 0,
            "manifest freezes the room without starting setup or simulation");
        for (unsigned peer = 0; peer < count; ++peer) {
            expect(room.offers_by_peer[peer] > 0 && !room.sessions[peer].result_ready(), "every admitted peer prepares before any offer finishes");
            if (peer) expect(room.sessions[peer].setup_pending() && !room.sessions[peer].accept_setup(), "guest cannot accept an unprepared setup");
        }
        if (host_first) room.held_offers.erase(0);
        else for (unsigned peer = 1; peer < count; ++peer) room.held_offers.erase(peer);
        room.rounds(10);
        expect(room.host().start_pending() && room.contexts == 0, "either completion order waits for every peer's claim");
        for (unsigned peer = 1; peer < count; ++peer)
            require(room.sessions[peer].request(net::LobbyAction::snapshot, room.sessions[peer].snapshot().revision));
        room.rounds(10);
        expect(room.host().start_pending() && room.contexts == 0,
            "snapshot refreshes while either side prepares cannot release setup before unanimous claims");
        room.held_offers.clear(); room.rounds(20);
        expect(!room.host().start_pending() && room.contexts == count, "matching unanimous preparation enables ordinary setup");
        for (unsigned peer = 1; peer < count; ++peer) require(room.sessions[peer].accept_setup());
        room.rounds(80);
        for (const auto& session : room.sessions) expect(session.result_ready(), "all peers pass existing setup and tick-zero barriers after prepared agreement");
    }
    for (const auto* domain : {"build", "mod-order", "mod-bytes", "roster", "seats", "seed", "options", "rules",
             "gc-ai-controllers", "gc-prepared-owner", "gc-prepared-provider", "gc-tick-zero"}) {
        Room room(2, 1, {}, false, true);
        if (std::string_view(domain).starts_with("gc-")) room.bad_state_domain = domain;
        else room.bad_input_domain = domain;
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(12);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0,
            "manifest/state mismatch cancels before any adapter is built");
        expect(room.host().notice() && room.host().notice()->message.find(domain) != std::string::npos
            && room.host().notice()->message.find("peer 1") != std::string::npos, "mismatch diagnostic identifies peer and domain");
        expect(room.discarded[0] && room.discarded[1], "mismatch discards both candidates");
    }
    for (const auto bad : {1U, 2U, 3U}) {
        Room room(2, 1, {}, false, true); room.bad_mounts = bad;
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(12);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.contexts == 0 && room.builds == 0,
            "per-layer content/order mismatch refuses before setup and play");
        expect(room.host().notice() && room.host().notice()->message.find("Fixture leaf") != std::string::npos
            && room.host().notice()->message.find("peer 1") != std::string::npos,
            "preflight and prepared tactical mismatches name the differing mod and peer");
    }
    {
        Room room(2, 1, {}, false, true); room.bad_offer = true;
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(12);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0
            && room.host().notice() && room.host().notice()->message.find("sealed offer") != std::string::npos,
            "a final offer difference refuses even if additional state claims match");
    }
}

// Review #6288: a Start that prepares first takes the early roster lock once, not again after unanimous claims.
void concurrent_preparation_locks_once() {
    unsigned calls[2]{};
    for (const bool concurrent : {false, true}) {
        Room room(2, 1, {}, true, concurrent); room.all_ready();
        require(room.host().start(room.revision())); room.rounds(20);
        if (concurrent) expect(room.lock_calls == 1, "preparation freezes the roster exactly once, before the guests' claims arrive");
        for (unsigned peer = 1; peer < 2; ++peer) require(room.sessions[peer].accept_setup());
        room.rounds(80);
        for (const auto& session : room.sessions) expect(session.result_ready(), "Start with an early roster lock completes");
        calls[concurrent] = room.lock_calls;
    }
    expect(calls[true] == calls[false], "preparing before Start asks the service to lock the same number of times as the ordinary Start");
}

void concurrent_preparation_cleanup() {
    {
        Room room(2, 1, {}, false, true); room.held_offers = {0, 1};
        std::optional<net::LobbyPrepare> changed;
        room.network.fault = [&](auto, auto, std::span<const std::uint8_t> bytes) {
            eawr::testing::FakeDelivery delivery;
            if (!bytes.empty() && bytes.front() == net::lobby_prepare_kind) {
                changed = std::get<net::LobbyPrepare>(require(net::decode_lobby_frame(bytes))); delivery.drop = true;
            }
            return delivery;
        };
        room.all_ready(); require(room.host().start(room.revision()));
        expect(changed.has_value(), "host published a preparation manifest before work");
        auto snapshot = std::get<net::LobbySnapshot>(require(net::decode_lobby_frame(changed->snapshot)));
        snapshot.game.options[0] = 1; changed->snapshot = require(net::encode_lobby_frame(snapshot));
        room.network.fault = {}; room.inject(0, 1, *changed); room.rounds(6);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.offers_by_peer[1] == 0
            && room.host().notice() && room.host().notice()->message.find("manifest") != std::string::npos,
            "changed frozen choices are refused before guest preparation and cancel the entire attempt");
    }
    for (const bool malformed : {false, true}) {
        Room room(2, 1, {}, false, true); room.held_offers = {0};
        std::optional<net::LobbyPrepared> captured;
        room.network.before_send = [&](auto, auto, std::span<const std::uint8_t> bytes) {
            if (!bytes.empty() && bytes.front() == net::lobby_prepared_kind)
                captured = std::get<net::LobbyPrepared>(require(net::decode_lobby_frame(bytes)));
        };
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(5);
        expect(captured.has_value(), "guest prepared claim is available before the host finishes");
        room.network.before_send = {};
        if (malformed) {
            auto bytes = require(net::encode_lobby_frame(*captured)); bytes.back() ^= 1;
            require(room.transports[1]->send(0, bytes));
        } else {
            captured->domains.front().digest[0] ^= 1; room.inject(1, 0, *captured);
        }
        room.rounds(5);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0,
            "malformed or conflicting duplicate claims cancel everyone before setup");
    }
    for (const auto action : {net::LobbyAction::unready, net::LobbyAction::leave}) {
        Room room(3, 1, {}, false, true); room.held_offers = {0, 1, 2}; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(4);
        require(room.client().request(action, room.revision())); room.rounds(4);
        room.held_offers.clear(); room.rounds(4);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0 && !room.host().start_pending(),
            "withdrawal/departure wins over any later preparation result");
    }
    {
        Room room(2, 1, {}, false, true); room.held_offers = {0};
        std::optional<net::LobbyPrepare> old_manifest; std::optional<net::LobbyPrepared> old_claim;
        room.network.before_send = [&](auto, auto, std::span<const std::uint8_t> bytes) {
            if (bytes.empty() || (bytes.front() != net::lobby_prepare_kind && bytes.front() != net::lobby_prepared_kind)) return;
            const auto decoded = require(net::decode_lobby_frame(bytes));
            if (const auto* manifest = std::get_if<net::LobbyPrepare>(&decoded)) old_manifest = *manifest;
            if (const auto* claim = std::get_if<net::LobbyPrepared>(&decoded)) old_claim = *claim;
        };
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(8);
        expect(old_manifest && old_claim, "old attempt generated its own manifest and guest claim");
        const auto first_attempt = room.host().snapshot().start_attempt;
        room.network.before_send = {};
        require(room.host().cancel_start()); room.rounds(4);
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(4);
        expect(first_attempt != room.host().snapshot().start_attempt, "retry always creates a fresh attempt");
        room.inject(0, 1, *old_manifest); room.inject(1, 0, *old_claim);
        auto rebound = *old_claim; rebound.revision = room.revision(); room.inject(1, 0, rebound);
        room.inject(0, 1, net::LobbySetupFrame{"test-room", room.revision(), {net::setup_start_kind}, first_attempt});
        room.rounds(4);
        expect(room.host().start_pending() && room.builds == 0, "old manifests, claims and setup envelopes cannot complete a retry");
        room.held_offers.clear(); room.rounds(10);
        require(room.client().accept_setup()); room.rounds(50);
        expect(room.client().result_ready(), "clean retry can still complete after stale traffic is rejected");
    }
    {
        Room room(3, 1, {}, false, true); room.all_ready();
        room.network.fail_send = [](auto from, auto to, std::span<const std::uint8_t> bytes) {
            return from == 0 && to == 2 && !bytes.empty() && bytes.front() == net::lobby_prepared_kind;
        };
        require(room.host().start(room.revision()));
        unsigned failures{};
        for (unsigned round = 0; round < 12; ++round) {
            ++room.network.round;
            for (unsigned peer = 0; peer < room.sessions.size(); ++peer) {
                auto pumped = room.sessions[peer].pump(room.network.round);
                if (!pumped) { expect(peer == 0, "only the failed host release reports a send failure"); ++failures; }
            }
        }
        expect(failures == 1 && room.host().snapshot().phase == net::LobbyPhase::staging && room.contexts == 0
            && !room.host().result_ready() && !room.client().result_ready(),
            "partial unanimous-release delivery cancels the attempt without creating setup for a subset");
    }
    {
        Room room(2, 1, {}, false, true); room.held_offers = {0, 1}; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(4);
        const auto attempt = room.host().snapshot().start_attempt;
        require(room.client().request(net::LobbyAction::leave, room.revision())); room.rounds(4);
        room.sessions[1] = require(net::LobbySession::join(*room.transports[1], "test-room", 0, room.hooks(1)));
        require(room.host().admit(1, "new-admission", selection(1), 2, room.revision())); room.rounds(4);
        room.all_ready(); require(room.host().start(room.revision())); room.rounds(4);
        expect(room.host().snapshot().start_attempt != attempt, "reconnected transport index receives a new preparation attempt");
        room.held_offers.clear(); room.rounds(12); require(room.client().accept_setup()); room.rounds(50);
        expect(room.client().result_ready(), "disconnect/rejoin retries with the new admitted seat binding");
    }
    {
        net::LobbyLimits limits; limits.preparation_timeout_ms = 20;
        Room room(2, 1, limits, false, true); room.held_offers = {1}; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(30);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0 && room.host().notice()
            && room.host().notice()->message.find("timed out") != std::string::npos, "missing prepared claim times out without subset commit");
    }
    {
        // #6288 review: a hook's empty refusal text still reaches the host at once, with a fixed message.
        Room room(2, 1, {}, false, true); room.bad_mounts = 4; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(12);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0 && room.host().notice()
            && room.host().notice()->message.find("peer 1: The campaign could not be prepared on this computer.") != std::string::npos,
            "an empty guest refusal message cancels the attempt immediately with the fixed text");
    }
    {
        // #6288 review round 3: a whitespace-only refusal text is replaced by the same fixed text.
        Room room(2, 1, {}, false, true); room.bad_mounts = 5; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(12);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0 && room.host().notice()
            && room.host().notice()->message.find("peer 1: The campaign could not be prepared on this computer.") != std::string::npos,
            "a whitespace-only guest refusal message cancels the attempt immediately with the fixed text");
    }
    {
        // The host's own pending derivation still expires: the 30-minute bound is the only limit once nothing is exempt.
        net::LobbyLimits limits; limits.preparation_timeout_ms = 20;
        Room room(2, 1, limits, false, true); room.held_offers = {0, 1}; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(30);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0 && room.host().notice()
            && room.host().notice()->message.find("timed out") != std::string::npos, "a held host offer times out without subset commit");
    }
    {
        Room room(3, 1, {}, false, true); room.all_ready();
        room.network.fail_send = [](auto, auto to, std::span<const std::uint8_t> bytes) {
            return to == 2 && !bytes.empty() && bytes.front() == net::lobby_prepare_kind;
        };
        expect(!room.host().start(room.revision()), "partial manifest broadcast reports its failure");
        room.rounds(6);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && !room.host().result_ready()
            && !room.client().result_ready() && room.builds == 0, "partial broadcast cannot commit the surviving subset");
    }
    {
        Room room(2, 1, {}, false, true); room.held_offers = {0, 1}; room.all_ready();
        require(room.host().start(room.revision())); room.rounds(4); room.network.open[1] = false;
        room.rounds(4);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.builds == 0, "disconnect cancels preparation before setup");
    }
    {
        Room room(2, 1, {}, true, true); room.all_ready(); room.held_offers = {0, 1};
        require(room.host().start(room.revision())); room.rounds(4); require(room.host().cancel_start()); room.rounds(4);
        expect(room.host().snapshot().phase == net::LobbyPhase::closed, "cancellation after irreversible roster lock requires room recreation");
    }
}

void preparation_codec() {
    Room room(2, 1, {}, false, true); room.all_ready(); room.held_offers = {0, 1};
    std::vector<Bytes> frames;
    room.network.before_send = [&](auto, auto, std::span<const std::uint8_t> bytes) {
        if (!bytes.empty() && (bytes.front() == net::lobby_prepare_kind || bytes.front() == net::lobby_prepared_kind))
            frames.emplace_back(bytes.begin(), bytes.end());
    };
    require(room.host().start(room.revision())); room.rounds(3); room.held_offers.erase(1); room.rounds(3);
    for (const auto& bytes : frames) {
        expect(require(net::encode_lobby_frame(require(net::decode_lobby_frame(bytes)))) == bytes, "preparation codec round trip is canonical");
        for (std::size_t size = 0; size < bytes.size(); ++size)
            expect(!net::decode_lobby_frame(std::span(bytes).first(size)), "truncated preparation frame is refused");
        auto old = bytes; old[1] = 2;
        const auto refused = net::decode_lobby_frame(old);
        expect(!refused && refused.error().message.find("version 3") != std::string::npos, "mixed lobby protocol refuses explicitly");
        auto trailing = bytes; trailing.push_back(0); expect(!net::decode_lobby_frame(trailing), "trailing preparation bytes are refused");
    }
    expect(frames.size() >= 2, "both preparation frame kinds were exercised");
    const auto decoded = require(net::decode_lobby_frame(frames.back()));
    auto claim = std::get<net::LobbyPrepared>(decoded);
    const auto original = require(net::encode_lobby_frame(claim));
    expect(!claim.layers.empty(), "prepared fixture carries actual layer claims");
    claim.layers.front().name = "Same content installed manually";
    const auto renamed = require(net::encode_lobby_frame(claim));
    expect(std::equal(original.end() - 32, original.end(), renamed.end() - 32),
        "local mod labels do not enter the prepared content checksum");
    auto tampered = original; tampered[tampered.size() - 33] ^= 1;
    expect(!net::decode_lobby_frame(tampered), "changed layer digest without a matching claim checksum is refused");
    claim.layers.front().digest = {};
    expect(!net::encode_lobby_frame(claim), "unavailable consumed-content checksum is refused");
    claim = std::get<net::LobbyPrepared>(decoded);
    claim.layers.resize(257, claim.layers.front());
    expect(!net::encode_lobby_frame(claim), "oversized preparation layer list is refused");
    claim = std::get<net::LobbyPrepared>(decoded);
    claim.domains.push_back(claim.domains.front());
    expect(!net::encode_lobby_frame(claim), "duplicate or unordered claim domains are refused");
}

int main() {
    try {
        concurrent_campaign_preparation(); concurrent_preparation_locks_once(); concurrent_preparation_cleanup(); preparation_codec();
        asynchronous_host_start(); asynchronous_guest_setup();
        revised_layer_join(); early_freeze_cancellation(); cancellation_during_roster_confirmation(); asynchronous_roster_lock(); credential_delivery_retry(); atomic_game_configuration(); codec(); authority_and_revisions(); chat_contracts(); recovery_and_budgets(); send_failures_and_envelopes();
        admission_kick_close(); concurrent_staging_departures(); cancel_send_failure(); editable_team_rules(); start_races(); guest_waits_for_own_barrier(); committed_guest_barrier_failure_is_typed(); mesh_refusal_and_start_gate(); retained_transport_maps_room_peers();
        std::string baseline, nine_baseline, close_three_baseline, close_nine_baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto close_three = close_inside_start_drain(3, workers);
            const auto close_nine = close_inside_start_drain(9, workers);
            if (close_three_baseline.empty()) close_three_baseline = close_three;
            if (close_nine_baseline.empty()) close_nine_baseline = close_nine;
            expect(close_three == close_three_baseline && close_nine == close_nine_baseline,
                "Start drain close/refill battles equal at 1/2/4/8 workers");
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
