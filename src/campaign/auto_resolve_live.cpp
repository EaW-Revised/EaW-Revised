#include "eawr/campaign/auto_resolve_live.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "../script/foc/tactical_ai_internal.hpp"
#include "../script/foc/host.hpp"
#include "checkpoint_fields_internal.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
using sim::math::Fixed;
const auto one = Fixed::from_raw(Fixed::scale);
std::string key(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    std::string out(value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1)); for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); return out;
}
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup); d.message = "live conflict: " + message;
    return core::Result<T>::failure(std::move(d));
}
bool behaves(const units::GalacticType& type, std::string_view behavior) {
    for (const auto* list : {&type.behavior, &type.space_behavior, &type.land_behavior})
        if (std::any_of(list->begin(), list->end(), [&](const auto& name) { return key(name) == key(behavior); })) return true;
    return false;
}
const units::GalacticType* bound(const CampaignSetupPlan& plan, const units::GalacticTypeTable& types, gc::TypeId id) {
    const auto binding = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(), [&](const auto& row) { return row.id == id; });
    return binding == plan.type_bindings().end() ? nullptr : types.find(binding->original_type);
}
const gc::MovementContents* contents(const gc::GalacticSnapshot& snapshot, sim::EntityId id) {
    if (!snapshot.movement()) return nullptr;
    const auto& rows = snapshot.movement()->contents;
    const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.container == id; });
    return at == rows.end() ? nullptr : &*at;
}
const gc::ControlPlanet* control_planet(const gc::GalacticSnapshot& snapshot, gc::PlanetId id) {
    if (!snapshot.control()) return nullptr;
    const auto& rows = snapshot.control()->planets;
    const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.planet == id; });
    return at == rows.end() ? nullptr : &*at;
}
struct ResolveState {
    sim::EntityId conflict{};
    AutoResolveRoundState round;
    std::optional<AutoResolveStrength> strength;
    AutoResolveRoster roster;
    std::optional<script::authoritative::RandomStream> random;
    std::optional<AutoResolveProgress> progress;
};
struct Prepared {
    CampaignSetupPlan plan;
    units::GalacticTypeTable types;
    data::Catalog catalog;
    data::galactic::GalacticData data;
    std::vector<gc::CensusObject> originals;
    std::vector<units::EnumValue> bits;
    std::vector<AutoResolveCategory> contrast;
    script::foc::detail::Host host;
    AutoResolveRoundRules rules;
    std::map<std::string, std::map<std::string, Fixed>> terrain;
    std::map<std::string, std::string> movement_class;
    std::map<gc::PlanetId, std::string> planet_types, planet_terrain;
    std::set<std::string> immune, unqualified;
    std::vector<StartingForceRelation> relations;
    Prepared(const CampaignSetupPlan& p) : plan(p) {}
};
core::Result<CombatantFacts> facts(const Prepared& p, const gc::GalacticSnapshot& snapshot,
    gc::FleetMember member, unsigned depth = 0) {
    if (depth > 32) return fail<CombatantFacts>("cyclic combatant contents");
    const auto* type = bound(p.plan, p.types, member.type);
    if (!type) return fail<CombatantFacts>("unbound combatant type");
    CombatantFacts out; out.combatant = !behaves(*type, "FLEET"); out.ai_power = type->ai_combat_power.value_or(Fixed{});
    out.stays_in_transport = type->stays_in_transport.value_or(false);
    out.kind = behaves(*type, "TRANSPORT") ? CombatantKind::transport : behaves(*type, "DUMMY_GROUND_COMPANY") ? CombatantKind::company
        : behaves(*type, "DUMMY_SPACE_FIGHTER_SQUADRON") || behaves(*type, "SQUADRON") ? CombatantKind::squadron
        : behaves(*type, "DUMMY_STAR_BASE") ? CombatantKind::space_base : behaves(*type, "DUMMY_GROUND_BASE") ? CombatantKind::ground_base
        : behaves(*type, "FLEET") ? CombatantKind::fleet : CombatantKind::ordinary;
    if (snapshot.movement()) {
        const auto& rows = snapshot.movement()->types;
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.type == member.type; });
        if (at != rows.end()) out.stealth = at->skips_combat;
    }
    if (const auto* children = contents(snapshot, member.id)) for (const auto& child : children->children) {
        auto value = facts(p, snapshot, child, depth + 1); if (!value) return value; out.contents.push_back(std::move(value).value());
    }
    return core::Result<CombatantFacts>::success(std::move(out));
}
core::Result<gc::PlanetId> retreat_destination(const gc::GalacticSnapshot& snapshot, gc::PlanetId source,
    gc::PlayerId player, bool surface) {
    const auto src = std::find_if(snapshot.planets().begin(), snapshot.planets().end(), [&](const auto& row) { return row.id == source; });
    if (src == snapshot.planets().end()) return fail<gc::PlanetId>("missing retreat source");
    // WGM-61..63: ordered selector tiers; equal nearest-distance is WGM-U03.
    const auto orbit = [&](const auto& planet) { return std::any_of(snapshot.fleets().begin(), snapshot.fleets().end(), [&](const auto& fleet) { return fleet.orbit == planet.id && fleet.owner == player; }); };
    const auto base = [&](const auto& planet) { const auto* c = control_planet(snapshot, planet.id); return c && (c->ground_base.owner == player || c->starbase.owner == player); };
    const auto neutral = [&](const auto& planet) { return snapshot.control() && std::any_of(snapshot.control()->players.begin(), snapshot.control()->players.end(), [&](const auto& row) { return row.player == planet.owner && row.neutral; }); };
    for (unsigned tier = 0; tier < 4; ++tier) {
        std::optional<Fixed> nearest; gc::PlanetId chosen{}; bool tied{};
        for (const auto& planet : snapshot.planets()) {
            if (planet.id == source) continue;
            const bool eligible = tier == 0 ? (surface ? base(planet) : orbit(planet)) : tier == 1 ? (surface ? orbit(planet) : base(planet)) : tier == 2 ? neutral(planet) : true;
            if (!eligible) continue;
            auto x = sim::math::subtract(planet.position.x, src->position.x), y = sim::math::subtract(planet.position.y, src->position.y), z = sim::math::subtract(planet.position.z, src->position.z);
            if (!x || !y || !z) return fail<gc::PlanetId>("retreat distance overflow");
            auto distance = sim::math::length(sim::math::Vec3{x.value(), y.value(), z.value()}); if (!distance) return core::Result<gc::PlanetId>::failure(distance.error());
            if (!nearest || distance.value() < *nearest) { nearest = distance.value(); chosen = planet.id; tied = false; }
            else if (distance.value() == *nearest) tied = true;
        }
        if (chosen) { if (tied) return fail<gc::PlanetId>("unqualified equal-distance retreat destinations (WGM-U03)"); return core::Result<gc::PlanetId>::success(chosen); }
    }
    return core::Result<gc::PlanetId>::success(source); // orbital fallback WGM-99; surface caller rejects WGM-U10.
}
} // namespace

std::array<AutoResolveProgress::SideRoster, 2> live_auto_resolve_progress_roster(
    const AutoResolveStrength& strength, const AutoResolveRoster& roster, const AutoResolveRoundState& round) {
    std::array<AutoResolveProgress::SideRoster, 2> result;
    for (std::size_t side = 0; side < result.size(); ++side) {
        auto& publication = result[side]; publication.owner = strength.gathered.sides[side].owner;
        const auto& queue = round.outcome ? round.outcome->retained_queues[side] : strength.gathered.sides[side].combatants;
        for (const auto& combatant : queue) {
            AutoResolveProgress::Combatant entry; entry.owner = publication.owner;
            if (combatant.planet) {
                entry.current_type = combatant.type;
                const auto& prepared = strength.gathered.sides[side].combatants;
                const auto original = std::find_if(prepared.begin(), prepared.end(), [&](const auto& value) { return value.id == combatant.id; });
                if (original != prepared.end()) entry.original_type = original->type;
            }
            else if (const auto object = std::find_if(roster.objects.begin(), roster.objects.end(),
                [&](const auto& value) { return value.id == combatant.id; }); object != roster.objects.end()) {
                entry.original_type = object->original_type; entry.current_type = object->current_type;
            } else if (const auto fleet = std::find_if(roster.fleets.begin(), roster.fleets.end(),
                [&](const auto& value) { return value.fleet.id == combatant.id; }); fleet != roster.fleets.end())
                entry.original_type = entry.current_type = fleet->squadron_type;
            // WAR-113/115: no slot promotion or combat-rating type substitution.
            publication.queue.push_back(std::move(entry));
        }
    }
    return result;
}

std::vector<ConflictForce> live_conflict_force_manifest(const ConflictRecord& conflict, const LayerPresence& presence) {
    const bool land = conflict.layer == BattleLayer::land;
    std::map<sim::EntityId, ConflictForce> forces;
    for (const auto* rows : {&presence.invading_forces, &presence.orbital_forces, &presence.landed}) {
        // WAR-14: raids add listed invaders and planet forces, excluding orbit fleets.
        if ((!land && rows == &presence.landed)
            || (land && conflict.raid && rows == &presence.orbital_forces)) continue;
        for (const auto& row : *rows) {
            if (row.force.owner != conflict.invader && row.force.owner != conflict.defender) continue;
            if (land ? !row.ground_capable : !row.space_allowed) continue;
            forces.emplace(row.force.id, row.force);
        }
    }
    std::vector<ConflictForce> out;
    for (const auto& [id, force] : forces) { (void)id; out.push_back(force); }
    return out;
}

core::Result<AutoResolveRoster> collect_live_auto_resolve_roster(const ConflictRecord& conflict,
    const gc::GalacticSnapshot& snapshot, const CampaignSetupPlan& plan, const units::GalacticTypeTable& types,
    std::span<const gc::CensusObject> originals) {
    AutoResolveRoster out; out.planet.id = conflict.planet;
    const auto planet = std::find_if(snapshot.planets().begin(), snapshot.planets().end(), [&](const auto& row) { return row.id == conflict.planet; });
    if (planet == snapshot.planets().end()) return fail<AutoResolveRoster>("missing conflict planet");
    out.planet.owner = planet->owner;
    if (!snapshot.player_economy()) return fail<AutoResolveRoster>("missing current player tech");
    for (const auto& row : snapshot.player_economy()->players) {
        if (row.current_tech < 0 || row.current_tech > std::numeric_limits<std::uint32_t>::max()) return fail<AutoResolveRoster>("invalid current tech");
        out.tech.push_back({row.player, static_cast<std::uint32_t>(row.current_tech)});
    }
    std::map<sim::EntityId, AutoResolveObject> objects;
    const auto add = [&](auto&& self, gc::FleetMember member, gc::PlayerId owner, unsigned depth) -> core::Result<void> {
        if (depth > 32) return fail<void>("cyclic roster contents");
        const auto* current = bound(plan, types, member.type); if (!current) return fail<void>("unbound live roster type");
        const auto metadata = std::find_if(originals.begin(), originals.end(), [&](const auto& row) { return row.id == member.id; });
        const auto commissioned = snapshot.commissioned_originals();
        const auto live_original = std::lower_bound(commissioned.begin(), commissioned.end(), member.id,
            [](const auto& row, auto id) { return row.id < id; });
        const auto* original = live_original != commissioned.end() && live_original->id == member.id ? bound(plan, types, live_original->original_type)
            : metadata == originals.end() ? current : bound(plan, types, metadata->original_type);
        if (!original) return fail<void>("unbound original roster type");
        AutoResolveObject value; value.id = member.id; value.owner = owner; value.original_type = original->name; value.current_type = current->name; value.current_type_id = member.type;
        value.kind = behaves(*current, "TRANSPORT") ? AutoResolveObjectKind::transport : behaves(*current, "DUMMY_STARSHIP") ? AutoResolveObjectKind::dummy_starship : AutoResolveObjectKind::unit;
        value.space_capable = !behaves(*current, "DUMMY_GROUND_BASE") && current->can_participate_in_space.value_or(true);
        const auto* children = contents(snapshot, member.id);
        if (children) for (const auto& child : children->children) {
            if (value.kind == AutoResolveObjectKind::transport) value.contents.push_back(child.id); else value.fleet_members.push_back(child.id);
        }
        CombatantFacts f; f.kind = value.kind == AutoResolveObjectKind::transport ? CombatantKind::transport : CombatantKind::ordinary;
        f.stays_in_transport = current->stays_in_transport.value_or(false);
        if (children) for (const auto& child : children->children) {
            const auto* type = bound(plan, types, child.type); if (!type) return fail<void>("unbound payload type");
            CombatantFacts payload; payload.combatant = true; payload.ai_power = type->ai_combat_power.value_or(Fixed{}); payload.stays_in_transport = type->stays_in_transport.value_or(false); f.contents.push_back(payload);
        }
        value.land_capable = can_participate_land_combat(f);
        value.has_special_abilities = !original->strength_bonuses.empty();
        if (!original->company_units.empty()) if (const auto* first = types.find(original->company_units.front())) value.has_special_abilities = value.has_special_abilities || !first->strength_bonuses.empty();
        const auto at = objects.find(value.id);
        if (at != objects.end()) return at->second == value ? core::Result<void>::success() : fail<void>("ambiguous live roster identity");
        objects.emplace(value.id, value);
        if (objects.size() > gc::galactic_max_objects) return fail<void>("roster limit");
        if (children) for (const auto& child : children->children) { auto added = self(self, child, owner, depth + 1); if (!added) return added; }
        return core::Result<void>::success();
    };
    std::vector<const gc::Fleet*> orbit;
    for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == conflict.planet && (fleet.owner == conflict.invader || fleet.owner == conflict.defender)) orbit.push_back(&fleet);
    std::stable_sort(orbit.begin(), orbit.end(), [](const auto* a, const auto* b) { return std::pair(a->motion.orbit_entry, a->id) < std::pair(b->motion.orbit_entry, b->id); });
    for (const auto* fleet : orbit) {
        AutoResolveFleet entry; entry.fleet = *fleet; entry.space_capable = true;
        for (const auto& member : fleet->members) { auto added = add(add, member, fleet->owner, 0); if (!added) return core::Result<AutoResolveRoster>::failure(added.error()); }
        out.fleets.push_back(std::move(entry)); out.orbit_fleets.push_back(fleet->id);
        (fleet->owner == conflict.invader ? out.invader_entries : out.defender_entries).push_back(fleet->id);
    }
    if (const auto* control = control_planet(snapshot, conflict.planet)) {
        for (const auto& force : control->landed) {
            auto added = add(add, {force.id, force.type}, force.owner, 0); if (!added) return core::Result<AutoResolveRoster>::failure(added.error());
            out.planet.landed.push_back(force.id);
        }
    }
    if (snapshot.production()) for (const auto& production : snapshot.production()->planets) if (production.planet == conflict.planet) {
        if (conflict.layer == BattleLayer::space && production.starbase_type) {
            const auto* base = bound(plan, types, production.starbase_type); if (!base) return fail<AutoResolveRoster>("unbound starbase type"); out.planet.base_type = base->name;
            out.planet.owner = production.starbase_owner;
        }
        // WAR-14: ordinary galactic ground specials retain their own actor identity.
        for (const auto& object : production.objects) if (object.domain == gc::ProductionDomain::ground && object.kind == gc::ProductionKind::structure) {
            auto added = add(add, {object.id, object.type}, object.owner, 0);
            if (!added) return core::Result<AutoResolveRoster>::failure(added.error());
            out.planet.ground_specials.push_back(object.id);
        }
        // WGM-187: production actors are not tactical build records.
    }
    if (const auto* control = control_planet(snapshot, conflict.planet)) {
        for (const auto& record : control->built_structures) {
            const auto* type = bound(plan, types, record.type);
            if (!type) return fail<AutoResolveRoster>("unbound persistent tactical structure");
            out.planet.built_structures.push_back(type->name);
        }
    }
    if (conflict.layer == BattleLayer::space && out.planet.base_type.empty() && conflict.station_type) {
        const auto* base = bound(plan, types, conflict.station_type); if (!base) return fail<AutoResolveRoster>("unbound conflict base"); out.planet.base_type = base->name; out.planet.owner = conflict.defender;
    }
    if (conflict.raid) for (const auto& force : conflict.forces) if (force.owner == conflict.invader) out.raid_entries.push_back(force.id);
    for (auto& [id, object] : objects) { (void)id; out.objects.push_back(std::move(object)); }
    return core::Result<AutoResolveRoster>::success(std::move(out));
}
bool has_unqualified_damage_handler(const data::XmlNode& abilities) {
    // The blaster redirect and absorb kinds accept only a projectile source (WAR-126); a galactic
    // request names the opposing combatant.
    const auto inspect = [](auto&& self, const data::XmlNode& node, bool projectile_gated) -> bool {
        const auto name = key(node.name);
        if (name == "ACTIVATION_STYLE" && key(node.raw_text) == "TAKE_DAMAGE" && !projectile_gated) return true;
        const bool gated = projectile_gated || name == "REDIRECT_BLASTER_ABILITY" || name == "ABSORB_BLASTER_ABILITY";
        return std::any_of(node.children.begin(), node.children.end(), [&](const auto& child) { return self(self, child, gated); });
    };
    return inspect(inspect, abilities, false);
}
namespace {
AutoResolveAttackServices attacks(const std::shared_ptr<Prepared>& p, const AutoResolveRoster& roster, gc::PlanetId planet) {
    AutoResolveAttackServices out;
    out.contrast = [p](const auto& type, const auto& category) {
        const auto bit = std::find_if(p->bits.begin(), p->bits.end(), [&](const auto& row) { return key(row.name) == key(category.name); });
        if (bit == p->bits.end()) return fail<Fixed>("unknown contrast category");
        script::foc::AiType self; self.type_id = assets::object_type_crc(type.name);
        const auto* authored = p->host.type(self.type_id);
        if (!authored) return fail<Fixed>("contrast type lacks tactical behavior flags");
        self.targeting = authored->targeting; self.unit_ai = authored->unit_ai;
        for (const auto& name : type.category_mask) for (const auto& row : p->bits) if (key(row.name) == key(name)) self.category_bits |= static_cast<std::uint64_t>(row.value);
        return script::numeric::to_fixed(script::foc::detail::average_contrast(p->host, self, static_cast<std::uint64_t>(bit->value)));
    };
    out.terrain = [p, planet](const auto& type, std::uint32_t) {
        const auto terrain = p->planet_terrain.find(planet); if (terrain == p->planet_terrain.end()) return core::Result<Fixed>::success(one);
        const auto row = p->terrain.find(terrain->second); if (row == p->terrain.end()) return core::Result<Fixed>::success(one);
        std::string name = type.name;
        if (!type.company_units.empty()) name = type.company_units.front();
        const auto movement = p->movement_class.find(key(name)); if (movement == p->movement_class.end()) return core::Result<Fixed>::success(one);
        const auto value = row->second.find(movement->second); return core::Result<Fixed>::success(value == row->second.end() ? one : value->second);
    };
    out.ground_attack = [](const auto& type) { return behaves(type, "DUMMY_GROUND_COMPANY") || behaves(type, "DUMMY_GROUND_BASE"); };
    const auto hero = [p, roster](const AutoResolveCombatant& combatant, bool after) -> core::Result<bool> {
        const auto object = std::find_if(roster.objects.begin(), roster.objects.end(), [&](const auto& row) { return row.id == combatant.id; });
        if (object == roster.objects.end()) return core::Result<bool>::success(false);
        for (const auto id : object->contents) {
            const auto child = std::find_if(roster.objects.begin(), roster.objects.end(), [&](const auto& row) { return row.id == id; });
            if (child == roster.objects.end()) return fail<bool>("missing hero payload");
            const auto* type = p->types.find(child->current_type); if (!type) return fail<bool>("missing hero type");
            if (type->named_hero.value_or(false) && (!after || p->immune.contains(key(type->name)))) return core::Result<bool>::success(true);
        }
        return core::Result<bool>::success(false);
    };
    out.contains_named_hero = [hero](const auto& combatant) { return hero(combatant, false); };
    out.post_transport_death_contains_hero = [hero](const auto& combatant) { return hero(combatant, true); };
    out.is_superweapon = [p](const auto& combatant) {
        const auto* type = p->types.find(combatant.hero_type.empty() ? combatant.type : combatant.hero_type);
        return core::Result<bool>::success(type && std::any_of(type->category_mask.begin(), type->category_mask.end(), [](const auto& name) { return key(name) == "SUPER"; }));
    };
    out.learning_type = [p](const auto& combatant) {
        const auto* type = p->types.find(combatant.type); if (!type) return core::Result<std::string>::success(std::string{});
        if (!type->company_units.empty()) {
            const auto* first = p->types.find(type->company_units.front());
            if (!first) return fail<std::string>("missing first company learning type");
            if (!behaves(*first, "TEAM")) return core::Result<std::string>::success(first->name);
        }
        return core::Result<std::string>::success(type->name);
    };
    return out;
}
AutoResolveOutcomeInputs outcome_inputs(const std::shared_ptr<Prepared>& p, AutoResolveRoster roster, gc::PlanetId planet) {
    AutoResolveOutcomeInputs inputs; inputs.planet_type = p->planet_types.at(planet);
    inputs.lethal_closure = [p, roster](const AutoResolveAllocation& allocation) -> core::Result<std::vector<sim::EntityId>> {
        std::vector<sim::EntityId> pending{allocation.id}, dead; std::set<sim::EntityId> seen;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back(); if (!seen.insert(id).second) return fail<std::vector<sim::EntityId>>("cyclic lethal delivery");
            const auto object = std::find_if(roster.objects.begin(), roster.objects.end(), [&](const auto& row) { return row.id == id; });
            if (object == roster.objects.end()) return fail<std::vector<sim::EntityId>>("missing ordinary lethal recipient");
            const auto name = key(object->current_type);
            if (p->unqualified.contains(name)) return fail<std::vector<sim::EntityId>>("unqualified ordinary damage ability/restoration (WAR-U04): " + name);
            if (p->immune.contains(name)) continue;
            dead.push_back(id);
            pending.insert(pending.end(), object->contents.rbegin(), object->contents.rend());
            pending.insert(pending.end(), object->fleet_members.rbegin(), object->fleet_members.rend());
        }
        std::sort(dead.begin(), dead.end()); return core::Result<std::vector<sim::EntityId>>::success(std::move(dead));
    };
    inputs.post_delivery_record = [p, roster, closure = inputs.lethal_closure](const auto& allocation) {
        auto dead = closure(allocation); if (!dead) return core::Result<std::vector<AutoResolveRecordEntry>>::failure(dead.error());
        std::vector<AutoResolveRecordEntry> record;
        const auto object = std::find_if(roster.objects.begin(), roster.objects.end(), [&](const auto& row) { return row.id == allocation.id; });
        if (object == roster.objects.end()) return fail<std::vector<AutoResolveRecordEntry>>("missing post-delivery container");
        const auto* original = p->types.find(object->original_type);
        if (original && !original->company_units.empty() && std::binary_search(dead.value().begin(), dead.value().end(), allocation.id))
            record.push_back({object->original_type, object->owner}); // WAR-97: retained original company portrait.
        for (const auto id : object->contents) if (!std::binary_search(dead.value().begin(), dead.value().end(), id)) {
            const auto child = std::find_if(roster.objects.begin(), roster.objects.end(), [&](const auto& row) { return row.id == id; });
            if (child == roster.objects.end()) return fail<std::vector<AutoResolveRecordEntry>>("missing post-delivery child");
            record.push_back({child->original_type, child->owner});
        }
        return core::Result<std::vector<AutoResolveRecordEntry>>::success(std::move(record));
    };
    inputs.base_level = [p](std::string_view name) {
        if (name.empty()) return core::Result<std::uint32_t>::success(0);
        const auto* type = p->types.find(name); if (!type || !type->base_level || *type->base_level < 0 || *type->base_level > std::numeric_limits<std::uint32_t>::max()) return fail<std::uint32_t>("invalid retained base level");
        return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(*type->base_level));
    };
    return inputs;
}
core::Result<void> load_inputs(Prepared& p, const vfs::Vfs& filesystem) {
    units::LoadInput input; input.catalog = &p.catalog; input.filesystem = &filesystem;
    if (p.plan.type_bindings().empty()) return fail<void>("missing bound types");
    for (const auto& type : p.types.types) input.types.push_back(type.name);
    auto tables = units::load_unit_tables(input); if (!tables) return core::Result<void>::failure(tables.error()); p.bits = std::move(tables).value().categories;
    if (p.bits.empty()) return fail<void>("missing dynamic category enum");
    for (const auto& row : p.bits) p.host.setup.content.categories.emplace(key(row.name), static_cast<std::uint64_t>(row.value));
    for (const auto& type : p.types.types) {
        script::foc::AiType ai; ai.name = key(type.name); ai.type_id = assets::object_type_crc(type.name);
        const auto* tactical_type = tables.value().find(type.name);
        if (!tactical_type) return fail<void>("auto-resolve type lacks tactical behavior flags");
        ai.targeting = tactical_type->targeting; ai.unit_ai = tactical_type->unit_ai;
        p.host.setup.content.types.push_back(ai);
        auto effective = p.catalog.resolve(type.name, data::Category::game_object); if (!effective) return core::Result<void>::failure(effective.error());
        const auto text = [&](std::string_view tag) {
            const auto* value = effective.value().value(tag); if (!value) return std::string{}; data::tag_trace::used(value->value); return key(value->value.raw_text);
        };
        p.movement_class[key(type.name)] = text("MovementClass");
        const auto immune = text("Immune_To_Damage"); if (data::retail::boolean(immune).value_or(false)) p.immune.insert(key(type.name));
        if (const auto* abilities = effective.value().value("Abilities")) {
            // WAR-86/WAR-U04: a damage-triggered handler is an explicit diagnostic unless its kind is
            // sourced as inert for an auto-resolve request (WAR-126).
            if (has_unqualified_damage_handler(abilities->value)) p.unqualified.insert(key(type.name));
        }
    }
    for (const auto& type : p.host.setup.content.types) { p.host.types.emplace(type.type_id, &type); p.host.types_by_name.emplace(type.name, &type); }
    std::map<std::string, std::string> modules;
    for (const auto* name : {"PGCommands", "PGBaseDefinitions", "PGBase", "PGDebug", "PGAICommands"}) {
        const auto path = std::string("Data/Scripts/Library/") + name + ".lua"; auto bytes = filesystem.open(path);
        if (!bytes) return core::Result<void>::failure(bytes.error());
        modules.emplace(path, std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    }
    auto contrast = script::foc::tactical_ai_detail::load_contrast(p.host, modules); if (!contrast) return contrast;
    for (const auto category : p.host.contrast_order) for (const auto& bit : p.bits) if (static_cast<std::uint64_t>(bit.value) == category) {
        // WAR-21: the authored ground enemy contrast entries are classified by their combat categories.
        const auto name = key(bit.name); p.contrast.push_back({bit.name, name == "INFANTRY" || name == "VEHICLE" || name == "AIR" || name == "STRUCTURE" || name == "LANDHERO"}); break;
    }
    if (p.contrast.empty()) return fail<void>("empty authored contrast list");
    auto terrain = data::load_document(filesystem, "Data/XML/AITerrainEffectiveness.xml"); if (!terrain) return core::Result<void>::failure(terrain.error());
    for (const auto& row : terrain.value().root.children) for (const auto& cell : row.children) {
        const auto value = data::retail::leading_number(cell.raw_text); if (!value || value->value.raw() < 0) return fail<void>("invalid terrain effectiveness");
        data::tag_trace::used(cell); p.terrain[key(row.name)][key(cell.name)] = value->value;
    }
    for (const auto& location : p.plan.locations()) if (location.planet) {
        p.planet_types.emplace(location.planet->id, location.original_type);
        auto effective = p.catalog.resolve(location.original_type, data::Category::game_object); if (!effective) return core::Result<void>::failure(effective.error());
        if (const auto* value = effective.value().value("Terrain")) { data::tag_trace::used(value->value); p.planet_terrain.emplace(location.planet->id, key(value->value.raw_text)); }
    }
    return core::Result<void>::success();
}
} // namespace

core::Result<LiveConflictAdapters> make_stock_auto_resolver(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan, std::span<const gc::CensusObject> originals, AutoResolveTimeProvider time,
    std::pair<GameSession::AutoResolveStateWriter, GameSession::AutoResolveStateReader>* checkpoint) {
    auto p = std::make_shared<Prepared>(plan); p->catalog = catalog; p->data = data; p->types = types; p->originals.assign(originals.begin(), originals.end());
    auto rules = auto_resolve_round_rules(data); if (!rules) return core::Result<LiveConflictAdapters>::failure(rules.error()); p->rules = rules.value();
    auto relations = prepare_initial_campaign_relations(data, plan.player_bindings());
    if (!relations) return core::Result<LiveConflictAdapters>::failure(relations.error());
    p->relations = std::move(relations).value().placement;
    if (auto loaded = load_inputs(*p, filesystem); !loaded) return core::Result<LiveConflictAdapters>::failure(loaded.error());
    if (!time) time = [](const gc::GalacticSnapshot& snapshot) {
        return Fixed::from_ratio(static_cast<std::int64_t>(snapshot.completed_tick()), gc::galactic_frames_per_second);
    };
    const auto runtime = std::make_shared<ResolveState>();
    const auto random_tick = std::make_shared<std::uint64_t>(0);
    if (checkpoint) {
        checkpoint->first = [runtime, random_tick]() -> core::Result<std::vector<std::uint8_t>> {
            try {
                gc::checkpoint_detail::Writer out;
                std::optional<std::pair<std::uint64_t, std::uint64_t>> random;
                if (runtime->random) random.emplace(*random_tick, runtime->random->draws());
                out(runtime->conflict, runtime->round, runtime->strength, runtime->roster, runtime->progress, random);
                return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
            } catch (const gc::checkpoint_detail::Invalid& error) { return fail<std::vector<std::uint8_t>>(error.what()); }
        };
        checkpoint->second = [p, runtime, random_tick](std::span<const std::uint8_t> bytes, const ConflictRecord& conflict,
            const gc::GalacticSnapshot& snapshot) -> core::Result<void> {
            try {
                ResolveState restored;
                std::optional<std::pair<std::uint64_t, std::uint64_t>> random;
                gc::checkpoint_detail::Reader in(bytes);
                in(restored.conflict, restored.round, restored.strength, restored.roster, restored.progress, random);
                if (in.remaining() || (restored.conflict && restored.conflict != conflict.id)
                    || restored.strength.has_value() != random.has_value()
                    || restored.progress.has_value() != random.has_value()
                    || (restored.conflict != 0) != random.has_value()
                    || (random && random->first > snapshot.completed_tick())
                    || (restored.progress && restored.progress->conflict != restored.conflict))
                    return fail<void>("inconsistent saved resolver owner");
                if (random) {
                    *random_tick = random->first;
                    restored.random.emplace(p->plan.galaxy().seed, random->first, 0, random->second);
                }
                *runtime = std::move(restored);
                return core::Result<void>::success();
            } catch (const gc::checkpoint_detail::Invalid& error) { return fail<void>(error.what()); }
        };
    }
    LiveConflictAdapters result;
    result.progress = [runtime]() { return runtime->progress; };
    result.withdraw = [runtime](sim::EntityId conflict, gc::PlayerId player) {
        if (runtime->conflict != conflict || !runtime->progress || !runtime->progress->initiated
            || !runtime->strength || !request_auto_resolve_retreat(runtime->round, *runtime->strength, player)) return false;
        runtime->progress->retreating_player = runtime->round.retreating_player;
        runtime->progress->withdraw_admitted = false;
        return true;
    };
    result.resolve = [p, runtime, time, random_tick](const ConflictRecord& conflict, const gc::GalacticSnapshot& snapshot, const sim::PartitionExecutor& executor) -> core::Result<std::optional<AutoResolveOutcomeApplication>> {
        auto now = time(snapshot); if (!now) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(now.error());
        const bool fresh = runtime->conflict != conflict.id;
        if (fresh) {
            auto roster = collect_live_auto_resolve_roster(conflict, snapshot, p->plan, p->types, p->originals); if (!roster) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(roster.error());
            auto gathered = gather_auto_resolve_sides(conflict, roster.value(), p->types); if (!gathered) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(gathered.error());
            auto strength = auto_resolve_strength(gathered.value(), p->types, p->contrast, p->bits, executor); if (!strength) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(strength.error());
            *runtime = {}; runtime->conflict = conflict.id; runtime->roster = std::move(roster).value(); runtime->strength = std::move(strength).value();
            runtime->random.emplace(p->plan.galaxy().seed, snapshot.completed_tick(), 0);
            *random_tick = snapshot.completed_tick();
            AutoResolveProgress progress; progress.conflict = conflict.id; progress.display_seconds = p->rules.display_seconds;
            for (std::size_t i = 0; i < 2; ++i) {
                const auto& side = runtime->strength->gathered.sides[i]; progress.combatants[i].owner = side.owner;
            }
            runtime->progress = std::move(progress);
        }
        const auto& gathered = runtime->strength->gathered;
        const auto& roster = runtime->roster;
        AutoResolveRoundContext context; context.terrain_known = p->planet_terrain.contains(conflict.planet);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto owner = gathered.sides[i].owner;
            const auto player = std::find_if(p->plan.player_bindings().begin(), p->plan.player_bindings().end(), [&](const auto& row) { return row.player.player_id == owner; });
            if (player == p->plan.player_bindings().end()) return fail<std::optional<AutoResolveOutcomeApplication>>("missing player traits");
            const auto* faction = p->data.faction(player->faction); if (!faction) return fail<std::optional<AutoResolveOutcomeApplication>>("missing faction traits");
            context.players[i] = {player->active, std::any_of(snapshot.seats().begin(), snapshot.seats().end(), [&](const auto& seat) { return seat.active && seat.player == owner && seat.kind == gc::SeatKind::human; }), faction->is_playable.value_or(false)};
        }
        const auto services = attacks(p, roster, conflict.planet);
        const auto frame = Fixed::from_ratio(1, gc::galactic_frames_per_second).value();
        if (snapshot.clock().pause_depth && p->rules.display_seconds > frame)
            return fail<std::optional<AutoResolveOutcomeApplication>>("unqualified positive modded resolver countdown under pause (GFS-U07)");
        auto step = fresh ? initiate_auto_resolve_round(now.value(), *runtime->strength, p->types, p->bits, p->rules, context, services, *runtime->random)
            : step_auto_resolve_round_transaction(runtime->round, now.value(), false, *runtime->strength, p->types, p->bits, p->rules, context, services, *runtime->random);
        if (!step) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(step.error());
        runtime->round = step.value().state;
        runtime->progress->roster = live_auto_resolve_progress_roster(*runtime->strength, runtime->roster, runtime->round);
        runtime->progress->initiated = true;
        runtime->progress->retreating_player = runtime->round.retreating_player;
        runtime->progress->withdraw_admitted = !runtime->round.retreating_player;
        auto elapsed = sim::math::subtract(now.value(), runtime->round.start_seconds); if (!elapsed) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(elapsed.error());
        runtime->progress->elapsed_mode_seconds = elapsed.value();
        if (step.value().progress == AutoResolveRoundProgress::not_done) return core::Result<std::optional<AutoResolveOutcomeApplication>>::success({});
        if (!p->planet_types.contains(conflict.planet)) return fail<std::optional<AutoResolveOutcomeApplication>>("missing original planet type");
        auto application = plan_auto_resolve_outcome(conflict, roster, *step.value().state.outcome, outcome_inputs(p, roster, conflict.planet));
        if (!application) return core::Result<std::optional<AutoResolveOutcomeApplication>>::failure(application.error());
        return core::Result<std::optional<AutoResolveOutcomeApplication>>::success(std::move(application).value());
    };
    result.outcomes.control = [](gc::GalacticSession& galaxy, const AutoResolveOutcomeApplication& a) {
        gc::ControlBattleReturn returned; returned.planet = a.planet; returned.invader = a.invader; returned.defender = a.defender;
        returned.winner = a.result.winner; returned.land = a.land; returned.station_level = a.result.station_level; returned.destroyed = a.destroyed;
        const auto snapshot = galaxy.snapshot();
        for (const auto id : a.land_invaders) {
            std::optional<gc::SurfaceForce> force;
            if (const auto* planet = control_planet(*snapshot, a.planet)) for (const auto& row : planet->landed) if (row.id == id) force = row;
            for (const auto& fleet : snapshot->fleets()) if (fleet.orbit == a.planet && fleet.owner == a.invader) for (const auto& member : fleet.members) if (member.id == id) force = gc::SurfaceForce{id, member.type, fleet.owner};
            if (!force) { return fail<void>("missing surviving land transport"); }
            returned.surviving_invaders.push_back(*force);
        }
        return galaxy.apply_control_battle(returned);
    };
    result.outcomes.retreat = [](gc::GalacticSession& galaxy, const AutoResolveOutcomeApplication& a) {
        if (!a.result.retreat_player) return core::Result<void>::success();
        auto destination = retreat_destination(*galaxy.snapshot(), a.planet, *a.result.retreat_player, a.land);
        if (!destination) return core::Result<void>::failure(destination.error());
        return galaxy.apply_retreat_return(*a.result.retreat_player, a.planet, destination.value(), a.land, a.retreat_fleets, a.destroyed);
    };
    result.access = [p](const gc::GalacticSnapshot& snapshot, const sim::PartitionExecutor& executor) -> core::Result<std::vector<gc::MovementAccess>> {
        if (!snapshot.movement() || !snapshot.control()) return fail<std::vector<gc::MovementAccess>>("runtime access requires current presence");
        std::vector<gc::MovementAccess> rows = snapshot.movement()->access;
        std::vector<std::optional<core::Diagnostic>> errors(rows.size());
        const auto enemy = [&](gc::PlayerId a, gc::PlayerId b) {
            return std::any_of(p->relations.begin(), p->relations.end(), [&](const auto& relation) { return relation.player == a && relation.other == b && relation.enemy; });
        };
        auto phase = executor.execute_phase("galactic-movement-access", sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, rows.size());
            for (auto i = range.begin; i < range.end; ++i) {
                auto& row = rows[i]; const auto* planet = control_planet(snapshot, row.planet);
                if (!planet) { errors[i] = fail<void>("missing runtime control planet").error(); continue; }
                row.potential_conflict = planet->starbase.level != 0 && enemy(row.player, planet->starbase.owner);
                for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == row.planet && enemy(row.player, fleet.owner)) {
                    bool any{}, all_stealth = true;
                    for (const auto& member : fleet.members) {
                        auto value = facts(*p, snapshot, member); if (!value) { errors[i] = value.error(); break; }
                        const auto stealth = [&](auto&& self, const CombatantFacts& f) -> bool {
                            if (f.contents.empty()) return f.kind != CombatantKind::transport && f.stealth;
                            return std::all_of(f.contents.begin(), f.contents.end(), [&](const auto& child) { return self(self, child); });
                        };
                        any = true; all_stealth = all_stealth && stealth(stealth, value.value());
                    }
                    row.potential_conflict = row.potential_conflict || (any && !all_stealth);
                }
            }
        });
        if (!phase) return core::Result<std::vector<gc::MovementAccess>>::failure(phase.error());
        for (const auto& error : errors) if (error) return core::Result<std::vector<gc::MovementAccess>>::failure(*error);
        return core::Result<std::vector<gc::MovementAccess>>::success(std::move(rows));
    };
    const auto admission = [p](const gc::GalacticSnapshot& snapshot, const gc::GalacticEvent& event, sim::EntityId first_free,
        const gc::GalacticSession::NativeInvasion* explicit_invasion) -> core::Result<std::optional<RecordedAdmission>> {
        const auto qualify = [&](gc::FleetMember member, gc::PlayerId owner, sim::EntityId parent) -> core::Result<AdmissionForce> {
            auto f = facts(*p, snapshot, member); if (!f) return core::Result<AdmissionForce>::failure(f.error());
            const auto* type = bound(p->plan, p->types, member.type);
            AdmissionForce out; out.force = {member.id, member.type, owner, parent}; out.force.hero = type->named_hero.value_or(false);
            out.power = type->ai_combat_power.value_or(Fixed{}); out.population = type->population_value.value_or(Fixed{});
            out.space_allowed = type->can_participate_in_space.value_or(true); out.triggers_space = triggers_space_combat(f.value());
            out.ground_capable = can_participate_land_combat(f.value()); out.transport = f.value().kind == CombatantKind::transport;
            out.whole_squadron = f.value().kind == CombatantKind::squadron; out.fleet_behavior = f.value().kind == CombatantKind::fleet;
            out.skips_combat = f.value().stealth; return core::Result<AdmissionForce>::success(out);
        };
        auto presence = collect_conflict_presence(snapshot, event, qualify, [&](const gc::Fleet& fleet) -> core::Result<OuterFleetFacts> {
            OuterFleetFacts out; out.fleet_behavior = true;
            for (const auto& member : fleet.members) {
                auto value = qualify(member, fleet.owner, fleet.id); if (!value) return core::Result<OuterFleetFacts>::failure(value.error());
                out.space_allowed = out.space_allowed || value.value().space_allowed;
                out.ground_capable = out.ground_capable || value.value().ground_capable;
            }
            return core::Result<OuterFleetFacts>::success(out);
        }, explicit_invasion);
        if (!presence) return core::Result<std::optional<RecordedAdmission>>::failure(presence.error());
        const bool land = event.kind == gc::GalacticEventKind::land_conflict_requested;
        if (!(land ? admits_land_conflict(presence.value()) : admits_space_conflict(presence.value()))) return core::Result<std::optional<RecordedAdmission>>::success({});
        if (first_free == std::numeric_limits<sim::EntityId>::max()) return fail<std::optional<RecordedAdmission>>("conflict allocator exhausted");
        ConflictRecord record; record.id = first_free; record.layer = land ? BattleLayer::land : BattleLayer::space; record.planet = event.value;
        record.invader = presence.value().invader; record.defender = presence.value().defender; record.station_level = presence.value().starbase_level;
        record.tactical.first_free_entity_id = first_free + 1;
        record.tactical.content_identity = p->plan.galaxy().content_identity;
        record.forces = live_conflict_force_manifest(record, presence.value());
        for (const auto& seat : snapshot.seats()) if (seat.active) record.seats.push_back({seat.seat_id, seat.player == record.invader || seat.player == record.defender ? seat.player : 0});
        const auto location = p->planet_types.find(record.planet); if (location == p->planet_types.end()) return fail<std::optional<RecordedAdmission>>("missing admission planet type");
        const auto* authored = p->data.planet(location->second); if (!authored) return fail<std::optional<RecordedAdmission>>("missing admission planet XML");
        record.map_identity = land ? authored->land_tactical_map : authored->space_tactical_map;
        ArbitrationInputs inputs; inputs.layer = record.layer;
        const auto human = [&](gc::PlayerId owner) { return std::any_of(snapshot.seats().begin(), snapshot.seats().end(), [&](const auto& seat) { return seat.active && seat.player == owner && seat.kind == gc::SeatKind::human; }); };
        if (const auto* queue = p->data.constant("Strategic_Queue_Tactical_Battles")) inputs.queue_tactical = data::retail::boolean(queue->text).value_or(false);
        inputs.multiplayer = std::count_if(snapshot.seats().begin(), snapshot.seats().end(), [](const auto& seat) { return seat.active && seat.kind == gc::SeatKind::human; }) > 1;
        inputs.invader_human = human(record.invader); inputs.defender_human = human(record.defender);
        inputs.local_involved = p->plan.local_observer() && (*p->plan.local_observer() == record.invader || *p->plan.local_observer() == record.defender);
        const auto* campaign = p->data.campaign(p->plan.campaign_name()); if (!campaign) return fail<std::optional<RecordedAdmission>>("missing campaign admission rules");
        inputs.tutorial_disables_auto = campaign->tutorial.value_or(false);
        inputs.story_restricts_auto = campaign->is_autoresolve_allowed == false || std::any_of(campaign->autoresolve_exclusion_locations.begin(), campaign->autoresolve_exclusion_locations.end(), [&](const auto& name) { return key(name) == key(location->second); });
        auto arbitration = arbitrate_conflict(inputs); if (!arbitration) return core::Result<std::optional<RecordedAdmission>>::failure(arbitration.error());
        return core::Result<std::optional<RecordedAdmission>>::success(RecordedAdmission{event.tick, std::move(record), arbitration.value()});
    };
    result.admission = [admission](const gc::GalacticSnapshot& snapshot, const gc::GalacticEvent& event, sim::EntityId first_free) {
        return admission(snapshot, event, first_free, nullptr);
    };
    result.native_admission = [admission](const gc::GalacticSnapshot& snapshot, const gc::GalacticSession::NativeInvasion& invasion, sim::EntityId first_free) {
        const gc::GalacticEvent event{snapshot.completed_tick(), gc::GalacticEventKind::land_conflict_requested, {}, invasion.fleet, invasion.request, invasion.planet};
        return admission(snapshot, event, first_free, &invasion);
    };
    return core::Result<LiveConflictAdapters>::success(std::move(result));
}

core::Result<void> install_live_conflict_adapters(GameSession& game, const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan, GameSession::ContentResolver content,
    AutoResolveTimeProvider time, std::shared_ptr<const StockSpaceEntry> space) {
    bind_story_identities(game, plan); // also rebuilt for fresh checkpoint owners
    const auto& census = game.galaxy().setup().census;
    std::pair<GameSession::AutoResolveStateWriter, GameSession::AutoResolveStateReader> checkpoint;
    auto adapters = make_stock_auto_resolver(filesystem, catalog, data, types, plan, census ? std::span<const gc::CensusObject>(census->objects) : std::span<const gc::CensusObject>{}, std::move(time), &checkpoint);
    if (!adapters) return core::Result<void>::failure(adapters.error());
    GameSession::ScriptFactory scripts;
    if (!content || space) {
        if (!space) {
            auto prepared = prepare_stock_space_entry(filesystem, catalog, data, types, plan);
            if (!prepared) return core::Result<void>::failure(prepared.error());
            space = std::move(prepared).value();
        }
        if (!content) content = [space](const ConflictRecord& record) { return space->content(record); };
        game.set_entry_resolver([space](const ConflictRecord& record, const gc::GalacticSnapshot& snapshot, sim::EntityId first_free) {
            return space->resolve(record, snapshot, first_free);
        });
        game.set_result_decorator([space](const ConflictRecord& record, const sim::tactical::TacticalSession& battle, BattleResult& result) {
            return space->decorate_result(record, battle, result);
        });
        if (auto bound = space->bind_game(game); !bound) return bound;
        auto ai = space_entry_script_services(space);
        scripts = std::move(ai.scripts);
        game.set_child_configurator(std::move(ai.bind));
    }
    game.set_movement_access_provider(std::move(adapters.value().access));
    game.set_auto_resolve_progress_reader(std::move(adapters.value().progress));
    game.set_auto_resolve_withdrawal(std::move(adapters.value().withdraw));
    game.set_auto_resolver(std::move(adapters.value().resolve));
    game.set_auto_resolve_checkpoint_services(std::move(checkpoint.first), std::move(checkpoint.second));
    game.set_auto_resolve_outcome_services(std::move(adapters.value().outcomes));
    game.set_conflict_service(std::move(adapters.value().admission), std::move(content), std::move(scripts));
    game.set_native_conflict_service(std::move(adapters.value().native_admission));
    return core::Result<void>::success();
}
} // namespace eawr::campaign
