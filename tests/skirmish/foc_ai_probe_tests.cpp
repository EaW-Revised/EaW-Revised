#include "foc_ai_fixtures.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace foc_ai_test_support {

namespace auth = eawr::script::authoritative;

// L-54: run the installed evaluator with a range outside [0, 1]. Its own
// state and result expose a host that ignores the supplied float bounds.
void progressive_random_probe(const std::filesystem::path& root, const Content& content) {
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        if (!mount) { expect(false, "L-54: evaluator layer mounts"); return; }
        specs.push_back(std::move(mount).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) { expect(false, "L-54: evaluator VFS mounts"); return; }
    const std::string path = "Data/Scripts/Evaluators/progressiverandom.lua";
    const auto bytes = filesystem.value().open(path);
    if (!bytes) { expect(false, "L-54: installed progressive evaluator loads"); return; }
    std::string source(bytes.value().size(), '\0');
    std::transform(bytes.value().begin(), bytes.value().end(), source.begin(),
        [](std::byte value) { return static_cast<char>(value); });
    auto modules = content.modules;
    const auto canonical = auth::ModuleManifest::normalise(path);
    const auto admitted = std::find_if(modules.begin(), modules.end(), [&](const auto& entry) {
        return auth::ModuleManifest::normalise(entry.first) == canonical;
    });
    if (admitted == modules.end()) modules.emplace(path, std::move(source));
    else expect(admitted->second == source, "L-54: the admitted evaluator preserves the installed VFS winner");
    auto setup = content.ai;
    setup.xml.clear();
    setup.freestore_module = "Data/Scripts/Test/RandomProbe.lua";
    modules[setup.freestore_module] =
        "require(\"Data/Scripts/Evaluators/progressiverandom\")\n"
        "function Base_Definitions() ServiceRate = 10000; UnitServiceRate = 10000 end\n"
        "function main()\n"
        "  threshold = 1.5\n"
        "  local result = Evaluate(2, 3)\n"
        "  assert(result == 0 and roll >= 2 and roll <= 3 and threshold > 1.5)\n"
        "end\n";
    std::string baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
            content.content.motion, content.content.fog, content.content.combat, content.victory, content.content.abilities,
            content.economy, content.content.map_box);
        if (!world) { expect(false, "L-54: evaluator world starts"); return; }
        auto session = foc::create_session(std::move(world).value(), setup, modules);
        if (!session) { expect(false, "L-54: evaluator session starts: " + session.error().message); return; }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = session.value().step(executor);
        expect(tick && tick.value().scripts.diagnostics.empty(), "L-54: installed evaluator respects its float range");
        if (!tick) return;
        if (workers == 1) baseline = tick.value().state_sha256;
        else expect(tick.value().state_sha256 == baseline, "L-54: installed evaluator agrees on 1/2/4/8 workers");
        std::cout << "ProgressiveRandom range probe: " << workers << " workers, "
                  << (tick.value().scripts.diagnostics.empty() ? "PASS" : "FAIL") << std::endl;
    }
}

// Stock plans retain the construction/economy inputs they use (WBP-40, SAE-01/08).
// The economy-disabled variant is diagnostic only; it does not establish stock AI liveness.
std::optional<Content> combat_engagement_witness(const std::filesystem::path& root, std::uint64_t seed,
    bool economy_disabled) {
    auto content = load(root, false, false, {}, false, seed);
    if (content && economy_disabled) content->economy = {};
    return content;
}

namespace {

std::pair<std::uint64_t, std::uint64_t> attack_activity(const Run& result) {
    std::uint64_t start = 0, order = 0;
    for (const auto& event : result.journal.plans) {
        if (event.player != 2 || (event.plan != "destroyunit" && event.plan != "destroyunitminimal"
            && event.plan != "flankplan" && event.plan != "turboattacklocation")) continue;
        if (event.event == "started" && (start == 0 || event.tick < start)) start = event.tick;
        if (event.event == "order" && (order == 0 || event.tick < order)) order = event.tick;
    }
    return {start, order};
}

} // namespace

void check_combat_engagement(const Run& result, const std::uint64_t seed, const std::uint64_t ticks) {
    expect(result.player_hits > 0, "the stock AI hits the player on combat seed " + std::to_string(seed));
    if (seed == 68) {
        const auto [start, order] = attack_activity(result);
        expect(start > 0 && start <= ticks && order > 0 && order <= ticks,
            "the stock combat witness starts and orders an attack-family plan");
    }
}

int passive_contact_control(const std::filesystem::path& root, const std::uint64_t ticks) {
    auto content = combat_engagement_witness(root, 68);
    if (!content) return 1;
    // Keep the same stock world and content. Only the AI's service is disabled.
    for (auto& player : content->ai.players) {
        player.ai = false;
        player.player_type.clear();
    }
    const auto result = run(*content, 1, ticks, {}, true);
    if (!result) return 1;
    const auto [start, order] = attack_activity(*result);
    std::cout << "forced-passive control: seed 68 ticks " << ticks << " player hits " << result->player_hits
              << " first hit " << result->first_player_hit << " attack " << start << '/' << order << '\n';
    // The exact same acceptance guard is used by the normal installed battle below.
    check_combat_engagement(*result, 68, ticks);
    return failures != 0 ? 1 : 0;
}

int stock_economy_witness(const std::filesystem::path& root) {
    constexpr std::uint64_t contact_window = 7200, closure_window = 18000;
    auto content = load(root, false, false, {}, false, 68);
    if (!content) return 1;
    expect(content->economy.player(2) != nullptr && !content->economy.menus.empty(),
        "stock economy witness retains its shared ledger and menus");
    std::optional<Run> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto result = run(*content, workers, contact_window, {}, true);
        if (!result) return 1;
        const auto [start, order] = attack_activity(*result);
        std::cout << "stock economy witness seed 68 workers " << workers << " ticks " << contact_window
            << " attack " << start << '/' << order << " AI hits " << result->stock_hits
            << " first hit " << result->first_stock_hit << " seconds " << result->seconds
            << " final " << result->hashes.back() << '\n';
        expect(start > 0 && start <= contact_window && order > 0 && order <= contact_window,
            "stock economy witness starts and orders an attack within the contact window");
        expect(result->stock_hits > 0 && result->first_stock_hit > 0 && result->first_stock_hit <= contact_window,
            "stock economy witness reaches an observed AI hit by tick 7200");
        expect(result->max_instructions < auth::Quotas{}.instructions_per_service / 4,
            "stock economy witness stays within the Lua budget");
        if (!baseline) baseline = std::move(*result);
        else expect(result->hashes == baseline->hashes && result->commands == baseline->commands
            && result->events == baseline->events && result->diagnostics == baseline->diagnostics
            && result->stock_hits == baseline->stock_hits && result->first_stock_hit == baseline->first_stock_hit,
            "stock economy witness hashes, commands, events, diagnostics and hits match on 1/2/4/8 workers");
    }
    auto short_replay = eawr::platform::headless_tick_hashes(baseline->record, content->content.sensors,
        content->content.durability, content->content.motion, content->content.combat, content->victory,
        content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(short_replay && short_replay.value() == baseline->world_hashes,
        "stock economy contact witness replays without scripts");

    auto closure = run(*content, 1, closure_window, {}, true);
    if (!closure) return 1;
    expect(closure->hashes.size() >= baseline->hashes.size()
        && std::equal(baseline->hashes.begin(), baseline->hashes.end(), closure->hashes.begin()),
        "stock economy closure run retains the contact witness prefix");
    expect(closure->stock_hits > 0 && closure->first_stock_hit > 0 && closure->first_stock_hit <= contact_window,
        "stock economy witness has hits by tick 18000 and first contact by tick 7200");
    expect(closure->max_instructions < auth::Quotas{}.instructions_per_service / 4,
        "stock economy closure witness stays within the Lua budget");
    expect(closure->pad_constructions_started > 0
        && closure->pad_constructions_started == closure->pad_constructions_completed
        && closure->pending_pad_constructions == 0,
        "stock economy witness completes all started pad constructions by tick 18000");
    // A plan still open at the cutoff counts as in flight when it started within the longest
    // completed secure-pad plan's duration of the cutoff; any older open plan is stuck (#3048).
    std::map<std::string, std::vector<std::uint64_t>> active; // target -> open start ticks
    std::uint64_t secure_starts = 0, secure_ends = 0, longest = 0;
    for (const auto& event : closure->journal.plans) {
        if (event.player != 2 || event.plan != "movetolocationrush") continue;
        if (event.event == "started") { active[event.target].push_back(event.tick); ++secure_starts; }
        else if (event.event == "finished" || event.event == "abandoned") {
            auto& open = active[event.target];
            expect(!open.empty(), "a secure-pad plan ends only after it starts");
            if (!open.empty()) {
                longest = std::max(longest, event.tick - open.front());
                open.erase(open.begin());
                ++secure_ends;
            }
        }
    }
    std::uint64_t in_flight = 0, stuck = 0;
    for (const auto& [target, open] : active) {
        for (const std::uint64_t start : open) ++(start + longest >= closure_window ? in_flight : stuck);
    }
    expect(secure_ends > 0 && stuck == 0 && secure_starts == secure_ends + in_flight,
        "stock economy witness ends every secure-pad plan started a plan's length before tick 18000");
    std::cout << "stock economy closure seed 68 ticks " << closure_window << " AI hits " << closure->stock_hits
        << " first hit " << closure->first_stock_hit << " pad builds " << closure->pad_constructions_started
        << '/' << closure->pad_constructions_completed << " pending " << closure->pending_pad_constructions
        << " secure plans " << secure_starts << '/' << secure_ends << " in flight " << in_flight
        << " longest " << longest << " seconds " << closure->seconds
        << " final " << closure->hashes.back() << '\n';
    auto long_replay = eawr::platform::headless_tick_hashes(closure->record, content->content.sensors,
        content->content.durability, content->content.motion, content->content.combat, content->victory,
        content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(long_replay && long_replay.value() == closure->world_hashes,
        "stock economy closure witness replays without scripts");
    return failures == 0 ? 0 : 1;
}

// FH-20: category search admits Hutt structures independently of R-09 opportunity priority.
void hutt_nearest_probe(const Content& content) {
    Content probe = content;
    probe.ai.xml.clear(); // This probe isolates the nearest binding from independent plans.
    const auto resource = skirmish::type_id("Orbital_Resource_Container");
    const auto type = std::find_if(probe.ai.content.types.begin(), probe.ai.content.types.end(),
        [&](const auto& entry) { return entry.type_id == resource; });
    const auto property = probe.ai.content.properties.find("NOTOPPORTUNITYTARGET");
    expect(type != probe.ai.content.types.end() && property != probe.ai.content.properties.end()
        && (type->property_bits & property->second) != 0 && !type->space_evaluator,
        "FH-20: stock Hutt resource retains its opportunity exclusion and no goal evaluator");
    const auto source = std::find_if(probe.ai.content.types.begin(), probe.ai.content.types.end(),
        [&](const auto& entry) { return entry.type_id == skirmish::type_id("Acclamator_Assault_Ship"); });
    expect(source != probe.ai.content.types.end() && source->locomotor && !source->star_base,
        "FH-13: the nearest probe source enters the freestore");
    const auto at = [](const std::int64_t x) {
        return eawr::sim::math::Vec3{eawr::sim::math::Fixed::from_raw(x * eawr::sim::math::Fixed::scale), {}, {}};
    };
    probe.start.setup.units = {
        {1, skirmish::type_id("Acclamator_Assault_Ship"), 2, at(0), eawr::sim::math::identity_quat(), {}},
        {2, resource, 7, at(100), eawr::sim::math::identity_quat(), {}},
        {3, resource, 4, at(50), eawr::sim::math::identity_quat(), {}},
        {4, skirmish::type_id("Skirmish_Rebel_Star_Base_1"), 1, at(1000), eawr::sim::math::identity_quat(), {}}};
    probe.start.setup.squadrons.clear();
    // FL-14: this query-only fixture replaces the starting companies, so their
    // registered objects and replenishment templates no longer belong to it.
    probe.start.setup.free_garrisons.clear();
    probe.start.units.clear();
    probe.content.fog.reset();
    probe.victory = {};
    probe.ai.freestore_module = "Data/Scripts/Test/HuttNearestProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 0\n"
        "  UnitServiceRate = 0\n"
        "end\n"
        "function main() end\n"
        "function On_Unit_Service(object)\n"
        "  if target_ordered then return end\n"
        "  target = Find_Nearest(object, \"Structure | Capital\", PlayerObject, false)\n"
        "  if target then\n"
        "    object.Attack_Target(target)\n"
        "    target_ordered = true\n"
        "  end\n"
        "end\n";
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto result = run(probe, workers, 120);
        expect(result.has_value(), "FH-20: nearest Hutt probe runs");
        if (!result) continue;
        std::size_t found = 0;
        for (const auto& command : result->record.commands) {
            if (const auto* attack = std::get_if<tactical::AttackPayload>(&command.payload)) {
                expect(attack->target == 2, "FH-20: nearest non-neutral Hutt structure wins over neutral scenery and farther starbase");
                ++found;
            }
        }
        expect(found == 1, "FH-20: the initial nearest search returns the Hutt structure despite opportunity exclusion");
        expect(result->diagnostics.empty(), "FH-20: the focused nearest script runs without diagnostics");
        std::cout << "Hutt nearest probe: " << workers << " workers, " << found << " attacks, "
                  << result->commands.size() << " routed commands\n";
        for (const auto& diagnostic : result->diagnostics) std::cout << diagnostic << '\n';
        if (workers == 1) reference = result->hashes;
        else expect(result->hashes == reference, "FH-20: Hutt nearest probe hashes agree on 1/2/4/8 workers");
    }
}

// #76 (space-abilities.md AB-44): a stand-in freestore that asks each of the AI's units for
// POWER_TO_WEAPONS through the Lua ability calls. The requests reach the world as ability
// commands, the same on 1 and 2 workers, and the record replays them without the script.
void ability_probe(const Content& content) {
    Content probe = content;
    probe.ai.xml.clear(); // This probe isolates explicit ability requests from independent plans.
    probe.ai.freestore_module = "Data/Scripts/Test/AbilityProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 1\n"
        "  UnitServiceRate = 1\n"
        "end\n"
        "function main()\n"
        "end\n"
        "function On_Unit_Service(object)\n"
        "  if object.Has_Ability(\"Power_To_Weapons\") and object.Is_Ability_Ready(\"Power_To_Weapons\")\n"
        "      and not object.Is_Ability_Active(\"Power_To_Weapons\") then\n"
        "    object.Activate_Ability(\"Power_To_Weapons\", true)\n"
        "  end\n"
        "end\n";
    const auto one = run(probe, 1, 300);
    const auto two = run(probe, 2, 300);
    expect(one && two && one->hashes == two->hashes, "the ability probe hashes alike on 1 and 2 workers");
    if (!one) return;
    std::size_t requests = 0;
    for (const auto& command : one->record.commands) {
        if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
            requests += ability->ability == tactical::AbilityKind::power_to_weapons
                && ability->action == tactical::AbilityAction::activate ? 1 : 0;
        }
    }
    std::cout << "ability probe: " << requests << " POWER_TO_WEAPONS request(s)\n";
    expect(requests > 0, "the Lua Activate_Ability reaches the world as an ability command");
    auto replayed = eawr::platform::headless_tick_hashes(one->record, content.content.sensors, content.content.durability,
        content.content.motion, content.content.combat, content.victory, content.content.fog,
        content.content.abilities, content.economy, content.content.map_box);
    expect(replayed && replayed.value() == one->world_hashes, "the ability probe's record replays without the script");
}

// WST-15: an AI battle with a Vengeance in the Underworld AI's fleet. The stock plans and the event library
// call Try_Ability(.., "STEALTH"); a focused stand-in freestore also switches it on. The requests
// reach the world as STEALTH ability commands, the battle hashes alike on 1, 2, 4 and 8 workers and
// the record replays without the scripts.
void vengeance_probe(const std::filesystem::path& root) {
    auto content = load(root, false, true, {"Vengeance_Frigate", "StarViper_Squadron"});
    if (!content) return;
    const auto vengeance_type = skirmish::type_id("Vengeance_Frigate");
    expect(std::any_of(content->start.units.begin(), content->start.units.end(), [&](const auto& unit) {
        return unit.state.type_id == vengeance_type && unit.state.owner == 2;
    }), "WST-15: the AI player's start carries a Vengeance");
    const auto count = [](const Run& result) {
        std::size_t requests = 0;
        for (const auto& command : result.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::stealth
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        return requests;
    };
    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, 2400);
        if (!result) return;
        runs.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == runs.front().hashes, "WST-15: the Vengeance battle hashes alike at every worker count");
        expect(runs[index].commands == runs.front().commands, "WST-15: the Vengeance battle's script commands match");
    }
    std::cout << "vengeance battle: " << count(runs.front()) << " STEALTH request(s) from the stock plans, "
              << runs.front().ai_hits << " AI hit(s)\n";
    for (const auto& diagnostic : runs.front().diagnostics) std::cout << "vengeance diagnostic: " << diagnostic << '\n';
    auto replayed = eawr::platform::headless_tick_hashes(runs.front().record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities,
        content->economy, content->content.map_box);
    expect(replayed && replayed.value() == runs.front().world_hashes, "WST-15: the Vengeance battle replays without the scripts");

    Content probe = *content;
    probe.ai.xml.clear();
    probe.ai.freestore_module = "Data/Scripts/Test/StealthProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 1\n"
        "  UnitServiceRate = 1\n"
        "end\n"
        "function main()\n"
        "end\n"
        "function On_Unit_Service(object)\n"
        "  if object.Has_Ability(\"STEALTH\") and object.Is_Ability_Ready(\"STEALTH\")\n"
        "      and not object.Is_Ability_Active(\"STEALTH\") then\n"
        "    object.Activate_Ability(\"STEALTH\", true)\n"
        "  end\n"
        "end\n";
    const auto one = run(probe, 1, 300);
    const auto two = run(probe, 2, 300);
    expect(one && two && one->hashes == two->hashes, "WST-15: the stealth probe hashes alike on 1 and 2 workers");
    if (!one) return;
    std::cout << "stealth probe: " << count(*one) << " STEALTH request(s)\n";
    expect(count(*one) > 0, "WST-15: the Lua Activate_Ability(\"STEALTH\") reaches the world as an ability command");
    auto probe_replay = eawr::platform::headless_tick_hashes(one->record, probe.content.sensors, probe.content.durability,
        probe.content.motion, probe.content.combat, probe.victory, probe.content.fog, probe.content.abilities,
        probe.economy, probe.content.map_box);
    expect(probe_replay && probe_replay.value() == one->world_hashes, "WST-15: the stealth probe's record replays without the script");
}

// WST-16 (#1905): the Underworld AI against an idle Rebel fleet whose Vengeance switches its cloak on
// at once. As in the debug build, the AI's queries and perception do not test the cloak: it may still
// plan on the hidden frigate, but only its attack orders on it are refused (WST-11), it never hits it
// while hidden, and it moves on: it keeps ordering and hitting the visible corvettes, and the refusals
// do not repeat every frame. The battle hashes alike on 1, 2, 4 and 8 workers and replays without the
// scripts.
int cloaked_enemy_probe(const std::filesystem::path& root, const std::uint64_t ticks) {
    auto content = load(root, false, true, {}, false, std::nullopt, {"Vengeance_Frigate", "Corellian_Corvette", "Corellian_Corvette"});
    if (!content) return 1;
    const auto vengeance_type = skirmish::type_id("Vengeance_Frigate");
    std::vector<eawr::sim::EntityId> cloakers;
    for (const auto& unit : content->start.units)
        if (unit.state.type_id == vengeance_type && unit.state.owner == 1) cloakers.push_back(unit.state.entity_id);
    expect(!cloakers.empty(), "WST-16: the idle player's start carries a Vengeance");
    if (cloakers.empty()) return 1;
    // Staged so contact does not depend on the AI's approach: the stock starts are about 4700 units apart and
    // the AI never reached either target within 2400 ticks. The Underworld fleet moves as one block to within
    // reach of the Vengeance, keeping its own formation offsets.
    {
        const auto idle = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 1 && unit.type_id == vengeance_type; });
        const auto raider = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [](const auto& unit) { return unit.owner == 2; });
        expect(idle != content->start.setup.units.end() && raider != content->start.setup.units.end(),
            "WST-16: both players start with units");
        if (idle == content->start.setup.units.end() || raider == content->start.setup.units.end()) return 1;
        const auto shift = [](const eawr::sim::math::Fixed from, const eawr::sim::math::Fixed to, const std::int64_t gap) {
            return to.trunc_to_integer() + gap - from.trunc_to_integer();
        };
        const auto dx = shift(raider->position.x, idle->position.x, 700);
        const auto dy = shift(raider->position.y, idle->position.y, 0);
        const auto dz = shift(raider->position.z, idle->position.z, 0);
        const auto move = [&](tactical::UnitState& unit) {
            if (unit.owner != 2) return;
            unit.position.x = eawr::sim::math::Fixed::from_integer(unit.position.x.trunc_to_integer() + dx).value();
            unit.position.y = eawr::sim::math::Fixed::from_integer(unit.position.y.trunc_to_integer() + dy).value();
            unit.position.z = eawr::sim::math::Fixed::from_integer(unit.position.z.trunc_to_integer() + dz).value();
        };
        for (auto& unit : content->start.setup.units) move(unit);
        for (auto& unit : content->start.units) move(unit.state);
        // WST-11 tests targeting, not damage immunity: DG-30 projectiles aimed at a visible
        // corvette can collide with the hidden frigate, and damage leaves its cloak on (WST-05).
        // Keep the stated three-ship enemy fleet, including no free starting fighter forces,
        // and move the visible ships into a separate firing corridor. The cloak remains the
        // nearer query target; stock AI must refuse it and continue fighting the visible ships.
        const auto corvette_type = skirmish::type_id("Corellian_Corvette");
        const auto retained = [&](const tactical::UnitState& unit) {
            return unit.owner != 1 || unit.type_id == vengeance_type || unit.type_id == corvette_type;
        };
        std::set<eawr::sim::EntityId> removed;
        for (const auto& unit : content->start.setup.units)
            if (!retained(unit)) removed.insert(unit.entity_id);
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        std::erase_if(content->start.setup.squadrons, [&](const auto& squadron) { return removed.contains(squadron.container); });
        std::erase_if(content->start.setup.free_garrisons, [](const auto& force) { return force.player == 1; });
        const auto separate = [&](tactical::UnitState& unit) {
            if (unit.owner == 1 && unit.type_id == corvette_type)
                unit.position.y = eawr::sim::math::Fixed::from_raw(unit.position.y.raw() + 1200 * eawr::sim::math::Fixed::scale);
        };
        for (auto& unit : content->start.setup.units) separate(unit);
        for (auto& unit : content->start.units) separate(unit.state);
        expect(std::count_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [](const auto& unit) { return unit.owner == 1; }) == 3,
            "WST-16: only the cloaked frigate and two visible corvettes form the enemy fleet");
        content->victory = {};
    }
    std::vector<tactical::PlayerCommand> input;
    for (std::size_t index = 0; index < cloakers.size(); ++index)
        input.push_back({{1, 1, index}, {cloakers[index]}, tactical::AbilityPayload{tactical::AbilityKind::stealth,
            tactical::AbilityAction::activate}});
    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks, input);
        if (!result) return 1;
        runs.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == runs.front().hashes, "WST-16: the cloaked-enemy battle hashes alike at every worker count");
        expect(runs[index].commands == runs.front().commands, "WST-16: the cloaked-enemy battle's script commands match");
    }
    const auto& first = runs.front();
    // The Vengeance's 60 s cloak holds from tick 1 while it neither fires nor meets a detector.
    const std::uint64_t cloak_end = 1 + 60 * 30;
    const auto hits_on = [&](const bool cloaked) {
        return std::count_if(first.ai_hit_targets.begin(), first.ai_hit_targets.end(), [&](const auto& hit) {
            return (std::find(cloakers.begin(), cloakers.end(), hit.second) != cloakers.end()) == cloaked && hit.first < cloak_end;
        });
    };
    const auto accepted_while_cloaked = std::count_if(first.ai_accepted.begin(), first.ai_accepted.end(),
        [&](const auto tick) { return tick > 1 && tick < cloak_end; });
    std::set<std::uint64_t> refusal_ticks(first.hidden_refusals.begin(), first.hidden_refusals.end());
    std::cout << "cloaked-enemy battle: " << ticks << " ticks, " << first.hidden_refusals.size()
              << " attack order(s) refused on the hidden Vengeance in " << refusal_ticks.size() << " tick(s), "
              << accepted_while_cloaked << " accepted AI order(s) while it was cloaked, " << hits_on(false)
              << " AI hit(s) on visible units and " << hits_on(true) << " on the cloaked Vengeance before tick " << cloak_end << '\n';
    for (const auto& contact : first.cloak_contacts) std::cout << "cloaked-enemy contact: " << contact << '\n';
    for (const auto& diagnostic : first.diagnostics) std::cout << "cloaked-enemy diagnostic: " << diagnostic << '\n';
    expect(!first.hidden_refusals.empty(), "WST-16: the AI tried the hidden Vengeance (its attack orders on it were refused)");
    expect(hits_on(true) == 0, "WST-11: the AI never hits the Vengeance while its cloak hides it");
    expect(hits_on(false) > 0, "WST-16: the AI moves on and engages the visible corvettes");
    expect(accepted_while_cloaked > 0, "WST-16: the AI keeps issuing accepted orders while the Vengeance is cloaked");
    expect(refusal_ticks.size() * 30 < std::min<std::uint64_t>(ticks, cloak_end),
        "WST-16: refused attack orders on the hidden frigate do not repeat every frame (under one per second)");
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(replayed && replayed.value() == first.world_hashes, "WST-16: the cloaked-enemy battle replays without the scripts");
    if (failures != 0) {
        std::cerr << failures << " cloaked-enemy check(s) failed\n";
        return 1;
    }
    std::cout << "cloaked-enemy battle passed\n";
    return 0;
}

// WLD-10: an AI battle with a Crusader present. The AI switches its point defence on through the engine's
// stand-in for the Crusader's object script when an enemy's weapons fire missiles, the request reaches the
// world as an ability command, the Crusader shoots shots down, and the battle hashes alike on 1, 2, 4 and 8
// workers and replays without the script.
void crusader_probe(const std::filesystem::path& root) {
    auto content = load(root, false, false, {}, true);
    if (!content) return;
    const auto crusader_type = skirmish::type_id("Crusader_Gunship");
    expect(std::any_of(content->start.units.begin(), content->start.units.end(), [&](const auto& unit) {
        return unit.state.type_id == crusader_type && unit.state.owner == 2;
    }), "WLD-10: the AI player's start carries a Crusader");
    // Staged so the battle cannot depend on how far the AI's fleet gets: the Crusader starts within the torpedo
    // bombers' reach (the Rebel player is idle), so its torpedoes come at it from the first seconds.
    {
        const auto bomber_name = [&](const tactical::UnitState& unit) {
            const auto name = content->type_names.find(unit.type_id);
            return name == content->type_names.end() ? std::string() : name->second;
        };
        const auto bomber = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 1 && bomber_name(unit).starts_with("Y-Wing"); });
        expect(bomber != content->start.setup.units.end(), "WLD-10: the Rebel player's start carries torpedo bombers");
        if (bomber == content->start.setup.units.end()) return;
        auto target = bomber->position;
        target.x = eawr::sim::math::Fixed::from_integer(target.x.trunc_to_integer() + 450).value();
        const auto move = [&](tactical::UnitState& unit) {
            if (unit.owner == 2 && unit.type_id == crusader_type) unit.position = target;
        };
        for (auto& unit : content->start.setup.units) move(unit);
        for (auto& unit : content->start.units) move(unit.state);
    }
    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, 1800);
        if (!result) return;
        runs.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == runs.front().hashes, "WLD-10: the Crusader battle hashes alike at every worker count");
        expect(runs[index].commands == runs.front().commands, "WLD-10: the Crusader battle's script commands match");
    }
    std::size_t requests = 0;
    for (const auto& command : runs.front().record.commands) {
        if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
            requests += ability->ability == tactical::AbilityKind::laser_defense
                && ability->action == tactical::AbilityAction::activate ? 1 : 0;
        }
    }
    std::cout << "crusader battle: " << requests << " LASER_DEFENSE request(s), " << runs.front().point_defence_zaps
              << " shot(s) shot down, " << runs.front().crusader_hits << " hit(s) on the Crusader, "
              << runs.front().ai_hits << " AI hit(s)\n";
    for (const auto& [shooter, count] : runs.front().crusader_hit_by)
        std::cout << "crusader hit by " << shooter << ": " << count << '\n';
    for (const auto& diagnostic : runs.front().diagnostics) std::cout << "crusader diagnostic: " << diagnostic << '\n';
    // The object script reacts to its deadly enemy, which the threat table names once the Crusader has been
    // hit: the torpedoes hit it, its AI switches the point defence on through the engine's stand-in, the
    // request reaches the world as an accepted ability command and the Crusader then shoots torpedoes down.
    expect(runs.front().crusader_hits > 0, "WLD-10: the Rebel torpedo bombers attack the AI's Crusader");
    expect(requests > 0, "WLD-10: a hit Crusader's AI switches its laser defence on");
    expect(runs.front().point_defence_zaps > 0, "WLD-05: a switched-on Crusader shoots down incoming torpedoes");
    auto replayed = eawr::platform::headless_tick_hashes(runs.front().record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities,
        content->economy, content->content.map_box);
    expect(replayed && replayed.value() == runs.front().world_hashes, "WLD-10: the Crusader battle replays without the script");
}

} // namespace foc_ai_test_support
