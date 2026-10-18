#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/battle/start.hpp"

#include <algorithm>
#include <map>
#include <limits>

namespace eawr::campaign {
namespace tactical = sim::tactical;
namespace {
template <typename T> core::Result<T> fail(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(tactical::diagnostic_codes::invalid_setup);
    diagnostic.message = std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
}
core::Result<tactical::TacticalContent> conflict_entry_content(const tactical::TacticalSetup& setup,
    const skirmish::StartInputs& inputs) {
    // Generic selected-force content retains scene ships with no authored reserves.
    // Full space-entry binding supplies its owner-tech campaign policy explicitly.
    units::SpawnPolicy policy;
    if (inputs.tables && inputs.tables->campaign_scene) policy.mode = units::SpawnPolicy::Mode::campaign_scene;
    return conflict_entry_content(setup, inputs, policy);
}
core::Result<tactical::TacticalContent> conflict_entry_content(const tactical::TacticalSetup& setup,
    const skirmish::StartInputs& inputs, const units::SpawnPolicy& spawn_policy) {
    if (!inputs.tables || !setup.first_free_entity_id || !setup.skirmish
        || setup.skirmish->map != inputs.map || setup.skirmish->map_sha256 != inputs.map_sha256
        || setup.content_identity != units::content_identity(*inputs.tables))
        return fail<tactical::TacticalContent>("conflict replay map or content identity differs from its inputs");
    if (auto valid = tactical::validate_setup(setup); !valid)
        return core::Result<tactical::TacticalContent>::failure(valid.error());
    std::vector<tactical::PlayerId> humans;
    for (const auto& slot : setup.skirmish->slots) if (slot.human) humans.push_back(slot.player);
    auto bound = skirmish::session_content(*inputs.tables, humans, spawn_policy);
    if (!bound) return core::Result<tactical::TacticalContent>::failure(bound.error());
    auto fog = skirmish::fog_rules(inputs);
    if (!fog) return core::Result<tactical::TacticalContent>::failure(fog.error());
    tactical::TacticalContent content;
    content.sensors = std::move(bound.value().sensors);
    content.durability = std::move(bound.value().durability);
    content.motion = std::move(bound.value().motion);
    content.combat = std::move(bound.value().combat);
    content.abilities = std::move(bound.value().abilities);
    content.fog = std::move(fog).value();
    content.map_box = skirmish::map_box(inputs);
    // WGT-03, WBP-01..05: neutral pads need their capture service in the child too.
    auto pads = skirmish::capture_rules(setup, inputs, *inputs.tables);
    if (!pads) return core::Result<tactical::TacticalContent>::failure(pads.error());
    content.economy.pads = std::move(pads).value();
    content.victory = skirmish::victory_rules(setup, *inputs.tables, humans,
        static_cast<tactical::VictoryCondition>(setup.skirmish->victory_condition));
    return core::Result<tactical::TacticalContent>::success(std::move(content));
}

core::Result<ConflictEntry> build_conflict_entry(const ConflictRecord& record,
    const skirmish::Fixture& fixture, const skirmish::StartInputs& inputs,
    const std::span<const ForcePlacement> placements, const sim::EntityId first_free,
    const std::uint64_t session_seed) {
    if (!inputs.tables || record.layer != BattleLayer::space || record.id == 0
        || first_free <= record.id || first_free == sim::invalid_entity_id
        || record.invader == 0 || record.defender == 0 || record.invader == record.defender
        || record.map_identity != fixture.map || record.forces.size() > sim::galactic::galactic_max_objects)
        return fail<ConflictEntry>("conflict needs space forces, distinct owners, a matching map and a free allocator boundary");
    for (const auto owner : {record.invader, record.defender})
        if (std::none_of(fixture.slots.begin(), fixture.slots.end(), [owner](const auto& slot) { return slot.slot == owner; }))
            return fail<ConflictEntry>("conflict owners need explicit faction and team bindings");
    std::map<tactical::TypeId, std::string> names;
    for (const auto& type : inputs.tables->units) {
        const auto [found, inserted] = names.emplace(battle::type_id(type.id), type.id);
        if (!inserted && found->second != type.id) return fail<ConflictEntry>("ambiguous tactical type ID");
    }
    std::map<sim::EntityId, ForcePlacement> placement_by_id;
    for (const auto& placement : placements)
        if (!placement_by_id.emplace(placement.force, placement).second)
            return fail<ConflictEntry>("duplicate force placement");
    std::vector<skirmish::SelectedStartUnit> selected;
    std::vector<sim::EntityId> persistent;
    sim::EntityId previous{};
    for (const auto& force : record.forces) {
        if (force.id <= previous || force.id >= first_free || force.id == record.id
            || (force.owner != record.invader && force.owner != record.defender && (force.role != ForceRole::orbital
                || std::none_of(fixture.slots.begin(), fixture.slots.end(), [&](const auto& slot) { return slot.slot == force.owner; })))
            // WGT-122: carrier-bound ground identities need no spatial profile.
            // Standalone deployed and reserve objects still require one.
            || (force.role != ForceRole::retained
                && !((force.role == ForceRole::rider || force.role == ForceRole::reinforcement) && force.carrier)
                && !names.contains(force.tactical_type ? force.tactical_type : force.type))
            || (force.role != ForceRole::deployed && force.role != ForceRole::escort
                && force.role != ForceRole::reinforcement && force.role != ForceRole::rider && force.role != ForceRole::retained
                && force.role != ForceRole::orbital))
            return fail<ConflictEntry>("conflict force IDs, types, roles or owners are invalid");
        previous = force.id;
        const auto placement = placement_by_id.find(force.id);
        if (force.role == ForceRole::reinforcement || force.role == ForceRole::rider || force.role == ForceRole::retained) {
            if (placement != placement_by_id.end()) return fail<ConflictEntry>("reinforcement pool force cannot have an initial placement");
            continue;
        }
        if (placement == placement_by_id.end()) return fail<ConflictEntry>("deployed force is missing its marker selection");
        const auto role = force.id == record.station ? battle::UnitRole::station : battle::UnitRole::fleet;
        const auto type = force.tactical_type ? force.tactical_type : force.type;
        if (role == battle::UnitRole::station && (force.type != record.station_type || force.owner != record.defender
            || inputs.tables->find(names.at(type))->kind != units::UnitKind::station))
            return fail<ConflictEntry>("station must retain its selected type and defender owner");
        selected.push_back({force.owner, names.at(type), role, placement->second.marker, placement->second.garrison_enabled});
        persistent.push_back(force.id);
        placement_by_id.erase(placement);
    }
    if (!placement_by_id.empty()) return fail<ConflictEntry>("placement names an unknown force");
    // WGT-05/07/19: preserve the prepared defender/invader and bucket take order.
    // Fixture callers leave creation_order zero, retaining their persistent-ID order.
    std::vector<std::size_t> creation(selected.size());
    for (std::size_t i = 0; i < creation.size(); ++i) creation[i] = i;
    const auto order_of = [&](sim::EntityId id) {
        const auto at = std::find_if(placements.begin(), placements.end(), [id](const auto& p) { return p.force == id; });
        return at->creation_order;
    };
    std::stable_sort(creation.begin(), creation.end(), [&](auto a, auto b) { return order_of(persistent[a]) < order_of(persistent[b]); });
    auto unordered_selected = selected;
    auto unordered_persistent = persistent;
    for (std::size_t i = 0; i < creation.size(); ++i) {
        selected[i] = unordered_selected[creation[i]]; persistent[i] = unordered_persistent[creation[i]];
    }
    for (const auto id : {record.station, record.superweapon})
        if (id != 0 && std::find(persistent.begin(), persistent.end(), id) == persistent.end())
            return fail<ConflictEntry>("station and superweapon must be explicitly deployed conflict forces");
    auto selected_fixture = fixture;
    selected_fixture.seed = child_seed(session_seed, record.id);
    // WGT-121: owner-qualified map admission is a GC rule; skirmish supplies no filter.
    const skirmish::MapObjectFilter map_filter = [&](const auto& placement, const auto& faction, const auto owner) {
        if (static_cast<std::int64_t>(placement.enable_at_level) > static_cast<std::int64_t>(record.station_level)) return false;
        return !placement.branched_discardable || !faction.can_control_planets || placement.marker || placement.marker_behavior
            || owner == record.defender
            || (owner == record.invader && (placement.loads_when_faction_present || record.corruption_mission));
    };
    auto built = skirmish::build_selected_start(selected_fixture, inputs, selected, true, map_filter);
    if (!built) return core::Result<ConflictEntry>::failure(built.error());
    auto start = std::move(built).value();
    // Background GC players can own map/orbital/upgrade objects without becoming contenders.
    for (auto& player : start.players) if (player.player.player_id != record.invader && player.player.player_id != record.defender) {
        player.player.flags = 0; player.lobby = false; player.human = false;
    }
    for (auto& player : start.setup.players) if (player.player_id != record.invader && player.player_id != record.defender) player.flags = 0;
    // The builder derived beacon rules while every lobby seat was commandable; only contenders keep one.
    std::erase_if(start.setup.beacon_rules, [&](const auto& rule) {
        return rule.player != record.invader && rule.player != record.defender;
    });
    // Placement runs with dense scratch IDs. Only after every shared placement pass finishes
    // do we translate to persistent IDs and allocate battle-only objects in creation order.
    std::map<sim::EntityId, sim::EntityId> remap;
    auto next = first_free;
    std::size_t selected_index{};
    for (const auto& unit : start.units) {
        const bool chosen = unit.role == battle::UnitRole::station || unit.role == battle::UnitRole::fleet;
        if (!chosen && next == std::numeric_limits<sim::EntityId>::max())
            return fail<ConflictEntry>("battle-only entity allocator exhausted");
        remap.emplace(unit.state.entity_id, chosen ? persistent.at(selected_index++) : next++);
    }
    for (auto& unit : start.units) {
        unit.state.entity_id = remap.at(unit.state.entity_id);
        const auto force = std::find_if(record.forces.begin(), record.forces.end(), [&](const auto& f) { return f.id == unit.state.entity_id; });
        if (force != record.forces.end()) unit.state.purchase_type = force->purchase_type;
    }
    for (auto& squadron : start.setup.squadrons) {
        squadron.container = remap.at(squadron.container);
        for (auto& member : squadron.members) member = remap.at(member);
    }
    for (auto& launch : start.launches) launch.spawner = remap.at(launch.spawner);
    std::sort(start.units.begin(), start.units.end(), [](const auto& a, const auto& b) { return a.state.entity_id < b.state.entity_id; });
    std::sort(start.setup.squadrons.begin(), start.setup.squadrons.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    start.setup.units.clear();
    for (const auto& unit : start.units) start.setup.units.push_back(unit.state);
    start.setup.first_free_entity_id = next;
    start.setup.skirmish = skirmish::recording_setup(selected_fixture, start).skirmish;
    // Background owner bindings are not commandable lobby seats (SKSU contract).
    std::erase_if(start.setup.skirmish->slots, [&](const auto& slot) {
        return slot.player != record.invader && slot.player != record.defender;
    });
    // Replay closure includes reserve types too: a later arrival needs the same content identity.
    for (auto& slot : start.setup.skirmish->slots) {
        slot.fleet.clear();
        for (const auto& force : record.forces) if (force.owner == slot.player) {
            const auto name = names.find(force.tactical_type ? force.tactical_type : force.type);
            if (name != names.end()) slot.fleet.push_back(name->second);
        }
    }
    if (auto valid = tactical::validate_setup(start.setup); !valid) return core::Result<ConflictEntry>::failure(valid.error());
    auto content = conflict_entry_content(start.setup, inputs);
    if (!content) return core::Result<ConflictEntry>::failure(content.error());
    ConflictEntry entry;
    entry.conflict = record;
    entry.conflict.tactical = start.setup;
    entry.content = std::move(content).value();
    entry.start = std::move(start);
    return core::Result<ConflictEntry>::success(std::move(entry));
}
} // namespace eawr::campaign
