#include "benchmark_internal.hpp"

namespace sim_bench {
namespace {

class Stream {
public:
    explicit Stream(const std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ += 0x9e3779b97f4a7c15ULL;
        auto value = state_;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    // Uniform in [-bound, bound] whole source units.
    [[nodiscard]] std::int64_t signed_units(const std::int64_t bound) noexcept {
        return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(2 * bound + 1)) - bound;
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] Fixed whole(const std::int64_t value) noexcept {
    return Fixed::from_raw(value * Fixed::scale);
}

[[nodiscard]] Fixed fraction(const std::int64_t numerator, const std::int64_t denominator) noexcept {
    return Fixed::from_raw(numerator * Fixed::scale / denominator);
}

struct UnitType {
    std::int64_t weight;       // share of the fleet, in percent
    std::int64_t reveal_range; // whole source units
    std::int64_t hull;         // whole source units
    std::size_t hardpoints;
    std::int64_t max_speed;
};

// Fighter, bomber, corvette, frigate, cruiser, capital ship: a FoC-like fleet mix.
constexpr std::array<UnitType, 6> unit_types{{
    {40, 1500, 60, 0, 400},
    {15, 1500, 90, 0, 300},
    {20, 2500, 600, 4, 200},
    {15, 3000, 1500, 8, 150},
    {7, 3500, 3000, 14, 100},
    {3, 4000, 6000, 24, 80},
}};

[[nodiscard]] tactical::DurabilityTable durability_table() {
    constexpr std::array<tactical::HardpointRole, 6> roles{
        tactical::HardpointRole::weapon, tactical::HardpointRole::weapon, tactical::HardpointRole::engine,
        tactical::HardpointRole::shield_generator, tactical::HardpointRole::weapon, tactical::HardpointRole::fighter_bay};
    tactical::DurabilityTable table;
    table.rules = {fraction(1, 5), fraction(2, 5), fraction(33, 100)};
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        const auto& type = unit_types[index];
        tactical::DurabilityProfile profile;
        profile.type_id = index + 1;
        profile.max_hull = whole(type.hull);
        profile.max_speed = whole(type.max_speed);
        profile.destroyed_with_hardpoints = type.hardpoints >= 24;
        for (std::size_t hardpoint = 0; hardpoint < type.hardpoints; ++hardpoint) {
            profile.hardpoints.push_back(tactical::HardpointProfile{
                roles[hardpoint % roles.size()], true, whole(type.hull / 8), {}, {}});
        }
        table.profiles.push_back(std::move(profile));
    }
    return table;
}

// Every type targets with one priority set (bigger ships first) within 1.5 x its sensor range;
// fighters and bombers fire an object weapon, the others their weapon hardpoints (roles above).
[[nodiscard]] tactical::CombatTable combat_table() {
    constexpr std::array<tactical::HardpointRole, 6> roles{
        tactical::HardpointRole::weapon, tactical::HardpointRole::weapon, tactical::HardpointRole::engine,
        tactical::HardpointRole::shield_generator, tactical::HardpointRole::weapon, tactical::HardpointRole::fighter_bay};
    tactical::CombatTable table;
    tactical::PrioritySet set;
    set.unlisted = whole(100);
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        set.rows.push_back({index + 1, whole(static_cast<std::int64_t>(unit_types.size() - index))});
    }
    table.priority_sets.push_back(set);
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        const auto& type = unit_types[index];
        tactical::CombatProfile profile;
        profile.type_id = index + 1;
        profile.category_bits = std::uint64_t{1} << index;
        profile.priority_set = 0;
        profile.max_attack_distance = whole(type.reveal_range * 3 / 2);
        const auto weapon = [&](const std::uint32_t hardpoint, const std::int64_t y) {
            tactical::WeaponProfile entry;
            entry.hardpoint = hardpoint;
            entry.range = whole(type.reveal_range);
            entry.min_recharge_hundredths = 50;
            entry.max_recharge_hundredths = 350;
            entry.pulse_count = 2;
            entry.pulse_delay_frames = 15;
            entry.cone_width = hardpoint == tactical::object_weapon ? Fixed{} : whole(120);
            entry.cone_height = hardpoint == tactical::object_weapon ? Fixed{} : whole(60);
            entry.opportunity_when_idle = true;
            entry.opportunity_when_targeting = true;
            entry.fire_a = {whole(20), whole(y), Fixed{}};
            entry.fire_b = entry.fire_a;
            return entry;
        };
        for (std::uint32_t hardpoint = 0; hardpoint < type.hardpoints; ++hardpoint) {
            const auto y = static_cast<std::int64_t>(hardpoint % 2 == 0 ? 10 : -10);
            profile.hardpoints.push_back({hardpoint, {Fixed{}, whole(y), Fixed{}}, true});
            if (roles[hardpoint % roles.size()] == tactical::HardpointRole::weapon) {
                profile.weapons.push_back(weapon(hardpoint, y));
            }
        }
        if (type.hardpoints == 0) {
            profile.weapons.push_back(weapon(tactical::object_weapon, 0));
        }
        table.profiles.push_back(std::move(profile));
    }
    return table;
}

// Four players in two teams; each team's fleet fills a disc of radius 7000 around x = -/+4000,
// so the discs overlap in the middle. Every tick each player orders a group of its own units
// to move, sends another group to attack an enemy, and deals scripted hull damage to random
// units (the only state-changing command of rules v1), so the command phase, destruction

} // namespace

[[nodiscard]] Battle generate(const Options& options) {
    Battle battle;
    Stream stream(options.seed);
    auto& setup = battle.replay.setup;
    setup.seed = options.seed;
    for (tactical::PlayerId player = 1; player <= player_count; ++player) {
        setup.players.push_back(tactical::Player{player, player <= 2 ? 1U : 2U, player <= 2 ? 1U : 2U,
            tactical::player_flag_commandable});
    }
    for (eawr::sim::EntityId id = 1; id <= options.units; ++id) {
        auto roll = static_cast<std::int64_t>(stream.next() % 100);
        std::size_t type = 0;
        while (roll >= unit_types[type].weight) {
            roll -= unit_types[type].weight;
            ++type;
        }
        const auto owner = static_cast<tactical::PlayerId>(1 + (id - 1) % player_count);
        const bool first_team = owner <= 2;
        std::int64_t x = 0;
        std::int64_t y = 0;
        do {
            x = stream.signed_units(7000);
            y = stream.signed_units(7000);
        } while (x * x + y * y > 7000 * 7000);
        tactical::UnitState unit;
        unit.entity_id = id;
        unit.type_id = type + 1;
        unit.owner = owner;
        unit.position = {whole(x + (first_team ? -4000 : 4000)), whole(y), whole(stream.signed_units(300))};
        unit.rotation = first_team ? eawr::sim::math::identity_quat()
                                   : eawr::sim::math::Quat{Fixed{}, Fixed{}, whole(1), Fixed{}};
        setup.units.push_back(unit);
    }
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        battle.sensors.push_back(tactical::SensorProfile{index + 1, whole(unit_types[index].reveal_range)});
    }
    battle.durability = durability_table();
    battle.combat = combat_table();

    const auto total_ticks = options.warmup + options.ticks;
    const auto own_units = [&](const tactical::PlayerId player, const std::size_t count) {
        std::set<eawr::sim::EntityId> chosen;
        const auto per_player = options.units / player_count;
        while (chosen.size() < std::min<std::size_t>(count, per_player)) {
            chosen.insert(player + player_count * (stream.next() % per_player));
        }
        return std::vector<eawr::sim::EntityId>(chosen.begin(), chosen.end());
    };
    const auto any_units = [&](const std::size_t count) {
        std::set<eawr::sim::EntityId> chosen;
        while (chosen.size() < count) {
            chosen.insert(1 + stream.next() % options.units);
        }
        return std::vector<eawr::sim::EntityId>(chosen.begin(), chosen.end());
    };
    for (std::uint64_t tick = 0; tick < total_ticks; ++tick) {
        for (tactical::PlayerId player = 1; player <= player_count; ++player) {
            const tactical::PlayerId enemy = player <= 2 ? 3 + tick % 2 : 1 + tick % 2;
            const auto target = enemy + player_count * (stream.next() % (options.units / player_count));
            const auto destination = eawr::sim::math::Vec3{
                whole(stream.signed_units(9000)), whole(stream.signed_units(7000)), Fixed{}};
            battle.replay.commands.push_back(tactical::PlayerCommand{
                {tick, player, 1}, own_units(player, 24), tactical::MovePayload{destination}});
            battle.replay.commands.push_back(tactical::PlayerCommand{
                {tick, player, 2}, own_units(player, 16), tactical::AttackPayload{target}});
            battle.replay.commands.push_back(tactical::PlayerCommand{
                {tick, player, 3}, any_units(8), tactical::DamagePayload{whole(4), tactical::hull_target}});
        }
    }
    battle.replay.final_tick_count = total_ticks;
    return battle;
}

} // namespace sim_bench
