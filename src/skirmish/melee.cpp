#include "eawr/skirmish/melee.hpp"

#include "skirmish_internal.hpp"

#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::skirmish {
namespace {

namespace tactical = sim::tactical;

struct Entry final {
    std::string_view type;
    int count;
};

// The M2 roster's ships and squadrons (units::load_unit_tables' pinned types). Population values
// from the FoC data: corvette and Tartan 2, Nebulon-B and Acclamator 3, Calamari cruiser 4,
// squadrons 1. Rebel: 11 ships (32 pop) + 10 squadrons = 42; Empire: 11 ships (27 pop) + 10
// squadrons = 37, each side's highest space cap (the level-5 star base's capacity plus the
// reinforcement points; the owner's anchor for the retail baseline, 2026-09-29).
constexpr Entry rebel_ships[] = {{"Corellian_Corvette", 4}, {"Nebulon_B_Frigate", 4}, {"Calamari_Cruiser", 3}};
constexpr Entry rebel_squadrons[] = {{"Rebel_X-Wing_Squadron", 6}, {"Y-Wing_Squadron", 4}};
constexpr Entry empire_ships[] = {{"Tartan_Patrol_Cruiser", 6}, {"Acclamator_Assault_Ship", 5}};
constexpr Entry empire_squadrons[] = {
    {"TIE_Fighter_Squadron", 4}, {"TIE_Interceptor_Squadron", 3}, {"TIE_Bomber_Squadron", 3}};

// The layout, in whole map units. The fronts stand `front_gap` apart across x = 0, well inside
// the capital ships' weapon ranges; ships stand in ranks of `rank_width` behind the squadrons.
constexpr std::int64_t front_gap = 1400;
constexpr std::int64_t ship_spacing = 320;
constexpr std::int64_t rank_depth = 380;
constexpr std::int64_t squadron_spacing = 260;
constexpr std::int64_t squadron_depth = 240;
constexpr std::int64_t jitter = 60;
constexpr std::size_t rank_width = 8;

// splitmix64: the layout's draws from the seed.
class Draws final {
public:
    explicit Draws(const std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t value = (state_ += 0x9E3779B97F4A7C15ULL);
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }
    std::int64_t offset(const std::int64_t spread) {
        return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(2 * spread + 1)) - spread;
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

[[nodiscard]] core::Diagnostic melee_error(std::string message) {
    return detail::error(diagnostic_codes::input, "melee: " + std::move(message));
}

// The side's types, `factor` times each count, shuffled by the draws.
[[nodiscard]] std::vector<std::string_view> roster(std::span<const Entry> entries, const int factor, Draws& draws) {
    std::vector<std::string_view> types;
    for (const auto& entry : entries) {
        for (int copy = 0; copy < entry.count * factor; ++copy) types.push_back(entry.type);
    }
    for (std::size_t index = types.size(); index > 1; --index) {
        std::swap(types[index - 1], types[draws.next() % index]);
    }
    return types;
}

// Faces the side toward the other side: yaw 0 for the west side, 180 degrees for the east.
[[nodiscard]] sim::math::Quat facing(const std::int64_t direction) {
    if (direction < 0) return sim::math::identity_quat();
    return {Fixed{}, Fixed{}, Fixed::from_raw(Fixed::scale), Fixed{}};
}

[[nodiscard]] std::optional<tactical::PlayerId> lobby_player(const SkirmishStart& start, const std::string_view faction) {
    for (const auto& player : start.players) {
        if (player.faction == faction && player.player.commandable()) return player.player.player_id;
    }
    return std::nullopt;
}

struct Side final {
    tactical::PlayerId player{};
    std::int64_t direction{}; // -1: the side west of x = 0, +1: east
    std::vector<std::string_view> ships;
    std::vector<std::string_view> squadrons;
    std::vector<sim::EntityId> ship_ids;
    std::vector<sim::EntityId> containers;
    sim::math::Vec3 centre{};
};

// The offset of place `index` along its rank, centred on y = 0.
[[nodiscard]] std::int64_t across(const std::size_t index, const std::int64_t spacing) {
    const auto column = static_cast<std::int64_t>(index % rank_width);
    return (2 * column - static_cast<std::int64_t>(rank_width - 1)) * spacing / 2;
}

} // namespace

std::optional<MeleeSize> melee_size(const std::string_view name) noexcept {
    if (name == "s") return MeleeSize::s;
    if (name == "m") return MeleeSize::m;
    if (name == "l") return MeleeSize::l;
    return std::nullopt;
}

std::string_view to_string(const MeleeSize size) noexcept {
    switch (size) {
    case MeleeSize::s: return "s";
    case MeleeSize::m: return "m";
    case MeleeSize::l: return "l";
    }
    return "s";
}

core::Result<Melee> build_melee(const units::UnitTables& tables, const SkirmishStart& start, const MeleeSize size,
    const std::uint64_t seed, const std::uint64_t ticks) {
    using Out = core::Result<Melee>;
    const auto rebel = lobby_player(start, "Rebel");
    const auto empire = lobby_player(start, "Empire");
    if (!rebel || !empire) return Out::failure(melee_error("the start has no Rebel and Empire lobby players"));
    const int factor = size == MeleeSize::s ? 1 : size == MeleeSize::m ? 2 : 4;
    Draws draws(seed);
    std::vector<Side> sides(2);
    sides[0].player = *rebel;
    sides[0].direction = -1;
    sides[0].ships = roster(rebel_ships, factor, draws);
    sides[0].squadrons = roster(rebel_squadrons, factor, draws);
    sides[1].player = *empire;
    sides[1].direction = 1;
    sides[1].ships = roster(empire_ships, factor, draws);
    sides[1].squadrons = roster(empire_squadrons, factor, draws);

    Melee out;
    auto& replay = out.replay;
    replay.setup.seed = seed;
    replay.setup.content_identity = start.setup.content_identity;
    replay.setup.players = start.setup.players;
    const auto next_id = [&] { return static_cast<sim::EntityId>(replay.setup.units.size() + 1U); };
    const auto place = [&](const std::string_view type, const Side& side, const std::int64_t x, const std::int64_t y) {
        tactical::UnitState unit;
        unit.entity_id = next_id();
        unit.type_id = type_id(type);
        unit.owner = side.player;
        unit.position = {whole(x + draws.offset(jitter)), whole(y + draws.offset(jitter)), Fixed{}};
        unit.rotation = facing(side.direction);
        replay.setup.units.push_back(unit);
        return unit;
    };
    for (auto& side : sides) {
        std::int64_t sum_x = 0;
        for (std::size_t index = 0; index < side.ships.size(); ++index) {
            if (tables.find(side.ships[index]) == nullptr) {
                return Out::failure(melee_error(std::string(side.ships[index]) + " is not in the unit tables"));
            }
            const auto rank = static_cast<std::int64_t>(index / rank_width);
            const auto x = side.direction * (front_gap / 2 + 250 + rank * rank_depth);
            side.ship_ids.push_back(place(side.ships[index], side, x, across(index, ship_spacing)).entity_id);
            sum_x += x;
        }
        // Squadrons: the container, then its craft on their Squadron_Offsets slots turned by the
        // side's yaw, as the M2 start places a company (add_squadron_craft).
        for (std::size_t index = 0; index < side.squadrons.size(); ++index) {
            const auto* type = tables.find(side.squadrons[index]);
            if (type == nullptr || type->kind != units::UnitKind::squadron || type->members.empty()) {
                return Out::failure(melee_error(std::string(side.squadrons[index]) + " is not a squadron of the unit tables"));
            }
            const auto rank = static_cast<std::int64_t>(index / rank_width);
            const auto x = side.direction * (front_gap / 2 + rank * squadron_depth);
            const auto container = place(side.squadrons[index], side, x, across(index, squadron_spacing));
            tactical::Squadron squadron;
            squadron.container = container.entity_id;
            for (const auto& member : type->members) {
                const auto* craft = tables.find(member.craft);
                if (craft == nullptr) return Out::failure(melee_error("craft " + member.craft + " is not in the unit tables"));
                const auto offset = member.offset.value_or(sim::math::Vec3{});
                const std::int64_t turn = side.direction < 0 ? 1 : -1;
                tactical::UnitState unit;
                unit.entity_id = next_id();
                unit.type_id = type_id(craft->id);
                unit.owner = side.player;
                unit.position = {Fixed::from_raw(container.position.x.raw() + turn * offset.x.raw()),
                    Fixed::from_raw(container.position.y.raw() + turn * offset.y.raw()),
                    Fixed::from_raw(container.position.z.raw() + offset.z.raw())};
                unit.rotation = container.rotation;
                replay.setup.units.push_back(unit);
                squadron.members.push_back(unit.entity_id);
                ++out.craft;
            }
            replay.setup.squadrons.push_back(std::move(squadron));
            side.containers.push_back(container.entity_id);
        }
        // The ships' centre: the ranks' mean x on the centre line.
        const auto count = static_cast<std::int64_t>(std::max<std::size_t>(side.ships.size(), 1));
        side.centre = {whole(sum_x / count), Fixed{}, Fixed{}};
        out.ships += side.ships.size();
        out.squadrons += side.squadrons.size();
    }
    // Tick 0: ship i attacks the enemy ship at the same place in its placement order (scaled to
    // the enemy's count), one command per target in ascending target ID; every squadron
    // attack-moves onto the enemy ships' centre. A ship whose target dies is left to its own
    // targeting, so the fight runs until one side has nothing left in range.
    for (std::size_t index = 0; index < sides.size(); ++index) {
        const auto& side = sides[index];
        const auto& enemy = sides[1 - index];
        std::map<sim::EntityId, std::vector<sim::EntityId>> attackers;
        for (std::size_t ship = 0; ship < side.ship_ids.size() && !enemy.ship_ids.empty(); ++ship) {
            attackers[enemy.ship_ids[ship * enemy.ship_ids.size() / side.ship_ids.size()]].push_back(side.ship_ids[ship]);
        }
        std::uint64_t sequence = 1;
        for (const auto& [target, units] : attackers) {
            replay.commands.push_back(
                tactical::PlayerCommand{{0, side.player, sequence++}, units, tactical::AttackPayload{target}});
        }
        if (!side.containers.empty()) {
            replay.commands.push_back(tactical::PlayerCommand{
                {0, side.player, sequence++}, side.containers, tactical::AttackMovePayload{enemy.centre, 0}});
        }
    }
    std::sort(replay.commands.begin(), replay.commands.end(),
        [](const tactical::PlayerCommand& left, const tactical::PlayerCommand& right) { return left.key < right.key; });
    replay.final_tick_count = ticks;
    return Out::success(std::move(out));
}

} // namespace eawr::skirmish
