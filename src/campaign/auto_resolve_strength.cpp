#include "eawr/campaign/auto_resolve_strength.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace eawr::campaign {
namespace {
using sim::math::Fixed;
using sim::EntityId;
using sim::galactic::PlayerId;
using units::GalacticType;
bool same(const std::string_view a, const std::string_view b) {
    const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [&](const char x, const char y) { return lower(x) == lower(y); });
}
bool is_transport(const GalacticType& type) {
    for (const auto* behaviors : {&type.behavior, &type.space_behavior, &type.land_behavior})
        if (std::any_of(behaviors->begin(), behaviors->end(),
            [](const auto& behavior) { return same(behavior, "TRANSPORT"); })) return true;
    return false;
}
core::Diagnostic invalid(std::string message) {
    core::Diagnostic result;
    result.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
    result.message = std::move(message);
    return result;
}
template <typename T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(invalid(std::move(message)));
}

class Gather {
public:
    Gather(const ConflictRecord& conflict, const AutoResolveRoster& roster, const units::GalacticTypeTable& types)
        : conflict_(conflict), roster_(roster), types_(types) {
        result.layer = conflict.layer;
        result.aggressor = conflict.invader;
        result.planet_owner = roster.planet.owner;
        result.terrain_index = conflict.terrain_index;
        result.built_structures = roster.planet.built_structures;
        for (const auto& item : roster.objects)
            if (!item.id || !objects_.emplace(item.id, &item).second) reject("duplicate or zero roster object ID");
        for (const auto& item : roster.fleets)
            if (!item.fleet.id || item.fleet.id == roster.planet.id || objects_.contains(item.fleet.id)
                || !fleets_.emplace(item.fleet.id, &item).second) reject("ambiguous fleet ID");
        for (const auto& item : roster.tech)
            if (!item.player || !tech_.emplace(item.player, item.level).second) reject("duplicate or zero tech owner");
    }
    bool reject(std::string message) { if (!error) error = invalid(std::move(message)); return false; }
    bool gathered_force(const EntityId id) const {
        return added_.contains(id) || skipped_.contains(id);
    }
    const GalacticType* type(const std::string_view name) {
        const auto* value = types_.find(name);
        if (!value) reject("unknown auto-resolve type: " + std::string(name));
        return value;
    }
    const AutoResolveObject* object(const EntityId id) {
        const auto found = objects_.find(id);
        if (found == objects_.end()) { reject("unknown roster object"); return nullptr; }
        return found->second;
    }
    std::optional<std::uint32_t> tech(const PlayerId player) {
        const auto found = tech_.find(player);
        if (found == tech_.end()) { reject("combatant owner needs explicit tech level"); return std::nullopt; }
        return found->second;
    }
    const GalacticType* container(const GalacticType& value) {
        const auto& name = conflict_.layer == BattleLayer::space ? value.unique_space_unit : value.unique_ground_unit;
        return name.empty() ? nullptr : type(name);
    }
    const GalacticType* resolve(const AutoResolveObject& value) {
        const auto* chosen = type(value.original_type);
        if (!chosen) return nullptr;
        if (value.kind == AutoResolveObjectKind::transport) {
            for (const auto id : value.contents) {
                const auto* member = object(id);
                if (!member) return nullptr;
                const auto* member_type = type(member->current_type.empty() ? member->original_type : member->current_type);
                if (!member_type) return nullptr;
                if (const auto* replacement = container(*member_type)) { chosen = replacement; break; }
                if (error) return nullptr;
            }
        } else {
            for (const auto& member : chosen->company_units) {
                const auto* member_type = type(member);
                if (!member_type) return nullptr;
                if (const auto* replacement = container(*member_type)) { chosen = replacement; break; }
                if (error) return nullptr;
            }
        }
        const auto* current = type(value.current_type.empty() ? value.original_type : value.current_type);
        if (!current) return nullptr;
        if (current != chosen) {
            if (current->ai_combat_power.value_or(Fixed{}) > chosen->ai_combat_power.value_or(Fixed{})) chosen = current;
        }
        return error ? nullptr : chosen;
    }
    const GalacticType* hero_type(const GalacticType& value) {
        if (!value.company_units.empty()) return type(value.company_units.front());
        if (!value.squadron_units.empty()) return type(value.squadron_units.front());
        return &value;
    }
    bool contains_killer(const GalacticType& original) {
        // WAR-25: immediate type members, company before squadron; not live transport contents.
        for (const auto* members : {&original.company_units, &original.squadron_units}) {
            for (const auto& name : *members) {
                const auto* member = type(name);
                if (!member) return false;
                if (member->superweapon_killer.value_or(false)) return true;
            }
        }
        return false;
    }
    bool add(const PlayerId owner, AutoResolveCombatant combatant) {
        if (owner != conflict_.invader && owner != conflict_.defender) return reject("a third combatant owner is invalid (WAR-12)");
        if (!added_.insert(combatant.id).second) return reject("duplicate combatant (WAR-15)");
        const auto level = tech(owner);
        if (!level) return false;
        combatant.tech = *level;
        for (auto& side : result.sides) {
            if (!side.owner) side.owner = owner;
            if (side.owner == owner) { side.combatants.push_back(std::move(combatant)); return true; }
        }
        return reject("more than two owners (WAR-12)");
    }
    bool add_object(const EntityId id, const bool forced = false) {
        const auto* value = object(id);
        if (!value) return false;
        const auto* current = type(value->current_type.empty() ? value->original_type : value->current_type);
        if (!current) return false;
        if ((conflict_.layer == BattleLayer::space && current->can_participate_in_space == false)
            || (!forced && !(conflict_.layer == BattleLayer::space ? value->space_capable : value->land_capable))
            || (conflict_.layer == BattleLayer::land && value->kind == AutoResolveObjectKind::dummy_starship)) {
            skipped_.insert(id); // WAR-15: validated but deliberately absent from the strength roster.
            return true;
        }
        const auto* resolved = resolve(*value);
        if (!resolved) return false;
        const auto* bonus_type = hero_type(*resolved);
        const auto* original = type(value->original_type);
        if (!bonus_type || !original) return false;
        const auto* hero = hero_type(*original);
        if (!hero) return false;
        const auto* chosen = resolved;
        if (const auto* replacement = container(*chosen)) chosen = replacement;
        if (error) return false;
        AutoResolveCombatant combatant;
        combatant.id = value->id;
        combatant.type = chosen->name;
        combatant.transport = is_transport(*chosen);
        combatant.named_hero = chosen->named_hero.value_or(false);
        combatant.tactical = value->tactical;
        combatant.hero_type = hero->name;
        combatant.bonus_type = bonus_type->name;
        combatant.has_special_abilities = value->has_special_abilities;
        combatant.contains_superweapon_killer = contains_killer(*original);
        return !error && add(value->owner, std::move(combatant));
    }
    bool skip_fleet(const AutoResolveFleet& value) {
        skipped_.insert(value.fleet.id);
        for (const auto& member : value.fleet.members) {
            const auto* unit = object(member.id);
            if (!unit || unit->owner != value.fleet.owner || member.type != (unit->current_type_id ? unit->current_type_id : assets::object_type_crc(unit->original_type)))
                return reject("fleet membership differs from its ordered object roster");
            skipped_.insert(member.id);
        }
        return true;
    }
    bool skip_orbit_fleet(const EntityId id) {
        const auto found = fleets_.find(id);
        if (found == fleets_.end()) return reject("unknown orbit fleet");
        return skip_fleet(*found->second);
    }
    bool add_entry(const EntityId id) {
        const auto found = fleets_.find(id);
        if (found == fleets_.end()) return add_object(id);
        const auto& value = *found->second;
        if (conflict_.layer == BattleLayer::space && !value.space_capable) return skip_fleet(value);
        if (!visited_fleets_.insert(id).second) return reject("duplicate fleet entry (WAR-15)");
        if (conflict_.layer == BattleLayer::space && !value.squadron_type.empty()) {
            const auto* chosen = type(value.squadron_type);
            if (!chosen) return false;
            if (chosen->can_participate_in_space == false) { skipped_.insert(id); return true; } // WAR-15
            AutoResolveCombatant combatant;
            combatant.id = id;
            combatant.type = chosen->name;
            combatant.named_hero = chosen->named_hero.value_or(false);
            const auto* hero = hero_type(*chosen);
            if (!hero) return false;
            combatant.hero_type = hero->name;
            combatant.bonus_type = hero->name;
            combatant.has_special_abilities = value.has_special_abilities;
            combatant.contains_superweapon_killer = contains_killer(*chosen);
            if (const auto* replacement = container(*chosen)) {
                combatant.type = replacement->name;
                combatant.named_hero = replacement->named_hero.value_or(false);
                chosen = replacement;
            }
            combatant.transport = is_transport(*chosen); // WAR-24 uses the final type
            if (error) return false;
            return add(value.fleet.owner, std::move(combatant));
        }
        for (const auto& member : value.fleet.members) {
            const auto* unit = object(member.id);
            if (!unit || unit->owner != value.fleet.owner || member.type != (unit->current_type_id ? unit->current_type_id : assets::object_type_crc(unit->original_type)))
                return reject("fleet membership differs from its ordered object roster");
            if (!add_object(member.id)) return false;
        }
        return true;
    }
    bool planet() {
        const auto& planet = roster_.planet;
        if (!planet.base_type.empty()) {
            const auto* chosen = type(planet.base_type);
            if (!chosen) return false;
            AutoResolveCombatant base;
            base.id = planet.id;
            base.type = chosen->name;
            base.planet = true;
            const auto* hero = hero_type(*chosen);
            if (!hero) return false;
            base.hero_type = hero->name;
            base.bonus_type = hero->name;
            base.has_special_abilities = planet.has_special_abilities;
            base.contains_superweapon_killer = contains_killer(*chosen);
            if (error) return false;
            if (!add(planet.owner, std::move(base))) return false;
        }
        if (conflict_.layer == BattleLayer::space) return true;
        for (const auto id : planet.landed) if (!add_object(id)) return false;
        for (const auto id : planet.ground_specials) if (!add_object(id, true)) return false;
        if (planet.base_type.empty()) {
            // WAR-14/15/22: record the planet even without a base. Add its structure-only
            // contribution after landed objects so the first landed owner still selects side A.
            AutoResolveCombatant entry;
            entry.id = planet.id;
            entry.planet = true;
            if (!add(planet.owner, std::move(entry))) return false;
        }
        return bombing_run();
    }
    bool bomber_from(const GalacticType& value) {
        if (value.land_bomber_type.empty()) return false;
        const auto* bomber = type(value.land_bomber_type);
        if (!bomber) return false;
        result.bomber_type = bomber->name;
        result.bombing_side = conflict_.invader;
        return true;
    }
    bool bombing_run() {
        const auto level = tech(conflict_.invader);
        if (!level) return false;
        for (const auto id : roster_.orbit_fleets) {
            const auto fleet = fleets_.find(id);
            if (fleet == fleets_.end()) return reject("unknown orbit fleet");
            if (fleet->second->fleet.owner != conflict_.invader) continue;
            for (const auto& member : fleet->second->fleet.members) {
                const auto* object_value = object(member.id);
                if (!object_value) return false;
                const auto* original = type(object_value->original_type);
                if (!original) return false;
                for (std::size_t tech_index = 0; tech_index < original->starting_spawned_units.size() && tech_index <= *level; ++tech_index) {
                    auto selected = tech_index;
                    while (selected > 0 && original->starting_spawned_units[selected].empty()) --selected;
                    for (const auto& spawn : original->starting_spawned_units[selected]) {
                        const auto* spawned = type(spawn.type);
                        if (!spawned) return false;
                        for (const auto& unit_name : spawned->squadron_units) {
                            const auto* unit = type(unit_name);
                            if (!unit) return false;
                            if (bomber_from(*unit)) return true;
                            if (error) return false;
                        }
                    }
                }
                for (const auto craft_id : object_value->fleet_members) {
                    const auto* craft = object(craft_id);
                    if (!craft) return false;
                    const auto* craft_type = type(craft->current_type.empty() ? craft->original_type : craft->current_type);
                    if (!craft_type) return false;
                    if (bomber_from(*craft_type)) return true;
                    if (error) return false;
                }
            }
        }
        return !error;
    }
    AutoResolveSides result;
    std::optional<core::Diagnostic> error;
private:
    const ConflictRecord& conflict_;
    const AutoResolveRoster& roster_;
    const units::GalacticTypeTable& types_;
    std::map<EntityId, const AutoResolveObject*> objects_;
    std::map<EntityId, const AutoResolveFleet*> fleets_;
    std::map<PlayerId, std::uint32_t> tech_;
    std::set<EntityId> added_, visited_fleets_, skipped_;
};

struct Contribution {
    std::vector<Fixed> force;
    std::array<Fixed, 32> bonuses{};
    std::optional<Fixed> weakest_power;
    bool superweapon{}, killer{};
};
core::Result<std::uint32_t> mask(const std::vector<std::string>& names, const std::span<const units::EnumValue> bits) {
    std::uint32_t result{};
    for (const auto& name : names) {
        const auto found = std::find_if(bits.begin(), bits.end(), [&](const auto& value) { return same(value.name, name); });
        if (found == bits.end()) return fail<std::uint32_t>("unknown strength category: " + name);
        result |= static_cast<std::uint32_t>(found->value);
    }
    return core::Result<std::uint32_t>::success(result);
}
core::Result<Contribution> contribution(const AutoResolveCombatant& combatant, const AutoResolveSides& gathered,
    const units::GalacticTypeTable& types, const std::span<const AutoResolveCategory> categories,
    const std::span<const units::EnumValue> bits) {
    Contribution result;
    if (combatant.tactical) return fail<Contribution>("mid-tactical team scaling is outside the galactic strength model");
    result.force.resize(categories.size() + 2);
    const auto* own = types.find(combatant.type);
    if (!own && !(combatant.planet && gathered.layer == BattleLayer::land && combatant.type.empty()))
        return fail<Contribution>("unknown gathered combatant type");
    const auto add_type = [&](const std::string_view name, const std::int32_t count, const std::size_t total) -> core::Result<void> {
        const auto* type = types.find(name);
        if (!type) return fail<void>("strength contribution needs resolved type");
        const auto type_mask = mask(type->category_mask, bits);
        if (!type_mask) return core::Result<void>::failure(type_mask.error());
        const auto multiplier = Fixed::from_integer(count);
        if (!multiplier) return core::Result<void>::failure(multiplier.error());
        const auto power = sim::math::multiply(type->ai_combat_power.value_or(Fixed{}), multiplier.value());
        if (!power) return core::Result<void>::failure(power.error());
        for (std::size_t i = 0; i < categories.size(); ++i) {
            const auto category = mask({categories[i].name}, bits);
            if (!category) return core::Result<void>::failure(category.error());
            if ((type_mask.value() & category.value()) == 0) continue;
            auto sum = sim::math::add(result.force[i + 2], power.value());
            if (!sum) return core::Result<void>::failure(sum.error());
            result.force[i + 2] = sum.value();
            sum = sim::math::add(result.force[total], power.value());
            if (!sum) return core::Result<void>::failure(sum.error());
            result.force[total] = sum.value();
            break; // WAR-21: first matching category; no match contributes nothing.
        }
        return core::Result<void>::success();
    };
    if (combatant.planet && gathered.layer == BattleLayer::land) {
        for (const auto& name : gathered.built_structures) {
            auto added = add_type(name, 1, 0);
            if (!added) return core::Result<Contribution>::failure(added.error());
        }
    } else {
        const auto total = gathered.layer == BattleLayer::space ? std::size_t{1} : std::size_t{0};
        if (!combatant.tactical && combatant.tech < own->starting_spawned_units.size()) {
            auto tech = combatant.tech;
            while (tech > 0 && own->starting_spawned_units[tech].empty()) --tech;
            for (const auto& spawn : own->starting_spawned_units[tech]) {
                const auto& row = own->starting_spawned_units[tech];
                const auto first = std::find_if(row.begin(), row.end(), [&](const auto& value) { return same(value.type, spawn.type); });
                auto added = add_type(spawn.type, first->count, total); // WAR-77: first resolved-type count
                if (!added) return core::Result<Contribution>::failure(added.error());
            }
        }
        if (!(combatant.transport && gathered.layer == BattleLayer::space && !combatant.named_hero)) {
            result.weakest_power = own->ai_combat_power.value_or(Fixed{});
            auto added = add_type(own->name, 1, total);
            if (!added) return core::Result<Contribution>::failure(added.error());
        }
    }
    const auto* bonus_type = types.find(combatant.bonus_type);
    if (combatant.has_special_abilities && !bonus_type) return fail<Contribution>("unknown hero bonus type");
    const std::vector<units::GalacticStrengthBonus> empty_bonuses;
    for (const auto& bonus : combatant.has_special_abilities ? bonus_type->strength_bonuses : empty_bonuses) {
        const auto bonus_mask = mask(bonus.categories, bits);
        if (!bonus_mask) return core::Result<Contribution>::failure(bonus_mask.error());
        if (bonus_mask.value() == 0) continue; // WAR-83: no category means no bonus
        Fixed value;
        if (bonus.defense) {
            value = bonus.enhancement.value_or(Fixed{});
        } else {
            const auto sum = sim::math::add(bonus.health.value_or(Fixed{}), bonus.damage.value_or(Fixed{}));
            if (!sum) return core::Result<Contribution>::failure(sum.error());
            value = sum.value();
        }
        for (std::size_t bit = 0; bit < result.bonuses.size(); ++bit)
            if ((bonus_mask.value() & (std::uint32_t{1} << bit)) != 0)
                result.bonuses[bit] = std::max(result.bonuses[bit], value);
    }
    if (!combatant.hero_type.empty()) {
        const auto* hero = types.find(combatant.hero_type);
        if (!hero) return fail<Contribution>("unknown superweapon hero type");
        const auto hero_mask = mask(hero->category_mask, bits);
        const auto super_mask = mask({"Super"}, bits);
        if (!hero_mask) return core::Result<Contribution>::failure(hero_mask.error());
        if (!super_mask) return core::Result<Contribution>::failure(super_mask.error());
        result.superweapon = (hero_mask.value() & super_mask.value()) != 0;
    }
    result.killer = combatant.contains_superweapon_killer;
    return core::Result<Contribution>::success(std::move(result));
}
} // namespace

core::Result<AutoResolveSides> gather_auto_resolve_sides(const ConflictRecord& conflict,
    const AutoResolveRoster& roster, const units::GalacticTypeTable& types) {
    if (!conflict.id || !conflict.invader || !conflict.defender || conflict.invader == conflict.defender
        || !conflict.planet || conflict.planet != roster.planet.id
        || (conflict.layer != BattleLayer::space && conflict.layer != BattleLayer::land))
        return fail<AutoResolveSides>("conflict needs a planet and distinct nonzero owners");
    EntityId previous{};
    std::vector<EntityId> force_entries;
    for (const auto& force : conflict.forces) {
        if (force.id <= previous || (force.owner != conflict.invader && force.owner != conflict.defender))
            return fail<AutoResolveSides>("conflict force IDs or owners are invalid");
        previous = force.id;
        const auto object = std::find_if(roster.objects.begin(), roster.objects.end(),
            [&](const auto& value) { return value.id == force.id; });
        if (object != roster.objects.end()) {
            if (object->owner != force.owner || (object->current_type_id ? object->current_type_id : assets::object_type_crc(object->original_type)) != force.type)
                return fail<AutoResolveSides>("conflict force differs from its roster object");
            force_entries.push_back(force.id);
            continue;
        }
        const auto fleet = std::find_if(roster.fleets.begin(), roster.fleets.end(),
            [&](const auto& value) { return value.fleet.id == force.id; });
        if (fleet != roster.fleets.end() && !fleet->squadron_type.empty()
            && fleet->fleet.owner == force.owner && assets::object_type_crc(fleet->squadron_type) == force.type) {
            force_entries.push_back(force.id);
            continue;
        }
        if (force.id == conflict.station && force.owner == roster.planet.owner
            && force.type == conflict.station_type && assets::object_type_crc(roster.planet.base_type) == force.type) {
            force_entries.push_back(roster.planet.id);
            continue;
        }
        return fail<AutoResolveSides>("conflict force has no corresponding roster object or base");
    }
    Gather gather(conflict, roster, types);
    if (conflict.layer == BattleLayer::space) {
        // WAR-13: fleet presence, not the first defender object, determines side A.
        const bool defender_fleet = std::any_of(roster.defender_entries.begin(), roster.defender_entries.end(),
            [&](const auto id) { return std::any_of(roster.fleets.begin(), roster.fleets.end(),
                [&](const auto& fleet) { return fleet.fleet.id == id && fleet.fleet.owner == conflict.defender; }); });
        gather.result.sides[0].owner = defender_fleet ? conflict.defender : conflict.invader;
        gather.result.sides[1].owner = defender_fleet ? conflict.invader : conflict.defender;
        for (const auto id : roster.defender_entries) if (!gather.error) static_cast<void>(gather.add_entry(id));
        for (const auto id : roster.invader_entries) if (!gather.error) static_cast<void>(gather.add_entry(id));
        if (!gather.error) static_cast<void>(gather.planet());
    } else {
        if (!gather.error) static_cast<void>(gather.planet());
        if (conflict.raid) {
            for (const auto id : roster.raid_entries) if (!gather.error) static_cast<void>(gather.add_object(id));
            // WAR-14: raids deliberately exclude the orbit fleets from combat.
            for (const auto id : roster.orbit_fleets) if (!gather.error) static_cast<void>(gather.skip_orbit_fleet(id));
        } else {
            for (const auto id : roster.orbit_fleets) {
                if (gather.error) break;
                const auto fleet = std::find_if(roster.fleets.begin(), roster.fleets.end(),
                    [id](const auto& value) { return value.fleet.id == id; });
                if (fleet == roster.fleets.end()) { gather.reject("unknown orbit fleet"); break; }
                if (fleet->fleet.owner == conflict.defender || fleet->fleet.owner == conflict.invader)
                    static_cast<void>(gather.add_entry(id));
            }
        }
    }
    if (gather.error) return core::Result<AutoResolveSides>::failure(*gather.error);
    for (const auto id : force_entries)
        if (!gather.gathered_force(id))
            return fail<AutoResolveSides>("conflict force was not gathered from the ordered roster");
    // WAR-13/14 specify ordered lists, not an exhaustive FT-08 forces manifest.
    // Reverse membership remains the conflict-to-roster adapter seam; do not infer it here.
    return core::Result<AutoResolveSides>::success(std::move(gather.result));
}

core::Result<AutoResolveStrength> auto_resolve_strength(const AutoResolveSides& gathered,
    const units::GalacticTypeTable& types, const std::span<const AutoResolveCategory> contrast,
    const std::span<const units::EnumValue> category_bits, const sim::PartitionExecutor& executor) {
    if (!gathered.sides[0].owner || !gathered.sides[1].owner
        || gathered.sides[0].owner == gathered.sides[1].owner
        || gathered.sides[0].combatants.empty() || gathered.sides[1].combatants.empty()
        || (gathered.aggressor != gathered.sides[0].owner && gathered.aggressor != gathered.sides[1].owner))
        return fail<AutoResolveStrength>("initiate needs two owned nonempty sides and a known aggressor (WAR-18)");
    AutoResolveStrength result;
    result.gathered = gathered;
    result.categories.assign(contrast.begin(), contrast.end());
    // Frozen copies, one disjoint output per combatant; no reduction inside a worker.
    std::vector<AutoResolveCombatant> inputs;
    for (const auto& side : gathered.sides) inputs.insert(inputs.end(), side.combatants.begin(), side.combatants.end());
    std::vector<core::Result<Contribution>> outputs;
    outputs.reserve(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) outputs.push_back(fail<Contribution>("unstaged strength input"));
    const auto staged = executor.execute_phase("auto-resolve-strength", sim::tick_partition_count, [&](const std::size_t partition) {
        const auto range = sim::partition_range(partition, inputs.size());
        for (auto i = range.begin; i < range.end; ++i) outputs[i] = contribution(inputs[i], gathered, types, contrast, category_bits);
    });
    if (!staged) return core::Result<AutoResolveStrength>::failure(staged.error());
    std::size_t index{};
    for (std::size_t side_index = 0; side_index < gathered.sides.size(); ++side_index) {
        auto& side = result.sides[side_index];
        side.force.resize(contrast.size() + 2);
        std::optional<Fixed> weakest;
        for (const auto& combatant : gathered.sides[side_index].combatants) {
            const auto& output = outputs[index++];
            if (!output) return core::Result<AutoResolveStrength>::failure(output.error());
            const auto& part = output.value();
            for (std::size_t i = 0; i < side.force.size(); ++i) {
                const auto sum = sim::math::add(side.force[i], part.force[i]);
                if (!sum) return core::Result<AutoResolveStrength>::failure(sum.error());
                side.force[i] = sum.value();
            }
            if (part.weakest_power && (!weakest || *part.weakest_power < *weakest)) {
                weakest = part.weakest_power; side.weakest = combatant.id; // WAR-24: first on ties
            }
            for (std::size_t bit = 0; bit < side.category_bonuses.size(); ++bit)
                side.category_bonuses[bit] = std::max(side.category_bonuses[bit], part.bonuses[bit]);
            if (part.superweapon) side.superweapon = combatant.id; // WAR-25: last on ties
            if (part.killer) side.superweapon_killer = combatant.id;
        }
    }
    return core::Result<AutoResolveStrength>::success(std::move(result));
}

core::Result<AutoResolvePreparation> prepare_auto_resolve(const AutoResolvePreparation& previous,
    const ConflictRecord& conflict, const std::uint64_t frame) {
    if (previous.conflict) return fail<AutoResolvePreparation>("prepare requires cleanup (WAR-11)");
    if (!conflict.id || previous.record_index >= previous.records.size()) return fail<AutoResolvePreparation>("invalid prepare input");
    AutoResolvePreparation result;
    result.records = previous.records;
    result.record_index = (previous.record_index + 1) % result.records.size();
    result.records[result.record_index] = {};
    result.conflict = conflict;
    result.start_frame = frame;
    return core::Result<AutoResolvePreparation>::success(std::move(result));
}
std::optional<std::size_t> auto_resolve_space_shortcut(const AutoResolveStrength& strength) noexcept {
    if (strength.gathered.layer != BattleLayer::space || strength.sides[0].superweapon || strength.sides[1].superweapon
        || strength.sides[0].force.size() < 2 || strength.sides[1].force.size() < 2
        || strength.sides[0].force[1] > Fixed{} || strength.sides[1].force[1] > Fixed{}
        || strength.gathered.sides[0].combatants.empty() || strength.gathered.sides[1].combatants.empty()) return std::nullopt;
    const auto a = strength.gathered.sides[0].combatants.size(), b = strength.gathered.sides[1].combatants.size();
    if (a != b) return a > b ? 0 : 1;
    return strength.gathered.aggressor == strength.gathered.sides[0].owner ? 1 : 0;
}
core::Result<AutoResolvePreparation> initiate_auto_resolve(const AutoResolvePreparation& prepared,
    const AutoResolveRoster& roster, const units::GalacticTypeTable& types,
    const std::span<const AutoResolveCategory> contrast, const std::span<const units::EnumValue> category_bits,
    const sim::PartitionExecutor& executor) {
    if (!prepared.conflict || prepared.strength || prepared.record_index >= prepared.records.size())
        return fail<AutoResolvePreparation>("initiate requires a prepared, not initiated conflict (WAR-18)");
    auto gathered = gather_auto_resolve_sides(*prepared.conflict, roster, types);
    if (!gathered) return core::Result<AutoResolvePreparation>::failure(gathered.error());
    auto strength = auto_resolve_strength(gathered.value(), types, contrast, category_bits, executor);
    if (!strength) return core::Result<AutoResolvePreparation>::failure(strength.error());
    auto result = prepared;
    result.strength = std::move(strength).value();
    result.records[result.record_index].planet = roster.planet.id;
    if (const auto win = auto_resolve_space_shortcut(*result.strength)) {
        result.winner = result.strength->gathered.sides[*win].owner;
        result.retreating_player = result.strength->gathered.sides[1 - *win].owner;
        result.fought = true; // WAR-27: visible before the first round-service call.
    }
    return core::Result<AutoResolvePreparation>::success(std::move(result));
}
AutoResolvePreparation cleanup_auto_resolve(const AutoResolvePreparation& previous) {
    AutoResolvePreparation result;
    result.records = previous.records;
    result.record_index = previous.record_index;
    return result;
}
} // namespace eawr::campaign
