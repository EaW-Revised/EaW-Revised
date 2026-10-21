#include "encoding.hpp"
#include "../replay_internal.hpp"

#include <cstddef>
#include <algorithm>
#include <utility>

namespace eawr::sim::galactic::detail {

void encode_planet_access(std::vector<std::uint8_t>& bytes, const PlanetAccessState& state) {
    put_u64(bytes, state.generation);
    put_u32(bytes, static_cast<std::uint32_t>(state.planets.size()));
    for (const auto& row : state.planets) {
        put_u64(bytes, row.planet); put_u8(bytes, row.restricted ? 1 : 0);
        put_u8(bytes, row.local_revealed ? (*row.local_revealed ? 2 : 1) : 0);
    }
}
std::optional<PlanetAccessState> decode_planet_access(std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes); PlanetAccessState out; std::uint32_t count{};
    if (!in.read_u64(out.generation) || !out.generation || !in.read_u32(count)
        || count > galactic_max_objects || std::size_t{count} * 10 != in.remaining()) return {};
    PlanetId previous{};
    for (std::uint32_t i = 0; i < count; ++i) {
        PlanetAccess row; std::uint8_t restricted{}, revealed{};
        if (!in.read_u64(row.planet) || row.planet <= previous || !in.read_u8(restricted)
            || restricted > 1 || !in.read_u8(revealed) || revealed > 2) return {};
        previous = row.planet; row.restricted = restricted != 0;
        if (revealed) row.local_revealed = revealed == 2;
        out.planets.push_back(row);
    }
    return out;
}

void put_u8(std::vector<std::uint8_t>& bytes, const std::uint8_t value) { bytes.push_back(value); }

void put_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU));
}

void put_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) bytes.push_back(static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU));
}

void put_i64(std::vector<std::uint8_t>& bytes, const std::int64_t value) { put_u64(bytes, static_cast<std::uint64_t>(value)); }

void put_bytes(std::vector<std::uint8_t>& bytes, const std::span<const std::uint8_t> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void encode_players(std::vector<std::uint8_t>& bytes, const std::span<const GalacticPlayer> players) {
    put_u32(bytes, static_cast<std::uint32_t>(players.size()));
    for (const auto& player : players) {
        put_u32(bytes, player.player_id);
        put_u32(bytes, player.faction_id);
        put_u32(bytes, player.flags);
    }
}

void encode_fiscal(std::vector<std::uint8_t>& bytes, const FiscalSetup& fiscal) {
    put_i64(bytes, fiscal.credit_cap_per_planet.raw());
    put_u32(bytes, static_cast<std::uint32_t>(fiscal.players.size()));
    for (const auto& row : fiscal.players) {
        put_u32(bytes, row.player); put_u8(bytes, row.earns_planet_income ? 1U : 0U);
        put_i64(bytes, row.positive_credit_multiplier.raw());
    }
    put_u32(bytes, static_cast<std::uint32_t>(fiscal.planets.size()));
    for (const auto& row : fiscal.planets) { put_u64(bytes, row.planet); put_i64(bytes, row.credits.raw()); }
    // Optional versioned static tail; absent legacy setups retain identical bytes.
    if (fiscal.contributions) {
        const auto& rules = *fiscal.contributions;
        put_u8(bytes, 1);
        put_i64(bytes, rules.maximum_alignment_bonus.raw()); put_i64(bytes, rules.maximum_alignment_penalty.raw());
        put_u32(bytes, static_cast<std::uint32_t>(rules.political_curve.size()));
        for (const auto& row : rules.political_curve) { put_i64(bytes, row.x.raw()); put_i64(bytes, row.y.raw()); }
        put_u32(bytes, static_cast<std::uint32_t>(rules.factions.size()));
        for (const auto& row : rules.factions) { put_u32(bytes, row.player); put_u8(bytes, static_cast<std::uint8_t>(row.alignment)); }
        put_u32(bytes, static_cast<std::uint32_t>(rules.planets.size()));
        for (const auto& row : rules.planets) { put_u64(bytes, row.planet); put_u8(bytes, static_cast<std::uint8_t>(row.alignment)); put_i64(bytes, row.strength.raw()); }
        put_u32(bytes, static_cast<std::uint32_t>(rules.routes.size()));
        for (const auto& row : rules.routes) { put_u64(bytes, row.route); put_i64(bytes, row.factor.raw()); }
    }
}

void encode_player_economy(std::vector<std::uint8_t>& bytes, const PlayerEconomySetup& economy) {
    put_u32(bytes, static_cast<std::uint32_t>(economy.players.size()));
    for (const auto& player : economy.players) {
        put_u32(bytes, player.player); put_i64(bytes, player.credits.raw());
        put_i64(bytes, player.credit_cap.raw()); put_i64(bytes, player.current_tech);
        put_i64(bytes, player.maximum_tech);
    }
}

void encode_planet(std::vector<std::uint8_t>& bytes, const Planet& planet) {
    put_u64(bytes, planet.id);
    put_i64(bytes, planet.position.x.raw());
    put_i64(bytes, planet.position.y.raw());
    put_i64(bytes, planet.position.z.raw());
    put_u32(bytes, planet.owner);
}

void encode_route(std::vector<std::uint8_t>& bytes, const Route& route) {
    put_u64(bytes, route.id);
    put_u64(bytes, route.from);
    put_u64(bytes, route.to);
}

void encode_fleet(std::vector<std::uint8_t>& bytes, const Fleet& fleet) {
    put_u64(bytes, fleet.id);
    put_u32(bytes, fleet.owner);
    put_u64(bytes, fleet.orbit);
    put_u32(bytes, static_cast<std::uint32_t>(fleet.members.size()));
    for (const auto& member : fleet.members) {
        put_u64(bytes, member.id);
        put_u64(bytes, member.type);
    }
    put_u8(bytes, fleet.raid_destination ? 1 : 0);
    if (fleet.raid_destination) put_u64(bytes, *fleet.raid_destination);
}

void encode_fleet_control(std::vector<std::uint8_t>& bytes, const FleetControlSetup& control,
    const std::span<const Fleet> fleets) {
    put_u8(bytes, control.gates.queued_campaign_conflict ? 1 : 0);
    put_u8(bytes, control.gates.active_speech ? 1 : 0);
    put_u32(bytes, static_cast<std::uint32_t>(control.types.size()));
    for (const auto& type : control.types) {
        put_u64(bytes, type.type); put_u8(bytes, type.transport ? 1 : 0);
        put_u8(bytes, type.can_hyperspace_without_ability ? 1 : 0); put_u8(bytes, type.requires_live_children ? 1 : 0);
    }
    put_u32(bytes, static_cast<std::uint32_t>(control.detached_units.size()));
    for (const auto& unit : control.detached_units) {
        put_u64(bytes, unit.member.id); put_u64(bytes, unit.member.type);
        put_u32(bytes, unit.owner); put_u64(bytes, unit.planet);
    }
    put_u32(bytes, static_cast<std::uint32_t>(fleets.size()));
    for (const auto& fleet : fleets) { put_u64(bytes, fleet.id); put_u8(bytes, fleet.orbit_slot); }
}

void encode_movement(std::vector<std::uint8_t>& bytes, const MovementSetup& s, std::span<const Fleet> fleets) {
    const auto count = [&](std::size_t value) { put_u32(bytes, static_cast<std::uint32_t>(value)); };
    const auto vec = [&](const math::Vec3 v) { put_i64(bytes, v.x.raw()); put_i64(bytes, v.y.raw()); put_i64(bytes, v.z.raw()); };
    put_i64(bytes, s.mode_modifier.raw());
    count(s.types.size());
    for (const auto& t : s.types) {
        put_u64(bytes, t.type); put_i64(bytes, t.hyperspace_speed.raw()); put_i64(bytes, t.build_cost.raw()); put_u32(bytes, t.squadron_capacity);
        put_u8(bytes, t.hyperspace_eligible ? 1 : 0); put_u8(bytes, t.squadron ? 1 : 0);
        put_u8(bytes, t.fleet_container ? 1 : 0); put_u8(bytes, t.skips_combat ? 1 : 0);
    }
    count(s.contents.size());
    for (const auto& c : s.contents) {
        put_u64(bytes, c.container); count(c.children.size());
        for (const auto& m : c.children) { put_u64(bytes, m.id); put_u64(bytes, m.type); }
    }
    count(s.players.size());
    for (const auto& p : s.players) { put_u32(bytes, p.player); put_i64(bytes, p.hyperspace_factor.raw()); put_i64(bytes, p.corruption_bonus.raw()); put_u8(bytes, p.benefits_from_corruption ? 1 : 0); }
    count(s.planets.size());
    for (const auto& p : s.planets) { put_u64(bytes, p.planet); put_i64(bytes, p.bounds_half_z.raw()); put_u8(bytes, p.corrupted ? 1 : 0); }
    count(s.access.size());
    for (const auto& a : s.access) { put_u32(bytes, a.player); put_u64(bytes, a.planet); put_u8(bytes, a.accessible ? 1 : 0); put_u8(bytes, a.potential_conflict ? 1 : 0); }
    count(fleets.size());
    for (const auto& f : fleets) {
        put_u64(bytes, f.id); vec(f.motion.position); vec(f.motion.facing); put_u64(bytes, f.motion.orbit_entry); put_u8(bytes, f.motion.transit ? 1 : 0);
        if (!f.motion.transit) continue;
        const auto& t = *f.motion.transit;
        count(t.path.size()); for (const auto id : t.path) put_u64(bytes, id);
        put_u32(bytes, t.hop); vec(t.desired); vec(t.velocity); vec(t.desired_facing);
        put_i64(bytes, t.base_speed.raw()); put_i64(bytes, t.speed.raw());
        count(t.packed.size()); for (const auto id : t.packed) put_u64(bytes, id);
        put_u64(bytes, t.destination_actor);
        put_u8(bytes, t.destination_slot ? 1 : 0); if (t.destination_slot) put_u8(bytes, *t.destination_slot);
        put_u32(bytes, t.producer.value); put_u64(bytes, t.sequence);
    }
    // Current transport-role table, followed by the optional authored raid policy.
    const auto transports = std::count_if(s.types.begin(), s.types.end(), [](const auto& t) { return t.transport; });
    {
        put_u8(bytes, 1); count(static_cast<std::size_t>(transports));
        for (const auto& t : s.types) if (t.transport) put_u64(bytes, t.type);
    }
    put_u8(bytes, s.raid ? 1 : 0);
    if (s.raid) {
        const auto& r = s.raid->rules;
        put_u32(bytes, r.required_faction); put_u32(bytes, r.free_categories); put_u32(bytes, r.limited_categories);
        put_u32(bytes, r.max_heroes); put_u32(bytes, r.max_limited);
        count(s.raid->types.size());
        for (const auto& t : s.raid->types) {
            put_u64(bytes, t.type); put_u32(bytes, t.categories); put_u8(bytes, t.hero); put_u8(bytes, t.stays_in_transport);
        }
    }
}

core::Result<DecodedMovement> decode_movement(std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes); DecodedMovement out;
    const auto fail = [&]() { return core::Result<DecodedMovement>::failure(diagnostic(diagnostic_codes::replay_malformed, "invalid movement extension")); };
    const auto fixed = [&](math::Fixed& value) { std::int64_t raw{}; if (!in.read_i64(raw)) return false; value = math::Fixed::from_raw(raw); return true; };
    const auto vec = [&](math::Vec3& v) { return fixed(v.x) && fixed(v.y) && fixed(v.z); };
    const auto flag = [&](bool& value) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; value = raw != 0; return true; };
    const auto count = [&](std::uint32_t& value, std::size_t width, std::size_t limit = galactic_max_objects) { return in.read_u32(value) && value <= limit && std::size_t{value} * width <= in.remaining(); };
    auto& s = out.inputs; std::uint32_t n{};
    if (!fixed(s.mode_modifier) || !count(n, 32)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) {
        MovementType t;
        if (!in.read_u64(t.type) || !fixed(t.hyperspace_speed) || !fixed(t.build_cost) || !in.read_u32(t.squadron_capacity)
            || !flag(t.hyperspace_eligible) || !flag(t.squadron) || !flag(t.fleet_container) || !flag(t.skips_combat)) return fail();
        s.types.push_back(t);
    }
    if (std::adjacent_find(s.types.begin(), s.types.end(), [](const auto& a, const auto& b) {
            return a.type >= b.type;
        }) != s.types.end()) return fail();
    if (!count(n, 12)) return fail();
    std::size_t total_children = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        MovementContents c; std::uint32_t child_count{};
        if (!in.read_u64(c.container) || !count(child_count, 16) || total_children + child_count > galactic_max_objects) return fail();
        total_children += child_count;
        for (std::uint32_t j = 0; j < child_count; ++j) { FleetMember m; if (!in.read_u64(m.id) || !in.read_u64(m.type)) return fail(); c.children.push_back(m); }
        s.contents.push_back(std::move(c));
    }
    if (!count(n, 21, galactic_max_players)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) { MovementPlayer p; if (!in.read_u32(p.player) || !fixed(p.hyperspace_factor) || !fixed(p.corruption_bonus) || !flag(p.benefits_from_corruption)) return fail(); s.players.push_back(p); }
    if (!count(n, 17, 4096)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) { MovementPlanet p; if (!in.read_u64(p.planet) || !fixed(p.bounds_half_z) || !flag(p.corrupted)) return fail(); s.planets.push_back(p); }
    if (!count(n, 14, galactic_max_players * 4096)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) { MovementAccess a; if (!in.read_u32(a.player) || !in.read_u64(a.planet) || !flag(a.accessible) || !flag(a.potential_conflict)) return fail(); s.access.push_back(a); }
    if (!count(n, 65)) return fail();
    std::size_t total_paths = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        FleetId id{}; FleetMotion m; bool presence{};
        if (!in.read_u64(id) || !vec(m.position) || !vec(m.facing) || !in.read_u64(m.orbit_entry) || !flag(presence)) return fail();
        if (presence) {
            m.transit.emplace(); auto& t = *m.transit; std::uint32_t path{}, packed{};
            if (!count(path, 8, 4096) || path < 2 || total_paths + path > galactic_max_objects) return fail();
            total_paths += path;
            for (std::uint32_t j = 0; j < path; ++j) { PlanetId p{}; if (!in.read_u64(p)) return fail(); t.path.push_back(p); }
            if (!in.read_u32(t.hop) || t.hop >= path - 1 || !vec(t.desired) || !vec(t.velocity) || !vec(t.desired_facing) || !fixed(t.base_speed) || !fixed(t.speed) || !count(packed, 8)) return fail();
            for (std::uint32_t j = 0; j < packed; ++j) { EntityId p{}; if (!in.read_u64(p)) return fail(); t.packed.push_back(p); }
            bool slot{}; if (!in.read_u64(t.destination_actor) || !flag(slot)) return fail();
            if (slot) { std::uint8_t v{}; if (!in.read_u8(v) || v > 2) return fail(); t.destination_slot = v; }
            if (!in.read_u32(t.producer.value) || !in.read_u64(t.sequence)) return fail();
        }
        out.fleets.emplace_back(id, std::move(m));
    }
    {
        std::uint8_t version{};
        if (!in.read_u8(version) || version != 1 || !count(n, 8)) return fail();
        TypeId previous{};
        for (std::uint32_t i = 0; i < n; ++i) {
            TypeId id{}; if (!in.read_u64(id) || id <= previous) return fail();
            previous = id;
            const auto at = std::lower_bound(s.types.begin(), s.types.end(), id, [](const auto& t, auto value) { return t.type < value; });
            if (at == s.types.end() || at->type != id || at->squadron || at->fleet_container) return fail();
            at->transport = true;
        }
    }
    bool raid{};
    if (!flag(raid)) return fail();
    if (raid) {
        s.raid.emplace(); auto& r = s.raid->rules;
        if (!in.read_u32(r.required_faction) || !in.read_u32(r.free_categories) || !in.read_u32(r.limited_categories)
            || !in.read_u32(r.max_heroes) || !in.read_u32(r.max_limited) || !count(n, 14)) return fail();
        TypeId previous{};
        for (std::uint32_t i = 0; i < n; ++i) {
            RaidType t;
            if (!in.read_u64(t.type) || t.type <= previous || !in.read_u32(t.categories) || !flag(t.hero) || !flag(t.stays_in_transport)) return fail();
            previous = t.type; s.raid->types.push_back(t);
        }
    }
    if (in.remaining() != 0) return fail();
    return core::Result<DecodedMovement>::success(std::move(out));
}

void encode_event(std::vector<std::uint8_t>& bytes, const GalacticEvent& event) {
    put_u64(bytes, event.tick);
    put_u8(bytes, static_cast<std::uint8_t>(event.kind));
    put_u32(bytes, event.producer.value);
    put_u64(bytes, event.subject);
    put_u64(bytes, event.sequence);
    put_u64(bytes, event.value);
    put_u8(bytes, static_cast<std::uint8_t>(event.reason));
}

std::array<std::uint8_t, 32> digest_bytes(const std::string_view hex) noexcept {
    const auto nibble = [](const char text) -> std::uint8_t {
        if (text >= '0' && text <= '9') return static_cast<std::uint8_t>(text - '0');
        if (text >= 'a' && text <= 'f') return static_cast<std::uint8_t>(text - 'a' + 10);
        return 0;
    };
    std::array<std::uint8_t, 32> result{};
    for (std::size_t index = 0; index < result.size() && 2 * index + 1 < hex.size(); ++index) {
        result[index] = static_cast<std::uint8_t>((nibble(hex[2 * index]) << 4U) | nibble(hex[2 * index + 1]));
    }
    return result;
}

void encode_seat_control(std::vector<std::uint8_t>& bytes, const SeatControlSetup& control) {
    put_u32(bytes, static_cast<std::uint32_t>(control.teams.size()));
    for (const auto& team : control.teams) {
        put_u32(bytes, team.side); put_u32(bytes, team.lead.value); put_u8(bytes, team.observer_ai ? 1 : 0);
    }
    put_u32(bytes, static_cast<std::uint32_t>(control.controllers.size()));
    for (const auto& controller : control.controllers) {
        put_u32(bytes, controller.player); put_u8(bytes, static_cast<std::uint8_t>(controller.kind));
        put_u8(bytes, static_cast<std::uint8_t>(controller.dropout));
        for (const auto* payload : {&controller.configuration, &controller.state}) {
            put_u32(bytes, static_cast<std::uint32_t>(payload->size())); put_bytes(bytes, *payload);
        }
    }
}
void encode_seat_control_state(std::vector<std::uint8_t>& bytes, const SeatControlState& state) {
    encode_seat_control(bytes, state.control); put_u8(bytes, state.battle ? 1 : 0);
    if (!state.battle) return;
    const auto& battle = *state.battle;
    put_u64(bytes, battle.conflict); put_u32(bytes, battle.human_player); put_u32(bytes, battle.ai_player);
    put_u32(bytes, battle.choosing_side); put_u32(bytes, battle.observer_player);
    put_u8(bytes, static_cast<std::uint8_t>(battle.role));
}

core::Diagnostic diagnostic(const std::string_view code, std::string message) {
    core::Diagnostic result;
    result.code = std::string(code);
    result.severity = core::Severity::error;
    result.message = std::move(message);
    return result;
}

} // namespace eawr::sim::galactic::detail
