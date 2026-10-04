#include "scenario_internal.hpp"

namespace sim_headless::scenario_detail {

core::Result<void> write_rows(const tactical::TacticalSession& session, const Labels& labels, const UnitIds& unit_ids,
    const std::vector<Traced>& traced, std::ostringstream& trace, const std::uint64_t tick) {
        const auto units = session.units();
        const auto snapshot = session.snapshot();
        for (const auto& object : traced) {
            const auto id = unit_ids.at(object.unit).first;
            const auto found = std::find_if(units.begin(), units.end(),
                [&](const tactical::UnitState& unit) { return id != 0 && unit.entity_id == id; });
            const bool alive = found != units.end();
            if (object.hardpoint) {
                // A hardpoint row on every tick its unit is alive: the shots the tick fired and its
                // target (an attack order's, else its opportunity target).
                if (!alive) continue;
                std::uint64_t shots = 0;
                for (const auto& event : snapshot->combat_events()) {
                    shots += event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == id
                        && event.weapon == *object.hardpoint;
                }
                std::string target;
                if (const auto state = session.combat_state(id)) {
                    auto target_id = state->direct ? state->attack_target : eawr::sim::EntityId{};
                    if (target_id == 0) target_id = state->weapons[object.weapon].opportunity.target;
                    if (target_id != 0) {
                        const auto label = labels.find(target_id);
                        target = label != labels.end() ? label->second : "entity." + std::to_string(target_id);
                    }
                }
                trace << tick << ',' << object.label << ",shots," << shots << '\n';
                trace << tick << ',' << object.label << ",target," << target << '\n';
                continue;
            }
            trace << tick << ',' << object.label << ",alive," << (alive ? 1 : 0) << '\n';
            if (!alive) continue;
            const auto matrix = math::to_matrix(found->rotation, found->position);
            if (!matrix) return core::Result<void>::failure(matrix.error());
            const auto& m = matrix.value().rows;
            trace << tick << ',' << object.label << ",fwd.x," << m[0][0].raw() << '\n'
                  << tick << ',' << object.label << ",fwd.y," << m[1][0].raw() << '\n'
                  << tick << ',' << object.label << ",fwd.z," << m[2][0].raw() << '\n';
            // WSQ-60: container health is object-local; its craft retain separate durability.
            if (const auto health = session.durability_state(id)) {
                trace << tick << ',' << object.label << ",hull," << health->hull.raw() << '\n';
            } else if (object.authored_hull) {
                trace << tick << ',' << object.label << ",hull," << object.authored_hull->raw() << '\n';
            }
            trace << tick << ',' << object.label << ",pos.x," << found->position.x.raw() << '\n'
                  << tick << ',' << object.label << ",pos.y," << found->position.y.raw() << '\n'
                  << tick << ',' << object.label << ",pos.z," << found->position.z.raw() << '\n';
            // A shielded unit writes the session's shield (#74); a type the session gives no shield
            // writes its authored Shield_Points.
            if (object.shield) {
                const auto health = session.durability_state(id);
                const auto* profile = session.durability().find(found->type_id);
                const bool modelled = session.durability().damage && health && profile != nullptr
                    && profile->max_shields.raw() > 0;
                trace << tick << ',' << object.label << ",shield,"
                      << (modelled ? health->shields.raw() : object.shield->raw()) << '\n';
            }
        }
        return core::Result<void>::success();
}

void write_combat_rows(const tactical::TacticalSession& session, const Labels& labels, const Types& types_by_id,
    Health& last_health, Live& live_before, std::ostringstream& combat_log, const std::uint64_t tick) {
    const auto name_of = [&](const eawr::sim::EntityId id) {
        const auto label = labels.find(id);
        return label != labels.end() ? label->second : "entity." + std::to_string(id);
    };
    // A weapon's index names its hardpoint or the object weapon; a hit's, its hardpoint or the hull.
    const auto part_of = [&](const tactical::TypeId type, const std::uint32_t index, const char* none) -> std::string {
        if (index == tactical::no_hardpoint) return none;
        const auto found = types_by_id.find(type);
        if (found == types_by_id.end() || index >= found->second->hardpoints.size()) return "hp." + std::to_string(index);
        return found->second->hardpoints[index].id;
    };
    const auto decimal_text = [](const Fixed value) {
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(4);
        text << static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale);
        return text.str();
    };
        std::map<eawr::sim::EntityId, tactical::TypeId> live;
        for (const auto& unit : session.units()) live.emplace(unit.entity_id, unit.type_id);
        for (const auto& [id, type] : live_before) {
            if (!live.contains(id)) combat_log << tick << ",destroyed," << name_of(id) << ",,,\n";
        }
        const auto type_of = [&](const eawr::sim::EntityId id) {
            const auto found = live.find(id);
            if (found != live.end()) return found->second;
            const auto before = live_before.find(id);
            return before != live_before.end() ? before->second : tactical::TypeId{};
        };
        for (const auto& event : session.snapshot()->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired) {
                combat_log << tick << ",fired," << name_of(event.shooter) << ','
                           << part_of(type_of(event.shooter), event.weapon, "object") << ',' << name_of(event.target) << ",1\n";
            } else if (event.kind == tactical::CombatEventKind::projectile_hit) {
                combat_log << tick << ",hit," << name_of(event.target) << ','
                           << part_of(type_of(event.target), event.target_hardpoint, "hull") << ',' << name_of(event.shooter)
                           << '/' << part_of(type_of(event.shooter), event.weapon, "object") << ',' << event.outcome;
                if (projectile_contact_diagnostics()) {
                    combat_log << ',' << name_of(event.selected_target);
                }
                combat_log << '\n';
            }
        }
        for (const auto& [id, type] : live) {
            const auto health = session.durability_state(id);
            if (!health) continue;
            const auto note = [&](const std::string& part, const Fixed value) {
                auto [slot, inserted] = last_health.try_emplace({id, part}, value.raw());
                if (!inserted && slot->second == value.raw()) return;
                slot->second = value.raw();
                combat_log << tick << ",health," << name_of(id) << ',' << part << ",," << decimal_text(value) << '\n';
            };
            note("hull", health->hull);
            note("shield", health->shields);
            for (std::size_t index = 0; index < health->hardpoints.size(); ++index) {
                note(part_of(type, static_cast<std::uint32_t>(index), "hull"), health->hardpoints[index]);
            }
        }
        live_before = std::move(live);
}

void trace_header(ScenarioRun& run, const Scenario& scenario, const std::string& bytes, const std::string& build_sha256) {
    std::ostringstream header;
    header << "{\n"
           << "  \"format\": \"eawr-trace\",\n"
           << "  \"format_version\": 1,\n"
           << "  \"source\": \"remake\",\n"
           << "  \"content_identity\": \"" << pins_identity(scenario.content) << "\",\n"
           << "  \"tick_seconds\": {\"numerator\": " << tactical::tick_numerator
           << ", \"denominator\": " << tactical::tick_denominator << "},\n"
           << "  \"build_identity\": {\"kind\": \"executable-sha256\", \"value\": \"" << build_sha256 << "\"},\n"
           << "  \"scenario_sha256\": \""
           << eawr::sim::sha256_hex(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()))
           << "\"\n"
           << "}\n";
    run.trace_header = header.str();
}

} // namespace sim_headless::scenario_detail
