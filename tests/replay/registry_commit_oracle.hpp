// Included inside tactical_tests.cpp's test namespace, after canonical_encoder_oracle.hpp. The registry commit's
// partitioned phase (`registry-commit`) must leave exactly what the serial commit it replaced leaves: the same
// registry (entity handles, per-pool packed order, component values), handle map, counters and ownership counts,
// for every worker count and partition order. The serial commit stays in production as commit_units_reference.
namespace registry_commit_oracle {

namespace sd = tactical::session_detail;
namespace math = eawr::sim::math;
using State = tactical::checkpoint_detail::Access::State;

std::uint64_t mix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

// The pre-commit world: every unit carries a deterministic mix of optional components.
std::vector<sd::LiveUnit> baseline_units(const tactical::TacticalSetup& setup, const std::size_t count) {
    std::vector<sd::LiveUnit> units;
    for (std::size_t index = 0; index < count; ++index) {
        const auto h = mix(index);
        sd::LiveUnit unit;
        unit.state.entity_id = 2 * (index + 1);
        unit.state.type_id = 1 + h % 4; // type 4 has no ownership key: the sentinel slot
        unit.state.owner = setup.players[index % setup.players.size()].player_id;
        unit.state.position.x = math::Fixed::from_raw(static_cast<std::int64_t>(index));
        if (h % 3 == 0) unit.state.purchase_type = 1 + (h >> 8) % 3;
        if ((h >> 3) % 7 == 0) unit.state.contained.push_back({.entity_id = index + 1, .members = {}});
        if (h % 2 == 0) { unit.motion.emplace(); unit.speed = math::Fixed::from_raw(static_cast<std::int64_t>(index % 9)); }
        if (h % 3 != 0) { unit.durability.emplace(); unit.durability->hull = math::Fixed::from_raw(100 + static_cast<std::int64_t>(index)); }
        if ((h >> 5) % 5 == 0) unit.combat.emplace();
        if ((h >> 7) % 7 == 0) unit.abilities.emplace();
        if ((h >> 9) % 4 == 1) unit.formation.emplace();
        if ((h >> 11) % 11 == 0) { unit.waypoints.emplace(); unit.waypoints->points.emplace_back(); }
        if ((h >> 13) % 13 == 0) unit.garrison_unit = true;
        if ((h >> 23) % 7 == 0) unit.script_invulnerable = true;
        if ((h >> 15) % 6 == 0) unit.position_history.emplace();
        if ((h >> 17) % 9 == 0) unit.arrival_vulnerable_until = index;
        if ((h >> 19) % 19 == 0) unit.upgrade_bonuses[0] = math::Fixed::from_raw(7);
        if ((h >> 21) % 23 == 0) unit.ion_stun.emplace();
        if ((h >> 23) % 29 == 0) unit.asteroid_contact = index;
        if ((h >> 25) % 31 == 0) unit.nebula.emplace();
        if ((h >> 27) % 37 == 0) unit.corrupt.emplace();
        if ((h >> 29) % 41 == 0) unit.sighting.emplace();
        if ((h >> 31) % 43 == 0) unit.approach.emplace();
        units.push_back(std::move(unit));
    }
    return units;
}

enum class Change { none, steady_writes, everything };

// The tick's survivors: destroyed units leave, new ones join between and around them (odd IDs), and a
// scene-dependent share of the rest changes in place or gains/loses components.
std::vector<sd::LiveUnit> survivor_units(const tactical::TacticalSetup& setup, const std::vector<sd::LiveUnit>& before,
    const Change change) {
    std::vector<sd::LiveUnit> units;
    if (change == Change::none) return before;
    units.reserve(before.size() + before.size() / 8 + 2);
    const auto fresh = [&](const std::uint64_t id, const std::size_t salt) {
        sd::LiveUnit unit;
        unit.state.entity_id = id;
        unit.state.type_id = 1 + salt % 3;
        unit.state.owner = setup.players[salt % setup.players.size()].player_id;
        unit.state.purchase_type = salt % 2 == 0 ? 0 : 2;
        if (salt % 2 == 0) { unit.durability.emplace(); unit.motion.emplace(); }
        if (salt % 3 == 0) { unit.waypoints.emplace(); unit.waypoints->points.emplace_back(); }
        if (salt % 4 == 0) { unit.garrison_unit = true; unit.combat.emplace(); }
        if (salt % 6 == 1) unit.script_invulnerable = true;
        if (salt % 5 == 0) unit.state.contained.push_back({.entity_id = id, .members = {}});
        if (salt % 6 == 0) { unit.arrival_vulnerable_until = salt; unit.upgrade_bonuses[1] = math::Fixed::from_raw(3); }
        return unit;
    };
    units.push_back(fresh(1, 0)); // before the first handle
    for (std::size_t index = 0; index < before.size(); ++index) {
        const auto h = mix(index * 31 + 7);
        if (change == Change::everything && h % 10 == 3) continue; // destroyed
        auto unit = before[index];
        if (h % 4 == 0) unit.state.position.y = math::Fixed::from_raw(static_cast<std::int64_t>(index) + 5);
        if (h % 5 == 0) unit.state.order.issued_tick = index + 1;
        if (h % 6 == 0 && unit.durability) unit.durability->hull = math::Fixed::from_raw(static_cast<std::int64_t>(index) + 1);
        if (h % 7 == 0 && unit.motion) unit.speed = math::Fixed::from_raw(static_cast<std::int64_t>(100 + index));
        if (change == Change::everything) {
            if ((h >> 4) % 9 == 0) unit.state.owner = setup.players[(index + 1) % setup.players.size()].player_id;
            if ((h >> 6) % 17 == 0) unit.state.type_id = 1 + (unit.state.type_id % 4);
            if ((h >> 8) % 8 == 0) unit.combat.reset();
            else if ((h >> 8) % 9 == 1 && !unit.combat) unit.combat.emplace();
            if ((h >> 10) % 10 == 0) unit.motion.reset();
            else if ((h >> 10) % 11 == 1 && !unit.motion) unit.motion.emplace();
            if ((h >> 12) % 10 == 0) unit.durability.reset();
            else if ((h >> 12) % 12 == 1 && !unit.durability) unit.durability.emplace();
            if ((h >> 14) % 6 == 0) unit.garrison_unit = !unit.garrison_unit;
            if ((h >> 25) % 5 == 0) unit.script_invulnerable = !unit.script_invulnerable;
            if ((h >> 16) % 8 == 0) { if (unit.waypoints) unit.waypoints.reset(); else { unit.waypoints.emplace(); unit.waypoints->points.emplace_back(); } }
            if ((h >> 18) % 10 == 0) { if (unit.state.contained.empty()) unit.state.contained.push_back({.entity_id = index + 9, .members = {}}); else unit.state.contained.clear(); }
            if ((h >> 20) % 9 == 0) { if (unit.arrival_vulnerable_until) unit.arrival_vulnerable_until.reset(); else unit.arrival_vulnerable_until = index + 4; }
            if ((h >> 22) % 9 == 0) unit.upgrade_bonuses[0] = unit.upgrade_bonuses[0] == math::Fixed{} ? math::Fixed::from_raw(5) : math::Fixed{};
            if ((h >> 24) % 11 == 0) { if (unit.abilities) unit.abilities.reset(); else unit.abilities.emplace(); }
            if ((h >> 26) % 12 == 0) { if (unit.formation) unit.formation.reset(); else unit.formation.emplace(); }
            if ((h >> 28) % 13 == 0) { if (unit.position_history) unit.position_history->previous.x = math::Fixed::from_raw(9); else unit.position_history.emplace(); }
            if ((h >> 30) % 13 == 0) unit.asteroid_contact = unit.asteroid_contact ? std::nullopt : std::optional<std::uint64_t>(index);
            if ((h >> 32) % 13 == 0) { if (unit.sighting) unit.sighting.reset(); else unit.sighting.emplace(); }
            if ((h >> 34) % 13 == 0) { if (unit.ion_stun) unit.ion_stun.reset(); else unit.ion_stun.emplace(); }
        }
        units.push_back(std::move(unit));
        if (h % 12 == 5) units.push_back(fresh(before[index].state.entity_id + 1, index));
    }
    units.push_back(fresh(2 * (before.size() + 2), before.size())); // after the last handle
    return units;
}

template <typename... Components>
std::vector<std::vector<std::uint32_t>> pool_order(State& state) {
    std::vector<std::vector<std::uint32_t>> pools;
    const auto one = [&]<typename Component>() {
        const entt::sparse_set& storage = state.registry.storage<Component>();
        std::vector<std::uint32_t> order;
        for (const auto entity : storage) order.push_back(static_cast<std::uint32_t>(entt::to_integral(entity)));
        pools.push_back(std::move(order));
    };
    (one.template operator()<Components>(), ...);
    return pools;
}

struct Outcome {
    std::vector<std::uint8_t> bytes;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> handles;
    std::vector<std::vector<std::uint32_t>> pools;
    std::vector<std::uint64_t> ownership;
    std::uint64_t writes{}, emplacements{};
    bool waypoints{};
    std::size_t structural{};
    bool operator==(const Outcome&) const = default;
};

Outcome commit(const tactical::TacticalSetup& setup, const std::vector<sd::LiveUnit>& before,
    const std::vector<sd::LiveUnit>& after, const eawr::sim::PartitionExecutor* executor) {
    State state(setup, {}, {}, {}, {}, {}, {}, {}, {}, {});
    for (const auto& player : setup.players)
        for (const tactical::TypeId type : {1U, 2U, 3U}) state.ownership_keys.emplace_back(player.player_id, type);
    std::sort(state.ownership_keys.begin(), state.ownership_keys.end());
    state.ownership_counts.assign(state.ownership_keys.size(), 0);
    state.committed_ownership_counts.assign(state.ownership_keys.size(), 0);
    state.rebuild(before, false);
    const auto emplaced = state.registry_emplacement_count;
    auto survivors = after;
    Outcome outcome;
    if (executor == nullptr) {
        state.commit_units_reference(survivors, outcome.writes);
    } else {
        const auto committed = state.commit_units(survivors, outcome.writes, *executor);
        expect(static_cast<bool>(committed), "registry commit phase succeeds");
    }
    outcome.emplacements = state.registry_emplacement_count - emplaced;
    outcome.waypoints = state.waypoint_queues_present;
    outcome.bytes = state.canonical_bytes();
    for (const auto& [id, handle] : state.handles) outcome.handles.emplace_back(id, entt::to_integral(handle));
    outcome.pools = pool_order<sd::StableId, sd::GarrisonUnit, sd::Identity, sd::Placement, sd::CurrentOrder,
        sd::CarriedHeroes, sd::Health, sd::AsteroidContact, sd::NebulaState, sd::Motion, sd::Combat, sd::Waiting,
        sd::Abilities, sd::Approaching, sd::Sighted, sd::IonStunned, sd::CorruptedSystems, sd::Waypoints,
        sd::PositionHistoryComponent, sd::ArrivalVulnerability, sd::UpgradeModifiers>(state);
    outcome.ownership.assign(state.committed_ownership_counts.begin(), state.committed_ownership_counts.end());
    outcome.structural = state.handles.size();
    return outcome;
}

} // namespace registry_commit_oracle

void test_registry_commit_oracle(const std::string& fixtures) {
    namespace oracle = registry_commit_oracle;
    auto setup = load_fixture(fixtures).setup;
    setup.units.clear();
    expect(setup.players.size() >= 2, "registry commit oracle needs two players");
    if (setup.players.size() < 2) return;
    for (const std::size_t count : {std::size_t{0}, std::size_t{40}, std::size_t{255}, std::size_t{256}, std::size_t{700}, std::size_t{2500}}) {
        const auto before = oracle::baseline_units(setup, count);
        for (const auto change : {oracle::Change::none, oracle::Change::steady_writes, oracle::Change::everything}) {
            const auto after = oracle::survivor_units(setup, before, change);
            const std::string label = "registry commit " + std::to_string(count) + " units, change "
                + std::to_string(static_cast<int>(change));
            const auto reference = oracle::commit(setup, before, after, nullptr);
            if (change != oracle::Change::none) {
                expect(reference.writes > 0 && reference.emplacements > 0,
                    label + " exercises component writes and structural emplacements");
                expect(reference.structural == after.size(), label + " leaves one handle per survivor");
            } else {
                expect(reference.writes == 0 && reference.emplacements == 0, label + " steady state writes nothing");
            }
            const eawr::sim::InlineExecutor inline_executor;
            expect(oracle::commit(setup, before, after, &inline_executor) == reference, label + " inline equals the serial oracle");
            for (const std::size_t workers : {1U, 2U, 4U, 8U, 10U}) {
                const eawr::platform::ThreadWorkerAdapter pool(workers);
                expect(oracle::commit(setup, before, after, &pool) == reference,
                    label + " on " + std::to_string(workers) + " workers equals the serial oracle");
                const ReverseInline reverse(workers);
                expect(oracle::commit(setup, before, after, &reverse) == reference,
                    label + " with reversed partitions on " + std::to_string(workers) + " workers equals the serial oracle");
            }
        }
    }
}
