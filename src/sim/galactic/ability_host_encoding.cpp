#include "encoding.hpp"
#include "../replay_internal.hpp"

namespace eawr::sim::galactic::detail {
namespace {
void text_value(std::vector<std::uint8_t>& out, const std::string& value) {
    put_u32(out, static_cast<std::uint32_t>(value.size())); out.insert(out.end(), value.begin(), value.end());
}
bool text_value(sim::detail::Reader& in, std::string& value) {
    std::uint32_t count{}; if (!in.read_u32(count) || count > 4096 || count > in.remaining()) return false;
    for (std::uint32_t i = 0; i < count; ++i) { std::uint8_t c{}; if (!in.read_u8(c) || !c) return false; value.push_back(static_cast<char>(c)); }
    return true;
}
void identities(std::vector<std::uint8_t>& out, const std::vector<TypeId>& rows) {
    put_u32(out, static_cast<std::uint32_t>(rows.size())); for (const auto id : rows) put_u64(out, id);
}
bool identities(sim::detail::Reader& in, std::vector<TypeId>& rows) {
    std::uint32_t count{}; if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 8) return false;
    for (std::uint32_t i = 0; i < count; ++i) { TypeId id{}; if (!in.read_u64(id) || !id) return false; rows.push_back(id); }
    return true;
}
void corruption_choice(std::vector<std::uint8_t>& out, const CorruptionChoice& c) {
    put_u8(out, static_cast<std::uint8_t>(c.type)); put_u8(out, static_cast<std::uint8_t>(c.required_mission_type));
    put_i64(out, c.price.raw()); put_i64(out, c.credit_reward.raw()); put_i64(out, c.unlock_build_cost.raw());
    put_u32(out, c.unit_count); put_u32(out, c.mission_count); identities(out, c.unit_types); identities(out, c.planet_types);
    text_value(out, c.briefing); text_value(out, c.map); text_value(out, c.script); put_u8(out, c.mode);
    put_u64(out, c.tactical_upgrade); put_u64(out, c.build_unlock);
}
bool corruption_choice(sim::detail::Reader& in, CorruptionChoice& c) {
    std::uint8_t type{}, required{}; std::int64_t price{}, reward{}, cost{};
    if (!in.read_u8(type) || type > 8 || !in.read_u8(required) || required > 8
        || !in.read_i64(price) || price < 0 || !in.read_i64(reward) || !in.read_i64(cost)
        || !in.read_u32(c.unit_count) || !in.read_u32(c.mission_count) || !identities(in, c.unit_types) || !identities(in, c.planet_types)
        || !text_value(in, c.briefing) || !text_value(in, c.map) || !text_value(in, c.script)
        || !in.read_u8(c.mode) || c.mode > 2 || !in.read_u64(c.tactical_upgrade) || !in.read_u64(c.build_unlock)) return false;
    c.type = static_cast<corruption_fixture::Type>(type); c.required_mission_type = static_cast<corruption_fixture::Type>(required);
    c.price = math::Fixed::from_raw(price); c.credit_reward = math::Fixed::from_raw(reward); c.unlock_build_cost = math::Fixed::from_raw(cost); return true;
}
void curve(std::vector<std::uint8_t>& out, const std::vector<AbilityCurvePoint>& values) {
    put_u32(out, static_cast<std::uint32_t>(values.size()));
    for (const auto& p : values) { put_i64(out, p.x.raw()); put_i64(out, p.y.raw()); }
}
bool curve(sim::detail::Reader& in, std::vector<AbilityCurvePoint>& values) {
    std::uint32_t count{};
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 16) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::int64_t x{}, y{};
        if (!in.read_i64(x) || !in.read_i64(y) || (!values.empty() && x <= values.back().x.raw())) return false;
        values.push_back({math::Fixed::from_raw(x), math::Fixed::from_raw(y)});
    }
    return true;
}
void theft(std::vector<std::uint8_t>& out, const PreparedTheft& t) {
    put_u64(out, t.technology); put_i64(out, t.price.raw()); put_i64(out, t.chance.raw());
    put_i64(out, t.absence.raw()); put_u8(out, t.advance ? 1 : 0);
}
}
void encode_prepared_ability(std::vector<std::uint8_t>& out, const PreparedAbility& p) {
    put_u64(out, p.preparation); put_u64(out, p.actor); put_u64(out, p.planet);
    put_u8(out, static_cast<std::uint8_t>(p.kind)); put_u32(out, p.choice); put_u32(out, p.item);
    put_i64(out, p.price.raw()); put_i64(out, p.absence.raw()); put_u8(out, p.owned ? 1 : 0);
    if (p.kind == AbilityKind::technology_theft) {
        put_u64(out, p.theft.technology); put_i64(out, p.theft.price.raw());
        put_i64(out, p.theft.chance.raw()); put_i64(out, p.theft.absence.raw()); put_u8(out, p.theft.advance ? 1 : 0);
    }
    if (p.kind == AbilityKind::corruption) { put_u8(out, static_cast<std::uint8_t>(p.corruption.type)); put_u8(out, p.corruption.slot); }
}
std::optional<PreparedAbility> decode_prepared_ability(std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes); PreparedAbility p; std::uint8_t kind{}, owned{}; std::int64_t price{}, absence{};
    if (!in.read_u64(p.preparation) || !in.read_u64(p.actor) || !p.actor
        || !in.read_u64(p.planet) || !p.planet || !in.read_u8(kind) || kind < 1 || kind > 5
        || !in.read_u32(p.choice) || !in.read_u32(p.item) || !in.read_i64(price) || price < 0
        || !in.read_i64(absence) || absence < 0 || !in.read_u8(owned) || owned > 1) return {};
    // WGCH-155/156: only a queued strategic drop carries no host-issued preparation (zero).
    if (!p.preparation && kind != 5) return {};
    if (kind == 1 && !p.item) return {};
    if (kind == 2) {
        std::int64_t theft_price{}, chance{}, theft_absence{}; std::uint8_t advance{};
        if (p.item || owned || !in.read_u64(p.theft.technology) || !p.theft.technology
            || !in.read_i64(theft_price) || theft_price != price || !in.read_i64(chance) || chance < 0 || chance > math::Fixed::scale
            || !in.read_i64(theft_absence) || theft_absence != absence || !in.read_u8(advance) || advance > 1) return {};
        p.theft.price = math::Fixed::from_raw(theft_price); p.theft.chance = math::Fixed::from_raw(chance);
        p.theft.absence = math::Fixed::from_raw(theft_absence); p.theft.advance = advance != 0;
    }
    if (kind == 3) {
        std::uint8_t type{};
        if (p.item || owned || absence || !in.read_u8(type) || type < 1 || type > 8 || !in.read_u8(p.corruption.slot) || p.corruption.slot > 2) return {};
        p.corruption.type = static_cast<corruption_fixture::Type>(type);
    }
    if ((kind == 4 || kind == 5) && (p.item || owned || absence)) return {};
    if (kind == 5 && price) return {};
    if (in.remaining()) return {};
    p.kind = static_cast<AbilityKind>(kind); p.price = math::Fixed::from_raw(price);
    p.absence = math::Fixed::from_raw(absence); p.owned = owned != 0; return p;
}
void encode_ability_setup(std::vector<std::uint8_t>& out, const GalacticAbilitySetup& setup) {
    put_u32(out, 6); put_i64(out, setup.default_hero_absence.raw()); // coordinator-reserved encoding 6 (destruction)
    put_u32(out, static_cast<std::uint32_t>(setup.profiles.size()));
    for (const auto& p : setup.profiles) {
        put_u64(out, p.type); put_i64(out, p.market.price_modifier.raw()); put_u8(out, p.market.causes_despawn ? 1 : 0);
        put_u8(out, p.named ? 1 : 0); put_u8(out, p.respawn_whole_team ? 1 : 0); put_u8(out, p.initially_enabled ? 1 : 0); put_u64(out, p.respawn_structure);
        put_u32(out, static_cast<std::uint32_t>(p.market.absence_min.size()));
        for (std::size_t i = 0; i < p.market.absence_min.size(); ++i) {
            put_i64(out, p.market.absence_min[i].raw()); put_i64(out, p.market.absence_max[i].raw());
        }
        put_u8(out, p.has_market ? 1 : 0); put_u8(out, p.theft ? 1 : 0);
        if (p.theft) {
            const auto& t = *p.theft;
            curve(out, t.availability); curve(out, t.ground); curve(out, t.space); curve(out, t.difficulty);
            put_i64(out, t.affinity_price.raw()); put_i64(out, t.affinity_chance.raw()); put_u8(out, t.initially_enabled ? 1 : 0);
            put_u32(out, static_cast<std::uint32_t>(t.absence_min.size()));
            for (std::size_t i = 0; i < t.absence_min.size(); ++i) { put_i64(out, t.absence_min[i].raw()); put_i64(out, t.absence_max[i].raw()); }
        }
        put_u8(out, p.corruption ? 1 : 0); put_u8(out, p.lifecycle_only ? 1 : 0);
        if (p.corruption) { text_value(out, p.corruption->name); put_u8(out, p.corruption->initially_enabled ? 1 : 0); }
        put_u8(out, p.hack ? 1 : 0);
        if (p.hack) { text_value(out, p.hack->name); put_u8(out, p.hack->initially_enabled ? 1 : 0); }
        put_u8(out, p.destruction ? 1 : 0);
        if (p.destruction) { text_value(out, p.destruction->name); put_u8(out, p.destruction->initially_enabled ? 1 : 0); }
    }
    put_u32(out, static_cast<std::uint32_t>(setup.technologies.size()));
    for (const auto& t : setup.technologies) { put_u64(out, t.type); put_i64(out, t.price.raw()); put_u8(out, t.space ? 1 : 0); }
    put_u32(out, static_cast<std::uint32_t>(setup.theft_planets.size()));
    for (const auto& p : setup.theft_planets) {
        put_u64(out, p.planet); put_i64(out, p.availability.raw()); put_i64(out, p.difficulty.raw());
        put_u8(out, p.auto_capture_minor ? (*p.auto_capture_minor ? 2 : 1) : 0);
        put_u32(out, static_cast<std::uint32_t>(p.affinity_types.size()));
        for (const auto id : p.affinity_types) put_u64(out, id);
    }
    put_u32(out, static_cast<std::uint32_t>(setup.activated_corruption_abilities.size()));
    for (const auto& name : setup.activated_corruption_abilities) text_value(out, name);
    put_u32(out, static_cast<std::uint32_t>(setup.corruption_planets.size()));
    for (const auto& p : setup.corruption_planets) {
        put_u64(out, p.planet); put_u64(out, p.type); put_u8(out, p.retains_influence ? 1 : 0);
        for (const auto& c : p.slots) corruption_choice(out, c);
    }
    put_u32(out, static_cast<std::uint32_t>(setup.corruption_players.size()));
    for (const auto& p : setup.corruption_players) { put_u32(out, p.player); for (const auto id : p.automatic_upgrades) put_u64(out, id); }
    put_u32(out, static_cast<std::uint32_t>(setup.corruption_prerequisites.size()));
    for (const auto& t : setup.corruption_prerequisites) { put_u64(out, t.type); put_u8(out, t.consumable ? 1 : 0); put_u8(out, t.named ? 1 : 0); }
    put_u32(out, static_cast<std::uint32_t>(setup.initial_corruption.size()));
    for (const auto& r : setup.initial_corruption) { put_u64(out, r.planet); put_u8(out, static_cast<std::uint8_t>(r.type)); put_u32(out, r.recipient); }
    put_u8(out, setup.multiplayer ? 1 : 0); put_u8(out, setup.fast_corruption ? 1 : 0); put_u8(out, setup.tutorial_corruption_allowed ? 1 : 0);
    put_u32(out, static_cast<std::uint32_t>(setup.ruined_income.size()));
    for (const auto& r : setup.ruined_income) { put_u64(out, r.planet); put_i64(out, r.credits.raw()); }
    put_u64(out, setup.hack_required_type); put_i64(out, setup.hack_cost.raw());
    put_u32(out, static_cast<std::uint32_t>(setup.activated_hack_abilities.size()));
    for (const auto& name : setup.activated_hack_abilities) text_value(out, name);
    put_u32(out, static_cast<std::uint32_t>(setup.activated_destruction_abilities.size()));
    for (const auto& name : setup.activated_destruction_abilities) text_value(out, name);
}
std::optional<GalacticAbilitySetup> decode_ability_setup(std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes); GalacticAbilitySetup result; std::uint32_t version{}, count{}; std::int64_t value{};
    if (!in.read_u32(version) || version != 6 || !in.read_i64(value) || value <= 0
        || !in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 32) return {};
    result.default_hero_absence = math::Fixed::from_raw(value); TypeId previous{};
    for (std::uint32_t i = 0; i < count; ++i) {
        GalacticAbilityProfile p; std::uint8_t despawn{}, named{}, whole{}, enabled{}; std::uint32_t ranges{};
        if (!in.read_u64(p.type) || p.type <= previous || !in.read_i64(value) || value < 0
            || !in.read_u8(despawn) || despawn > 1 || !in.read_u8(named) || named > 1
            || !in.read_u8(whole) || whole > 1 || !in.read_u8(enabled) || enabled > 1 || !in.read_u64(p.respawn_structure)
            || !in.read_u32(ranges) || (ranges != 0 && ranges != 5) || ranges > in.remaining() / 16) return {};
        previous = p.type; p.market.price_modifier = math::Fixed::from_raw(value);
        p.market.causes_despawn = despawn != 0; p.named = named != 0; p.respawn_whole_team = whole != 0; p.initially_enabled = enabled != 0;
        for (std::uint32_t j = 0; j < ranges; ++j) {
            std::int64_t low{}, high{};
            if (!in.read_i64(low) || low < 0 || !in.read_i64(high) || high < low) return {};
            p.market.absence_min.push_back(math::Fixed::from_raw(low)); p.market.absence_max.push_back(math::Fixed::from_raw(high));
        }
        std::uint8_t market{}, has_theft{};
        if (!in.read_u8(market) || market > 1 || !in.read_u8(has_theft) || has_theft > 1) return {};
        p.has_market = market != 0;
        if (has_theft) {
            auto& t = p.theft.emplace(); std::int64_t price{}, chance{}; std::uint8_t theft_enabled{};
            if (!curve(in, t.availability) || !curve(in, t.ground) || !curve(in, t.space) || !curve(in, t.difficulty)
                || !in.read_i64(price) || !in.read_i64(chance) || !in.read_u8(theft_enabled) || theft_enabled > 1
                || !in.read_u32(ranges) || (ranges && ranges != 5) || ranges > in.remaining() / 16) return {};
            t.affinity_price = math::Fixed::from_raw(price); t.affinity_chance = math::Fixed::from_raw(chance); t.initially_enabled = theft_enabled != 0;
            for (std::uint32_t j = 0; j < ranges; ++j) {
                std::int64_t low{}, high{};
                if (!in.read_i64(low) || low < 0 || !in.read_i64(high) || high < low) return {};
                t.absence_min.push_back(math::Fixed::from_raw(low)); t.absence_max.push_back(math::Fixed::from_raw(high));
            }
        }
        std::uint8_t corruption{}, lifecycle{};
        if (!in.read_u8(corruption) || corruption > 1 || !in.read_u8(lifecycle) || lifecycle > 1) return {};
        p.lifecycle_only = lifecycle != 0;
        if (corruption) {
            auto& c = p.corruption.emplace(); std::uint8_t flag{};
            if (!text_value(in, c.name) || c.name.empty() || !in.read_u8(flag) || flag > 1) return {};
            c.initially_enabled = flag != 0;
        }
        std::uint8_t has_hack{}, has_destruction{};
        if (!in.read_u8(has_hack) || has_hack > 1) return {};
        if (has_hack) {
            auto& h = p.hack.emplace(); std::uint8_t flag{};
            if (!text_value(in, h.name) || h.name.empty() || !in.read_u8(flag) || flag > 1) return {};
            h.initially_enabled = flag != 0;
        }
        if (!in.read_u8(has_destruction) || has_destruction > 1
            || (!market && !has_theft && !corruption && !lifecycle && !has_hack && !has_destruction)) return {};
        if (has_destruction) {
            auto& d = p.destruction.emplace(); std::uint8_t flag{};
            if (!text_value(in, d.name) || d.name.empty() || !in.read_u8(flag) || flag > 1) return {};
            d.initially_enabled = flag != 0;
        }
        result.profiles.push_back(std::move(p));
    }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 17) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        TheftTechnology t; std::uint8_t space{};
        if (!in.read_u64(t.type) || !t.type || !in.read_i64(value) || !in.read_u8(space) || space > 1) return {};
        t.price = math::Fixed::from_raw(value); t.space = space != 0; result.technologies.push_back(t);
    }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 29) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        TheftPlanet p; std::int64_t difficulty{}; std::uint8_t capture{}; std::uint32_t affinities{};
        if (!in.read_u64(p.planet) || !p.planet || (!result.theft_planets.empty() && p.planet <= result.theft_planets.back().planet)
            || !in.read_i64(value) || !in.read_i64(difficulty) || !in.read_u8(capture) || capture > 2
            || !in.read_u32(affinities) || affinities > galactic_max_objects || affinities > in.remaining() / 8) return {};
        p.availability = math::Fixed::from_raw(value); p.difficulty = math::Fixed::from_raw(difficulty);
        if (capture) p.auto_capture_minor = capture == 2;
        TypeId last{};
        for (std::uint32_t j = 0; j < affinities; ++j) { TypeId id{}; if (!in.read_u64(id) || id <= last) return {}; last = id; p.affinity_types.push_back(id); }
        result.theft_planets.push_back(std::move(p));
    }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 4) return {};
    for (std::uint32_t i = 0; i < count; ++i) { std::string name; if (!text_value(in, name) || name.empty()) return {}; result.activated_corruption_abilities.push_back(std::move(name)); }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 17) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        CorruptionPlanet p; std::uint8_t retained{};
        if (!in.read_u64(p.planet) || !p.planet || !in.read_u64(p.type) || !p.type || !in.read_u8(retained) || retained > 1) return {};
        p.retains_influence = retained != 0;
        for (auto& c : p.slots) if (!corruption_choice(in, c)) return {};
        result.corruption_planets.push_back(std::move(p));
    }
    if (!in.read_u32(count) || count > galactic_max_players || count > in.remaining() / 76) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        CorruptionPlayer p; if (!in.read_u32(p.player) || !p.player) return {};
        for (auto& id : p.automatic_upgrades) if (!in.read_u64(id)) return {};
        result.corruption_players.push_back(p);
    }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 10) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        CorruptionPrerequisiteType p; std::uint8_t consume{}, named{};
        if (!in.read_u64(p.type) || !p.type || !in.read_u8(consume) || consume > 1 || !in.read_u8(named) || named > 1) return {};
        p.consumable = consume != 0; p.named = named != 0; result.corruption_prerequisites.push_back(p);
    }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 13) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        InitialCorruption r; std::uint8_t type{};
        if (!in.read_u64(r.planet) || !r.planet || !in.read_u8(type) || type > 8 || !in.read_u32(r.recipient) || !r.recipient) return {};
        r.type = static_cast<corruption_fixture::Type>(type); result.initial_corruption.push_back(r);
    }
    std::uint8_t multiplayer{}, fast{}, tutorial{};
    if (!in.read_u8(multiplayer) || multiplayer > 1 || !in.read_u8(fast) || fast > 1 || !in.read_u8(tutorial) || tutorial > 1) return {};
    result.multiplayer = multiplayer != 0; result.fast_corruption = fast != 0; result.tutorial_corruption_allowed = tutorial != 0;
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 16) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        RuinedPlanetIncome r; std::int64_t credits{};
        if (!in.read_u64(r.planet) || !r.planet || (!result.ruined_income.empty() && r.planet <= result.ruined_income.back().planet)
            || !in.read_i64(credits) || credits < 0) return {};
        r.credits = math::Fixed::from_raw(credits); result.ruined_income.push_back(r);
    }
    std::int64_t hack_cost{};
    if (!in.read_u64(result.hack_required_type) || !in.read_i64(hack_cost) || hack_cost < 0 || !in.read_u32(count)
        || count > galactic_max_objects || count > in.remaining() / 4) return {};
    result.hack_cost = math::Fixed::from_raw(hack_cost);
    for (std::uint32_t i = 0; i < count; ++i) { std::string name; if (!text_value(in, name) || name.empty()) return {}; result.activated_hack_abilities.push_back(std::move(name)); }
    if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining() / 4) return {};
    for (std::uint32_t i = 0; i < count; ++i) { std::string name; if (!text_value(in, name) || name.empty()) return {}; result.activated_destruction_abilities.push_back(std::move(name)); }
    return in.remaining() ? std::nullopt : std::optional(std::move(result));
}
void encode_ability_state(std::vector<std::uint8_t>& out, const GalacticAbilityState& s) {
    put_u32(out, 6); put_u64(out, s.next_request); // coordinator-reserved encoding 6
    put_u32(out, static_cast<std::uint32_t>(s.actors.size()));
    for (const auto& a : s.actors) {
        put_u64(out, a.actor); put_u64(out, a.type); put_u8(out, a.enabled ? 1 : 0); put_u8(out, a.cancelled ? 1 : 0);
        put_u8(out, a.theft_enabled ? 1 : 0); put_u8(out, a.theft_cancelled ? 1 : 0);
        put_u8(out, a.corruption_enabled ? 1 : 0); put_u8(out, a.corruption_cancelled ? 1 : 0);
        put_u8(out, a.hack_enabled ? 1 : 0); put_u8(out, a.hack_cancelled ? 1 : 0);
        put_u8(out, a.destruction_enabled ? 1 : 0); put_u8(out, a.destruction_cancelled ? 1 : 0);
        put_u8(out, a.contested_check_disabled ? 1 : 0);
    }
    put_u32(out, static_cast<std::uint32_t>(s.preparations.size()));
    for (const auto& p : s.preparations) {
        put_u64(out, p.id); put_u64(out, p.actor); put_u64(out, p.planet); put_i64(out, p.absence.raw());
        put_u8(out, p.open ? 1 : 0); put_u32(out, static_cast<std::uint32_t>(p.offers.size()));
        for (const auto& offer : p.offers) { put_u32(out, offer.item); put_i64(out, offer.price.raw()); put_u8(out, offer.owned ? 1 : 0); }
        put_u8(out, static_cast<std::uint8_t>(p.kind)); put_u32(out, static_cast<std::uint32_t>(p.theft_choices.size()));
        for (const auto& t : p.theft_choices) theft(out, t);
        put_u32(out, static_cast<std::uint32_t>(p.corruption_choices.size()));
        for (const auto& c : p.corruption_choices) { put_u8(out, static_cast<std::uint8_t>(c.type)); put_u8(out, c.slot); }
    }
    put_u32(out, static_cast<std::uint32_t>(s.results.size()));
    for (const auto& r : s.results) {
        put_u64(out, r.request); put_u64(out, r.preparation); put_u64(out, r.actor); put_u64(out, r.planet);
        put_u32(out, r.player); put_u32(out, r.item); put_u8(out, static_cast<std::uint8_t>(r.outcome));
        put_u8(out, static_cast<std::uint8_t>(r.kind)); put_u64(out, r.technology);
    }
    put_u32(out, static_cast<std::uint32_t>(s.transitions.size()));
    for (const auto& r : s.transitions) {
        put_u64(out, r.planet); const auto& t = r.transition;
        put_u8(out, static_cast<std::uint8_t>(t.current)); put_u8(out, static_cast<std::uint8_t>(t.target));
        put_u8(out, t.increasing ? 1 : 0); put_u8(out, t.decreasing ? 1 : 0);
        put_u64(out, t.start); put_u64(out, t.end); put_u64(out, t.last_change);
    }
    put_u32(out, static_cast<std::uint32_t>(s.missions.size()));
    for (const auto& m : s.missions) {
        put_u64(out, m.request); put_u64(out, m.actor); put_u64(out, m.planet); put_u32(out, m.player);
        put_u8(out, static_cast<std::uint8_t>(m.choice.type)); put_u8(out, m.choice.slot); put_u8(out, static_cast<std::uint8_t>(m.state));
    }
    put_u32(out, static_cast<std::uint32_t>(s.corruption_usage.size()));
    for (const auto& p : s.corruption_usage) { put_u32(out, p.player); for (const auto count : p.uses) put_u32(out, count); }
    put_u32(out, static_cast<std::uint32_t>(s.planet_entitlements.size()));
    for (const auto& p : s.planet_entitlements) { put_u64(out, p.planet); put_u32(out, p.player); identities(out, p.automatic_upgrades); identities(out, p.tactical_upgrades); identities(out, p.build_unlocks); }
}
} // namespace eawr::sim::galactic::detail
