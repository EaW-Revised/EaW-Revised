#include "encoding.hpp"
#include "eawr/core/sha256.hpp"
#include "../replay_internal.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace eawr::sim::galactic::detail {
namespace {
using Reader = sim::detail::Reader;

bool decode_row(Reader& in, bool& value) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; value = raw != 0; return true; }
bool decode_row(Reader& in, math::Fixed& value) { std::int64_t raw{}; if (!in.read_i64(raw)) return false; value = math::Fixed::from_raw(raw); return true; }
bool decode_row(Reader& in, ProductionDomain& value) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; value = static_cast<ProductionDomain>(raw); return true; }
bool decode_row(Reader& in, ProductionKind& value) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 3) return false; value = static_cast<ProductionKind>(raw); return true; }
void encode_actor_ability(std::vector<std::uint8_t>& bytes, ActorCombatRole role, const ActorAbilityState& value) {
    put_u8(bytes, static_cast<std::uint8_t>(role));
    put_u8(bytes, value.present ? 1 : 0); put_u8(bytes, value.combat_skip ? 1 : 0);
    put_u8(bytes, value.self_target ? 1 : 0); put_i64(bytes, value.evade_chance.raw());
}
bool decode_actor_ability(Reader& in, ActorCombatRole& role, ActorAbilityState& value) {
    std::uint8_t raw{};
    if (!in.read_u8(raw) || raw > static_cast<std::uint8_t>(ActorCombatRole::squadron)
        || !decode_row(in, value.present) || !decode_row(in, value.combat_skip)
        || !decode_row(in, value.self_target) || !decode_row(in, value.evade_chance)
        || value.evade_chance.raw() < 0 || value.evade_chance.raw() > math::Fixed::scale
        || (!value.present && (value.combat_skip || value.self_target || value.evade_chance.raw()))) return false;
    role = static_cast<ActorCombatRole>(raw); return true;
}

void encode_row(std::vector<std::uint8_t>& bytes, const FleetProductionBlocker& v) {
    put_u64(bytes, v.fleet);
    put_u8(bytes, v.pending_deletion ? 1 : 0);
    put_u8(bytes, v.skips_space_combat ? 1 : 0);
    put_u8(bytes, v.cancels_starbase_production ? 1 : 0);
}
bool decode_row(Reader& in, FleetProductionBlocker& v) {
    return in.read_u64(v.fleet) && decode_row(in, v.pending_deletion)
        && decode_row(in, v.skips_space_combat) && decode_row(in, v.cancels_starbase_production);
}

void encode_row(std::vector<std::uint8_t>&, const ProductionOffer&);
bool decode_row(Reader&, ProductionOffer&);
void encode_row(std::vector<std::uint8_t>&, const ProductionEntry&);
bool decode_row(Reader&, ProductionEntry&);
void encode_row(std::vector<std::uint8_t>&, const ProductionObject&);
bool decode_row(Reader&, ProductionObject&);
void encode_row(std::vector<std::uint8_t>&, const ProductionCount&);
bool decode_row(Reader&, ProductionCount&);
void encode_row(std::vector<std::uint8_t>&, const ProductionPlayer&);
bool decode_row(Reader&, ProductionPlayer&);
void encode_row(std::vector<std::uint8_t>&, const PlanetProduction&);
bool decode_row(Reader&, PlanetProduction&);
void encode_row(std::vector<std::uint8_t>&, const ProductionSetup&);
bool decode_row(Reader&, ProductionSetup&);

template <class T> void rows(std::vector<std::uint8_t>& bytes, const std::vector<T>& values) {
    put_u32(bytes, static_cast<std::uint32_t>(values.size()));
    for (const auto& row : values) {
        if constexpr (std::is_same_v<T, TypeId>) put_u64(bytes, row);
        else if constexpr (std::is_same_v<T, std::vector<TypeId>>) rows(bytes, row);
        else encode_row(bytes, row);
    }
}
template <class T> bool rows(Reader& in, std::vector<T>& values) {
    std::uint32_t count{};
    // Every row occupies at least four bytes. Bound before allocation, including nested lists.
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 4) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        T value{}; bool ok{};
        if constexpr (std::is_same_v<T, TypeId>) ok = in.read_u64(value);
        else if constexpr (std::is_same_v<T, std::vector<TypeId>>) ok = rows(in, value);
        else ok = decode_row(in, value);
        if (!ok) return false;
        values.push_back(std::move(value));
    }
    return true;
}

void encode_row(std::vector<std::uint8_t>& bytes, const ProductionOffer& v) {
    put_u64(bytes, v.type);
    put_u8(bytes, static_cast<std::uint8_t>(v.domain));
    put_u8(bytes, static_cast<std::uint8_t>(v.kind));
    put_i64(bytes, v.price.raw());
    put_i64(bytes, v.seconds.raw());
    put_i64(bytes, v.population.raw());
    put_i64(bytes, v.tech);
    put_i64(bytes, v.required_ground_level);
    put_i64(bytes, v.required_space_level);
    put_i64(bytes, v.base_level);
    put_i64(bytes, v.target_tech);
    put_i64(bytes, v.planet_limit);
    put_i64(bytes, v.current_limit);
    put_i64(bytes, v.lifetime_limit);
    put_u8(bytes, v.affiliated ? 1 : 0);
    put_u8(bytes, v.cinematic ? 1 : 0);
    put_u8(bytes, v.initially_locked ? 1 : 0);
    put_u8(bytes, v.initially_placed ? 1 : 0);
    put_u8(bytes, v.requires_initial_placement ? 1 : 0);
    put_u8(bytes, v.direct_build_allowed ? 1 : 0);
    put_u8(bytes, v.surface_exception ? 1 : 0);
    put_u8(bytes, v.slicer_unlockable ? 1 : 0);
    rows(bytes, v.required_structures);
    rows(bytes, v.required_orbiting_units);
    put_i64(bytes, v.authored_price.raw());
    put_u8(bytes, v.sellable ? 1 : 0);
}
bool decode_row(Reader& in, ProductionOffer& v) {
    return in.read_u64(v.type)
        && decode_row(in, v.domain)
        && decode_row(in, v.kind)
        && decode_row(in, v.price)
        && decode_row(in, v.seconds)
        && decode_row(in, v.population)
        && in.read_i64(v.tech)
        && in.read_i64(v.required_ground_level)
        && in.read_i64(v.required_space_level)
        && in.read_i64(v.base_level)
        && in.read_i64(v.target_tech)
        && in.read_i64(v.planet_limit)
        && in.read_i64(v.current_limit)
        && in.read_i64(v.lifetime_limit)
        && decode_row(in, v.affiliated)
        && decode_row(in, v.cinematic)
        && decode_row(in, v.initially_locked)
        && decode_row(in, v.initially_placed)
        && decode_row(in, v.requires_initial_placement)
        && decode_row(in, v.direct_build_allowed)
        && decode_row(in, v.surface_exception)
        && decode_row(in, v.slicer_unlockable)
        && rows(in, v.required_structures)
        && rows(in, v.required_orbiting_units)
        && decode_row(in, v.authored_price)
        && decode_row(in, v.sellable);
}
void encode_row(std::vector<std::uint8_t>& bytes, const ProductionEntry& v) {
    put_u64(bytes, v.type);
    put_u32(bytes, v.builder);
    put_u64(bytes, v.build_id);
    put_i64(bytes, v.price.raw());
    put_i64(bytes, v.full_seconds.raw());
    put_i64(bytes, v.last_remaining.raw());
    put_u64(bytes, v.start_frame);
    put_u64(bytes, v.duration_frames);
    put_u8(bytes, v.pay_as_you_go ? 1 : 0);
}
bool decode_row(Reader& in, ProductionEntry& v) {
    return in.read_u64(v.type)
        && in.read_u32(v.builder)
        && in.read_u64(v.build_id)
        && decode_row(in, v.price)
        && decode_row(in, v.full_seconds)
        && decode_row(in, v.last_remaining)
        && in.read_u64(v.start_frame)
        && in.read_u64(v.duration_frames)
        && decode_row(in, v.pay_as_you_go);
}
void encode_row(std::vector<std::uint8_t>& bytes, const ProductionObject& v) {
    put_u64(bytes, v.id);
    put_u64(bytes, v.type);
    put_u32(bytes, v.owner);
    put_i64(bytes, v.paid_cost.raw());
    put_u8(bytes, static_cast<std::uint8_t>(v.domain));
    put_u8(bytes, static_cast<std::uint8_t>(v.kind));
}
bool decode_row(Reader& in, ProductionObject& v) {
    return in.read_u64(v.id)
        && in.read_u64(v.type)
        && in.read_u32(v.owner)
        && decode_row(in, v.paid_cost)
        && decode_row(in, v.domain)
        && decode_row(in, v.kind);
}
void encode_row(std::vector<std::uint8_t>& bytes, const ProductionCount& v) {
    put_u64(bytes, v.type);
    put_u64(bytes, v.owned);
    put_u64(bytes, v.historical);
}
bool decode_row(Reader& in, ProductionCount& v) {
    return in.read_u64(v.type)
        && in.read_u64(v.owned)
        && in.read_u64(v.historical);
}
void encode_row(std::vector<std::uint8_t>& bytes, const ProductionPlayer& v) {
    put_u32(bytes, v.player);
    put_u8(bytes, v.human ? 1 : 0);
    put_u8(bytes, v.pay_as_you_go ? 1 : 0);
    put_u8(bytes, v.uses_population_cap ? 1 : 0);
    put_i64(bytes, v.population_capacity.raw());
    put_i64(bytes, v.population_used.raw());
    rows(bytes, v.counts);
    rows(bytes, v.locked);
    rows(bytes, v.unlocked);
}
bool decode_row(Reader& in, ProductionPlayer& v) {
    return in.read_u32(v.player)
        && decode_row(in, v.human)
        && decode_row(in, v.pay_as_you_go)
        && decode_row(in, v.uses_population_cap)
        && decode_row(in, v.population_capacity)
        && decode_row(in, v.population_used)
        && rows(in, v.counts)
        && rows(in, v.locked)
        && rows(in, v.unlocked);
}
void encode_planet_inputs(std::vector<std::uint8_t>& bytes, const PlanetProduction& v) {
    put_u64(bytes, v.planet);
    put_u32(bytes, v.target_owner);
    put_u32(bytes, v.starbase_owner);
    put_u32(bytes, v.ground_occupier);
    put_u32(bytes, v.orbit_occupier);
    put_u64(bytes, v.starbase_type);
    put_i64(bytes, v.ground_level);
    put_i64(bytes, v.space_level);
    put_i64(bytes, v.maximum_space_level);
    put_i64(bytes, v.ground_slots);
    put_i64(bytes, v.space_slots);
    put_i64(bytes, v.landed_transports);
    put_i64(bytes, v.maximum_ground_forces);
    put_u8(bytes, v.enabled ? 1 : 0);
    put_u8(bytes, v.surface_accessible ? 1 : 0);
    put_u8(bytes, v.starbase_blocked ? 1 : 0);
}
void encode_row(std::vector<std::uint8_t>& bytes, const PlanetProduction& v) {
    encode_planet_inputs(bytes, v);
    rows(bytes, v.offers);
    for (const auto& queue : v.queues) rows(bytes, queue);
    rows(bytes, v.objects);
}
bool decode_row(Reader& in, PlanetProduction& v) {
    return in.read_u64(v.planet)
        && in.read_u32(v.target_owner)
        && in.read_u32(v.starbase_owner)
        && in.read_u32(v.ground_occupier)
        && in.read_u32(v.orbit_occupier)
        && in.read_u64(v.starbase_type)
        && in.read_i64(v.ground_level)
        && in.read_i64(v.space_level)
        && in.read_i64(v.maximum_space_level)
        && in.read_i64(v.ground_slots)
        && in.read_i64(v.space_slots)
        && in.read_i64(v.landed_transports)
        && in.read_i64(v.maximum_ground_forces)
        && decode_row(in, v.enabled)
        && decode_row(in, v.surface_accessible)
        && decode_row(in, v.starbase_blocked)
        && rows(in, v.offers)
        && rows(in, v.queues[0])
        && rows(in, v.queues[1])
        && rows(in, v.objects);
}
void encode_production_rows(std::vector<std::uint8_t>& bytes, const ProductionSetup& v,
    const ProductionOfferDigests* digests) {
    put_u32(bytes, v.maximum_human_queue);
    rows(bytes, v.players);
    if (!digests) rows(bytes, v.planets);
    else {
        put_u32(bytes, static_cast<std::uint32_t>(v.planets.size()));
        for (std::size_t i = 0; i < v.planets.size(); ++i) {
            const auto& planet = v.planets[i];
            encode_planet_inputs(bytes, planet);
            put_bytes(bytes, digests->at(i));
            for (const auto& queue : planet.queues) rows(bytes, queue);
            rows(bytes, planet.objects);
        }
    }
    put_u32(bytes, static_cast<std::uint32_t>(v.technology_types.size()));
    for (const auto& type : v.technology_types) {
        put_u64(bytes, type.type); put_i64(bytes, type.tech);
        put_u8(bytes, type.initially_locked ? 1 : 0); put_u8(bytes, type.slicer_unlockable ? 1 : 0);
        put_u32(bytes, static_cast<std::uint32_t>(type.affiliated_players.size()));
        for (auto player : type.affiliated_players) put_u32(bytes, player);
    }
}
void encode_row(std::vector<std::uint8_t>& bytes, const ProductionSetup& v) {
    encode_production_rows(bytes, v, nullptr);
}
bool decode_row(Reader& in, ProductionSetup& v) {
    std::uint32_t count{}; std::size_t affiliations{}; TypeId previous{};
    if (!in.read_u32(v.maximum_human_queue) || !rows(in, v.players) || !rows(in, v.planets)
        || !in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 22) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        TechnologyType type; std::uint32_t players{};
        if (!in.read_u64(type.type) || !type.type || type.type <= previous || !in.read_i64(type.tech)
            || type.tech < 0 || type.tech > std::numeric_limits<std::int32_t>::max()
            || !decode_row(in, type.initially_locked) || !decode_row(in, type.slicer_unlockable)
            || !in.read_u32(players) || players > galactic_max_players || players > in.remaining() / 4) return false;
        previous = type.type; affiliations += players;
        if (affiliations > galactic_max_objects) return false;
        PlayerId last{};
        for (std::uint32_t n = 0; n < players; ++n) {
            PlayerId player{};
            if (!in.read_u32(player) || !player || player <= last
                || std::none_of(v.players.begin(), v.players.end(), [&](const auto& row) { return row.player == player; })) return false;
            last = player; type.affiliated_players.push_back(player);
        }
        v.technology_types.push_back(std::move(type));
    }
    return true;
}

} // namespace
void encode_census(std::vector<std::uint8_t>& bytes, const PopulationCensus& census) {
    put_u8(bytes, 2); // WGP-191: typed History combat-power inputs
    put_u32(bytes, static_cast<std::uint32_t>(census.types.size()));
    for (const auto& row : census.types) {
        put_u64(bytes, row.type); put_i64(bytes, row.population.raw()); put_i64(bytes, row.additional_capacity);
        put_i64(bytes, row.authored_price.raw()); put_u8(bytes, row.sale_policy);
        put_u8(bytes, row.named_hero ? 1 : 0); put_u8(bytes, row.superweapon ? 1 : 0);
        put_u8(bytes, row.history ? 1 : 0);
        if (row.history) {
            put_i64(bytes, row.history->power.raw()); put_u8(bytes, row.history->fleet ? 1 : 0);
            put_u8(bytes, row.history->transport ? 1 : 0); put_u8(bytes, row.history->squadron ? 1 : 0);
        }
    }
    put_u32(bytes, static_cast<std::uint32_t>(census.players.size()));
    for (const auto& row : census.players) {
        put_u32(bytes, row.player); put_i64(bytes, row.base_capacity); put_u8(bytes, row.ai_uses_cap ? 1 : 0);
        rows(bytes, row.unusable_planets);
    }
    put_u32(bytes, static_cast<std::uint32_t>(census.objects.size()));
    for (const auto& row : census.objects) {
        put_u64(bytes, row.id); put_u64(bytes, row.original_type); put_u32(bytes, row.owner);
        put_u64(bytes, row.planet); put_u64(bytes, row.container);
        put_u8(bytes, row.dead ? 1 : 0); put_u8(bytes, row.delete_pending ? 1 : 0);
        put_u64(bytes, row.current_type); put_u64(bytes, row.generated_source);
        encode_actor_ability(bytes, row.combat_role, row.abilities);
        put_u8(bytes, row.garrison ? (*row.garrison ? 2 : 1) : 0);
        put_i64(bytes, row.paid_cost.raw());
    }
    // WGEP-78: sparse composite links follow the versioned atomic census rows.
    const auto composites = std::count_if(census.types.begin(), census.types.end(), [](const auto& row) { return !row.members.empty(); });
    if (composites) {
        put_u32(bytes, static_cast<std::uint32_t>(composites));
        for (const auto& row : census.types) if (!row.members.empty()) {
            put_u64(bytes, row.type); rows(bytes, row.members);
        }
    }
    // A validated company has nonempty members including its transport, so its
    // composite section always precedes this optional company binding tail.
    const auto companies = std::count_if(census.types.begin(), census.types.end(), [](const auto& row) { return row.company; });
    if (companies) {
        put_u8(bytes, 1); put_u32(bytes, static_cast<std::uint32_t>(companies));
        for (const auto& row : census.types) if (row.company) { put_u64(bytes, row.type); put_u64(bytes, row.transport); }
    }
}
std::optional<PopulationCensus> decode_census(std::span<const std::uint8_t> bytes) {
    Reader in(bytes); PopulationCensus census; std::uint8_t census_version{};
    if (!in.read_u8(census_version) || census_version != 2) return std::nullopt;
    const auto count = [&](std::uint32_t& size, std::size_t width) {
        return in.read_u32(size) && size <= galactic_max_objects && size <= in.remaining() / width;
    };
    std::uint32_t size{};
    if (!count(size, 35)) return std::nullopt;
    for (std::uint32_t i = 0; i < size; ++i) {
        CensusType row;
        if (!in.read_u64(row.type) || !decode_row(in, row.population) || !in.read_i64(row.additional_capacity)
            || !decode_row(in, row.authored_price) || !in.read_u8(row.sale_policy) || row.sale_policy > 2
            || !decode_row(in, row.named_hero) || !decode_row(in, row.superweapon)) return std::nullopt;
        bool history{}; if (!decode_row(in, history)) return std::nullopt;
        if (history) {
            row.history.emplace();
            if (!decode_row(in, row.history->power) || !decode_row(in, row.history->fleet)
                || !decode_row(in, row.history->transport) || !decode_row(in, row.history->squadron)) return std::nullopt;
        }
        census.types.push_back(row);
    }
    if (std::adjacent_find(census.types.begin(), census.types.end(), [](const auto& a, const auto& b) {
            return a.type >= b.type;
        }) != census.types.end()) return std::nullopt;
    if (!count(size, 17) || size > galactic_max_players) return std::nullopt;
    for (std::uint32_t i = 0; i < size; ++i) {
        CensusPlayer row;
        if (!in.read_u32(row.player) || !in.read_i64(row.base_capacity) || !decode_row(in, row.ai_uses_cap)
            || !rows(in, row.unusable_planets)) return std::nullopt;
        census.players.push_back(std::move(row));
    }
    if (!count(size, 75)) return std::nullopt;
    for (std::uint32_t i = 0; i < size; ++i) {
        CensusObject row; std::uint8_t garrison{};
        if (!in.read_u64(row.id) || !in.read_u64(row.original_type) || !in.read_u32(row.owner)
            || !in.read_u64(row.planet) || !in.read_u64(row.container) || !decode_row(in, row.dead)
            || !decode_row(in, row.delete_pending) || !in.read_u64(row.current_type)
            || !in.read_u64(row.generated_source) || !decode_actor_ability(in, row.combat_role, row.abilities)
            || !in.read_u8(garrison) || garrison > 2 || !decode_row(in, row.paid_cost)) return std::nullopt;
        if (garrison) row.garrison = garrison == 2;
        census.objects.push_back(row);
    }
    if (std::adjacent_find(census.objects.begin(), census.objects.end(), [](const auto& a, const auto& b) {
            return a.id >= b.id;
        }) != census.objects.end()) return std::nullopt;
    if (in.remaining()) {
        if (!count(size, 20) || !size || size > census.types.size()) return std::nullopt;
        TypeId previous{}; std::size_t links{};
        for (std::uint32_t i = 0; i < size; ++i) {
            TypeId id{};
            if (!in.read_u64(id) || id <= previous) return std::nullopt;
            previous = id;
            auto at = std::lower_bound(census.types.begin(), census.types.end(), id, [](const auto& row, auto value) { return row.type < value; });
            if (at == census.types.end() || at->type != id || !rows(in, at->members) || at->members.empty()) return std::nullopt;
            links += at->members.size();
            if (links > galactic_max_objects) return std::nullopt;
        }
    }
    if (in.remaining()) {
        std::uint8_t version{};
        if (!in.read_u8(version) || version != 1 || !count(size, 16) || !size) return std::nullopt;
        TypeId previous{};
        for (std::uint32_t i = 0; i < size; ++i) {
            TypeId id{}, transport{};
            if (!in.read_u64(id) || id <= previous || !in.read_u64(transport) || !transport) return std::nullopt;
            previous = id;
            auto at = std::lower_bound(census.types.begin(), census.types.end(), id, [](const auto& row, auto value) { return row.type < value; });
            if (at == census.types.end() || at->type != id || at->members.empty()) return std::nullopt;
            at->company = true; at->transport = transport;
        }
    }
    if (in.remaining() != 0) return std::nullopt;
    return census;
}
void encode_population(std::vector<std::uint8_t>& bytes, std::span<const PopulationTotal> totals) {
    put_u32(bytes, static_cast<std::uint32_t>(totals.size()));
    for (const auto& row : totals) {
        put_u32(bytes, row.player); put_i64(bytes, row.capacity.raw()); put_i64(bytes, row.used.raw());
        put_i64(bytes, row.reserved.raw()); put_u8(bytes, row.uses_cap ? 1 : 0);
    }
}

static void encode_production_impl(std::vector<std::uint8_t>& bytes, const ProductionSetup& production,
    const ProductionOfferDigests* digests) {
    put_u8(bytes, digests ? 8 : 7); // distinct production hash domain; wire format remains 7
    if (digests) encode_production_rows(bytes, production, digests);
    else encode_row(bytes, production);
    put_u8(bytes, production.fleet_blockers ? 1 : 0);
    if (production.fleet_blockers) rows(bytes, *production.fleet_blockers);
    put_u8(bytes, production.nested_creation ? 1 : 0);
    if (production.nested_creation) {
        const auto& setup = *production.nested_creation;
        put_u64(bytes, setup.fleet_type);
        put_u64(bytes, setup.squadron_type);
        put_u32(bytes, static_cast<std::uint32_t>(setup.types.size()));
        for (const auto& row : setup.types) {
            put_u64(bytes, row.type); put_u64(bytes, row.transport);
            put_u8(bytes, static_cast<std::uint8_t>(row.role)); rows(bytes, row.members);
            put_u8(bytes, row.skips_combat ? 1 : 0); put_u8(bytes, row.always_spawn_in_orbit ? 1 : 0);
            encode_actor_ability(bytes, row.combat_role, row.abilities);
        }
        put_u32(bytes, static_cast<std::uint32_t>(setup.relations.size()));
        for (const auto& row : setup.relations) {
            put_u32(bytes, row.player); put_u32(bytes, row.other); put_u8(bytes, row.enemy ? 1 : 0);
        }
    }
}
void encode_production(std::vector<std::uint8_t>& bytes, const ProductionSetup& production) {
    encode_production_impl(bytes, production, nullptr);
}
ProductionOfferDigests production_offer_digests(const ProductionSetup& production) {
    ProductionOfferDigests result;
    result.reserve(production.planets.size());
    for (const auto& planet : production.planets) {
        std::vector<std::uint8_t> bytes;
        rows(bytes, planet.offers);
        result.push_back(core::sha256(bytes));
    }
    return result;
}
void encode_production_hash(std::vector<std::uint8_t>& bytes, const ProductionSetup& production,
    const ProductionOfferDigests& digests) {
    encode_production_impl(bytes, production, &digests);
}
void encode_production_menu_inputs(std::vector<std::uint8_t>& bytes, const ProductionSetup& production,
    PlayerId local, std::span<const PlanetId> visible) {
    put_u32(bytes, production.maximum_human_queue);
    for (const auto& player : production.players) if (player.player == local) encode_row(bytes, player);
    put_u8(bytes, production.fleet_blockers ? 1 : 0);
    if (production.fleet_blockers) rows(bytes, *production.fleet_blockers);
    for (const auto& planet : production.planets) {
        if (!std::binary_search(visible.begin(), visible.end(), planet.planet)) continue;
        encode_planet_inputs(bytes, planet);
        // Admission reads queue membership and types, never countdown or money paid.
        for (const auto& queue : planet.queues) {
            put_u32(bytes, static_cast<std::uint32_t>(queue.size()));
            for (const auto& entry : queue) put_u64(bytes, entry.type);
        }
        rows(bytes, planet.objects);
    }
}
std::optional<ProductionSetup> decode_production(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes); ProductionSetup result; std::uint8_t version{}; bool present{};
    if (!reader.read_u8(version) || version != 7 || !decode_row(reader, result) || !decode_row(reader, present)) return std::nullopt;
    if (present) {
        result.fleet_blockers.emplace();
        if (!rows(reader, *result.fleet_blockers)) return std::nullopt;
    }
    if (!decode_row(reader, present)) return std::nullopt;
    if (present) {
        auto& setup = result.nested_creation.emplace(); std::uint32_t count{};
        if (!reader.read_u64(setup.fleet_type) || !reader.read_u64(setup.squadron_type) || !reader.read_u32(count)
            || count > galactic_max_objects || count > reader.remaining() / 35) return std::nullopt;
        std::size_t members{};
        for (std::uint32_t i = 0; i < count; ++i) {
            NestedCreationType row; std::uint8_t role{};
            if (!reader.read_u64(row.type) || !reader.read_u64(row.transport) || !reader.read_u8(role) || role > 3
                || !rows(reader, row.members) || !decode_row(reader, row.skips_combat)
                || !decode_row(reader, row.always_spawn_in_orbit)
                || !decode_actor_ability(reader, row.combat_role, row.abilities)) return std::nullopt;
            row.role = static_cast<NestedCreationRole>(role); members += row.members.size();
            if (members > galactic_max_objects) return std::nullopt;
            setup.types.push_back(std::move(row));
        }
        if (!reader.read_u32(count) || count > galactic_max_players * galactic_max_players || count > reader.remaining() / 9) return std::nullopt;
        for (std::uint32_t i = 0; i < count; ++i) {
            NestedCreationRelation row;
            if (!reader.read_u32(row.player) || !reader.read_u32(row.other) || !decode_row(reader, row.enemy)) return std::nullopt;
            setup.relations.push_back(row);
        }
    }
    if (reader.remaining() != 0) return std::nullopt;
    return result;
}
} // namespace eawr::sim::galactic::detail
