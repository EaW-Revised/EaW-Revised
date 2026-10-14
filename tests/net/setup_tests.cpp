#include "setup_test_support.hpp"
#include "fake_transport.hpp"

#include <filesystem>
#include <array>
#include <iostream>

namespace {
using namespace setup_test;
struct Pair {
    eawr::testing::FakeNetwork network{2};
    eawr::testing::FakeTransport host_transport{network, 0}, client_transport{network, 1};
    sim::InlineExecutor executor;
    net::DesyncMonitor host_monitor, client_monitor;
    int builds{};
    net::SessionAdapter* client_adapter{};
    unsigned barrier_checks{};
    net::SetupSession host, client;
    Pair(const std::filesystem::path& directory, sim::MatchAgreement local = identity(), bool perturb = false,
        net::SetupOptions options = {}, std::optional<net::SetupOptions> client_options = {}, bool departures = false)
        : host_monitor(host_transport, monitor_options(directory / "host", options.session.input_delay)),
          client_monitor(client_transport, monitor_options(directory / "client", options.session.input_delay)),
          host(require(net::SetupSession::host(host_transport, configured_offer(options, departures), providers(host_monitor, executor), options))),
          client(require(net::SetupSession::join(client_transport,
              providers(client_monitor, executor, std::move(local), perturb, &builds, &client_adapter),
              client_options.value_or(options)))) {}
    static net::SetupOffer configured_offer(const net::SetupOptions& options, bool departures) {
        auto selected = offer(); selected.final_tick_count = options.session.final_tick_count;
        selected.input_delay = options.session.input_delay;
        if (departures) {
            auto initial = setup();
            initial.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2}};
            selected.setup = require(net::encode_skirmish_setup(initial));
        }
        return selected;
    }
    static net::DesyncOptions monitor_options(const std::filesystem::path& path, std::uint64_t delay) {
        net::DesyncOptions options;
        static_cast<void>(path); // Setup contracts use the monitor's bounded in-memory bundle.
        options.input_delay = delay;
        return options;
    }
    void round(bool success = true) {
        ++network.round;
        auto a = host.pump(network.round), b = client.pump(network.round);
        if (success) { require(std::move(a)); require(std::move(b)); }
        if (success && client.phase() == net::SetupPhase::barrier && !client_monitor.setup_agreed() && client_adapter) {
            expect(!client.take_result() && client_adapter->completed_tick() == 0, "client cannot step while tick-zero hash is withheld");
            ++barrier_checks;
        }
    }
    void ready() {
        for (unsigned i = 0; i < 100; ++i) {
            round();
            if (host.phase() == net::SetupPhase::ready && client.phase() == net::SetupPhase::ready) return;
        }
        throw std::runtime_error("setup never reached ready");
    }
    void refused() {
        for (unsigned i = 0; i < 100; ++i) {
            round(false);
            if (host.refusal() && client.refusal()) return;
        }
        throw std::runtime_error("refusal did not reach both peers");
    }
};
void encoding() {
    auto selected = offer();
    auto encoded = require(net::encode_setup_offer(selected));
    auto decoded = require(net::decode_setup_offer(encoded));
    expect(require(net::encode_setup_offer(decoded)) == encoded, "offer round trip must be canonical");
    expect(require(net::decode_skirmish_setup(decoded.setup)) == setup(), "SKSU and full setup survive offer");
    for (std::size_t size = 0; size < encoded.size(); ++size)
        expect(!net::decode_setup_offer(std::span(encoded).first(size)), "every truncated offer must be rejected");
    auto tampered = encoded; tampered[50] ^= 1;
    expect(!net::decode_setup_offer(tampered), "tampered setup refused before building");
    auto excess = encoded; excess.push_back(0);
    expect(!net::decode_setup_offer(excess), "trailing offer byte refused");
    auto legacy = encoded;
    legacy[1] = 1;
    legacy.erase(legacy.begin() + 21, legacy.begin() + 29); // v1 has no final tick field.
    const auto legacy_digest = eawr::core::sha256(std::span(legacy).first(legacy.size() - 32));
    std::copy(legacy_digest.begin(), legacy_digest.end(), legacy.end() - 32);
    expect(!net::decode_setup_offer(legacy), "literal v1-layout offer refused before build");
    // A self-consistent digest is not sufficient: bounds and semantic checks
    // still reject malicious version/length fields before allocation/build.
    for (const std::size_t offset : {std::size_t{1}, std::size_t{29}}) {
        auto forged = encoded;
        std::fill_n(forged.begin() + static_cast<std::ptrdiff_t>(offset), 4, std::uint8_t{255});
        const auto digest = eawr::core::sha256(std::span(forged).first(forged.size() - 32));
        std::copy(digest.begin(), digest.end(), forged.end() - 32);
        expect(!net::decode_setup_offer(forged), "forged version/text length refused even with valid integrity digest");
    }
    expect(!net::decode_setup_offer(Bytes(net::max_setup_bytes + 1, 40)), "oversize offer refused");
    for (std::size_t i = 0; i < encoded.size() - 32; ++i) {
        auto mutated = encoded; mutated[i] ^= 1;
        const auto digest = eawr::core::sha256(std::span(mutated).first(mutated.size() - 32));
        std::copy(digest.begin(), digest.end(), mutated.end() - 32);
        auto value = net::decode_setup_offer(mutated);
        expect(!value || require(net::encode_setup_offer(value.value())) == mutated,
            "valid-digest body mutation must reject or remain canonical");
    }
    selected.seats[1].seat = selected.seats[0].seat;
    expect(!net::encode_setup_offer(selected), "repeated seat refused");
    selected = offer(); selected.seats[1].player = 3;
    expect(!net::encode_setup_offer(selected), "AI slot cannot be claimed by a human peer");
    selected = offer(); selected.seats[1].peer = 0;
    expect(!net::encode_setup_offer(selected), "duplicate peer index refused");
    auto single = setup(); single.skirmish->slots[1].human = false;
    selected = offer(); selected.setup = require(net::encode_skirmish_setup(single));
    selected.seats[1].player = 1;
    auto roster = net::encode_setup_offer(selected);
    expect(static_cast<bool>(roster), "two independent coop seats may bind one human owner against AI");
    for (const auto count : {3U, 4U, 8U, 9U}) {
        selected = offer();
        selected.seats.clear();
        for (net::PeerIndex peer = 0; peer < count; ++peer)
            selected.seats.push_back({peer, sim::SeatId{peer + 1}, peer % 2 + 1});
        expect(static_cast<bool>(net::encode_setup_offer(selected)), "two human sides allow arbitrary seat splits up to nine");
        auto coop = setup(); coop.skirmish->slots[1].human = false;
        selected.setup = require(net::encode_skirmish_setup(coop));
        for (auto& seat : selected.seats) seat.player = 1;
        expect(static_cast<bool>(net::encode_setup_offer(selected)), "up to nine coop seats share one owner against AI");
    }
    selected.seats.push_back({9, sim::SeatId{10}, 1});
    expect(!net::encode_setup_offer(selected), "a tenth human seat is refused");
    auto three = setup(); three.skirmish->slots[2].human = true;
    selected = offer(); selected.setup = require(net::encode_skirmish_setup(three));
    selected.seats.push_back({2, sim::SeatId{12}, 3});
    const auto three_bytes = require(net::encode_setup_offer(selected));
    expect(require(net::decode_setup_offer(three_bytes)).seats == selected.seats, "three loaded human sides round trip");
    // A mod's fourth faction row stays loadable; only a fourth human side is refused.
    three.players.push_back({4, 4, 3, tactical::player_flag_commandable});
    three.skirmish->slots.push_back({4, false, 1, {"fleet-mod"}});
    selected.setup = require(net::encode_skirmish_setup(three));
    expect(require(net::decode_setup_offer(require(net::encode_setup_offer(selected)))).seats == selected.seats,
        "three humans with a fourth AI faction row remain loadable");
    three.skirmish->slots.back().human = true;
    selected.setup = require(net::encode_skirmish_setup(three));
    selected.seats.push_back({3, sim::SeatId{13}, 4});
    expect(!net::encode_setup_offer(selected), "a fourth human side is refused");
    auto allied = setup(); allied.players[1].team_id = allied.players[0].team_id;
    selected = offer(); selected.setup = require(net::encode_skirmish_setup(allied));
    expect(!net::encode_setup_offer(selected), "coop controllers must share their owner instead of duplicating allied assets");
    auto unopposed = setup(); unopposed.skirmish->slots[1].human = false;
    for (auto& player : unopposed.players) player.team_id = unopposed.players[0].team_id;
    selected = offer(); selected.setup = require(net::encode_skirmish_setup(unopposed));
    selected.seats[1].player = 1;
    expect(!net::encode_setup_offer(selected), "coop cannot start without an opposing AI side");
    selected = offer(); selected.seed++;
    expect(!net::encode_setup_offer(selected), "seed must agree with setup");
    selected = offer(); selected.final_tick_count = 0;
    expect(!net::encode_setup_offer(selected), "zero-length match refused");
    selected = offer(); selected.domain = "campaign-v1";
    expect(!net::encode_setup_offer(selected), "unimplemented mode domain refused");
    net::SetupRefusal refusal{net::SetupReason::tick_zero, "world/script", "tick-zero state differs",
        std::string(64, 'a'), std::string(64, 'b')};
    auto bytes = require(net::encode_setup_refusal(refusal));
    expect(require(net::decode_setup_refusal(bytes)) == refusal, "typed refusal round trip");
    for (std::size_t size = 0; size < bytes.size(); ++size)
        expect(!net::decode_setup_refusal(std::span(bytes).first(size)), "truncated refusal rejected");
}
void recovery_policy_encoding() {
    auto selected = offer();
    selected.recovery = {true, true, 17, 23, 45, 2U * 1024U * 1024U, 99};
    const auto bytes = require(net::encode_setup_offer(selected));
    expect(require(net::decode_setup_offer(bytes)).recovery == selected.recovery, "recovery flags and bounds are agreed with setup");
    selected.recovery.hard_cap_ms = 10;
    expect(!net::encode_setup_offer(selected), "invalid recovery cap rejected before admission");
    const std::array<std::uint64_t net::SetupRecoveryPolicy::*, 4> timeouts{
        &net::SetupRecoveryPolicy::grace_ms, &net::SetupRecoveryPolicy::progress_timeout_ms,
        &net::SetupRecoveryPolicy::hard_cap_ms, &net::SetupRecoveryPolicy::resync_timeout_ms};
    for (std::size_t index = 0; index < timeouts.size(); ++index) {
        selected = offer(); selected.recovery.hard_cap_ms = net::max_recovery_timeout_ms;
        selected.recovery.*timeouts[index] = net::max_recovery_timeout_ms;
        auto bounded = require(net::encode_setup_offer(selected));
        expect(static_cast<bool>(net::decode_setup_offer(bounded)), "maximum recovery interval is accepted");
        selected.recovery.*timeouts[index] = net::max_recovery_timeout_ms + 1;
        expect(!net::encode_setup_offer(selected), "excessive recovery interval rejected by encoder");
        // Forge the untrusted wire interval with a valid body digest.
        const auto offset = bounded.size() - 32 - 40 + index * 8 + (index == 3 ? 8 : 0);
        const auto value = net::max_recovery_timeout_ms + 1;
        for (unsigned byte = 0; byte < 8; ++byte) bounded[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
        const auto digest = eawr::core::sha256(std::span(bounded).first(bounded.size() - 32));
        std::copy(digest.begin(), digest.end(), bounded.end() - 32);
        expect(!net::decode_setup_offer(bounded), "excessive recovery interval rejected from authenticated offer bytes");
    }
}
void disabled_recovery_presets(const std::filesystem::path& directory) {
    for (const unsigned preset : {0U, 1U, 2U}) {
        net::SetupOptions local;
        if (preset != 1) local.session.reconnect.emplace();
        if (preset != 0) local.session.resync.emplace();
        Pair pair(directory / std::to_string(preset), identity(), false, {}, local);
        pair.refused();
        expect(pair.client.refusal()->reason == net::SetupReason::options
            && *pair.client.refusal() == *pair.host.refusal(),
            "both recovery flags off rejects any preset reconnect/resync on both peers");
    }
}
void text_contracts() {
    const std::string replacement = "\xEF\xBF\xBD";
    const std::vector<std::pair<std::string, std::string>> cases{
        {"a\xE2\x82", "a" + replacement}, {"\xC0\xAF", replacement + replacement},
        {"a\x80z", "a" + replacement + "z"}, {"\xED\xA0\x80", replacement + replacement + replacement},
        {std::string("a\0b", 3), "a" + replacement + "b"},
    };
    const auto word = [](Bytes& bytes, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto text = [&](Bytes& bytes, const std::string& value) {
        word(bytes, static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end());
    };
    const auto forge = [](Bytes bytes, const std::string& target, const std::string& bad, bool offer_digest) {
        const auto found = std::search(bytes.begin(), bytes.end(), target.begin(), target.end());
        expect(found != bytes.end() && bad.size() <= target.size(), "find forged text field");
        std::copy(bad.begin(), bad.end(), found);
        if (offer_digest) {
            const auto digest = eawr::core::sha256(std::span(bytes).first(bytes.size() - 32));
            std::copy(digest.begin(), digest.end(), bytes.end() - 32);
        }
        return bytes;
    };
    const auto selected = offer();
    const auto encoded = require(net::encode_setup_offer(selected));
    for (const auto& [raw, clean] : cases) {
        Bytes wire{net::setup_refuse_kind, static_cast<std::uint8_t>(net::SetupReason::identity)};
        text(wire, raw); text(wire, raw); text(wire, ""); text(wire, "");
        const auto decoded = require(net::decode_setup_refusal(wire));
        expect(decoded.domain == clean && decoded.message == clean, "raw refusal decode repairs UTF-8 and NUL");
        net::SetupRefusal refusal{net::SetupReason::identity, raw, raw, {}, {}};
        expect(require(net::decode_setup_refusal(require(net::encode_setup_refusal(refusal)))) == decoded,
            "refusal encoder uses the same repair rule");
        auto invalid_setup = setup(); invalid_setup.skirmish->match.space_win_condition = raw;
        expect(!net::encode_skirmish_setup(invalid_setup), "TCP setup encoder rejects malformed text");
        if (raw.find('\0') == std::string::npos) {
            const auto invalid_replay = require(tactical::write_replay({invalid_setup, 0, {}}));
            expect(!net::decode_skirmish_setup(invalid_replay), "complete raw malformed field is refused on TCP decode");
        }
        auto bad_build = selected; bad_build.identity.build = raw;
        expect(!net::encode_setup_offer(bad_build), "hashed build encoder rejects invalid text");
        expect(!net::decode_setup_offer(forge(encoded, selected.identity.build, raw, true)),
            "hashed build decoder rejects invalid text even with a valid offer digest");
        for (const auto& field : {std::string("test"), setup().skirmish->match.win_condition,
                setup().skirmish->match.space_win_condition, setup().skirmish->slots[0].fleet[0]}) {
            expect(!net::decode_skirmish_setup(forge(selected.setup, field, raw, false)),
                "TCP replay setup rejects malformed text without repair");
            expect(!net::decode_setup_offer(forge(encoded, field, raw, true)),
                "offer refuses malformed nested setup text");
        }
        expect(!net::decode_setup_offer(forge(encoded, "numeric-abi", raw, true)), "identity names reject invalid text");
    }
    const std::string unicode = "J\xC3\xBCrgen \xF0\x9F\x9A\x80";
    auto valid = setup(); valid.skirmish->map = "data/art/maps/" + unicode + ".ted";
    valid.skirmish->match.win_condition = unicode; valid.skirmish->match.space_win_condition = unicode;
    valid.skirmish->slots[0].fleet = {unicode};
    for (auto& domain : valid.match_identity->domains)
        if (domain.name == "map-path") domain = sim::identity_domain("map-path", valid.skirmish->map);
    valid.match_identity = sim::compute_match_identity(*valid.match_identity);
    const auto replay = require(tactical::write_replay({valid, 0, {}}));
    expect(require(net::encode_skirmish_setup(valid)) == replay, "valid non-ASCII replay bytes stay identical");
    expect(require(net::decode_skirmish_setup(replay)) == valid, "valid non-ASCII setup survives TCP decode exactly");
    auto unicode_offer = offer(); unicode_offer.setup = replay; unicode_offer.identity.build = unicode;
    unicode_offer.identity.content = *valid.match_identity;
    const auto unicode_bytes = require(net::encode_setup_offer(unicode_offer));
    expect(require(net::encode_setup_offer(require(net::decode_setup_offer(unicode_bytes)))) == unicode_bytes,
        "valid non-ASCII offer bytes and integrity hash stay identical");
    net::SetupRefusal limited{net::SetupReason::identity, std::string(127, 'a') + "\xC3\xA9",
        std::string(1023, 'b') + "\xF0\x9F\x9A\x80", {}, {}};
    auto bounded = require(net::decode_setup_refusal(require(net::encode_setup_refusal(limited))));
    expect(bounded.domain == std::string(127, 'a') && bounded.message == std::string(1023, 'b'),
        "refusal encoder cuts fields on character boundaries");
    Bytes boundary{net::setup_refuse_kind, static_cast<std::uint8_t>(net::SetupReason::identity)};
    text(boundary, std::string(127, 'a') + "\x80"); text(boundary, std::string(1023, 'b') + "\x80");
    text(boundary, ""); text(boundary, "");
    const auto repaired_boundary = require(net::decode_setup_refusal(boundary));
    expect(repaired_boundary.domain == std::string(127, 'a') && repaired_boundary.message == std::string(1023, 'b'),
        "refusal decoder bounds expanded replacements on character boundaries");
    expect(require(net::encode_setup_offer(require(net::decode_setup_offer(encoded)))) == encoded,
        "ordinary offer bytes and hashes are unchanged");
}
void runtime_maximum_lead() {
    for (const unsigned peers : {2U, 3U}) {
        eawr::testing::FakeNetwork network(peers);
        sim::InlineExecutor executor;
        auto initial = setup();
        initial.departure_policy.emplace();
        auto selected = offer();
        selected.final_tick_count = 40;
        for (unsigned peer = 0; peer < 2; ++peer) {
            initial.departure_policy->humans.push_back(peer + 1);
            initial.skirmish->slots[peer].human = true;
        }
        if (peers == 3) selected.seats.push_back({2, sim::SeatId{15}, 2});
        selected.setup = require(net::encode_skirmish_setup(initial));
        std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
        std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
        std::vector<net::SetupSession> setup_sessions;
        net::SetupOptions options;
        options.session.final_tick_count = 40;
        for (unsigned peer = 0; peer < peers; ++peer) {
            transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
            net::DesyncOptions monitor_options;
            monitor_options.exchange_every = 1000;
            monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
            auto provider = providers(*monitors.back(), executor);
            setup_sessions.push_back(peer == 0
                ? require(net::SetupSession::host(*transports.back(), selected, std::move(provider), options))
                : require(net::SetupSession::join(*transports.back(), std::move(provider), options)));
        }
        for (unsigned rounds = 0; rounds < 100; ++rounds) {
            ++network.round;
            for (auto& session : setup_sessions) require(session.pump(network.round));
            if (std::all_of(setup_sessions.begin(), setup_sessions.end(), [](const auto& session) {
                return session.phase() == net::SetupPhase::ready;
            })) break;
        }
        std::vector<net::SetupResult> results;
        for (auto& session : setup_sessions) results.push_back(require(session.take_result()));
        for (auto& result : results) result.session().enable_departures([seats = selected.seats](sim::SeatCommandKey key) {
            const auto found = std::find_if(seats.begin(), seats.end(), [&](const auto& seat) { return seat.seat == key.seat; });
            expect(found != seats.end(), "maximum-lead synthetic seat exists");
            return net::encode_tactical_input(key, {found->seat, found->player, true, false}, {{}, tactical::QuitPayload{}});
        });
        for (unsigned rounds = 0; rounds < 5; ++rounds) {
            ++network.round;
            require(results[0].session().pump(network.round));
        }
        expect(results[0].adapter().completed_tick() == 4, "setup-created departing peer reaches lead D+1");
        network.open[0] = false;
        std::vector<std::vector<net::DepartureEvent>> departures(peers);
        for (unsigned rounds = 0; rounds < 160; ++rounds) {
            ++network.round;
            for (unsigned peer = 1; peer < peers; ++peer) {
                auto pumped = require(results[peer].session().pump(network.round));
                departures[peer].insert(departures[peer].end(), pumped.departure_events.begin(), pumped.departure_events.end());
            }
        }
        for (unsigned peer = 1; peer < peers; ++peer) {
            expect(departures[peer].size() == 1 && departures[peer].front().key.tick == net::sealed_input_horizon(3),
                "setup runtime admits monitored cutoff 2D+2 on all survivors");
            expect(results[peer].adapter().completed_tick() == 40 && monitors[peer]->agreed_end(),
                "maximum-lead setup runtime survivors complete final agreement");
        }
        if (peers == 3) expect(results[1].adapter().state_hash() == results[2].adapter().state_hash(),
            "three-peer maximum-lead setup survivors agree state");
    }
}

void runtime_departure(const std::filesystem::path& directory) {
    net::SetupOptions options;
    options.session.final_tick_count = 90;
    Pair pair(directory, identity(), false, options, {}, true);
    pair.ready();
    auto host = require(pair.host.take_result()), client = require(pair.client.take_result());
    for (auto* result : {&host, &client}) {
        const auto seats = result->offer().seats;
        result->session().enable_departures([seats](sim::SeatCommandKey key) {
            const auto found = std::find_if(seats.begin(), seats.end(), [&](const auto& seat) { return seat.seat == key.seat; });
            expect(found != seats.end(), "runtime synthetic departure seat is bound");
            return net::encode_tactical_input(key, {found->seat, found->player, true, false}, {{}, tactical::QuitPayload{}});
        });
    }
    for (unsigned i = 0; i < 20; ++i) {
        ++pair.network.round;
        require(host.session().pump(pair.network.round)); require(client.session().pump(pair.network.round));
    }
    pair.network.open[1] = false;
    std::vector<net::DepartureEvent> departures;
    for (unsigned i = 0; i < 200 && host.adapter().completed_tick() < 90; ++i) {
        ++pair.network.round;
        auto pumped = require(host.session().pump(pair.network.round));
        departures.insert(departures.end(), pumped.departure_events.begin(), pumped.departure_events.end());
    }
    expect(departures.size() == 1 && departures.front().reason == net::DepartureReason::disconnect
        && departures.front().key.seat == host.offer().seats[1].seat,
        "setup-created runtime forwards agreed abrupt departure membership");
    expect(host.adapter().completed_tick() == 90 && pair.host_monitor.agreed_end()
        && !host.session().failure() && !pair.host_monitor.frozen(),
        "setup runtime survives abrupt closure through final hash agreement without transport error");
}

void agreement(const std::filesystem::path& directory) {
    net::SetupOptions options; options.session.final_tick_count = 90;
    Pair pair(directory, identity(), false, options);
    expect(!pair.client.take_result(), "client cannot start before offer/barrier");
    pair.network.fault = [](auto, auto, auto frame) {
        eawr::testing::FakeDelivery delivery;
        if (frame[0] == net::setup_accept_kind) delivery.duplicate = true;
        if (frame[0] == net::desync_combined_kind) delivery.ordered_delay = 5;
        return delivery;
    };
    pair.ready();
    expect(pair.barrier_checks > 0, "withheld barrier was inspected before readiness");
    for (unsigned i = 0; i < 5; ++i) pair.round(); // Ready alone cannot drive the sim.
    auto host = require(pair.host.take_result()), client = require(pair.client.take_result());
    expect(!pair.client.take_result(), "result can only be taken once");
    expect(!pair.client.cancel(), "setup cannot cancel a transferred session");
    expect(!client.refusal(), "post-handoff cancel does not silently gate the active session");
    expect(host.adapter().completed_tick() == 0 && client.adapter().completed_tick() == 0, "barrier never steps tick one");
    expect(host.adapter().state_hash() == client.adapter().state_hash(), "tick-zero hashes match");
    expect(host.offer().seats[1].seat.value == 11 && host.offer().input_delay == 3, "agreed seat and D retained");
    expect(host.offer().final_tick_count == 90 && client.offer().final_tick_count == 90, "session end tick is agreed");
    submit_attack(host, 1, 2); submit_attack(client, 2, 1);
    for (unsigned i = 0; i < 2000 && (!pair.host_monitor.agreed_end() || !pair.client_monitor.agreed_end()); ++i) {
        ++pair.network.round;
        require(host.session().pump(pair.network.round)); require(client.session().pump(pair.network.round));
    }
    expect(host.adapter().completed_tick() == 90 && client.adapter().completed_tick() == 90, "short battle completes");
    expect(host.adapter().state_hash() == client.adapter().state_hash(), "short battle agrees");
    expect(pair.host_monitor.agreed_end() && pair.client_monitor.agreed_end(), "final hash agreement");
}
void identity_refusals(const std::filesystem::path& directory) {
    for (std::size_t i = 0; i < identity().content.domains.size(); ++i) {
        auto local = identity(); const auto name = local.content.domains[i].name;
        local.content.domains[i].digest[0] ^= 1;
        local.content = sim::compute_match_identity(std::move(local.content));
        Pair pair(directory / name, local); pair.refused();
        expect(pair.client.refusal()->reason == net::SetupReason::identity && pair.client.refusal()->domain == name,
            "wrong refused identity domain: " + name);
        expect(*pair.host.refusal() == *pair.client.refusal(), "host must retain the same identity refusal");
        expect(pair.builds == 0, "mismatched client must not build a simulation");
    }
    auto local = identity(); local.build += "-different";
    Pair build(directory / "build", local); build.refused();
    expect(build.host.refusal()->domain == "build" && build.builds == 0, "handshake-only build mismatch");
    local = identity(); local.build.clear();
    Pair absent_build(directory / "absent-build", local); absent_build.refused();
    expect(absent_build.client.refusal()->domain == "build", "missing build is a named build refusal");
    local = identity(); local.content.domains.erase(local.content.domains.begin());
    local.content = sim::compute_match_identity(std::move(local.content));
    Pair missing(directory / "missing", local); missing.refused();
    expect(missing.client.refusal()->domain == "ai-data", "missing local domain names the absent domain");
}
void divergence(const std::filesystem::path& directory) {
    Pair pair(directory, identity(), true); pair.refused();
    expect(pair.client.refusal()->reason == net::SetupReason::tick_zero
        && pair.host.refusal()->reason == net::SetupReason::tick_zero, "typed tick-zero mismatch required");
    expect(pair.client.refusal()->local_hash != pair.client.refusal()->remote_hash
        && pair.client.refusal()->local_hash.size() == 64 && pair.host.refusal()->remote_hash.size() == 64,
        "tick-zero refusal must include both hashes");
    expect(!pair.client.take_result() && !pair.host.take_result(), "divergent peers cannot start");
    expect(pair.host_monitor.report()->first_divergent_tick == 0, "reuse monitor tick-zero divergence");
}
void preview_start(const std::filesystem::path& directory) {
    net::SetupOptions options; options.auto_accept = false; options.auto_start = false;
    Pair pair(directory, identity(), false, options);
    for (unsigned i = 0; i < 10; ++i) pair.round();
    expect(pair.client.phase() == net::SetupPhase::reviewing_offer && pair.client.offered()
        && pair.client.offered()->setup == offer().setup && pair.builds == 0, "read-only preview before local build/accept");
    expect(!pair.client.start() && !pair.host.accept(), "host/client roles enforced");
    for (unsigned i = 0; i < 40; ++i) { pair.network.round += 1000; pair.round(); }
    expect(!pair.host.refusal() && !pair.client.refusal(), "human preview longer than timeout stays open on both peers");
    require(pair.client.accept());
    for (unsigned i = 0; i < 40; ++i) { pair.network.round += 1000; pair.round(); }
    expect(pair.host.phase() != net::SetupPhase::ready && pair.client.phase() != net::SetupPhase::ready
        && !pair.client.start_received(), "acceptance alone cannot launch UI match");
    require(pair.host.start()); pair.ready();
    expect(pair.client.start_received(), "joiner observes host start");
    auto host = require(pair.host.take_result()), client = require(pair.client.take_result());
    expect(host.adapter().completed_tick() == 0 && client.adapter().completed_tick() == 0,
        "buffered review frames do not bypass tick zero");
}
void credential_start_gate(const std::filesystem::path& directory) {
    Pair pair(directory);
    bool delivered{};
    auto provider = providers(pair.host_monitor, pair.executor);
    provider.credentials_delivered = [&] { return delivered; };
    pair.host = require(net::SetupSession::host(pair.host_transport, offer(), std::move(provider)));
    // The Pair constructor already queued its offer; use a fresh join to consume
    // the identical canonical offer once, with no duplicate wire delivery.
    pair.network.inbox[1].erase(pair.network.inbox[1].begin());
    for (unsigned round = 0; round < 20; ++round) pair.round();
    expect(!pair.client.start_received() && pair.host.phase() != net::SetupPhase::ready,
        "accepted setup cannot Start without every credential acknowledgement");
    delivered = true; pair.ready();
    expect(pair.client.start_received(), "completed credential delivery releases Start");
}
void session_policy(const std::filesystem::path& directory) {
    net::SetupOptions a; a.session.final_tick_count = 90;
    net::SetupOptions b = a; b.session.final_tick_count = 300;
    Pair pair(directory / "mismatch", identity(), false, a, b); pair.refused();
    expect(pair.client.refusal()->reason == net::SetupReason::options && pair.builds == 0,
        "different final tick counts refuse before local simulation construction");
    expect(*pair.host.refusal() == *pair.client.refusal(), "both peers retain the final-tick mismatch reason");
    a.session.input_delay = 6;
    Pair delay(directory / "delay", identity(), false, a); delay.ready();
    auto host = require(delay.host.take_result()), client = require(delay.client.take_result());
    expect(host.offer().input_delay == 6 && client.offer().input_delay == 6, "non-default D agrees with caller monitors");
}
void provider_failure(const std::filesystem::path& directory) {
    for (unsigned kind = 0; kind < 2; ++kind) {
        Pair pair(directory / std::to_string(kind));
        auto local = providers(pair.client_monitor, pair.executor);
        local.identify = [kind](const auto&) {
            eawr::core::Diagnostic error;
            if (kind == 1) { error.message = std::string(2048, 'x'); error.message[3] = '\0'; }
            return eawr::core::Result<sim::MatchAgreement>::failure(std::move(error));
        };
        pair.client = require(net::SetupSession::join(pair.client_transport, std::move(local)));
        pair.refused();
        expect(pair.client.refusal()->reason == net::SetupReason::local_build
            && !pair.client.refusal()->message.empty() && pair.client.refusal()->message.size() <= 1024,
            "empty/oversize provider diagnostic has a bounded fallback reason");
        expect(*pair.host.refusal() == *pair.client.refusal(), "provider failure reason is sent instead of timing out host");
    }
}
void buffered_offer(const std::filesystem::path& directory) {
    Pair pair(directory);
    ++pair.network.round;
    auto received = pair.client_transport.poll();
    expect(received.has_value(), "UI reads host offer before ownership handoff");
    require(pair.client.receive_offer(std::move(*received)));
    expect(pair.builds == 1 && pair.client.offered()->seed == offer().seed, "buffer replay uses local adapter provider");
    pair.ready();
    auto host = require(pair.host.take_result()), client = require(pair.client.take_result());
    expect(host.adapter().completed_tick() == 0 && client.adapter().completed_tick() == 0
        && host.adapter().state_hash() == client.adapter().state_hash(), "buffered offer has the same tick-zero barrier");
    Pair malformed(directory / "malformed");
    ++malformed.network.round;
    auto bad = malformed.client_transport.poll();
    expect(bad.has_value(), "malformed buffered offer available");
    bad->frame.resize(1);
    expect(!malformed.client.receive_offer(std::move(*bad)), "malformed buffered offer refused");
    malformed.refused();
    expect(malformed.host.refusal()->reason == net::SetupReason::malformed && malformed.builds == 0,
        "buffered malformed reason reaches host with no build");
    Pair non_offer(directory / "non-offer");
    expect(!non_offer.client.receive_offer({0, {net::setup_start_kind}}) && non_offer.builds == 0,
        "non-offer buffered frame reports an error without building");
}
void bad_frames(const std::filesystem::path& directory) {
    for (unsigned fault = 0; fault < 3; ++fault) {
        Pair pair(directory / std::to_string(fault));
        auto& frame = pair.network.inbox[1].front().received.frame;
        if (fault == 0) frame[50] ^= 1;
        if (fault == 1) frame.resize(net::max_setup_bytes + 1);
        if (fault == 2) frame = {net::setup_offer_kind};
        pair.refused();
        expect(pair.client.refusal()->reason == net::SetupReason::malformed && pair.builds == 0,
            "bad offer gets a typed error and no client build");
        expect(*pair.host.refusal() == *pair.client.refusal(), "bad offer reason reaches host");
    }
    Pair cancel(directory / "cancel"); expect(!cancel.host.cancel(), "cancel returns typed refusal");
    cancel.refused();
    expect(cancel.client.refusal()->reason == net::SetupReason::cancelled, "cancellation reaches client");
    net::SetupOptions options; options.timeout_ms = 10;
    Pair timeout(directory / "timeout", identity(), false, options);
    for (unsigned i = 0; i < 12; ++i) {
        ++timeout.network.round; static_cast<void>(timeout.host.pump(timeout.network.round));
    }
    expect(timeout.host.refusal() && timeout.host.refusal()->reason == net::SetupReason::timeout, "silent peer times out");
    Pair zero_timeout(directory / "zero-timeout", identity(), false, options);
    zero_timeout.network.fault = [](auto, auto, auto frame) {
        eawr::testing::FakeDelivery fault;
        fault.drop = frame[0] == net::desync_combined_kind;
        return fault;
    };
    zero_timeout.refused();
    expect(zero_timeout.client.refusal()->reason == net::SetupReason::timeout
        && zero_timeout.host.refusal()->reason == net::SetupReason::timeout, "started tick-zero barrier still has a network deadline");
    Pair ready_cancel(directory / "ready-cancel"); ready_cancel.ready();
    expect(!ready_cancel.host.cancel(), "ready setup can be cancelled before result handoff");
    ready_cancel.refused();
    expect(!ready_cancel.client.take_result() && ready_cancel.client.refusal()->reason == net::SetupReason::cancelled,
        "ready status must keep servicing cancellation frames");
}
}
int main(int argc, char** argv) {
    try {
        expect(argc == 2, "diagnostic output directory required");
        const std::filesystem::path directory(argv[1]);
        encoding(); recovery_policy_encoding(); disabled_recovery_presets(directory / "disabled-recovery"); text_contracts(); runtime_maximum_lead(); runtime_departure(directory / "departure"); agreement(directory / "agreement"); identity_refusals(directory / "identity");
        divergence(directory / "divergence"); preview_start(directory / "preview"); credential_start_gate(directory / "credential-gate");
        session_policy(directory / "policy"); provider_failure(directory / "provider");
        buffered_offer(directory / "buffer"); bad_frames(directory / "bad");
        std::cout << "setup contracts passed: canonical SKSU, eight domains/build, refusal, preview/start, tick zero and battle\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
