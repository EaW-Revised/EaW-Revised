#include "eawr/sim/galactic/story_adapters.hpp"
#include "eawr/sim/galactic/story_rewards.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace eawr::sim::galactic::story {
namespace {
bool equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    for (std::size_t i = 0; i < a.size(); ++i) if (upper(a[i]) != upper(b[i])) return false;
    return true;
}
bool contains(std::span<const std::string> names, std::string_view name) {
    return std::any_of(names.begin(), names.end(), [&](const auto& n) { return equal(n, name); });
}
bool dispatchable(const DispatchGate& g) {
    return g.foreground_state && !g.attract_demo && g.active_mode && (g.foreground || g.multiplayer || g.replay);
}
void deliver(std::vector<Delivery>& out, const Notification& n, std::span<const Recipient> recipients) {
    // WST-14: recipient order is supplied by the engine, never inferred here (WST-U01).
    for (const auto& r : recipients)
        if (r.active && (!n.player || *n.player == r.owner)) out.push_back({r.plot, n});
}
bool fleet_match(const AdmissionEvent& e, const TravelNotice& n, bool friendly) {
    if (!e.planets.empty() && !contains(e.planets, n.planet)) return false;
    if (e.load_tactical_map) { // WST-35: hero present at planet is copied into query fleet input.
        return e.heroes.empty() || std::any_of(e.heroes.begin(), e.heroes.end(),
            [&](const auto& h) { return contains(n.fleet.heroes, h); });
    }
    if (e.kind == Travel::move_hero || e.kind == Travel::deploy_hero) {
        return (n.hero && contains(e.heroes, *n.hero)) || std::any_of(n.fleet.heroes.begin(), n.fleet.heroes.end(),
            [&](const auto& h) { return contains(e.heroes, h); });
    }
    if ((e.filter == OwnerFilter::friendly && !friendly) || (e.filter == OwnerFilter::enemy && friendly)) return false;
    if (n.fleet.stealth && !e.allow_stealth) return false;
    if (e.raid && !n.fleet.raid_capable) return false;
    if (!std::all_of(e.required_types.begin(), e.required_types.end(),
        [&](const auto& t) { return contains(n.fleet.types, t); })) return false;
    if (e.exclusive && !std::all_of(n.fleet.types.begin(), n.fleet.types.end(),
        [&](const auto& t) { return contains(e.required_types, t); })) return false;
    return e.orbit_types.empty() || std::any_of(e.orbit_types.begin(), e.orbit_types.end(),
        [&](const auto& t) { return contains(n.fleet.orbit_types, t); });
}
template<class T> auto* named(std::vector<T>& rows, std::string_view name) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return equal(r.name, name); });
    return it == rows.end() ? nullptr : &*it;
}
RewardResult invalid(std::string message) { return {RewardStatus::invalid, std::move(message), {}}; }
PlayerState* player(FixtureState& s, Player id) {
    const auto it = std::find_if(s.players.begin(), s.players.end(), [&](const auto& p) { return p.id == id; });
    return it == s.players.end() ? nullptr : &*it;
}
PlayerState* faction(FixtureState& s, std::string_view name) {
    const auto it = std::find_if(s.players.begin(), s.players.end(), [&](const auto& p) { return equal(p.faction, name); });
    return it == s.players.end() ? nullptr : &*it;
}
bool safe(const Planet& p, Player owner) {
    // Live diplomacy/orbit occupancy is a resolver seam; fixtures use exact owner identity.
    return std::all_of(p.orbit_owners.begin(), p.orbit_owners.end(), [&](Player id) { return id == owner; });
}
Planet* placement(FixtureState& s, Player owner, std::string_view requested) {
    auto* reference = named(s.planets, requested);
    if (!reference) {
        const auto u = std::find_if(s.units.begin(), s.units.end(), [&](const auto& row) { return row.live && row.owner == owner; });
        if (u != s.units.end()) reference = named(s.planets, u->planet);
        if (!reference) {
            const auto p = std::find_if(s.planets.begin(), s.planets.end(), [&](const auto& row) { return row.owner == owner; });
            if (p != s.planets.end()) reference = &*p;
        }
    }
    if (!reference) return nullptr;
    // WST-40 fallback stages; fixture squared distance with stable input-order ties.
    // Live search metric and original tie/container order remain named resolver seams.
    const auto nearest = [&](bool safe_only) {
        Planet* best = nullptr;
        auto distance = std::numeric_limits<std::int64_t>::max();
        for (auto& p : s.planets) {
            if (p.owner != owner || (safe_only && !safe(p, owner))) continue;
            const auto dx = p.x - reference->x, dy = p.y - reference->y;
            const auto d = dx * dx + dy * dy;
            if (d < distance) { distance = d; best = &p; }
        }
        return best;
    };
    auto* result = nearest(false);
    if (result && !safe(*result, owner)) result = nearest(true);
    return result;
}
RewardResult apply_to(FixtureState& s, Player owner, const Reward& reward) {
    auto* p = player(s, owner);
    if (!p) return invalid("unknown reward owner");
    return std::visit([&](const auto& r) -> RewardResult {
        using T = std::decay_t<decltype(r)>;
        RewardResult out;
        if constexpr (std::is_same_v<T, Spawn>) { // WST-40, authored count defaults to one.
            const auto* type = named(s.types, r.type);
            if (!type || r.count <= 0) return invalid("unknown spawn type or non-positive count");
            for (const auto& row : s.planets)
                if (row.x < -1000000 || row.x > 1000000 || row.y < -1000000 || row.y > 1000000)
                    return invalid("fixture placement coordinates outside metric bounds");
            auto* dest = placement(s, owner, r.planet);
            if (!dest) return invalid("no owned safe spawn planet");
            const auto count = static_cast<std::uint64_t>(r.count);
            if (s.next_object == 0 || count > std::numeric_limits<Object>::max() - s.next_object)
                return invalid("spawn identity overflow");
            for (std::int64_t i = 0; i < r.count; ++i) {
                const auto id = s.next_object++;
                if (std::any_of(s.units.begin(), s.units.end(), [&](const auto& u) { return u.id == id; }))
                    return invalid("spawn identity collision");
                s.units.push_back({id, type->name, dest->name, owner, true, p->ai});
                out.effects.push_back({EffectKind::spawned, id, dest->name, 0, {}});
                if (p->ai) out.effects.push_back({EffectKind::free_store, id, dest->name, 0, {}});
                else {
                    if (p->local_human) out.effects.push_back({EffectKind::human_flash, id, dest->name, 1500, {}});
                    out.effects.push_back({EffectKind::guarding_fleets, id, dest->name, 0, {}});
                    if (type->fights_in_space) out.effects.push_back({EffectKind::orbit_relink, id, dest->name, 0, {}});
                }
            }
        } else if constexpr (std::is_same_v<T, ProductionReward>) { // WST-40 locks differ from disabled.
            if (r.types.empty()) return invalid("production reward needs a type");
            for (const auto& type : r.types) {
                auto* def = named(s.types, type);
                if (!def) return invalid("unknown production type");
                if (r.kind == ProductionSwitch::buildable && !r.lock && (!equal(def->faction, p->faction) || def->tech > p->tech)) continue;
                auto row = std::find_if(p->production.begin(), p->production.end(), [&](const auto& t) { return equal(t.type, type); });
                if (row == p->production.end()) { p->production.push_back({type, false, false}); row = p->production.end() - 1; }
                switch (r.kind) {
                case ProductionSwitch::buildable: row->locked = r.lock; break;
                case ProductionSwitch::lock: row->locked = true; break;
                case ProductionSwitch::disable: row->disabled = true; break;
                case ProductionSwitch::enable: row->disabled = false; break;
                default: return invalid("invalid production switch");
                }
                out.effects.push_back({EffectKind::production_changed, 0, {}, 0, {}});
            }
        } else if constexpr (std::is_same_v<T, CreditsReward>) { // WST-41 -> WST-33 feedback uses whole balance.
            if ((r.amount > 0 && p->credits > std::numeric_limits<std::int64_t>::max() - r.amount)
                || (r.amount < 0 && p->credits < std::numeric_limits<std::int64_t>::min() - r.amount)) return invalid("credit overflow");
            p->credits += r.amount;
            out.effects.push_back({EffectKind::credits_changed, 0, {}, 0, EconomyNotice{Economy::credits, p->credits, {}, {}, BaseKind::either, owner}});
        } else if constexpr (std::is_same_v<T, TechReward>) { // WST-41: max before current; WST-33 feedback.
            auto* target = faction(s, r.faction);
            if (!target || r.level < 0) return invalid("invalid technology faction or level");
            target->max_tech = std::max(target->max_tech, r.level);
            target->tech = r.level;
            out.effects.push_back({EffectKind::tech_changed, 0, {}, 0, EconomyNotice{Economy::tech_level, r.level, {}, {}, BaseKind::either, target->id}});
        } else if constexpr (std::is_same_v<T, RestrictionReward>) { // WST-42 verified Boolean switches only.
            auto* planet = named(s.planets, r.planet);
            if (!planet) return invalid("unknown restricted planet");
            switch (r.kind) {
            case Restriction::entry: planet->restricted = r.enabled; break;
            case Restriction::autoresolve: planet->restrict_autoresolve = r.enabled; break;
            case Restriction::corruption: planet->restrict_corruption = r.enabled; break;
            case Restriction::sabotage: planet->restrict_sabotage = r.enabled; break;
            case Restriction::black_market: planet->restrict_black_market = r.enabled; break;
            case Restriction::abilities: planet->restrict_abilities = r.enabled; break;
            case Restriction::reveal: planet->revealed = r.enabled; break;
            default: return invalid("invalid planet restriction");
            }
            out.effects.push_back({EffectKind::restriction_changed, 0, planet->name, 0, {}});
        } else if constexpr (std::is_same_v<T, VictoryReward>) { // WST-45 code delays and immediate monitor call.
            const auto* winner = faction(s, r.faction);
            if (!winner) return invalid("unknown victory faction");
            s.victory = {0, winner->id, r.suppress_text ? 1 : 7, r.suppress_text, true};
            out.effects.push_back({EffectKind::victory_test, 0, {}, s.victory.delay_seconds * 1000, {}});
        } else if constexpr (std::is_same_v<T, VictoryWaitReward>) { // WST-45.
            if (!r.supplied) return RewardResult{RewardStatus::inert, {}, {}};
            if (!r.enable) {
                if (s.victory.waits == std::numeric_limits<std::uint64_t>::max()) return invalid("victory wait overflow");
                ++s.victory.waits;
            } else if (s.victory.waits != 0) --s.victory.waits;
        } else if constexpr (std::is_same_v<T, DeferredReward>) {
            if (r.name == "DISABLE_AUTORESOLVE" || r.name == "ENABLE_AUTORESOLVE") return {RewardStatus::inert, "WST-42 inert reward", {}};
            if (r.name == "SET_SPAWN" || r.name == "ENABLE_OVERWHELMING_ODDS") return {RewardStatus::inert, "WST-42 recognised without dispatch", {}};
            out = {RewardStatus::unverified, "WST-U09 or owning-service seam: " + r.name, {}};
        }
        return out;
    }, reward);
}
} // namespace

RewardResult apply(GalacticSession& world, const Reward& reward, const PartitionExecutor& executor) {
    const auto* forced = std::get_if<VictoryReward>(&reward);
    const auto* wait = std::get_if<VictoryWaitReward>(&reward);
    if (!forced && !wait) return {RewardStatus::unverified, "live reward provider is unbound", {}};
    if ((forced && forced->faction.empty()) || (wait && !wait->supplied)) return {RewardStatus::inert, {}, {}};
    if (!world.victory()) return invalid("active galactic mode has no victory monitor");
    Player winner{};
    if (forced) {
        for (const auto& p : world.victory()->context.players)
            if (equal(p.faction, forced->faction)) { winner = p.player; break; }
        // WGS-173: unresolved faction warns but installs no condition/result.
        if (!winner) return {RewardStatus::inert, "unknown victory faction: " + forced->faction, {}};
    }
    auto applied = forced ? world.apply_story_victory(winner, forced->suppress_text, executor)
                          : world.apply_story_victory_wait(wait->enable, executor);
    if (!applied) return invalid(applied.error().message);
    return {};
}

std::string_view event_name(const Payload& payload) {
    // Public XML names mapped from WST-31..39; this is not the shared engine name table (WST-08).
    return std::visit([](const auto& n) -> std::string_view {
        using T = std::decay_t<decltype(n)>;
        std::string_view name;
        if constexpr (std::is_same_v<T, TravelNotice>) {
            switch (n.kind) {
            case Travel::enter: return "STORY_ENTER"; case Travel::move_hero: return "STORY_MOVE";
            case Travel::land: return "STORY_LAND_ON"; case Travel::deploy_hero: return "STORY_DEPLOY";
            case Travel::fleet_bounced: return "STORY_FLEET_BOUNCED"; case Travel::invasion_bounced: return "STORY_INVASION_BOUNCED";
            case Travel::planet_destroyed: return "STORY_PLANET_DESTROYED";
            }
        } else if constexpr (std::is_same_v<T, EconomyNotice>) {
            switch (n.kind) {
            case Economy::construct: return "STORY_CONSTRUCT"; case Economy::construct_level: return "STORY_CONSTRUCT_LEVEL";
            case Economy::credits: return "STORY_ACCUMULATE"; case Economy::tech_level: return "STORY_TECH_LEVEL"; case Economy::era: return "STORY_BEGIN_ERA";
            }
        } else if constexpr (std::is_same_v<T, LossNotice>) {
            switch (n.kind) { case Loss::destroy: return "STORY_DESTROY"; case Loss::destroy_base: return "STORY_DESTROY_BASE"; }
        } else if constexpr (std::is_same_v<T, BattleNotice>) {
            switch (n.kind) {
            case Battle::won: return "STORY_WIN_BATTLES"; case Battle::lost: return "STORY_LOSE_BATTLES";
            case Battle::retreat: return "STORY_RETREAT"; case Battle::victory: return "STORY_VICTORY"; case Battle::mission_lost: return "STORY_MISSION_LOST";
            }
        } else if constexpr (std::is_same_v<T, CorruptionNotice>) {
            switch (n.kind) { case Corruption::changed: return "STORY_CORRUPTION_CHANGED"; case Corruption::complete: return "STORY_CORRUPTION_TACTICAL_COMPLETE"; case Corruption::failed: return "STORY_CORRUPTION_TACTICAL_FAILED"; }
        } else if constexpr (std::is_same_v<T, TextNotice>) {
            switch (n.kind) {
            case Text::ai_notification: return "STORY_AI_NOTIFICATION"; case Text::speech_done: return "STORY_SPEECH_DONE";
            case Text::generic: return "STORY_GENERIC"; case Text::click_gui: return "STORY_CLICK_GUI";
            case Text::open_corruption: return "STORY_OPEN_CORRUPTION"; case Text::black_market: return "STORY_BUY_BLACK_MARKET"; case Text::sabotage: return "STORY_GALACTIC_SABOTAGE";
            }
        } else if constexpr (std::is_same_v<T, ViewNotice>) {
            switch (n.kind) { case View::select_planet: return "STORY_SELECT_PLANET"; case View::zoom_in: return "STORY_ZOOM_INTO_PLANET"; case View::zoom_out: return "STORY_ZOOM_OUT_PLANET"; }
        } else if constexpr (std::is_same_v<T, ElapsedNotice>) name = "STORY_ELAPSED";
        else if constexpr (std::is_same_v<T, MovieDoneNotice>) name = "STORY_MOVIE_DONE";
        else if constexpr (std::is_same_v<T, ConquestNotice>) name = "STORY_CONQUER";
        else if constexpr (std::is_same_v<T, ConquestCountNotice>) name = "STORY_CONQUER_COUNT";
        else if constexpr (std::is_same_v<T, TacticalNotice>) {
            switch (n.kind) {
            case Tactical::destroy: return "STORY_TACTICAL_DESTROY";
            case Tactical::space_started: return "STORY_SPACE_TACTICAL";
            case Tactical::land_started: return "STORY_LAND_TACTICAL";
            case Tactical::reinforce: return "STORY_REINFORCE";
            case Tactical::capture_hero: return "STORY_CAPTURE_HERO";
            case Tactical::defeat_hero: return "STORY_DEFEAT_HERO";
            case Tactical::garrison: return "STORY_GARRISON_UNIT";
            case Tactical::attack_hardpoint: return "STORY_ATTACK_HARDPOINT";
            case Tactical::select_unit: return "STORY_SELECT_UNIT";
            case Tactical::arrived: return "STORY_UNIT_ARRIVED";
            case Tactical::full_stop: return "STORY_FULL_STOP";
            case Tactical::command: return "STORY_COMMAND_UNIT";
            case Tactical::guard: return "STORY_GUARD_UNIT";
            case Tactical::proximity: return "STORY_UNIT_PROXIMITY";
            case Tactical::fog_object: return "STORY_FOG_OBJECT_REVEAL";
            case Tactical::fog_position: return "STORY_FOG_POSITION_REVEAL";
            case Tactical::capture_structure: return "STORY_CAPTURE_STRUCTURE";
            case Tactical::load_map: return "STORY_LOAD_TACTICAL_MAP";
            }
        }
        return name;
    }, payload);
}
std::string_view notification_seam(const Payload& payload) {
    return std::holds_alternative<TacticalNotice>(payload) ? "WST-35/38/39: tactical emitter/evaluator seam" : std::string_view{};
}
std::vector<LossNotice> destruction(std::string type, std::string original, std::string planet, Player owner) {
    std::vector<LossNotice> out{{Loss::destroy, type, planet, BaseKind::either, owner}};
    if (!equal(type, original)) out.push_back({Loss::destroy, std::move(original), std::move(planet), BaseKind::either, owner});
    return out;
}
FakeFeed::FakeFeed(FeedState state) : state_(std::move(state)) {
    std::uint64_t previous{};
    for (const auto& n : state_.pending) {
        if (n.sequence <= previous || n.sequence > state_.last_sequence || event_name(n.payload).empty())
            throw std::invalid_argument("invalid story feed snapshot");
        previous = n.sequence;
    }
}
std::vector<Delivery> FakeFeed::submit(Notification n, const DispatchGate& g, std::span<const Recipient> recipients) {
    if (event_name(n.payload).empty()) throw std::invalid_argument("invalid story notification kind");
    if (n.sequence == 0 || n.sequence <= state_.last_sequence) return {};
    state_.last_sequence = n.sequence;
    if (!g.foreground_state || g.attract_demo) return {}; // WST-13 dropped, not queued.
    std::vector<Delivery> out;
    if (dispatchable(g)) deliver(out, n, recipients);
    else {
        if (const auto* tactical = std::get_if<TacticalNotice>(&n.payload))
            if (tactical->kind == Tactical::destroy || tactical->kind == Tactical::space_started || tactical->kind == Tactical::land_started) return {}; // WST-13 exceptions.
        state_.pending.push_back(std::move(n));
    }
    return out;
}
std::vector<Delivery> FakeFeed::service(const DispatchGate& g, std::span<const Recipient> recipients) {
    std::vector<Delivery> out;
    if (!dispatchable(g) || g.single_step) return out;
    for (const auto& n : state_.pending) deliver(out, n, recipients);
    state_.pending.clear();
    return out;
}
bool matches_ai(const TextNotice& n, std::span<const std::string> planets, std::span<const std::string> texts) {
    return n.kind == Text::ai_notification && contains(texts, n.text)
        && (n.planet ? (!planets.empty() && contains(planets, *n.planet)) : planets.empty());
}
Admission admission(std::span<const AdmissionEvent> events, const TravelNotice& n, std::span<const OwnerRelation> relations, BaseKind kind) {
    Admission out;
    for (const auto& e : events) {
        const auto relation = std::find_if(relations.begin(), relations.end(), [&](const auto& r) { return r.event_owner == e.owner; });
        if (e.filter != OwnerFilter::any && relation == relations.end()) continue;
        const bool friendly = relation != relations.end() && relation->friendly;
        if (!e.subplot_active || !e.active || e.triggered || e.disabled || !fleet_match(e, n, friendly)) continue;
        if (!e.load_tactical_map && e.kind == Travel::enter) out.restricted_entry = true;
        if (kind == BaseKind::space && !e.load_tactical_map && e.link_tactical && (e.kind == Travel::enter || e.kind == Travel::move_hero)) out.special_space_map = true;
        if (kind == BaseKind::ground && ((e.load_tactical_map && (e.map_kind == BaseKind::either || e.map_kind == kind))
            || (!e.load_tactical_map && (e.kind == Travel::land || e.kind == Travel::deploy_hero)))) out.special_land_map = true;
    }
    return out;
}
RewardResult apply(FixtureState& state, Player owner, const Reward& reward) {
    auto staged = state;
    auto result = apply_to(staged, owner, reward);
    if (result.status == RewardStatus::applied) state = std::move(staged);
    return result;
}
} // namespace eawr::sim::galactic::story
