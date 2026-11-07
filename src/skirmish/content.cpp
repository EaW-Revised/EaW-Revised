#include "eawr/units/content_selection.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"

#include "skirmish_internal.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace eawr::skirmish {

namespace {
void drop_skipped_launch_content(SessionContent& content, const units::UnitTables& tables,
    std::vector<core::Diagnostic>& skipped, core::LoadNoteBuffer& notes);
}

core::Result<SessionContent> session_content(
    const units::UnitTables& tables, const std::span<const sim::tactical::PlayerId> humans,
    const units::SpawnPolicy& spawn_policy, const units::ContentErrors errors,
    std::vector<core::Diagnostic>* const skipped) {
    using ContentResult = core::Result<SessionContent>;
    SessionContent content;
    std::vector<core::Diagnostic> ignored_diagnostics;
    auto* diagnostics = skipped ? skipped : &ignored_diagnostics;
    core::LoadNoteBuffer load_notes;
    content.sensors = sensor_table(tables, &load_notes);
    auto durability = units::durability_table(tables, skipped, &load_notes);
    if (!durability) return ContentResult::failure(durability.error());
    content.durability = std::move(durability).value();
    auto motion = units::motion_table(tables, spawn_policy, skipped, &load_notes);
    if (!motion) return ContentResult::failure(motion.error());
    content.motion = std::move(motion).value();
    auto combat = units::combat_table(tables, errors, diagnostics, &load_notes);
    if (!combat) return ContentResult::failure(combat.error());
    content.combat = std::move(combat).value();
    auto abilities = units::ability_table(tables, humans,
        [](std::string_view unit, std::string_view ability) { return roster_ability_reason(unit, ability).empty(); },
        true, errors, diagnostics, &load_notes);
    if (!abilities) return ContentResult::failure(abilities.error());
    content.abilities = std::move(abilities).value();
    drop_skipped_launch_content(content, tables, *diagnostics, load_notes);
    core::LoadNoteCollector collected;
    collected.merge(load_notes);
    content.load_notes = collected.snapshot();
    return ContentResult::success(std::move(content));
}

namespace {

constexpr std::string_view skipped_prefix = "skipped unit type ";

// The type name inside a skip diagnostic ("skipped unit type 'ID': why"), or empty for another message.
std::string skipped_name(const core::Diagnostic& diagnostic) {
    const std::string_view message = diagnostic.message;
    const auto at = message.find(skipped_prefix);
    if (at == std::string_view::npos) return {};
    auto rest = message.substr(at + skipped_prefix.size());
    if (rest.empty() || rest.front() != '\'') return {};
    rest.remove_prefix(1);
    const auto end = rest.find('\'');
    return end == std::string_view::npos ? std::string{} : std::string(rest.substr(0, end));
}

core::LoadNote placement_note(const std::string& what, const std::string& type, const core::Diagnostic& why) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::input);
    diagnostic.message = what + " places unit type '" + type + "', which this content skipped (#4589): " + why.message;
    core::LoadNote note;
    note.code = "EAWR-LOAD-0450";
    note.object = what;
    note.tag = "type";
    note.authored_value = type;
    note.action = core::LoadAction::skipped_placement;
    note.effective_value = "omitted";
    note.diagnostic_code = diagnostic.code;
    note.message = diagnostic.message;
    return note;
}

using Rejections = std::map<sim::tactical::TypeId, core::Diagnostic>;
Rejections rejections(const std::span<const core::Diagnostic> skipped) {
    Rejections result;
    for (const auto& diagnostic : skipped) {
        const auto name = skipped_name(diagnostic);
        if (!name.empty()) result.try_emplace(type_id(name), diagnostic);
    }
    return result;
}

void expand_skipped_teams(const units::UnitTables& tables, std::vector<core::Diagnostic>& skipped) {
    const units::ContentTypeSelection type_selection(tables);
    auto rejected = rejections(skipped);
    if (rejected.empty()) return;
    // AG-11: a team whose craft was rejected must never be created, including later
    // replenishment and reserve launches. Keep the original diagnostic as its reason.
    for (const auto& unit : tables.units) {
        if (!type_selection.keeps(unit)) continue;
        const auto id = type_id(unit.id);
        if (rejected.contains(id)) continue;
        for (const auto& member : unit.members) {
            const auto found = rejected.find(type_id(member.craft));
            if (found == rejected.end()) continue;
            auto diagnostic = found->second;
            diagnostic.message = "skipped unit type '" + unit.id + "': " + found->second.message;
            rejected.emplace(id, diagnostic);
            skipped.push_back(std::move(diagnostic));
            break;
        }
    }
}

void drop_skipped_launch_content(SessionContent& content, const units::UnitTables& tables,
    std::vector<core::Diagnostic>& skipped, core::LoadNoteBuffer& notes) {
    expand_skipped_teams(tables, skipped);
    const auto rejected = rejections(skipped);
    if (rejected.empty()) return;
    for (const auto& [type, diagnostic] : rejected) {
        (void)type;
        auto note = placement_note(skipped_name(diagnostic), skipped_name(diagnostic), diagnostic);
        note.code = "EAWR-LOAD-0451";
        note.action = core::LoadAction::skipped_object;
        note.diagnostic_code = diagnostic.code;
        note.message = diagnostic.message;
        notes.append(std::move(note));
    }
    const auto filter = [&](auto& spawner) {
        std::erase_if(spawner.entries, [&](const auto& entry) {
            const auto found = rejected.find(entry.squadron);
            if (found == rejected.end()) return false;
            auto note = placement_note("spawner type " + std::to_string(spawner.type_id),
                skipped_name(found->second), found->second);
            note.tag = "Starting_Spawned_Units_Tech_0";
            // Multiple rejected entries on one spawner are distinct authored references.
            note.object += " launch " + std::to_string(entry.squadron);
            notes.append(std::move(note));
            return true;
        });
    };
    for (auto& spawner : content.motion.squadrons.spawners) filter(spawner);
    for (auto& [key, spawner] : content.motion.squadrons.campaign_spawners) {
        (void)key;
        filter(spawner);
    }
}

// Close removal over whole squadron placements so neither orphan craft nor empty
// team containers survive. Preserve all remaining entity IDs and allocator boundaries.
void drop_setup_entities(sim::tactical::TacticalSetup& setup, std::set<sim::EntityId>& removed,
    const Rejections& rejected, std::vector<core::LoadNote>* const notes) {
    // Healthy teammates leave this placement, but remain valid independent garrison templates.
    auto placement_roots = removed;
    sim::EntityId highest{};
    const auto visit_id = [&](const sim::EntityId entity) { highest = std::max(highest, entity); };
    const auto visit_carried = [&](auto&& self, const auto& object) -> void {
        visit_id(object.entity_id);
        for (const auto& member : object.members) self(self, member);
    };
    for (const auto& unit : setup.units) {
        visit_id(unit.entity_id);
        for (const auto& object : unit.contained) visit_carried(visit_carried, object);
    }
    for (const auto& team : setup.squadrons) {
        visit_id(team.container);
        for (const auto member : team.members) visit_id(member);
        if (!removed.contains(team.container) && std::none_of(team.members.begin(), team.members.end(),
                [&](const auto member) { return removed.contains(member); })) continue;
        removed.insert(team.container);
        placement_roots.insert(team.container);
        removed.insert(team.members.begin(), team.members.end());
    }
    for (const auto& garrison : setup.free_garrisons) for (const auto entity : garrison.registered) visit_id(entity);
    // A carried team is one placement. Removing its parent or any rejected member
    // removes that whole carried placement, including nested identities.
    const auto carried_reason = [&](auto&& self, const auto& object) -> const core::Diagnostic* {
        const auto found = rejected.find(object.type_id);
        if (found != rejected.end()) return &found->second;
        for (const auto& member : object.members) if (const auto* why = self(self, member)) return why;
        return nullptr;
    };
    const auto refers_removed = [&](auto&& self, const auto& object) -> bool {
        if (removed.contains(object.entity_id) || removed.contains(object.parent)) return true;
        return std::any_of(object.members.begin(), object.members.end(), [&](const auto& member) { return self(self, member); });
    };
    const auto mark_carried = [&](auto&& self, const auto& object) -> void {
        removed.insert(object.entity_id);
        for (const auto& member : object.members) self(self, member);
    };
    for (auto& unit : setup.units) {
        std::erase_if(unit.contained, [&](const auto& object) {
            const auto* why = carried_reason(carried_reason, object);
            if (!why && !removed.contains(unit.entity_id) && !refers_removed(refers_removed, object)) return false;
            if (notes && !removed.contains(unit.entity_id)) {
                auto diagnostic = why ? *why : detail::error(diagnostic_codes::input, "contained placement refers to an omitted entity");
                notes->push_back(placement_note("contained placement " + std::to_string(object.entity_id)
                    + " of carrier " + std::to_string(unit.entity_id), why ? skipped_name(*why) : std::to_string(object.type_id), diagnostic));
            }
            mark_carried(mark_carried, object);
            return true;
        });
    }
    if (!removed.empty() && !setup.first_free_entity_id && highest < std::numeric_limits<sim::EntityId>::max())
        setup.first_free_entity_id = highest + 1;
    std::set<sim::tactical::TypeId> removed_types;
    for (const auto& unit : setup.units) if (placement_roots.contains(unit.entity_id)) removed_types.insert(unit.type_id);
    std::erase_if(setup.squadrons, [&](const auto& team) { return removed.contains(team.container); });
    std::erase_if(setup.units, [&](const auto& unit) { return removed.contains(unit.entity_id); });
    for (auto& garrison : setup.free_garrisons) {
        std::erase_if(garrison.registered, [&](const auto entity) { return removed.contains(entity); });
        std::erase_if(garrison.templates, [&](const auto type) {
            const auto found = rejected.find(type);
            if (found == rejected.end() && !removed_types.contains(type)) return false;
            if (notes) {
                const auto diagnostic = found != rejected.end() ? found->second
                    : detail::error(diagnostic_codes::input, "free garrison template belongs to an omitted placement");
                notes->push_back(placement_note("free garrison of player " + std::to_string(garrison.player)
                    + " template " + std::to_string(type), found != rejected.end() ? skipped_name(found->second)
                    : std::to_string(type), diagnostic));
            }
            return true;
        });
    }
    // The empty binding is derived metadata; each authored template removal above has its own note.
    std::erase_if(setup.free_garrisons, [](const auto& garrison) { return garrison.templates.empty(); });
}

} // namespace

std::vector<std::string> skipped_type_ids(const std::span<const core::Diagnostic> skipped) {
    std::vector<std::string> ids;
    for (const auto& diagnostic : skipped) {
        auto name = skipped_name(diagnostic);
        if (!name.empty()) ids.push_back(std::move(name));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<std::string> skipped_type_ids_of(const units::UnitTables& tables) {
    std::vector<core::Diagnostic> skipped;
    (void)units::combat_table(tables, units::ContentErrors::skip_object, &skipped);
    (void)units::ability_table(tables, {},
        [](std::string_view unit, std::string_view ability) { return roster_ability_reason(unit, ability).empty(); },
        true, units::ContentErrors::skip_object, &skipped);
    expand_skipped_teams(tables, skipped);
    return skipped_type_ids(skipped);
}

std::string content_load_summary(const std::span<const core::Diagnostic> diagnostics) {
    const auto rejected = skipped_type_ids(diagnostics);
    std::vector<std::string> warned;
    std::size_t other_warnings = 0;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != core::Severity::warning) continue;
        std::string name;
        for (const std::string_view prefix : {"unit '", "unit type '"}) {
            const std::string_view message = diagnostic.message;
            if (!message.starts_with(prefix)) continue;
            const auto end = message.find('\'', prefix.size());
            if (end != std::string_view::npos) name = message.substr(prefix.size(), end - prefix.size());
        }
        if (name.empty()) { ++other_warnings; continue; }
        const auto same = [&](const auto& id) { return detail::iequals(id, name); };
        if (std::none_of(rejected.begin(), rejected.end(), same)
            && std::none_of(warned.begin(), warned.end(), same)) warned.push_back(std::move(name));
    }
    std::string summary = std::to_string(rejected.size()) + (rejected.size() == 1 ? " unit type skipped, " : " unit types skipped, ")
        + std::to_string(warned.size()) + (warned.size() == 1 ? " unit with content warnings" : " units with content warnings");
    if (other_warnings != 0) summary += ", " + std::to_string(other_warnings)
        + (other_warnings == 1 ? " other content warning" : " other content warnings");
    return summary;
}

void drop_skipped_setup(sim::tactical::TacticalSetup& setup,
    const std::span<const core::Diagnostic> skipped, std::vector<core::LoadNote>* const notes) {
    const auto previous_notes = notes ? notes->size() : 0;
    const auto rejected = rejections(skipped);
    if (rejected.empty()) return;
    std::set<sim::EntityId> removed;
    for (const auto& unit : setup.units) {
        const auto found = rejected.find(unit.type_id);
        if (found == rejected.end()) continue;
        removed.insert(unit.entity_id);
        if (notes) notes->push_back(placement_note("the recorded unit " + std::to_string(unit.entity_id),
            skipped_name(found->second), found->second));
    }
    drop_setup_entities(setup, removed, rejected, notes);
    if (notes && setup.skirmish) for (auto index = previous_notes; index < notes->size(); ++index)
        (*notes)[index].file = setup.skirmish->map;
}

void drop_skipped_starts(SkirmishStart& start, const std::span<const core::Diagnostic> skipped,
    std::vector<core::LoadNote>* const notes) {
    const auto previous_notes = notes ? notes->size() : 0;
    const auto rejected = rejections(skipped);
    if (rejected.empty()) return;
    std::set<sim::EntityId> removed;
    for (const auto& unit : start.units) {
        auto found = rejected.find(type_id(unit.type));
        if (found == rejected.end() && !unit.craft_type.empty()) found = rejected.find(type_id(unit.craft_type));
        if (found == rejected.end()) continue;
        removed.insert(unit.state.entity_id);
        if (notes) notes->push_back(placement_note("starting unit " + std::to_string(unit.state.entity_id)
            + " (record " + std::to_string(unit.record) + ")", skipped_name(found->second), found->second));
    }
    drop_setup_entities(start.setup, removed, rejected, notes);
    std::erase_if(start.units, [&](const auto& unit) { return removed.contains(unit.state.entity_id); });
    std::map<sim::EntityId, const sim::tactical::UnitState*> retained_states;
    for (const auto& state : start.setup.units) retained_states.emplace(state.entity_id, &state);
    for (auto& unit : start.units) {
        const auto retained = retained_states.find(unit.state.entity_id);
        if (retained != retained_states.end()) unit.state.contained = retained->second->contained;
    }
    bool dropped_launch = false;
    std::erase_if(start.launches, [&](const auto& launch) {
        const auto found = rejected.find(type_id(launch.squadron));
        if (found == rejected.end() && !removed.contains(launch.spawner)) return false;
        if (notes) {
            const auto diagnostic = found != rejected.end() ? found->second
                : detail::error(diagnostic_codes::input, "launch spawner placement was omitted");
            notes->push_back(placement_note("the launch of spawner " + launch.spawner_type
                + " entity " + std::to_string(launch.spawner), launch.squadron, diagnostic));
        }
        dropped_launch = true;
        return true;
    });
    std::erase_if(start.arrivals.rows, [&](const auto& demand) {
        const auto found = rejected.find(type_id(demand.type));
        if (found == rejected.end() && !removed.contains(demand.anchor)) return false;
        if (notes) {
            const auto diagnostic = found != rejected.end() ? found->second
                : detail::error(diagnostic_codes::input, "arrival anchor placement was omitted");
            notes->push_back(placement_note("arrival " + demand.type + " of anchor " + std::to_string(demand.anchor), demand.type, diagnostic));
        }
        return true;
    });
    if (notes) for (auto index = previous_notes; index < notes->size(); ++index) (*notes)[index].file = start.map;
    if (removed.empty() && !dropped_launch) return;
    core::LoadNoteBuffer power_notes;
    for (auto& player : start.players) {
        player.combat_power_tick_zero = {};
        player.combat_power_launches = {};
        for (const auto& unit : start.units) {
            if (unit.state.owner != player.player.player_id || !unit.combat_power
                || unit.role == battle::UnitRole::map_object) continue;
            player.combat_power_tick_zero = units::add_power(player.combat_power_tick_zero, *unit.combat_power,
                "player " + std::to_string(player.player.player_id), "starting combat power: ", diagnostic_codes::input, &power_notes);
        }
        for (const auto& launch : start.launches) {
            if (launch.owner != player.player.player_id) continue;
            player.combat_power_launches = units::add_power(player.combat_power_launches, launch.combat_power,
                "player " + std::to_string(player.player.player_id), "launch combat power: ", diagnostic_codes::input, &power_notes);
        }
    }
    if (notes) {
        core::LoadNoteCollector collected;
        collected.merge(power_notes);
        for (auto note : collected.snapshot()) notes->push_back(std::move(note));
    }
}

std::size_t drop_skipped_menu_options(sim::tactical::EconomyRules& rules, const std::span<const std::string> skipped,
    std::vector<std::string>* const log) {
    if (skipped.empty()) return 0;
    std::vector<sim::tactical::TypeId> types;
    for (const auto& id : skipped) types.push_back(battle::type_id(id));
    std::sort(types.begin(), types.end());
    std::size_t removed = 0;
    for (auto& menu : rules.menus) {
        const auto drop = std::remove_if(menu.options.begin(), menu.options.end(), [&](const auto& option) {
            return std::binary_search(types.begin(), types.end(), option.type);
        });
        for (auto it = drop; it != menu.options.end(); ++it) {
            if (log != nullptr) {
                log->push_back("build menu of station type " + std::to_string(menu.station) + ": entry " + std::to_string(it->type)
                    + " removed, its unit type was skipped (#4589)");
            }
        }
        removed += static_cast<std::size_t>(menu.options.end() - drop);
        menu.options.erase(drop, menu.options.end());
    }
    return removed;
}

void bind_skipped_types(SkirmishStart& start, const std::span<const std::string> skipped) {
    if (skipped.empty()) return;
    std::string joined;
    for (const auto& type : skipped) joined += type + "\n";
    const auto bind = [&](sim::MatchIdentity& identity) {
        std::erase_if(identity.domains, [](const auto& domain) { return domain.name == "skipped-types"; });
        identity.domains.push_back(sim::identity_domain("skipped-types", joined));
        identity = sim::compute_match_identity(std::move(identity));
    };
    bind(start.identity);
    if (start.setup.match_identity) bind(*start.setup.match_identity);
}

namespace detail {

std::optional<std::int64_t> q24_from_binary32_bits(const std::uint32_t bits) noexcept {
    const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
    if ((bits >> 31U) != 0U || exponent == 0U || exponent == 0xFFU) return std::nullopt;
    const std::int64_t mantissa = static_cast<std::int64_t>((bits & 0x7FFFFFU) | 0x800000U);
    // value = mantissa * 2^(exponent - 150); raw = value * 2^24.
    const int shift = static_cast<int>(exponent) - 150 + static_cast<int>(sim::math::Fixed::fractional_bits);
    if (shift >= 0) {
        if (shift > 62 - 24) return std::nullopt;
        return mantissa << shift;
    }
    return shift <= -24 ? std::int64_t{0} : mantissa >> -shift;
}

} // namespace detail

sim::tactical::MapBox map_box_of_extents(const std::optional<std::pair<std::uint32_t, std::uint32_t>>& extents) {
    if (!extents) return {};
    const auto width = detail::q24_from_binary32_bits(extents->first);
    const auto height = detail::q24_from_binary32_bits(extents->second);
    if (!width || !height || *width <= 0 || *height <= 0) return {};
    return {sim::math::Fixed::from_raw(-(*width / 2)), sim::math::Fixed::from_raw(-(*height / 2))};
}

sim::tactical::MapBox map_box(const StartInputs& inputs) { return map_box_of_extents(inputs.map_extents); }

core::Result<std::optional<sim::tactical::FogRules>> fog_rules(const StartInputs& inputs) {
    using FogResult = core::Result<std::optional<sim::tactical::FogRules>>;
    if (!inputs.map_extents) return FogResult::success(std::nullopt);
    const auto width = detail::q24_from_binary32_bits(inputs.map_extents->first);
    const auto height = detail::q24_from_binary32_bits(inputs.map_extents->second);
    const auto cell = inputs.fog_cell_size.raw();
    if (!width || !height || *width <= 0 || *height <= 0 || cell <= 0) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::input);
        diagnostic.message = "the map's declared extents (binary32 bits " + std::to_string(inputs.map_extents->first) + ", "
            + std::to_string(inputs.map_extents->second) + ") or the fog cell size (Q24 raw " + std::to_string(cell)
            + ") are not positive";
        return FogResult::failure(std::move(diagnostic));
    }
    // V-18: cells rounded up and at most 512 a side, as FoC sizes its fog grid.
    const auto cells = [&](const std::int64_t extent) {
        return static_cast<std::uint32_t>(std::min<std::int64_t>((extent + cell - 1) / cell, 512));
    };
    auto step = sim::tactical::fog_ramp_down_step(inputs.fog_regrow_seconds);
    if (!step) return FogResult::failure(step.error());
    sim::tactical::FogRules rules;
    rules.map_left = sim::math::Fixed::from_raw(-(*width / 2));
    rules.map_top = sim::math::Fixed::from_raw(*height / 2);
    rules.cell_size = inputs.fog_cell_size;
    rules.cells_wide = cells(*width);
    rules.cells_tall = cells(*height);
    rules.ramp_down_step = step.value();
    // V-22 / WHZ-09: fog initialization uses raw, unrotated obstacle circles.
    if (inputs.tables != nullptr) {
        const units::FootprintTypeSelection selection(*inputs.tables);
        for (const auto& placement : inputs.placements) {
            if (!placement.position) continue;
            const units::SpaceFootprint* footprint = nullptr;
            if (const auto* winner = selection.find(placement.type))
                footprint = winner->unit ? &winner->unit->footprint : &winner->obstacle->footprint;
            if (footprint == nullptr || !footprint->space_obstacle || !footprint->obstacle_radius
                || !(footprint->hazard.nebula || footprint->hazard.asteroid_field
                    || footprint->hazard.impassable_asteroid || footprint->hazard.ion_storm)) continue;
            const auto x = sim::math::add(placement.position->x, footprint->hazard.obstacle_offset.x);
            const auto y = sim::math::add(placement.position->y, footprint->hazard.obstacle_offset.y);
            if (!x || !y) {
                core::Diagnostic diagnostic;
                diagnostic.code = std::string(diagnostic_codes::input);
                diagnostic.message = "dense fog obstacle offset exceeds the coordinate range";
                return FogResult::failure(std::move(diagnostic));
            }
            rules.dense_circles.push_back({{x.value(), y.value(), Fixed{}}, *footprint->obstacle_radius});
        }
    }
    if (auto valid = sim::tactical::validate_fog_rules(rules); !valid) return FogResult::failure(valid.error());
    return FogResult::success(rules);
}

sim::tactical::VictoryRules victory_rules(const sim::tactical::TacticalSetup& setup, const units::UnitTables& tables,
    const std::span<const sim::tactical::PlayerId> humans, const sim::tactical::VictoryCondition condition) {
    sim::tactical::VictoryRules rules;
    rules.condition = condition;
    for (const auto& type : tables.units) {
        if (type.kind == units::UnitKind::station && type.victory_relevant) rules.starbase_types.push_back(type_id(type.id));
        if (type.victory_relevant) rules.relevant_types.push_back(type_id(type.id));
    }
    std::sort(rules.starbase_types.begin(), rules.starbase_types.end());
    rules.starbase_types.erase(std::unique(rules.starbase_types.begin(), rules.starbase_types.end()),
        rules.starbase_types.end());
    std::sort(rules.relevant_types.begin(), rules.relevant_types.end());
    rules.relevant_types.erase(std::unique(rules.relevant_types.begin(), rules.relevant_types.end()), rules.relevant_types.end());
    for (const auto& player : setup.players) {
        rules.installed_players.push_back(player.player_id); // WBF-08 includes non-playable players.
        if (!player.commandable()) continue;
        rules.contenders.push_back(player.player_id);
        rules.controlled_players.push_back(player.player_id);
        if (std::find(humans.begin(), humans.end(), player.player_id) != humans.end()) {
            rules.humans.push_back(player.player_id);
        }
    }
    std::sort(rules.contenders.begin(), rules.contenders.end());
    std::sort(rules.humans.begin(), rules.humans.end());
    std::sort(rules.controlled_players.begin(), rules.controlled_players.end());
    std::sort(rules.installed_players.begin(), rules.installed_players.end());
    return rules;
}

sim::tactical::VictoryRules victory_rules(const SkirmishStart& start, const units::UnitTables& tables) {
    std::vector<sim::tactical::PlayerId> humans;
    for (const auto& player : start.players) {
        if (player.lobby && player.human) humans.push_back(player.player.player_id);
    }
    auto rules = victory_rules(start.setup, tables, humans, start.victory_condition);
    // WBF-35: only lobby players have human/AI controllers in this local start.
    rules.controlled_players.clear();
    for (const auto& player : start.players) if (player.lobby) rules.controlled_players.push_back(player.player.player_id);
    for (const auto& unit : start.units) if (unit.victory_relevant) rules.relevant_types.push_back(unit.state.type_id);
    std::sort(rules.controlled_players.begin(), rules.controlled_players.end());
    std::sort(rules.relevant_types.begin(), rules.relevant_types.end());
    rules.relevant_types.erase(std::unique(rules.relevant_types.begin(), rules.relevant_types.end()), rules.relevant_types.end());
    return rules;
}

std::vector<sim::tactical::PlayerId> human_slots(const Fixture& fixture) {
    std::vector<sim::tactical::PlayerId> humans;
    for (const auto& slot : fixture.slots) {
        if (slot.human) humans.push_back(slot.slot);
    }
    return humans;
}

} // namespace eawr::skirmish
