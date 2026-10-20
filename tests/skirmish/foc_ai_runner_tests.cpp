#include "foc_ai_fixtures.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/sim/tactical/space.hpp"
#include "../../apps/sim_headless/squadron_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace foc_ai_test_support {

namespace auth = eawr::script::authoritative;

namespace {

std::string argument_text(const auth::Value& value) {
    if (const auto* object = std::get_if<auth::Handle>(&value.data))
        return "handle(" + std::to_string(object->kind) + ":" + std::to_string(object->id) + ")";
    if (const auto* number = std::get_if<eawr::script::numeric::LuaNumber>(&value.data)) {
        const auto integer = eawr::script::numeric::to_exact_integer(*number);
        return integer ? std::to_string(integer.value()) : "number-bits(" + std::to_string(number->repr) + ")";
    }
    if (const auto* text = std::get_if<std::string>(&value.data)) return *text;
    if (const auto* flag = std::get_if<bool>(&value.data)) return *flag ? "true" : "false";
    if (const auto* list = std::get_if<auth::ValueList>(&value.data)) {
        std::string out = "[";
        for (const auto& item : *list) out += argument_text(item) + " ";
        return out + "]";
    }
    return "nil";
}

} // namespace

namespace {

std::string command_text(const auth::RoutedCommand& routed) {
    std::string out = routed.command.verb + " t" + std::to_string(routed.command.tick) + " i" +
        std::to_string(routed.command.issuer) + " s" + std::to_string(routed.command.sequence);
    out += routed.submitted ? " key " + std::to_string(routed.key.tick) + "/" + std::to_string(routed.key.player_id) + "/" +
            std::to_string(routed.key.sequence)
                            : " dropped " + routed.dropped.message;
    for (const auto& argument : routed.command.arguments) out += " " + argument_text(argument);
    return out;
}

} // namespace

// Diagnostic battle with the stock scripts. Requests and actual transitions are separate:
// redundant requests and timed expiration must remain visible in a cause investigation.
int ability_trace(const Content& content, std::uint64_t ticks, const std::filesystem::path& path) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory, content.content.abilities,
        content.economy, content.content.map_box);
    if (!world) { std::cerr << world.error().message << '\n'; return 1; }
    auto setup = content.ai;
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, content.modules);
    if (!session) { std::cerr << session.error().message << '\n'; return 1; }
    const eawr::platform::ThreadWorkerAdapter executor(1);
    std::ofstream output(path);
    output << "tick,event,unit,type,ability,action,issuer,nearest_enemy,attack_range,enemies_in_range,formation_target,order\n";
    std::map<std::pair<eawr::sim::EntityId, tactical::AbilityKind>, bool> active;
    std::map<eawr::sim::EntityId, eawr::sim::EntityId> last_target;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto result = session.value().step(executor);
        if (!result) { std::cerr << result.error().message << '\n'; return 1; }
        const auto& current = session.value().world();
        const auto row = [&](std::string_view event, eawr::sim::EntityId id, tactical::AbilityKind kind,
            std::int64_t action, std::uint64_t issuer) {
            const auto units = current.units();
            const auto unit = std::find_if(units.begin(), units.end(), [&](const auto& item) { return item.entity_id == id; });
            if (unit == units.end()) return;
            double nearest = -1;
            for (const auto& other : units) {
                if ((other.owner != 1 && other.owner != 2) || other.owner == unit->owner) continue;
                const double dx = static_cast<double>(other.position.x.raw() - unit->position.x.raw()) / eawr::sim::math::Fixed::scale;
                const double dy = static_cast<double>(other.position.y.raw() - unit->position.y.raw()) / eawr::sim::math::Fixed::scale;
                const double distance = std::hypot(dx, dy);
                if (nearest < 0 || distance < nearest) nearest = distance;
            }
            const auto mind = current.squadron_state(id);
            auto type = unit->type_id;
            if (mind && !mind->roster.empty()) {
                const auto member = std::find_if(units.begin(), units.end(), [&](const auto& item) {
                    return std::find(mind->roster.begin(), mind->roster.end(), item.entity_id) != mind->roster.end();
                });
                if (member != units.end()) type = member->type_id;
            }
            double range = 0;
            for (const auto& profile : content.ai.content.types) if (profile.type_id == type) {
                const auto fixed = eawr::script::numeric::to_fixed(profile.max_attack_distance);
                if (fixed) range = static_cast<double>(fixed.value().raw()) / eawr::sim::math::Fixed::scale;
            }
            output << current.completed_tick() << ',' << event << ',' << id << ',' << content.type_names.at(unit->type_id)
                << ',' << tactical::to_string(kind) << ',' << action << ',' << issuer << ',' << nearest << ',' << range << ','
                << (nearest >= 0 && nearest <= range ? 1 : 0) << ','
                << (mind ? mind->target : 0) << ',' << static_cast<int>(unit->order.kind) << '\n';
        };
        for (const auto& routed : result.value().script_input) {
            const auto& command = routed.command;
            if (command.verb != foc::verb_ability || command.arguments.size() != 4) continue;
            const auto& id = std::get<auth::Handle>(command.arguments[1].data);
            const auto kind = eawr::script::numeric::to_exact_integer(std::get<eawr::script::numeric::LuaNumber>(command.arguments[2].data));
            const auto action = eawr::script::numeric::to_exact_integer(std::get<eawr::script::numeric::LuaNumber>(command.arguments[3].data));
            if (kind && action) row(routed.submitted ? "request" : "dropped", id.id,
                static_cast<tactical::AbilityKind>(kind.value()), action.value(), command.issuer);
        }
        for (const auto& instance : result.value().world.snapshot->instances()) {
            if (instance.owner != 2) continue;
            for (const auto& slot : instance.abilities) {
                const auto key = std::pair{instance.entity_id, slot.kind};
                const bool before = active[key];
                if (slot.active != before) row("transition", instance.entity_id, slot.kind, slot.active ? 1 : 2, 0);
                active[key] = slot.active;
            }
        }
        const auto current_units = current.units();
        for (const auto& squadron : current.squadrons()) {
            const auto mind = current.squadron_state(squadron.container);
            if (!mind || mind->target == 0 || mind->target == last_target[squadron.container]) continue;
            last_target[squadron.container] = mind->target;
            bool locked = false;
            for (const auto member : squadron.members) {
                const auto state = current.ability_state(member);
                const auto unit = std::find_if(current_units.begin(), current_units.end(),
                    [&](const auto& item) { return item.entity_id == member; });
                if (!state || unit == current_units.end() || unit->owner != 2) continue;
                const auto* profile = current.abilities().find(unit->type_id);
                if (profile == nullptr) continue;
                const auto slot = tactical::ability_slot(*profile, tactical::AbilityKind::spoiler_lock);
                locked = locked || (slot && state->slots[*slot].active);
            }
            row("engagement", squadron.container, tactical::AbilityKind::spoiler_lock, locked ? 1 : 2, 0);
        }
    }
    const auto& history = *setup.journal;
    if (history.costs_dropped || history.plans_dropped || history.services_dropped)
        std::cout << "AI journal: costs_dropped=" << history.costs_dropped
                  << " plans_dropped=" << history.plans_dropped
                  << " services_dropped=" << history.services_dropped << '\n';
    std::ofstream journal(path.string() + ".plans.csv");
    journal << "tick,plan,event,detail\n";
    for (const auto& event : setup.journal->plans)
        journal << event.tick << ',' << event.plan << ',' << event.event << ",\"" << event.detail << "\"\n";
    std::cout << "Ability trace: " << ticks << " ticks, " << path << '\n';
    return output && journal ? 0 : 1;
}

// `input`: the idle player's own commands, each stepped in at its key's tick.
std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks,
    const std::vector<tactical::PlayerCommand>& input, bool observe_stock, bool observe_autofire,
    std::ostream* squadron_rows) {
    sim_headless::SquadronTrace squadron_trace;
    if (squadron_rows) sim_headless::SquadronTrace::header(*squadron_rows);
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory,
        content.content.abilities, content.economy, content.content.map_box);
    expect(static_cast<bool>(world), "the world is created: "
        + (world ? std::string() : world.error().code + " " + world.error().message));
    if (!world) return std::nullopt;
    if (workers == 1) {
        const auto snapshot = world.value().snapshot();
        std::cout << "Initial object fog (snapshot player bits):";
        for (std::size_t index = 0; index < snapshot->players().size(); ++index)
            std::cout << " bit" << index << "=player" << snapshot->players()[index].player_id;
        std::cout << '\n';
        for (const auto& instance : snapshot->instances()) {
            const auto name = content.type_names.find(instance.type_id);
            std::cout << "  unit " << instance.entity_id << " owner " << instance.owner
                      << " type " << (name == content.type_names.end() ? "(unknown)" : name->second)
                      << " visible_to " << instance.visible_to << " x "
                      << instance.fixed_transform.rows[0][3].trunc_to_integer() << " y "
                      << instance.fixed_transform.rows[1][3].trunc_to_integer() << " reveal_range "
                      << (instance.reveal_range ? instance.reveal_range->trunc_to_integer() : 0) << '\n';
        }
    }
    auto setup = content.ai;
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, content.modules);
    expect(static_cast<bool>(session), "the AI session is created: " + (session ? std::string() : session.error().code + " " + session.error().message));
    if (!session) return std::nullopt;
    std::set<eawr::sim::EntityId> ai_units, crusaders;
    std::map<eawr::sim::EntityId, tactical::PlayerId> observed_owners;
    for (const auto& unit : content.start.units) {
        if (unit.state.owner == 2) ai_units.insert(unit.state.entity_id);
        if (observe_stock) observed_owners.emplace(unit.state.entity_id, unit.state.owner);
        if (unit.state.owner == 2 && unit.state.type_id == skirmish::type_id("Crusader_Gunship")) crusaders.insert(unit.state.entity_id);
    }
    auto stock_ai_units = observe_stock ? ai_units : std::set<eawr::sim::EntityId>{};
    // Match live dispatch: small named phases stay inline; larger phases still use the pool (ADR-009).
    const eawr::platform::ThreadWorkerAdapter executor(workers,
        eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
    Run out;
    std::int64_t previous_hull = 0;
    bool blast_armed = false;
    std::size_t leech_trace = 0;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        const auto previous_snapshot = session.value().world().snapshot();
        std::vector<tactical::PlayerCommand> due;
        for (const auto& command : input)
            if (command.key.tick == session.value().world().completed_tick()) due.push_back(command);
        // Independently sample readiness before switch-on. The target is sampled after
        // this tick's targeting/commands, so a same-tick acquisition has a witness too.
        std::set<std::pair<eawr::sim::EntityId, tactical::AbilityKind>> ready_before;
        if (observe_autofire) {
            const auto& battle = session.value().world();
            for (const auto& source : battle.units()) {
                if (source.owner != 2) continue;
                const auto state = battle.ability_state(source.entity_id);
                const auto* profile = content.content.abilities.find(source.type_id);
                const auto health = battle.durability_state(source.entity_id);
                if (!state || !profile) continue;
                tactical::AbilityGate gate;
                gate.hardpoint_profile = content.content.durability.find(source.type_id);
                gate.hardpoint_state = health ? &*health : nullptr;
                for (const auto kind : {tactical::AbilityKind::tractor_beam, tactical::AbilityKind::leech_shields}) {
                    const auto slot = tactical::ability_slot(*profile, kind);
                    if (slot && !state->slots[*slot].active
                        && tactical::ability_ready(profile->abilities[*slot], state->slots[*slot], gate, tick))
                        ready_before.emplace(source.entity_id, kind);
                }
            }
        }
        auto stepped = session.value().step(executor, due);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) return std::nullopt;
        if (tick == 0 || (tick + 1) % 300 == 0)
            std::cout << "AI-PROGRESS workers=" << workers << " completed=" << session.value().world().completed_tick()
                      << '/' << ticks << " elapsed="
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count()
                      << std::endl;
        if (squadron_rows) squadron_trace.observe(*squadron_rows, session.value().world(), content.content);
        if (observe_autofire) {
            const auto& battle = session.value().world();
            const auto live = battle.units();
            for (const auto& source : live) {
                if (source.owner != 2) continue;
                const auto state = battle.ability_state(source.entity_id);
                const auto attack = battle.combat_state(source.entity_id);
                const auto* profile = content.content.abilities.find(source.type_id);
                if (!state || !attack || !profile || attack->attack_target == 0) continue;
                const auto target = std::find_if(live.begin(), live.end(), [&](const auto& unit) {
                    return unit.entity_id == attack->attack_target;
                });
                if (target == live.end() || target->owner == source.owner) continue;
                const auto* combat = content.content.combat.find(source.type_id);
                for (const auto& nested : profile->special) {
                    const auto kind = nested.kind == tactical::SpecialAbilityKind::tractor_beam ? tactical::AbilityKind::tractor_beam
                        : nested.kind == tactical::SpecialAbilityKind::leech_shields ? tactical::AbilityKind::leech_shields
                        : tactical::AbilityKind::none;
                    if (kind == tactical::AbilityKind::none) continue;
                    auto& first_ready = kind == tactical::AbilityKind::tractor_beam ? out.first_tractor_ready_target : out.first_leech_ready_target;
                    const auto& first_lock = kind == tactical::AbilityKind::tractor_beam ? out.first_tractor_lock : out.first_leech_lock;
                    if (first_ready || first_lock) continue;
                    if (!ready_before.contains({source.entity_id, kind})) continue;
                    const auto maximum = nested.beam_max_range.raw() > 0 ? nested.beam_max_range
                        : combat ? combat->max_attack_distance.value_or(eawr::sim::math::Fixed{}) : eawr::sim::math::Fixed{};
                    auto minimum = nested.beam_min_range.raw() > 0 ? nested.beam_min_range
                        : combat ? combat->min_attack_distance : eawr::sim::math::Fixed{};
                    // Conservative fully-in-range witness: centre inside the maximum, outside
                    // a positive minimum plus either footprint extent. No new range assumption for mods.
                    if (const auto* footprint = content.content.motion.footprint(target->type_id); footprint && minimum.raw() > 0)
                        minimum = eawr::sim::math::Fixed::from_raw(minimum.raw() + std::max(footprint->x_extent.raw(), footprint->y_extent.raw()));
                    const auto target_health = battle.durability_state(target->entity_id);
                    const auto* target_combat = content.content.combat.find(target->type_id);
                    if (target_health && target_health->hull.raw() > 0 && target_combat
                        && tactical::special_type_matches(nested.filter, target->type_id, target_combat->category_bits)
                        && (kind != tactical::AbilityKind::leech_shields || target_health->shields.raw() > 0)
                        && tactical::within_range(source.position, target->position, maximum, tactical::RangeMetric::planar)
                        && (minimum.raw() <= 0 || !tactical::within_range(source.position, target->position, minimum, tactical::RangeMetric::planar))) first_ready = tick;
                }
            }
        }
        const auto& result = stepped.value();
        if (workers == 1) {
            for (const auto& unit : result.world.snapshot->instances()) {
                const auto type = content.type_names.find(unit.type_id);
                if (type != content.type_names.end() && (type->second == "Broadside_Class_Cruiser" ||
                    type->second == "Marauder_Missile_Cruiser")) out.artillery_seen.insert(unit.entity_id);
            }
        }
        // WAB-83: witness the range precondition independently of the stand-in script.
        for (const auto& holder : result.world.snapshot->instances()) {
            if (holder.owner != 2) continue;
            if (std::any_of(holder.abilities.begin(), holder.abilities.end(),
                [](const auto& status) { return status.kind == tactical::AbilityKind::full_salvo && status.active; }))
                ++out.salvo_active_ticks;
            const bool ready = std::any_of(holder.abilities.begin(), holder.abilities.end(),
                [](const auto& status) { return status.kind == tactical::AbilityKind::full_salvo && status.ready; });
            if (!ready) continue;
            ++out.salvo_ready_ticks;
            for (const auto& other : result.world.snapshot->instances()) {
                if (other.owner != 1) continue;
                const auto dx = (holder.fixed_transform.rows[0][3].raw() - other.fixed_transform.rows[0][3].raw()) >> 24;
                const auto dy = (holder.fixed_transform.rows[1][3].raw() - other.fixed_transform.rows[1][3].raw()) >> 24;
                const auto dz = (holder.fixed_transform.rows[2][3].raw() - other.fixed_transform.rows[2][3].raw()) >> 24;
                const auto gap = static_cast<std::int64_t>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
                if (out.salvo_min_gap < 0 || gap < out.salvo_min_gap) out.salvo_min_gap = gap;
            }
        }
        // WSD-06: the player's hull in the tick after a blast object existed, against the tick before it detonated.
        std::int64_t player_hull = 0;
        for (const auto& instance : result.world.snapshot->instances()) {
            if (instance.owner == 1 && instance.durability) player_hull += instance.durability->hull.trunc_to_integer();
        }
        for (const auto& holder : result.world.snapshot->instances()) {
            const bool has = holder.owner == 2 && std::any_of(holder.abilities.begin(), holder.abilities.end(),
                [](const auto& status) { return status.kind == tactical::AbilityKind::self_destruct; });
            if (!has) continue;
            if (out.sd_alive_first == 0) out.sd_alive_first = tick + 1;
            out.sd_alive_last = tick + 1;
            for (const auto& status : holder.abilities) {
                if (status.kind == tactical::AbilityKind::self_destruct && status.ready) ++out.sd_ready_ticks;
            }
            for (const auto& other : result.world.snapshot->instances()) {
                if (other.owner != 1) continue;
                const std::int64_t dx = (holder.fixed_transform.rows[0][3].raw() - other.fixed_transform.rows[0][3].raw()) >> 24;
                const std::int64_t dy = (holder.fixed_transform.rows[1][3].raw() - other.fixed_transform.rows[1][3].raw()) >> 24;
                const auto gap = static_cast<std::int64_t>(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
                if (out.sd_min_gap < 0 || gap < out.sd_min_gap) out.sd_min_gap = gap;
            }
        }
        if (blast_armed) out.blast_hull_drop += std::max<std::int64_t>(0, previous_hull - player_hull);
        blast_armed = !result.world.snapshot->ability_spawns().empty();
        if (blast_armed) ++out.blast_objects;
        previous_hull = player_hull;
        // WLS-04: retain a bounded admission trace when the real-data fixture refuses a lock.
        for (const auto& routed : result.script_input) {
            const auto& arguments = routed.command.arguments;
            if (workers != 1 || leech_trace >= 4 || routed.command.verb != foc::verb_ability
                || arguments.size() != 5 || !routed.submitted) continue;
            ++leech_trace;
            const auto source = std::get<auth::Handle>(arguments[1].data).id;
            const auto target = std::get<auth::Handle>(arguments[4].data).id;
            std::cout << "leech-admission " << command_text(routed) << " source " << source << " target " << target;
            for (const auto& instance : result.world.snapshot->instances()) {
                if (instance.entity_id != source && instance.entity_id != target) continue;
                const auto state = session.value().world().durability_state(instance.entity_id);
                std::cout << " [" << content.type_names.at(instance.type_id) << " nebula " << instance.in_nebula
                          << " shields " << (state ? state->shields.raw() : 0) << ']';
            }
            for (const auto& event : result.world.snapshot->events())
                if (event.player == routed.key.player_id && event.sequence == routed.key.sequence)
                    std::cout << ' ' << tactical::to_string(event.kind) << ':' << tactical::to_string(event.reason);
            std::cout << '\n';
        }
        out.hashes.push_back(result.state_sha256);
        out.world_hashes.push_back(result.world.state_sha256);
        // WSW-36..40: log the untouched combat fixture's stock special activity,
        // without imposing an unlock or changing its economy/setup to force it.
        for (const auto& account : result.world.snapshot->economy()) if (account.player == 2) {
            for (const auto& [type, count] : account.lifetime) {
                const auto name = content.type_names.find(type);
                if (name != content.type_names.end() && (name->second == "RS_Ion_Cannon_Use_Upgrade" ||
                    name->second == "ES_Hypervelocity_Gun_Use_Upgrade" || name->second == "US_Plasma_Cannon_Use_Upgrade"))
                    out.special_use_purchases = std::max(out.special_use_purchases, count);
            }
        }
        for (const auto& projectile : result.world.snapshot->projectiles()) {
            if (projectile.owner != 2 || out.special_projectile_ids.contains(projectile.id)) continue;
            const auto instances = result.world.snapshot->instances();
            const auto source = std::lower_bound(instances.begin(), instances.end(), projectile.shooter,
                [](const auto& unit, eawr::sim::EntityId id) { return unit.entity_id < id; });
            if (source == instances.end() || source->entity_id != projectile.shooter) continue;
            if (source->planetary_weapon || std::any_of(source->manual_weapons.begin(), source->manual_weapons.end(),
                [&](const auto& weapon) { return weapon.hardpoint == projectile.weapon; })) {
                ++out.special_projectiles;
                out.special_projectile_ids.insert(projectile.id);
            }
        }
        for (const auto& routed : result.script_input) {
            out.commands.push_back(command_text(routed));
            const auto& command = routed.command;
            if (!routed.submitted || command.verb != foc::verb_ability || command.arguments.size() < 4) continue;
            const auto kind = eawr::script::numeric::to_exact_integer(std::get<eawr::script::numeric::LuaNumber>(command.arguments[2].data));
            if (kind && static_cast<tactical::AbilityKind>(kind.value()) == tactical::AbilityKind::buzz_droids) ++out.plan_buzz;
            if (kind && static_cast<tactical::AbilityKind>(kind.value()) == tactical::AbilityKind::lure) ++out.plan_lure;
        }
        for (const auto& event : result.world.snapshot->events()) {
            if (observe_stock && event.player == 2) {
                if (event.kind == tactical::EventKind::pad_construction_started) ++out.pad_constructions_started;
                if (event.kind == tactical::EventKind::pad_construction_completed) ++out.pad_constructions_completed;
                if (event.kind == tactical::EventKind::order_rejected) out.ai_rejections.push_back(event);
            }
            if (event.kind == tactical::EventKind::order_rejected && event.player == 2
                && event.reason == tactical::RejectReason::target_hidden) out.hidden_refusals.push_back(event.tick);
            if (event.kind == tactical::EventKind::order_accepted && event.player == 2) out.ai_accepted.push_back(event.tick);
            if (event.kind == tactical::EventKind::order_accepted && event.player == 2) {
                const auto instances = result.world.snapshot->instances();
                const auto unit = std::find_if(instances.begin(), instances.end(),
                    [&](const tactical::TacticalInstance& instance) { return instance.entity_id == event.unit; });
                const auto name = unit == instances.end() ? content.type_names.end() : content.type_names.find(unit->type_id);
                ++out.orders_by_type[name == content.type_names.end() ? std::string("(gone)") : name->second];
            }
            out.events.push_back(std::to_string(event.tick) + " " + std::string(tactical::to_string(event.kind)) + " " +
                std::to_string(event.unit));
        }
        std::map<eawr::sim::EntityId, eawr::sim::EntityId> focus; // WHE-24: concentrated target -> its holder
        for (const auto& instance : result.world.snapshot->instances()) {
            // The snapshot publishes a target for the beams only; the slot keeps the concentrated one.
            for (std::size_t index = 0; index < instance.abilities.size(); ++index) {
                if (instance.abilities[index].kind != tactical::AbilityKind::concentrate_fire || !instance.abilities[index].active) continue;
                ++out.concentrate_frames;
                const auto state = session.value().world().ability_state(instance.entity_id);
                if (state && index < state->slots.size() && state->slots[index].target != 0)
                    focus.emplace(state->slots[index].target, instance.entity_id);
            }
            if (observe_stock) {
                observed_owners.insert_or_assign(instance.entity_id, instance.owner);
                if (instance.owner == 2) stock_ai_units.insert(instance.entity_id);
            }
            if (instance.corrupt) out.corrupted.insert(instance.entity_id);
            const auto name = content.type_names.find(instance.type_id);
            if (name != content.type_names.end() && name->second == "Buzz_Droids") out.droids.insert(instance.entity_id);
            for (const auto& ability : instance.abilities)
                if (ability.kind == tactical::AbilityKind::leech_shields && ability.active && ability.target != 0)
                    { ++out.shield_leech_frames; if (!out.first_leech_lock) out.first_leech_lock = tick; }
            for (const auto& ability : instance.abilities)
                if (ability.kind == tactical::AbilityKind::tractor_beam && ability.active && ability.target != 0)
                    { ++out.tractor_frames; if (!out.first_tractor_lock) out.first_tractor_lock = tick; }
            for (const auto& ability : instance.abilities)
                if (ability.kind == tactical::AbilityKind::lure && ability.active) ++out.lure_frames;
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (ai_units.contains(event.shooter)
                && (event.kind == tactical::CombatEventKind::weapon_fired || event.kind == tactical::CombatEventKind::projectile_hit)) {
                const auto before = previous_snapshot->instances();
                const auto target = std::find_if(before.begin(), before.end(),
                    [&](const auto& instance) { return instance.entity_id == event.target; });
                if (target != before.end() && target->stealth_on
                    && target->type_id == skirmish::type_id("Vengeance_Frigate")) {
                    const auto after = result.world.snapshot->instances();
                    const auto published = std::find_if(after.begin(), after.end(),
                        [&](const auto& instance) { return instance.entity_id == event.target; });
                    out.cloak_contacts.push_back(std::to_string(event.tick) + " "
                        + std::string(tactical::to_string(event.kind)) + " shooter=" + std::to_string(event.shooter)
                        + " target=" + std::to_string(event.target) + " selected=" + std::to_string(event.selected_target)
                        + " hidden-before=" + std::to_string(target->stealth_invisible)
                        + " hidden-after=" + std::to_string(published != after.end() && published->stealth_invisible)
                        + " cloak-after=" + std::to_string(published != after.end() && published->stealth_on));
                }
            }
            if (event.kind == tactical::CombatEventKind::point_defence_zap) ++out.point_defence_zaps;
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            if (const auto held = focus.find(event.target); held != focus.end() && held->second != event.shooter)
                ++out.recruit_hits;
            if (observe_stock && stock_ai_units.contains(event.shooter)) {
                ++out.stock_hits;
                if (out.first_stock_hit == 0) out.first_stock_hit = event.tick;
                // Retain last published owners so a lethal hit still has a target identity.
                // A hit on a neutral pad is not contact with the human player's fleet.
                const auto target = observed_owners.find(event.target);
                if (target != observed_owners.end() && target->second == 1) {
                    ++out.player_hits;
                    if (out.first_player_hit == 0) out.first_player_hit = event.tick;
                }
            }
            if (crusaders.contains(event.target)) {
                ++out.crusader_hits;
                const auto instances = result.world.snapshot->instances();
                const auto shooter = std::find_if(instances.begin(), instances.end(),
                    [&](const auto& instance) { return instance.entity_id == event.shooter; });
                const auto name = shooter == instances.end() ? content.type_names.end() : content.type_names.find(shooter->type_id);
                ++out.crusader_hit_by[name == content.type_names.end() ? std::string("(gone)") : name->second];
            }
            if (ai_units.contains(event.shooter)) {
                out.ai_hit_targets.emplace_back(event.tick, event.target);
                ++out.ai_hits;
                if (out.first_ai_hit == 0) out.first_ai_hit = event.tick;
            }
            out.hits_csv += std::to_string(event.tick) + "," + std::to_string(event.shooter) + "," +
                std::to_string(event.target) + "," + std::to_string(event.aim.x.trunc_to_integer()) + "," +
                std::to_string(event.aim.y.trunc_to_integer()) + "," + std::to_string(event.aim.z.trunc_to_integer()) + "\n";
        }
        for (const auto& diagnostic : result.scripts.diagnostics) out.diagnostics.insert(diagnostic.code + " " + diagnostic.message);
        std::int64_t tick_load = 0;
        for (const auto& load : result.scripts.loads) tick_load += load.instructions;
        out.max_instructions = std::max(out.max_instructions, tick_load);
        out.total_instructions += tick_load;
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (observe_stock) for (const auto& [id, pad] : session.value().world().pads()) {
        (void)id;
        if (pad.under_construction != 0) ++out.pending_pad_constructions;
    }
    out.record = session.value().record();
    out.journal = *setup.journal;
    return out;
}

} // namespace foc_ai_test_support
