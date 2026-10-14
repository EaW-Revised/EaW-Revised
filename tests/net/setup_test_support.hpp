#pragma once
#include "eawr/net/setup.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/core/build_identity.hpp"

#include <stdexcept>
#include <algorithm>

namespace setup_test {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using Bytes = std::vector<std::uint8_t>;
inline net::ReconnectTokens pair_keys(net::PeerIndex self, std::uint32_t count) {
    net::ReconnectTokens keys(count);
    for (net::PeerIndex remote = 0; remote < count; ++remote) if (remote != self)
        keys[remote].fill(static_cast<std::uint8_t>(1 + std::min(self, remote) * net::max_match_seats + std::max(self, remote)));
    return keys;
}
inline void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T require(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}
inline sim::math::Fixed units(std::int64_t value) {
    return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale);
}
inline sim::MatchAgreement identity() {
    sim::MatchIdentity content;
    content.domains = {
        sim::identity_domain("rules", "tactical-rules"), sim::identity_domain("numeric-abi", "fixed"),
        sim::identity_domain("sandbox", "sandbox"), sim::identity_domain("units", "synthetic-units"),
        sim::identity_domain("map-path", "data/art/maps/test.ted"),
        sim::identity_domain("map-digest", std::string(64, 'a')),
        sim::identity_domain("script-manifest", "scripts"), sim::identity_domain("ai-data", "ai")};
    return {std::string(eawr::core::build_identity()), sim::compute_match_identity(std::move(content))};
}
inline tactical::TacticalSetup setup() {
    tactical::TacticalSetup value;
    value.seed = 741;
    value.players = {{1, 1, 0, tactical::player_flag_commandable}, {2, 2, 1, tactical::player_flag_commandable},
        {3, 3, 2, tactical::player_flag_commandable}};
    for (std::uint32_t owner = 1; owner <= 3; ++owner) {
        tactical::UnitState unit;
        unit.entity_id = owner; unit.type_id = 1; unit.owner = owner;
        unit.position = {units(owner == 1 ? -50 : 50), units(owner == 3 ? 100 : 0), {}};
        unit.rotation = sim::math::identity_quat();
        value.units.push_back(unit);
    }
    tactical::ReplaySkirmishSetup metadata;
    metadata.map = "data/art/maps/test.ted"; metadata.map_sha256 = std::string(64, 'a');
    metadata.victory_condition = 1;
    metadata.match.credits = units(5000); metadata.match.start_tech = 1; metadata.match.max_tech = 5;
    metadata.match.game_timer = 123; metadata.match.win_integer = 3; metadata.match.win_float = units(2);
    metadata.match.auto_resolve = 1; metadata.match.allow_random_events = true;
    metadata.match.win_condition = "conquest"; metadata.match.space_win_condition = "starbase";
    metadata.slots = {{1, true, 2, {"fleet-a"}}, {2, true, 4, {"fleet-b"}}, {3, false, 1, {"fleet-ai"}}};
    value.skirmish = metadata;
    value.match_identity = identity().content;
    for (const auto& domain : value.match_identity->domains) if (domain.name == "units") value.content_identity = domain.digest;
    return value;
}
inline net::SetupOffer offer() {
    net::SetupOffer value;
    value.seed = setup().seed; value.setup = require(net::encode_skirmish_setup(setup()));
    value.identity = identity(); value.seats = {{0, sim::SeatId{7}, 1}, {1, sim::SeatId{11}, 2}};
    return value;
}
inline tactical::TacticalContent content() {
    tactical::TacticalContent value;
    value.sensors = {{1, units(1000)}};
    value.motion.rules.arc_degrees = units(15); value.motion.rules.expansion_distance = units(300);
    value.motion.profiles = {{1, units(2), units(1), units(1), units(5), units(1), {}, {}}};
    tactical::WeaponProfile weapon;
    weapon.range = units(500); weapon.min_recharge_hundredths = 20; weapon.max_recharge_hundredths = 30;
    weapon.opportunity_when_targeting = true;
    tactical::CombatProfile profile;
    profile.type_id = 1; profile.category_bits = 1; profile.max_attack_distance = units(500); profile.weapons = {weapon};
    value.combat.profiles = {profile};
    return value;
}
inline net::SetupProviders providers(net::DesyncMonitor& monitor, const sim::PartitionExecutor& executor,
    sim::MatchAgreement local = identity(), bool perturb = false, int* builds = nullptr,
    net::SessionAdapter** adapter_address = nullptr) {
    net::SetupProviders value;
    value.monitor = &monitor;
    value.tick_zero_agreed = [&monitor] { return monitor.setup_agreed(); };
    value.tick_zero_mismatch = [&monitor]() -> std::optional<net::TickZeroMismatch> {
        if (monitor.report() && monitor.report()->first_divergent_tick == 0)
            return net::TickZeroMismatch{monitor.report()->local_hash, monitor.report()->remote_hash, monitor.report()->channel};
        return {};
    };
    value.identify = [local = std::move(local)](const auto&) { return eawr::core::Result<sim::MatchAgreement>::success(local); };
    value.build = [&executor, perturb, builds, adapter_address](const net::SetupOffer& selected)
        -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
        if (builds) ++*builds;
        auto initial = require(net::decode_skirmish_setup(selected.setup));
        if (perturb) ++initial.seed;
        std::vector<tactical::SeatBinding> bindings;
        for (const auto& seat : selected.seats) bindings.push_back({seat.seat, seat.player, true, false});
        auto simulation = tactical::TacticalSession::create(initial, content());
        if (!simulation) return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::failure(simulation.error());
        auto adapter = std::make_unique<net::TacticalSessionAdapter>(std::move(simulation).value(), executor, std::move(bindings));
        if (adapter_address) *adapter_address = adapter.get();
        return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
    };
    return value;
}
inline void submit_attack(net::SetupResult& result, std::uint32_t owner, std::uint32_t target) {
    const auto& seat = result.offer().seats[owner - 1];
    const tactical::SeatBinding binding{seat.seat, seat.player, true, false};
    require(result.session().submit([&](sim::SeatCommandKey key) {
        return net::encode_tactical_input(key, binding, {{owner}, tactical::AttackPayload{target}});
    }));
}
} // namespace setup_test
