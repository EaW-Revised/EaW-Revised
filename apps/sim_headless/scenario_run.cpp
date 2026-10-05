#include "scenario_internal.hpp"
#include <cstdlib>

namespace sim_headless::scenario_detail {

bool projectile_contact_diagnostics() {
    static const bool enabled = [] {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t size = 0;
        const bool present = _dupenv_s(&value, &size, "EAWR_PROJECTILE_CONTACT_DIAGNOSTICS") == 0 && value != nullptr;
        std::free(value);
        return present;
#else
        return std::getenv("EAWR_PROJECTILE_CONTACT_DIAGNOSTICS") != nullptr;
#endif
    }();
    return enabled;
}

core::Result<ScenarioRun> execute_scenario(const Scenario& scenario, const std::string& path, const std::string& bytes,
    core::Result<eawr::units::UnitTables>& tables, const std::vector<tactical::SensorProfile>& sensors,
    core::Result<tactical::DurabilityTable>& durability, core::Result<tactical::MotionTable>& motion,
    core::Result<tactical::CombatTable>& combat, std::size_t workers, const std::string& build_sha256) {
    tactical::TacticalSetup setup;
    setup.content_identity = eawr::units::content_identity(tables.value());
    std::map<std::string, tactical::PlayerId> player_ids;
    for (const auto& [label, faction] : scenario.players) {
        const auto id = static_cast<tactical::PlayerId>(setup.players.size() + 1);
        player_ids.emplace(label, id);
        setup.players.push_back({id, id, eawr::skirmish::faction_id(faction), tactical::player_flag_commandable});
    }
    std::vector<Traced> traced;
    std::map<std::string, std::pair<eawr::sim::EntityId, tactical::PlayerId>> unit_ids;
    std::map<std::string, tactical::UnitState> pending_spawns;
    std::map<eawr::sim::EntityId, std::string> labels;
    std::vector<Observed> observed;
    for (const auto& unit : scenario.units) {
        const auto owner = player_ids.find(unit.owner);
        const auto* type = tables.value().find(unit.type);
        if (owner == player_ids.end() || type == nullptr) {
            return fail<ScenarioRun>("unit '" + unit.label + "' names an unknown owner or unit type", path);
        }
        tactical::UnitState state;
        state.type_id = eawr::skirmish::type_id(unit.type);
        state.owner = owner->second;
        state.position = unit.position;
        // The unit's Space_Layer puts it Layer_Z_Adjust above the requested height.
        const auto adjust = type->movement.layer_z_adjust.value_or(Fixed{});
        auto staged_z = math::add(unit.position.z, adjust);
        if (!staged_z) return core::Result<ScenarioRun>::failure(staged_z.error());
        state.position.z = staged_z.value();
        auto rotation = staged_rotation(unit.facing_degrees, Fixed::from_raw(-adjust.raw()));
        if (!rotation) return core::Result<ScenarioRun>::failure(rotation.error());
        state.rotation = rotation.value();
        eawr::sim::EntityId id{};
        if (type->kind == eawr::units::UnitKind::squadron && !unit.at_start && !unit.observed) {
            return fail<ScenarioRun>("squadron '" + unit.label + "' must spawn at the start", path);
        }
        if (unit.observed) {
            observed.push_back({unit.label, state.type_id, state.owner, false}); // bound once created
        } else if (unit.at_start) {
            id = static_cast<eawr::sim::EntityId>(setup.units.size() + 1);
            state.entity_id = id;
            setup.units.push_back(state);
            labels.emplace(id, unit.label);
            if (type->kind == eawr::units::UnitKind::squadron) {
                // A squadron is its team container and its craft, each on its Squadron_Offsets slot
                // turned by the company's yaw, as the skirmish start places them (#75, #536).
                auto squadron = squadron_craft(*type, tables.value(), state, unit.facing_degrees, setup, unit.label, labels);
                if (!squadron) return core::Result<ScenarioRun>::failure(squadron.error());
                setup.squadrons.push_back(std::move(squadron).value());
            }
        } else {
            pending_spawns.emplace(unit.label, state); // staged at its spawn event
        }
        unit_ids.emplace(unit.label, std::pair{id, owner->second});
        const bool container = type->kind == eawr::units::UnitKind::squadron;
        std::optional<Fixed> container_hull;
        if (container && type->team_hull) {
            const auto& scalars = tables.value().constants.scalars;
            const auto multiplier = std::find_if(scalars.begin(), scalars.end(), [](const auto& value) {
                return value.tag == "Object_Max_Health_Multiplier_Space";
            });
            if (multiplier == scalars.end() || !multiplier->value) {
                return fail<ScenarioRun>("container hull needs Object_Max_Health_Multiplier_Space", path);
            }
            auto health = math::multiply(*type->team_hull, *multiplier->value);
            if (!health) return core::Result<ScenarioRun>::failure(health.error());
            container_hull = health.value();
        }
        traced.push_back({unit.label, unit.label, id,
            container ? std::optional{type->team_shield_points.value_or(Fixed{})} : type->shield_points,
            std::nullopt, 0, container_hull});
    }
    // A recorded craft pose replaces generated start placement, before session creation and
    // replay setup encoding. It must name an existing staged member, never a later launch.
    for (const auto& unit : scenario.units) {
        if (!unit.apply_initial_pose) continue;
        const auto named = std::find_if(labels.begin(), labels.end(),
            [&](const auto& entry) { return entry.second == unit.label; });
        const auto staged = std::find_if(setup.units.begin(), setup.units.end(),
            [&](const auto& entry) { return named != labels.end() && entry.entity_id == named->first; });
        if (staged == setup.units.end() || staged->type_id != eawr::skirmish::type_id(unit.type)
            || staged->owner != player_ids.at(unit.owner)) {
            return fail<ScenarioRun>("initial pose for '" + unit.label + "' needs a matching tick-zero member", path);
        }
        const auto rotation = staged_rotation(unit.facing_degrees, Fixed{});
        if (!rotation) return core::Result<ScenarioRun>::failure(rotation.error());
        staged->position = unit.position; // recorded world position already includes the layer
        staged->rotation = rotation.value();
    }
    // A hardpoint under test is a weapon slot of its unit's combat profile.
    for (const auto& hardpoint : scenario.hardpoints) {
        const auto unit = std::find_if(scenario.units.begin(), scenario.units.end(),
            [&](const ScenarioUnit& entry) { return entry.label == hardpoint.unit; });
        const auto* type = unit != scenario.units.end() ? tables.value().find(unit->type) : nullptr;
        if (type == nullptr) return fail<ScenarioRun>("hardpoint '" + hardpoint.label + "' names an unknown unit", path);
        const auto named = std::find_if(type->hardpoints.begin(), type->hardpoints.end(),
            [&](const eawr::units::Hardpoint& entry) { return lower(entry.id) == lower(hardpoint.hardpoint); });
        const auto* profile = combat.value().find(eawr::skirmish::type_id(unit->type));
        if (named == type->hardpoints.end() || profile == nullptr) {
            return fail<ScenarioRun>("hardpoint '" + hardpoint.label + "' is not a hardpoint of " + unit->type, path);
        }
        const auto index = static_cast<std::uint32_t>(named - type->hardpoints.begin());
        const auto slot = std::find_if(profile->weapons.begin(), profile->weapons.end(),
            [&](const tactical::WeaponProfile& weapon) { return weapon.hardpoint == index; });
        if (slot == profile->weapons.end()) {
            return fail<ScenarioRun>("hardpoint '" + hardpoint.label + "' is not a weapon", path);
        }
        traced.push_back({hardpoint.label, hardpoint.unit, unit_ids.at(hardpoint.unit).first, std::nullopt, index,
            static_cast<std::size_t>(slot - profile->weapons.begin()), std::nullopt});
    }
    std::sort(traced.begin(), traced.end(), [](const Traced& a, const Traced& b) { return a.label < b.label; });

    // #76: the recorder quick-loads the map, so no player is human (AB-41).
    auto abilities = eawr::units::ability_table(tables.value());
    if (!abilities) return core::Result<ScenarioRun>::failure(abilities.error());
    auto session_result = tactical::TacticalSession::create(setup, sensors, durability.value(), motion.value(),
        std::nullopt, combat.value(), tactical::VictoryRules{}, abilities.value());
    if (!session_result) return core::Result<ScenarioRun>::failure(session_result.error());
    auto session = std::move(session_result).value();
    ScenarioRun run;
    std::map<tactical::PlayerId, std::uint64_t> sequence;
    // Spawns and removals are staged between ticks (TacticalSession::stage_spawn), so the unit is
    // alive, or gone, from the event's tick on, as in the recordings.
    std::multimap<std::uint64_t, const ScenarioEvent*> staged;
    // Orders are submitted once their tick's spawns are staged, so an order can name a unit, or
    // an attack target, spawned at or before its tick. A tick-T command runs in the step after
    // tick T either way, so an order between start units runs as if it were submitted up front.
    std::vector<const ScenarioEvent*> orders;
    for (const auto& event : scenario.events) {
        if (!unit_ids.contains(event.unit)) return fail<ScenarioRun>("event names an unknown unit '" + event.unit + "'", path);
        if (event.action == "spawn" || event.action == "remove") {
            if ((event.action == "spawn") != pending_spawns.contains(event.unit)) {
                return fail<ScenarioRun>("spawn events are for spawn 'event' units only ('" + event.unit + "')", path);
            }
            staged.emplace(event.tick, &event);
            continue;
        }
        if ((event.action == "attack" || !event.target.empty()) && !unit_ids.contains(event.target)) {
            return fail<ScenarioRun>(event.action + " names an unknown target '" + event.target + "'", path);
        }
        for (const auto& label : event.with) {
            if (!unit_ids.contains(label)) return fail<ScenarioRun>("move names an unknown unit '" + label + "'", path);
        }
        orders.push_back(&event);
    }
    std::size_t next_order = 0;
    const auto submit_orders = [&](const std::uint64_t completed) -> core::Result<void> {
        for (; next_order < orders.size() && orders[next_order]->tick <= completed; ++next_order) {
            const auto& event = *orders[next_order];
            const auto& unit = unit_ids.at(event.unit);
            if (unit.first == 0) {
                return fail<void>("an order names '" + event.unit + "' before its spawn event", path);
            }
            tactical::PlayerCommand command;
            command.key = {event.tick, unit.second, sequence[unit.second]++};
            command.units = {unit.first};
            for (const auto& label : event.with) {
                const auto other = unit_ids.at(label).first;
                if (other == 0) return fail<void>("a move names '" + label + "' before its spawn event", path);
                command.units.push_back(other);
            }
            if (event.action == "move") {
                command.payload = tactical::MovePayload{event.position};
            } else if (event.action == "face") {
                command.payload = tactical::FacePayload{event.position};
            } else if (event.action == "stop") {
                command.payload = tactical::StopPayload{};
            } else if (event.action == "attack") {
                const auto target = unit_ids.at(event.target).first;
                if (target == 0) {
                    return fail<void>("attack names target '" + event.target + "' before its spawn event", path);
                }
                command.payload = tactical::AttackPayload{target};
            } else if (event.action == "attack_move" || event.action == "guard") {
                eawr::sim::EntityId target = eawr::sim::invalid_entity_id;
                if (!event.target.empty()) {
                    target = unit_ids.at(event.target).first;
                    if (target == 0) {
                        return fail<void>(event.action + " names target '" + event.target + "' before its spawn event", path);
                    }
                }
                if (event.action == "guard") {
                    command.payload = tactical::GuardPayload{event.position, target};
                } else {
                    command.payload = tactical::AttackMovePayload{event.position, target};
                }
            } else if (event.action == "damage") {
                // The retail Lua Take_Damage(amount[, hardpoint]) as the scripted-damage command (HD-30).
                const auto staged_unit = std::find_if(scenario.units.begin(), scenario.units.end(),
                    [&](const ScenarioUnit& entry) { return entry.label == event.unit; });
                const auto* type = tables.value().find(staged_unit->type);
                auto hardpoint = tactical::hull_target;
                if (!event.hardpoint.empty()) {
                    const auto named = std::find_if(type->hardpoints.begin(), type->hardpoints.end(),
                        [&](const eawr::units::Hardpoint& entry) { return lower(entry.id) == lower(event.hardpoint); });
                    if (named == type->hardpoints.end()) {
                        return fail<void>("damage names hardpoint '" + event.hardpoint + "', not one of " + staged_unit->type, path);
                    }
                    hardpoint = static_cast<std::uint32_t>(named - type->hardpoints.begin());
                }
                command.payload = tactical::DamagePayload{event.amount, hardpoint};
            } else if (event.action == "ability") {
                // The recorder's Lua Activate_Ability(name, true) as an ability command (#76, AB-10).
                const auto kind = tactical::ability_kind(event.ability);
                if (kind == tactical::AbilityKind::none) {
                    run.warnings.push_back("tick " + std::to_string(event.tick) + ": ability " + event.ability + " on "
                        + event.unit + " is not modelled (space-abilities.md AB-03) and was skipped");
                    continue;
                }
                tactical::AbilityPayload ability{kind, tactical::AbilityAction::activate};
                // #561: a targeted ability (ION_CANNON_SHOT, space-abilities AB-61) at a scenario unit.
                if (!event.target.empty()) {
                    ability.target = unit_ids.at(event.target).first;
                    if (ability.target == 0) {
                        return fail<void>("ability names target '" + event.target + "' before its spawn event", path);
                    }
                }
                command.payload = ability;
            } else if (event.action == "ability_probe") {
                continue; // a read-only probe of the recorder
            } else {
                return fail<void>("event action '" + event.action + "' is not staged by sim_headless yet", path);
            }
            auto submitted = session.submit(command);
            if (!submitted) return submitted;
        }
        return core::Result<void>::success();
    };

    std::ostringstream trace;
    std::ostringstream hashes;
    trace << "tick,object,field,value\n";
    hashes << "tick,sha256\n";
    const auto rows = [&](const std::uint64_t tick) { return write_rows(session, labels, unit_ids, traced, trace, tick); };
    // The combat log (#536): names by label, a craft `<squadron>.<n>`, else `entity.<id>`;
    // hardpoints by their XML name. Health rows are written when the value changes.
    std::ostringstream combat_log;
    combat_log << "tick,kind,object,part,other,value";
    if (projectile_contact_diagnostics()) combat_log << ",selected_target";
    combat_log << '\n';
    std::map<tactical::TypeId, const eawr::units::UnitType*> types_by_id;
    for (const auto& type : tables.value().units) types_by_id.emplace(eawr::skirmish::type_id(type.id), &type);
    std::map<std::pair<eawr::sim::EntityId, std::string>, std::int64_t> last_health;
    std::map<eawr::sim::EntityId, tactical::TypeId> live_before;
    const auto combat_rows = [&](const std::uint64_t tick) { write_combat_rows(session, labels, types_by_id, last_health, live_before, combat_log, tick); };
    const auto bind_observed = [&]() {
        if (observed.empty()) return;
        for (const auto& unit : session.units()) {
            // A staged squadron's craft carry `<squadron>.<n>`; a slot of that name traces one (#457).
            if (const auto named = labels.find(unit.entity_id); named != labels.end()) {
                for (auto& slot : observed) {
                    if (slot.bound || slot.label != named->second || slot.type_id != unit.type_id
                        || slot.owner != unit.owner) continue;
                    slot.bound = true;
                    unit_ids.at(slot.label).first = unit.entity_id;
                    break;
                }
                continue;
            }
            for (auto& slot : observed) {
                if (slot.bound || slot.type_id != unit.type_id || slot.owner != unit.owner) continue;
                slot.bound = true;
                unit_ids.at(slot.label).first = unit.entity_id;
                labels.emplace(unit.entity_id, slot.label);
                break;
            }
        }
    };
    bind_observed();
    auto first = rows(0);
    combat_rows(0);
    if (!first) return core::Result<ScenarioRun>::failure(first.error());
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    while (session.completed_tick() + 1 < scenario.duration) {
        auto tick = session.step(executor);
        if (!tick) return core::Result<ScenarioRun>::failure(tick.error());
        const auto completed = tick.value().completed_tick;
        for (auto [event, end] = staged.equal_range(completed); event != end; ++event) {
            const auto& action = *event->second;
            auto& entry = unit_ids.at(action.unit);
            if (action.action == "spawn") {
                auto spawned = session.stage_spawn(pending_spawns.at(action.unit));
                if (!spawned) return core::Result<ScenarioRun>::failure(spawned.error());
                entry.first = spawned.value();
                labels.emplace(spawned.value(), action.unit);
            } else {
                auto removed = session.stage_remove(entry.first);
                if (!removed) return core::Result<ScenarioRun>::failure(removed.error());
            }
            run.staged = true;
        }
        bind_observed();
        auto submitted = submit_orders(completed);
        if (!submitted) return core::Result<ScenarioRun>::failure(submitted.error());
        // The hash row is the stepped state; a staged change shows in the next row.
        hashes << completed << ',' << tick.value().state_sha256 << '\n';
        auto written = rows(completed);
        combat_rows(completed);
        if (!written) return core::Result<ScenarioRun>::failure(written.error());
    }
    run.trace_csv = trace.str();
    run.combat_csv = combat_log.str();
    run.hashes_csv = hashes.str();
    trace_header(run, scenario, bytes, build_sha256);
    auto replay = session.record();
    auto encoded = tactical::write_replay(replay);
    if (!encoded) return core::Result<ScenarioRun>::failure(encoded.error());
    run.replay = std::move(encoded).value();
    return core::Result<ScenarioRun>::success(std::move(run));
}

} // namespace sim_headless::scenario_detail
