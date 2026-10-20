// The FoC tactical AI in the M2 battle (#79, docs/behaviour/foc-tactical-ai.md "#79 host"):
// the Empire AI's retail freestore and goal engine against an idle Rebel player on the pinned start. The
// fixture must give the same script commands, world events and state hashes on 1, 2, 4 and
// 8 workers (ADR-009), a headless replay of its record must reproduce the world without
// running a script, and the per-tick Lua load stays below the service budget. A real-time
// run paused mid-attack and resumed on fast forward must replay like an unpaused one (#459).
//
//   foc_ai_tests [ticks [hits.csv]]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)
//   foc_ai_tests --combat-engagement-trace [ticks [trace.txt [seed]]]
//                                     the economy-disabled diagnostic, including later admitted AI units
//   foc_ai_tests --stock-engagement-trace [ticks [trace.txt [seed]]]
//                                     the full stock battle, including actual human-player contact
//   foc_ai_tests --passive-contact-control [ticks]
//                                     disables AI planning/scripts; the contact guard must fail
//   foc_ai_tests --underworld [ticks [trace.csv]] the Underworld AI's StarVipers against an idle Rebel fleet:
//                                     a stand-in freestore fires BUZZ_DROIDS through the Lua calls (WAB-70..76)
//   foc_ai_tests --vengeance [ticks]            the Underworld AI's Vengeance frigates against the same fleet:
//                                     a stand-in freestore switches SELF_DESTRUCT on through the Lua calls (WSD-01..09)
//   foc_ai_tests --underworld-stock [ticks]       the same fleets under the stock Underworld plans alone: the
//                                     plans' own Target_In_Range handler activates BUZZ_DROIDS (EX-46)
//   foc_ai_tests --interceptor [ticks]          the Underworld AI's Interceptor IV frigates against the same fleet:
//                                     a stand-in freestore switches FULL_SALVO on through the Lua calls (WAB-80..83)
//   foc_ai_tests --cloaked [ticks]              the Underworld AI against a Rebel fleet whose Vengeance cloaks:
//                                     the AI's queries stay stealth-blind and it moves on from refused attacks (WST-16)
//
// hits.csv lists every projectile hit of the one-worker run (tick, shooter, target, x, y, z):
// where and when the fleets meet, for placing an eye-check camera.

#include "foc_ai_fixtures.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "../../src/script/foc/ai_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace auth = eawr::script::authoritative;

using namespace foc_ai_test_support;

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC AI battle\n";
        return 0;
    }
    foc::ai::Engine::observe_effects(count_effects);
    struct EffectReport {
        ~EffectReport() { foc::ai::Engine::observe_effects(nullptr); if (effect_barriers != 0) report_effects("run"); }
    } effect_report;
    if (argc > 1 && std::string_view(argv[1]) == "--player-stream-profile")
        return player_stream_profile(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5400,
            argc > 3 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 0);
    if (argc > 1 && std::string_view(argv[1]) == "--player-stream-restore")
        return player_stream_profile(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5400, 8, true);
    if (argc > 1 && std::string_view(argv[1]) == "--serial-profile") {
        std::vector<std::size_t> workers{1, 6, 2, 4, 8};
        if (argc > 3) {
            workers.clear();
            for (int index = 3; index < argc; ++index)
                workers.push_back(static_cast<std::size_t>(std::strtoull(argv[index], nullptr, 10)));
        }
        return serial_profile(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5400, workers);
    }
    if (argc > 1 && std::string_view(argv[1]) == "--passive-contact-control")
        return passive_contact_control(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 7200);
    if (argc > 1 && std::string_view(argv[1]) == "--squadron-idle-trace") {
        const auto ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, false, {}, false, 68, {}, {}, true);
        if (!content) return 1;
        std::ostringstream rows;
        const auto result = run(*content, 1, ticks, {}, false, false, &rows);
        if (!result) return 1;
        const std::string path = argc > 3 ? argv[3] : "squadron-idle-trace.csv";
        std::ofstream csv(path);
        csv << rows.str();
        const auto replay = tactical::write_replay(result->record);
        expect(static_cast<bool>(csv) && static_cast<bool>(replay), "stock squadron trace and command recording serialize");
        if (replay) {
            std::ofstream recorded(path + ".eawr-replay", std::ios::binary);
            recorded.write(reinterpret_cast<const char*>(replay.value().data()), static_cast<std::streamsize>(replay.value().size()));
            expect(static_cast<bool>(recorded), "stock 3v3 command recording writes");
        }
        std::cout << "SQ-TRACE-BEGIN\n" << rows.str() << "SQ-TRACE-END\n";
        std::cout << "Stock 3v3 diagnostic: ticks=" << ticks << " commands=" << result->commands.size()
            << " script_diagnostics=" << result->diagnostics.size() << '\n';
        expect(!result->commands.empty(), "stock 3v3 diagnostic observes active AI commands");
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--stock-economy-witness") {
        if (argc != 2) return 1;
        return stock_economy_witness(*root);
    }
    const bool stock = argc > 1 && std::string_view(argv[1]) == "--underworld-stock";
    if (argc > 1 && (std::string_view(argv[1]) == "--underworld" || stock)) {
        // WAB-70..76: the Underworld AI fires buzz droids from its StarVipers; the world,
        // the script commands and a headless replay of the record agree on 1/2/4/8 workers.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, true);
        if (!content) return 1;
        // In this short M2 engagement the stock Underworld plans (move, scout, destroy-unit, area
        // sweep) reach no BUZZ_DROIDS call, so a stand-in freestore asks every StarViper squadron
        // for it through the stock Lua calls, as the ability probe does for POWER_TO_WEAPONS.
        // The stock mode keeps the authored plans: BUZZ_DROIDS then comes from their own handlers (EX-46).
        if (!stock) content->ai.xml.clear();
        if (!stock) {
        content->ai.freestore_module = "Data/Scripts/Test/BuzzDroidsProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, PlayerObject, false)\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"BUZZ_DROIDS\") and object.Is_Ability_Ready(\"BUZZ_DROIDS\")\n"
            "      and object.Get_Distance(enemy) < 350 then\n"
            "    object.Activate_Ability(\"BUZZ_DROIDS\", enemy)\n"
            "  elseif not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        }
        if (argc > 3 && !stock) return ability_trace(*content, ticks, argv[3]); // plan and ability journal
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WAB-72: Underworld battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WAB-76: Underworld script commands match worker count 1");
        }
        std::size_t buzz = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload))
                buzz += ability->ability == tactical::AbilityKind::buzz_droids ? 1 : 0;
        }
        std::cout << "Underworld battle: " << ticks << " ticks, " << buzz << " BUZZ_DROIDS command(s), "
                  << first.droids.size() << " droid object(s), AI hits " << first.ai_hits << '\n';
        std::cout << "AI orders by unit type:";
        for (const auto& [type, count] : first.orders_by_type) std::cout << ' ' << type << '=' << count;
        std::cout << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(buzz > 0, "WAB-76: the Lua Activate_Ability reaches the world as a BUZZ_DROIDS point command");
        if (stock) {
            std::cout << "BUZZ_DROIDS script requests (stock plans only): " << first.plan_buzz << '\n';
            expect(first.plan_buzz > 0, "EX-46: the stock plans' Target_In_Range handler activates BUZZ_DROIDS for a squadron");
        }
        expect(!first.droids.empty(), "WAB-71: an AI activation creates a droid object");
        expect(!first.orders_by_type.contains("Buzz_Droids"), "WAB-71: droids have no locomotor and stay out of the freestore");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-70: the Underworld record replays without the script");
        if (failures != 0) return 1;
        std::cout << "Underworld buzz-droid battle passed\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--cloaked") {
        return cloaked_enemy_probe(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2400);
    }
    if (argc > 1 && (std::string_view(argv[1]) == "--keldabe" || std::string_view(argv[1]) == "--keldabe-autofire")) {
        // WAB-36: --keldabe-autofire keeps the stock plans; the Keldabe's autofire alone must lock its target.
        const bool autofire = std::string_view(argv[1]) == "--keldabe-autofire";
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1800;
        auto content = load(*root, false, true, {"Kedalbe_Battleship"});
        if (!content) return 1;
        // WLS-04: stock Lua makes no shield-leech call. A fixture freestore exercises the targeted API.
        // Start within the nested beam range: ordinary weapons can otherwise stop the approach farther away.
        // The conservative readiness witness needs a shielded ship target whose centre
        // is fully in range. The nearby starbase has a larger footprint and can admit
        // a lock before that conservative bound, so isolate the autofire fixture's ships.
        if (autofire) {
            const auto keldabe = skirmish::type_id("Kedalbe_Battleship");
            const auto corvette = skirmish::type_id("Corellian_Corvette");
            const auto retained = [&](const auto& unit) { return unit.type_id == keldabe || unit.type_id == corvette; };
            std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
            std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
            content->start.setup.squadrons.clear();
            content->start.setup.free_garrisons.clear();
            content->victory = {};
        }
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(), [](const auto& unit) {
            return unit.owner == 1 && unit.type_id == skirmish::type_id("Corellian_Corvette");
        });
        expect(enemy != content->start.setup.units.end(), "WLS-04: the installed battle contains a shielded Corvette");
        if (enemy == content->start.setup.units.end()) return 1;
        auto position = enemy->position;
        position.x = eawr::sim::math::Fixed::from_raw(position.x.raw() + 800 * eawr::sim::math::Fixed::scale);
        for (auto& unit : content->start.setup.units)
            if (unit.type_id == skirmish::type_id("Kedalbe_Battleship")) unit.position = position;
        for (auto& unit : content->start.units)
            if (unit.state.type_id == skirmish::type_id("Kedalbe_Battleship")) unit.state.position = position;
        if (!autofire) {
            content->ai.xml.clear();
            content->ai.freestore_module = "Data/Scripts/Test/ShieldLeechProbe.lua";
            content->modules[content->ai.freestore_module] =
                "function Base_Definitions() ServiceRate = 1; UnitServiceRate = 1 end\n"
                "function main() FogOfWar.Reveal_All(PlayerObject) end\n"
                "function On_Unit_Service(object)\n"
                "  enemy = Find_Nearest(object, 'Corellian_Corvette', PlayerObject, false)\n"
                "  if not enemy then return end\n"
                "  if object.Has_Ability('LEECH_SHIELDS') and object.Is_Ability_Ready('LEECH_SHIELDS')\n"
                "    and not object.Is_Ability_Active('LEECH_SHIELDS') and object.Get_Distance(enemy) < 1800\n"
                "    and enemy.Get_Shield() > 0 then\n"
                "    object.Activate_Ability('LEECH_SHIELDS', enemy)\n"
                "  end\n"
                "  if not object.Has_Attack_Target() then object.Attack_Target(enemy) end\n"
                "end\n";
        }
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks, {}, false, true);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const auto& first = runs.front();
        for (std::size_t i = 1; i < runs.size(); ++i) {
            expect(runs[i].hashes == first.hashes, "WLS-04: Keldabe battle hashes match on 1/2/4/8 workers");
            expect(runs[i].commands == first.commands, "WLS-04: targeted Lua commands match on 1/2/4/8 workers");
        }
        const auto requests = std::count_if(first.record.commands.begin(), first.record.commands.end(), [](const auto& command) {
            const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload);
            return ability && ability->ability == tactical::AbilityKind::leech_shields && ability->target != 0;
        });
        if (autofire) {
            expect(requests == 0, "WAB-36: the stock plans make no shield-leech request");
            expect(first.shield_leech_frames > 0, "WAB-36: the AI Keldabe autofires its shield leech under the stock plans");
            expect(first.first_leech_lock && first.first_leech_ready_target && *first.first_leech_lock <= *first.first_leech_ready_target,
                "WAB-36: installed leech locks no later than its first ready in-range attack target");
            if (first.first_leech_lock && first.first_leech_ready_target) std::cout << "leech first lock tick " << *first.first_leech_lock
                << ", conservative ready target tick " << *first.first_leech_ready_target << '\n';
        } else {
            expect(requests > 0, "WLS-04: targeted Lua activation reaches the Keldabe world");
            expect(first.shield_leech_frames > 0, "WLS-04: the installed Keldabe accepts and services a targeted lock");
        }
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WLS-04: Keldabe battle replays without scripts");
        std::cout << (autofire ? "Keldabe stock-plan battle: " : "Keldabe battle: ") << requests << " targeted shield-leech commands, " << first.shield_leech_frames
                  << " active lock frames, AI hits " << first.ai_hits << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--concentrate") {
        // WAB-36, WHE-24 (legacy #2997): no stock skirmish script asks for CONCENTRATE_FIRE. The AI
        // Home One's autofire alone switches it on under the stock plans, and its escorts, recruited
        // at activation, fire on the concentrated target.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1800;
        auto content = load(*root, true, false, {}, false, std::nullopt, {},
            {"Home_One", "Nebulon_B_Frigate", "Corellian_Corvette"});
        if (!content) return 1;
        const auto home_one = skirmish::type_id("Home_One");
        const std::set<tactical::TypeId> kept{home_one, skirmish::type_id("Nebulon_B_Frigate"),
            skirmish::type_id("Corellian_Corvette"), skirmish::type_id("Tartan_Patrol_Cruiser"),
            skirmish::type_id("Acclamator_Assault_Ship")};
        // Only the two fleets: stations, fighters and map objects would be other attack targets.
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !kept.contains(unit.type_id); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !kept.contains(unit.state.type_id); });
        content->start.setup.squadrons.clear();
        content->start.setup.free_garrisons.clear();
        content->victory = {};
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(), [](const auto& unit) {
            return unit.owner == 1 && unit.type_id == skirmish::type_id("Tartan_Patrol_Cruiser");
        });
        expect(enemy != content->start.setup.units.end(), "WAB-36: the installed battle contains an Empire Tartan");
        if (enemy == content->start.setup.units.end()) return 1;
        // The Rebel group starts within weapon reach of the Empire group, Home One first in line.
        const auto anchor = enemy->position;
        std::int64_t offset = 800;
        std::map<eawr::sim::EntityId, eawr::sim::math::Vec3> placed;
        for (auto& unit : content->start.setup.units) {
            if (unit.owner != 2) continue;
            if (unit.type_id != home_one) offset += 200;
            const std::int64_t distance = unit.type_id == home_one ? 800 : offset;
            auto position = anchor;
            position.x = eawr::sim::math::Fixed::from_raw(anchor.x.raw() + distance * eawr::sim::math::Fixed::scale);
            unit.position = position;
            placed.emplace(unit.entity_id, position);
        }
        for (auto& unit : content->start.units)
            if (const auto found = placed.find(unit.state.entity_id); found != placed.end()) unit.state.position = found->second;
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const auto& first = runs.front();
        for (std::size_t i = 1; i < runs.size(); ++i) {
            expect(runs[i].hashes == first.hashes, "WAB-36: Home One battle hashes match on 1/2/4/8 workers");
            expect(runs[i].commands == first.commands, "WAB-36: Home One battle commands match on 1/2/4/8 workers");
        }
        const auto requests = std::count_if(first.record.commands.begin(), first.record.commands.end(), [](const auto& command) {
            const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload);
            return ability && ability->ability == tactical::AbilityKind::concentrate_fire;
        });
        expect(requests == 0, "WAB-36: the stock plans make no concentrate-fire request");
        expect(first.concentrate_frames > 0, "WAB-36: the AI Home One autofires CONCENTRATE_FIRE under the stock plans");
        expect(first.recruit_hits > 0, "WHE-24: the recruited escorts fire on the concentrated target");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-36: Home One battle replays without scripts");
        std::cout << "Home One stock-plan battle: " << requests << " concentrate commands, " << first.concentrate_frames
                  << " concentrate frames, " << first.recruit_hits << " hits on the target by other shooters\n";
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--tractor") {
        // WAB-36 (legacy #2997): no stock skirmish script asks for TRACTOR_BEAM. The AI Star Destroyer's
        // autofire alone locks it onto its attack target under the stock plans.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1800;
        auto content = load(*root, false, false, {}, false, std::nullopt, {}, {"Star_Destroyer"});
        if (!content) return 1;
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(), [](const auto& unit) {
            return unit.owner == 1 && unit.type_id == skirmish::type_id("Corellian_Corvette");
        });
        expect(enemy != content->start.setup.units.end(), "WAB-36: the installed battle contains a Rebel Corvette");
        if (enemy == content->start.setup.units.end()) return 1;
        // Start inside the tractor's 1000 range of a corvette, as the Keldabe battle does for its beam.
        auto position = enemy->position;
        position.x = eawr::sim::math::Fixed::from_raw(position.x.raw() + 800 * eawr::sim::math::Fixed::scale);
        const auto destroyer = skirmish::type_id("Star_Destroyer");
        const auto corvette = skirmish::type_id("Corellian_Corvette");
        // Only the destroyer and the corvettes: the Rebel station and fighters are nearer attack targets
        // that the tractor's category filter (Transport, Corvette, Frigate) refuses, as in the IG-2000 battle.
        const auto retained = [&](const auto& unit) { return unit.type_id == destroyer || unit.type_id == corvette; };
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        content->start.setup.squadrons.clear();
        content->start.setup.free_garrisons.clear();
        content->victory = {};
        for (auto& unit : content->start.setup.units) if (unit.type_id == destroyer) unit.position = position;
        for (auto& unit : content->start.units) if (unit.state.type_id == destroyer) unit.state.position = position;
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks, {}, false, true);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const auto& first = runs.front();
        for (std::size_t i = 1; i < runs.size(); ++i) {
            expect(runs[i].hashes == first.hashes, "WAB-36: Star Destroyer battle hashes match on 1/2/4/8 workers");
            expect(runs[i].commands == first.commands, "WAB-36: Star Destroyer battle commands match on 1/2/4/8 workers");
        }
        const auto requests = std::count_if(first.record.commands.begin(), first.record.commands.end(), [](const auto& command) {
            const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload);
            return ability && ability->ability == tactical::AbilityKind::tractor_beam;
        });
        expect(requests == 0, "WAB-36: the stock plans make no tractor-beam request");
        expect(first.tractor_frames > 0, "WAB-36: the AI Star Destroyer autofires its tractor beam under the stock plans");
        expect(first.first_tractor_lock && first.first_tractor_ready_target && *first.first_tractor_lock <= *first.first_tractor_ready_target,
            "WAB-36: installed tractor locks no later than its first ready in-range attack target");
        if (first.first_tractor_lock && first.first_tractor_ready_target) std::cout << "tractor first lock tick " << *first.first_tractor_lock
            << ", conservative ready target tick " << *first.first_tractor_ready_target << '\n';
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-36: Star Destroyer battle replays without scripts");
        std::cout << "Star Destroyer stock-plan battle: " << requests << " tractor commands, " << first.tractor_frames
                  << " tractor lock frames, AI hits " << first.ai_hits << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--ig2000") {
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 450;
        auto content = load(*root, false, true, {"IG-2000"});
        if (!content) return 1;
        // Exercise the authored object script in a close engagement with the full production
        // AI host and installed unit/weapon data.
        const auto hero = skirmish::type_id("IG-2000"), target = skirmish::type_id("Nebulon_B_Frigate");
        const auto ai_type = [&](const eawr::sim::tactical::TypeId id) -> const eawr::script::foc::AiType* {
            const auto& types = content->ai.content.types;
            const auto found = std::find_if(types.begin(), types.end(), [&](const auto& type) { return type.type_id == id; });
            return found == types.end() ? nullptr : &*found;
        };
        const auto* hero_type = ai_type(hero);
        const auto* target_type = ai_type(target);
        expect(hero_type && hero_type->hero && target_type && !target_type->hero,
            "WTA-36: the AI's Is_Hero answers true for the installed IG-2000 and false for a Nebulon-B");
        const auto retained = [&](const auto& unit) { return unit.type_id == hero || unit.type_id == target; };
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        content->start.setup.squadrons.clear();
        // FL-14: the removed stations' registered garrisons do not belong to this fixture.
        content->start.setup.free_garrisons.clear();
        for (auto& unit : content->start.setup.units)
            unit.position = {eawr::sim::math::Fixed::from_integer(unit.type_id == hero ? 0 : 200).value(), {}, {}};
        for (auto& unit : content->start.units)
            unit.state.position = {eawr::sim::math::Fixed::from_integer(unit.state.type_id == hero ? 0 : 200).value(), {}, {}};
        content->victory = {};
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WHE-67: IG-2000 battle hashes match on 1/2/4/8 workers");
            expect(runs[index].commands == first.commands, "WHE-67: IG-2000 commands match on 1/2/4/8 workers");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands)
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload))
                requests += ability->ability == tactical::AbilityKind::corrupt_systems && ability->position ? 1 : 0;
        expect(requests > 0 && !first.corrupted.empty(), "WHE-67: the production object script casts and corrupts the enemy");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WHE-67: the IG-2000 record replays without AI");
        std::cout << "IG-2000 battle: " << requests << " cast requests, " << first.corrupted.size() << " corrupted enemies\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--interceptor") {
        // WAB-80..83: the Underworld AI switches FULL_SALVO on for its Interceptor IV frigates; the world, the
        // script commands and a headless replay of the record agree on 1/2/4/8 workers.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, true, {"Interceptor4_Frigate", "Interceptor4_Frigate"});
        if (!content) return 1;
        // WAB-83: isolate the ability call from station-attachment approach geometry (OR-07).
        // A ship can hit an attached station weapon while staying beyond the stand-in's
        // body-distance trigger. Stage only the Interceptors and two corvettes inside that
        // trigger, preserving their relative formation, before the first Lua service.
        const auto interceptor = skirmish::type_id("Interceptor4_Frigate");
        const auto corvette = skirmish::type_id("Corellian_Corvette");
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 1 && unit.type_id == corvette; });
        const auto holder = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 2 && unit.type_id == interceptor; });
        expect(enemy != content->start.setup.units.end() && holder != content->start.setup.units.end(),
            "WAB-83: the controlled encounter contains the installed ability holder and target");
        if (enemy == content->start.setup.units.end() || holder == content->start.setup.units.end()) return 1;
        const auto dx = enemy->position.x.raw() + 800 * eawr::sim::math::Fixed::scale - holder->position.x.raw();
        const auto dy = enemy->position.y.raw() - holder->position.y.raw();
        const auto dz = enemy->position.z.raw() - holder->position.z.raw();
        const auto retained = [&](const auto& unit) { return unit.type_id == interceptor || unit.type_id == corvette; };
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        content->start.setup.squadrons.clear();
        content->start.setup.free_garrisons.clear();
        content->victory = {};
        const auto place = [&](tactical::UnitState& unit) {
            if (unit.type_id != interceptor) return;
            unit.position.x = eawr::sim::math::Fixed::from_raw(unit.position.x.raw() + dx);
            unit.position.y = eawr::sim::math::Fixed::from_raw(unit.position.y.raw() + dy);
            unit.position.z = eawr::sim::math::Fixed::from_raw(unit.position.z.raw() + dz);
        };
        for (auto& unit : content->start.setup.units) place(unit);
        for (auto& unit : content->start.units) place(unit.state);
        // The stand-in makes the stock handler's Has_Ability / readiness / active / activation calls.
        content->ai.xml.clear();
        content->ai.freestore_module = "Data/Scripts/Test/FullSalvoProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, PlayerObject, false)\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"FULL_SALVO\") and object.Is_Ability_Ready(\"FULL_SALVO\")\n"
            "      and not object.Is_Ability_Active(\"FULL_SALVO\") and object.Get_Distance(enemy) < 1200 then\n"
            "    object.Activate_Ability(\"FULL_SALVO\", true)\n"
            "  end\n"
            "  if not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WAB-80: Interceptor battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WAB-83: Interceptor script commands match worker count 1");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::full_salvo
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        std::cout << "Interceptor battle: " << ticks << " ticks, " << requests << " FULL_SALVO request(s), AI hits "
                  << first.ai_hits << "; ready holder frames " << first.salvo_ready_ticks
                  << ", active holder frames " << first.salvo_active_ticks << ", closest player gap while ready " << first.salvo_min_gap << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(requests > 0, "WAB-83: the Lua Activate_Ability reaches the world as a FULL_SALVO activation");
        expect(first.salvo_active_ticks > 0, "WAB-80: the installed FULL_SALVO request activates the world ability");
        expect(first.ai_hits > 0, "WAB-81: the Interceptors' weapons hit the player");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-80: the Interceptor record replays without the script");
        if (failures != 0) return 1;
        std::cout << "Interceptor full-salvo battle passed\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--awing") {
        // WLU-10: the Underworld AI's A-wings (the A-Wing variant keeps LURE). The stock escort plan's
        // Try_Ability(.., "LURE") is counted from a stock run; a stand-in freestore then makes the Lua calls
        // that switch LURE on once an X-wing (a listed target type) is inside the radius. The world, the
        // script commands and a headless replay of the record agree on 1/2/4/8 workers.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, true, {"Underworld_A_Wing_Squadron", "Underworld_A_Wing_Squadron"});
        if (!content) return 1;
        const auto stock_run = run(*content, 1, ticks);
        if (!stock_run) return 1;
        std::cout << "A-wing stock battle: " << ticks << " ticks, " << stock_run->plan_lure << " LURE script request(s), "
                  << stock_run->lure_frames << " luring craft frame(s)\n";
        content->ai.xml.clear();
        content->ai.freestore_module = "Data/Scripts/Test/LureProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, \"Rebel_X-Wing_Squadron\", PlayerObject, false)\n"
            "  if not enemy then enemy = Find_Nearest(object, PlayerObject, false) end\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"LURE\") and object.Is_Ability_Ready(\"LURE\")\n"
            "      and object.Get_Distance(enemy) < 350 then\n"
            "    object.Activate_Ability(\"LURE\", true)\n"
            "  elseif not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WLU-10: A-wing battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WLU-10: A-wing script commands match worker count 1");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::lure
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        std::cout << "A-wing battle: " << ticks << " ticks, " << requests << " LURE request(s), " << first.lure_frames
                  << " luring craft frame(s), AI hits " << first.ai_hits << '\n';
        for (const auto& [name, count] : first.orders_by_type) std::cout << "  accepted orders " << name << ": " << count << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(requests > 0, "WLU-10: the Lua Activate_Ability reaches the world as a LURE activation");
        expect(first.lure_frames > 0, "WLU-02: an AI activation switches the craft's LURE on");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WLU-10: the A-wing record replays without the script");
        if (failures != 0) return 1;
        std::cout << "A-wing lure battle passed\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--vengeance") {
        // WSD-01..07: the Underworld AI's Vengeance frigates; a stand-in freestore makes the Lua calls that switch
        // SELF_DESTRUCT on once an enemy is inside the blast (the stock library never names the ability, WSD-09).
        // The world, the script commands and a headless replay of the record agree on 1/2/4/8 workers; the
        // blast leaves the Vengeance dead and hurts the player.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5400;
        auto content = load(*root, false, true, {"Vengeance_Frigate", "Vengeance_Frigate"});
        if (!content) return 1;
        content->ai.xml.clear();
        content->ai.freestore_module = "Data/Scripts/Test/SelfDestructProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, \"Corellian_Corvette\", PlayerObject, false)\n"
            "  if not enemy then enemy = Find_Nearest(object, PlayerObject, false) end\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"SELF_DESTRUCT\") and object.Is_Ability_Ready(\"SELF_DESTRUCT\")\n"
            "      and not object.Is_Ability_Active(\"SELF_DESTRUCT\") then\n"
            "    if object.Get_Distance(enemy) < 700 then\n"
            "      object.Activate_Ability(\"SELF_DESTRUCT\", true)\n"
            "    else\n"
            "      object.Move_To(enemy)\n"
            "    end\n"
            "  elseif not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WSD-03: Vengeance battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WSD-09: Vengeance script commands match worker count 1");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::self_destruct
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        std::size_t destroyed_ai = 0;
        for (const auto& event : first.events) {
            if (event.find(" unit_destroyed ") == std::string::npos) continue;
            const auto id = std::stoull(event.substr(event.rfind(' ') + 1));
            for (const auto& unit : content->start.units) destroyed_ai += unit.state.entity_id == id && unit.state.owner == 2 ? 1 : 0;
        }
        std::cout << "Vengeance battle: " << ticks << " ticks, " << requests << " SELF_DESTRUCT request(s), "
                  << first.blast_objects << " blast tick(s), player hull lost to blasts " << first.blast_hull_drop
                  << ", Underworld units destroyed " << destroyed_ai << '\n';
        std::cout << "  holder alive ticks " << first.sd_alive_first << ".." << first.sd_alive_last << ", ready ticks "
                  << first.sd_ready_ticks << ", closest approach " << first.sd_min_gap << ", " << first.commands.size()
                  << " routed command(s)\n";
        for (const auto& [name, count] : first.orders_by_type) std::cout << "  accepted orders " << name << ": " << count << '\n';
        std::size_t shown = 0;
        for (const auto& event : first.events) {
            if (event.find(" order_rejected ") == std::string::npos && event.find(" ability_") == std::string::npos) continue;
            if (shown++ < 6) std::cout << "  event " << event << '\n';
        }
        for (const auto& command : first.commands) {
            if (command.rfind("ability", 0) == 0 || command.find("dropped") != std::string::npos) std::cout << "  command " << command << '\n';
        }
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(requests > 0, "WSD-09: the Lua Activate_Ability reaches the world as a SELF_DESTRUCT activation");
        expect(first.blast_objects > 0 && destroyed_ai > 0, "WSD-05: the countdown ends the Vengeance with its blast object");
        expect(first.blast_hull_drop >= 400, "WSD-06: the blast hurts the player's ships in reach");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WSD-03: the Vengeance record replays without the script");
        if (failures != 0) return 1;
        std::cout << "Vengeance self-destruct battle passed\n";
        return 0;
    }
    const bool source_free_trace = argc > 1 && std::string_view(argv[1]) == "--combat-trace-source-free";
    const bool ordinary_plans_trace = argc > 1 && std::string_view(argv[1]) == "--combat-trace-ordinary-plans";
    const bool economy_disabled_trace = argc > 1 && std::string_view(argv[1]) == "--combat-engagement-trace";
    const bool stock_engagement_trace = argc > 1 && std::string_view(argv[1]) == "--stock-engagement-trace";
    const bool engagement_trace = economy_disabled_trace || stock_engagement_trace;
    const bool combat_tracing = source_free_trace || ordinary_plans_trace || engagement_trace
        || (argc > 1 && std::string_view(argv[1]) == "--combat-trace");
    if (combat_tracing) {
        const auto ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 600;
        const std::uint64_t battle_seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10)
            : engagement_trace ? 68 : skirmish::m2_fixture().seed;
        auto content = engagement_trace ? combat_engagement_witness(*root, battle_seed, economy_disabled_trace)
            : load(*root, false, false, {}, false, battle_seed);
        if (!content) return 1;
        if (ordinary_plans_trace) std::erase_if(content->modules, [](const auto& module) {
            return module.first.ends_with("/groundtospacedamage.lua") || module.first.ends_with("/groundtospacedisable.lua");
        });
        if (source_free_trace) {
            std::set<tactical::TypeId> sources;
            for (const auto& type : content->ai.content.types) if (type.space_special_weapon) sources.insert(type.type_id);
            std::erase_if(content->start.setup.units, [&](const auto& unit) { return sources.contains(unit.type_id); });
            std::erase_if(content->start.units, [&](const auto& unit) { return sources.contains(unit.state.type_id); });
        }
        const auto result = run(*content, 1, ticks, {}, engagement_trace);
        if (!result) return 1;
        std::ofstream output(argc > 3 ? argv[3] : "combat-trace.txt");
        for (const auto& command : result->commands) output << command << '\n';
        for (const auto& event : result->journal.plans)
            output << event.tick << ',' << event.player << ',' << event.plan << ',' << event.goal << ','
                   << event.target << ',' << event.event << ',' << event.detail << '\n';
        output << "AI hits " << result->ai_hits << '\n';
        output << "AI first hit " << result->first_ai_hit << '\n';
        if (engagement_trace) {
            output << "All admitted AI hits " << result->stock_hits << '\n';
            output << "All admitted AI first hit " << result->first_stock_hit << '\n';
            output << "AI hits on player " << result->player_hits << '\n';
            output << "AI first hit on player " << result->first_player_hit << '\n';
            for (const auto& event : result->ai_rejections)
                output << event.tick << ",order_rejected," << event.unit << ','
                    << tactical::to_string(event.order) << ',' << tactical::to_string(event.reason) << '\n';
            std::ofstream hits(std::string(argc > 3 ? argv[3] : "combat-trace.txt") + ".hits.csv");
            hits << "tick,shooter,target,x,y,z\n" << result->hits_csv;
            expect(static_cast<bool>(hits), "the engagement hit trace is written");
        }
        expect(static_cast<bool>(output), "the combat trace is written");
        return failures != 0 ? 1 : 0;
    }
    const bool tracing = argc > 1 && std::string_view(argv[1]) == "--ability-trace";
    const std::uint64_t ticks = tracing ? (argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600)
                                      : (argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 3600);
    auto content = load(*root, tracing);
    if (!content) return 1;
    if (tracing) return ability_trace(*content, ticks, argc > 3 ? argv[3] : "abilities.csv");

    progressive_random_probe(*root, *content);
    if (failures != 0) return 1;

    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks);
        if (!result) return 1;
        std::cout << workers << " worker(s): " << ticks << " ticks in " << result->seconds << " s, final "
                  << result->hashes.back() << '\n';
        runs.push_back(std::move(*result));
    }
    const Run& first = runs.front();
    if (argc > 2) {
        std::ofstream hits(argv[2], std::ios::binary);
        hits << "tick,shooter,target,x,y,z\n" << first.hits_csv;
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == first.hashes, "every tick's combined hash matches worker count 1");
        expect(runs[index].commands == first.commands, "the script commands match worker count 1");
        expect(runs[index].events == first.events, "the world events match worker count 1");
        expect(runs[index].diagnostics == first.diagnostics, "the script diagnostics match worker count 1");
    }

    std::size_t submitted = 0;
    for (const auto& command : first.commands) submitted += command.find(" key ") != std::string::npos ? 1 : 0;
    std::cout << "script commands: " << first.commands.size() << " (" << submitted << " submitted), AI hits: "
              << first.ai_hits << ", world events: " << first.events.size() << '\n';
    std::cout << "AI orders by unit type:";
    for (const auto& [type, count] : first.orders_by_type) std::cout << ' ' << type << '=' << count;
    std::cout << '\n';
    std::map<std::string, std::uint64_t> plan_events;
    for (const auto& event : first.journal.plans) ++plan_events[event.event];
    std::cout << "AI plan activity:";
    for (const auto& [event, count] : plan_events) std::cout << ' ' << event << '=' << count;
    std::cout << "; artillery units seen=" << first.artillery_seen.size() << '\n';
    std::cout << "Lua load: max " << first.max_instructions << " instructions in a tick, mean "
              << (first.total_instructions / static_cast<std::int64_t>(ticks)) << " (budget "
              << auth::Quotas{}.instructions_per_service << " per instance)\n";
    for (const auto& diagnostic : first.diagnostics) std::cout << "diagnostic: " << diagnostic << '\n';
    expect(submitted > 0, "the AI orders its units");
    // #452: the AI's Attack_Move and Guard_Target reach the session as the real orders, never dropped.
    std::map<std::string, std::size_t> verbs;
    for (const auto& command : first.commands) {
        const std::string verb = command.substr(0, command.find(' '));
        ++verbs[verb];
        if (verb == foc::verb_attack_move || verb == foc::verb_guard) {
            expect(command.find(" key ") != std::string::npos, "the order is submitted: " + command);
        }
    }
    for (const auto& [verb, count] : verbs) std::cout << "  " << verb << ": " << count << '\n';
    // GS-04/PL-01 and WSW-01/38: source identities and the stock plan inventory
    // affect keyed choices. Seed 67 remains the deterministic record witness;
    // nearby seeds below exercise combat progression.
    if (first.ai_hits == 0) {
        std::cout << "No-hit AI routing:\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(40, first.commands.size()); ++index)
            std::cout << first.commands[index] << '\n';
    }
    std::cout << "Stock special-weapon observation: completed use purchases " << first.special_use_purchases
        << ", travelling special projectiles " << first.special_projectiles
        << " (stock skirmish economy)\n";
    // #518: the squadrons the Empire station and the Acclamator launch (SK-23) are the AI's
    // squadrons like its tick-zero ones: it orders their team containers, never their craft (FH-13).
    const auto ordered = [&first](const char* type) {
        const auto found = first.orders_by_type.find(type);
        return found == first.orders_by_type.end() ? std::uint64_t{0} : found->second;
    };
    expect(ordered("TIE_Fighter_Squadron") + ordered("TIE_Bomber_Squadron") > 0,
           "#518: the AI orders its launched squadrons as squadrons");
    expect(ordered("TIE_Fighter") + ordered("TIE_Bomber") + ordered("TIE_Interceptor") == 0,
           "#518: the AI never orders a squadron's craft on its own");
    expect(first.max_instructions < auth::Quotas{}.instructions_per_service / 4, "the AI's Lua load stays below a quarter of the budget");

    // The record replays without scripts: the world hashes come back from the commands alone.
    const auto replay_began = std::chrono::steady_clock::now();
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    const double replay_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_began).count();
    expect(replayed && replayed.value() == first.world_hashes, "the headless replay of the record reproduces the world");
    std::cout << "world alone (headless replay, 1 worker): " << replay_seconds << " s; with the AI: " << first.seconds
              << " s\n";

    // The live session hosts the same AI on its simulation thread (driven pacing, 4 workers).
    eawr::platform::LiveSession::Options options;
    options.workers = 4;
    options.pacing = eawr::platform::LiveSession::Pacing::driven;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [&content](tactical::TacticalSession world) {
        return foc::create_session(std::move(world), content->ai, content->modules);
    };
    options.scripts = scripts;
    auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, options, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(static_cast<bool>(live), "the live session starts with the AI");
    if (live) {
        live.value()->advance_to(ticks);
        expect(live.value()->wait_for(ticks, std::chrono::minutes(10)), "the live session reaches the last tick");
        live.value()->stop();
        expect(live.value()->tick_hashes() == first.world_hashes, "the live session's hashes are the AI run's world hashes");
        const auto report = live.value()->script_report();
        expect(report.ticks == ticks && report.max_instructions == first.max_instructions,
               "the live session reports the same Lua load");
    }

    std::optional<Content> combat_content;
    std::optional<Run> combat_first;
    std::uint64_t combat_window = ticks;
    for (const std::uint64_t seed : {66U, 68U}) {
        // This is a project progression window, not a sourced FoC deadline.
        // WDF-05 map-relative grids moved seed 66's first hit from 3487 to 5244.
        // WTA-01/02's 7357 first-hit measurement was at f728e45d. Main merged before
        // 0038e7de, including the refused-attack hold (WTA-19, legacy #3109): bombers now
        // wait instead of failing and re-proposing, moving seed 66's first hit to 10850.
        // The live event-counter correction leaves the 21600-tick command/plan trace
        // byte-identical at 0038e7de and 05d84570 (365 hits), SHA-256:
        // b60f61f514f255639633aaacd951b6e14464637f70dd74d1a2b9545c3883ca68.
        // Keep the WDF-05 margin formula: 10850 + 1970 = 12820, rounded up to 12900.
        // Explicit horizons remain available.
        // FM-20/WSQ-58's sourced hold removes the earlier incidental idle drift.
        // Keep the stock witness's 7200-tick coverage bound, with its full rules:
        // an empty economy cannot exercise the production purchase/construction path.
        const std::uint64_t witness_ticks = argc == 1 ? (seed == 66 ? 12900 : 7200) : ticks;
        auto witness = seed == 68 ? combat_engagement_witness(*root, seed)
                                 : load(*root, false, false, {}, false, seed);
        if (!witness) return 1;
        if (seed == 68) {
            std::cout << "combat-engagement witness (stock economy): seed 68 window " << witness_ticks << '\n';
            expect(witness->economy.player(2) != nullptr && !witness->economy.menus.empty(),
                "the combat/pause witness retains the stock ledger and construction menus");
        }
        std::optional<Run> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*witness, workers, witness_ticks, {}, true);
            if (!result) return 1;
            std::cout << "combat seed " << seed << " workers " << workers << " AI hits " << result->ai_hits
                      << " first hit " << result->first_ai_hit << " (authored roster)\n";
            if (seed == 68) std::cout << "combat seed 68 all-admitted workers " << workers
                << " AI hits " << result->stock_hits << " first hit " << result->first_stock_hit << '\n';
            std::cout << "combat seed " << seed << " player-contact workers " << workers
                << " AI hits " << result->player_hits << " first hit " << result->first_player_hit << '\n';
            check_combat_engagement(*result, seed, witness_ticks);
            expect(result->max_instructions < auth::Quotas{}.instructions_per_service / 4,
                "the combat witness stays within the Lua budget");
            if (!baseline) baseline = std::move(result);
            else {
                expect(result->hashes == baseline->hashes && result->commands == baseline->commands
                    && result->events == baseline->events && result->diagnostics == baseline->diagnostics
                    && result->player_hits == baseline->player_hits && result->first_player_hit == baseline->first_player_hit,
                    "combat witness hashes, commands, events, diagnostics and player contact match on 1/2/4/8 workers");
            }
        }
        const auto plain = eawr::platform::headless_tick_hashes(baseline->record, witness->content.sensors,
            witness->content.durability, witness->content.motion, witness->content.combat, witness->victory,
            witness->content.fog, witness->content.abilities, witness->economy, witness->content.map_box);
        expect(plain && plain.value() == baseline->world_hashes, "combat witness replays without scripts");
        if (seed == 68) {
            combat_content = std::move(witness);
            combat_first = std::move(baseline);
            combat_window = witness_ticks;
        }
    }
    auto combat_scripts = std::make_shared<eawr::platform::LiveScripts>();
    combat_scripts->wrap = [&combat_content](tactical::TacticalSession world) {
        return foc::create_session(std::move(world), combat_content->ai, combat_content->modules);
    };

    // #459 with the AI: a real-time battle paused mid-attack and resumed on fast forward
    // (TM-07, TP-02). The freestore services inside the tick, on sim seconds (FH-11), so it
    // pauses with the sim, never runs while paused, and services per tick at any rate; its
    // commands keep their next-tick keys. The replay equals an unpaused run's.
    const std::uint64_t pause_tick = combat_first->first_player_hit + 30;
    const std::uint64_t halt = pause_tick + 150;
    expect(combat_first->first_player_hit > 0 && halt <= combat_window, "the AI attacks early enough to pause mid-attack");
    const auto with_ai = [&](eawr::platform::LiveSession::Pacing pacing, std::uint32_t rate) {
        eawr::platform::LiveSession::Options live_options = options;
        live_options.scripts = combat_scripts;
        live_options.pacing = pacing;
        live_options.target_rate = rate;
        return eawr::platform::LiveSession::start(combat_content->start.setup, combat_content->content.sensors,
            combat_content->content.durability, combat_content->content.motion, combat_content->content.combat, live_options,
            combat_content->victory, combat_content->content.fog, combat_content->content.abilities,
            combat_content->economy, combat_content->content.map_box);
    };
    auto paused = with_ai(eawr::platform::LiveSession::Pacing::real_time, 1000);
    auto unpaused = with_ai(eawr::platform::LiveSession::Pacing::driven, 30);
    expect(paused && unpaused, "the paused and unpaused AI sessions start");
    if (combat_first->first_player_hit > 0 && halt <= combat_window && paused && unpaused) {
        using namespace std::chrono_literals;
        auto& session = *paused.value();
        session.halt_at(halt);
        // The approach at the highest rate, then fast forward (TM-06) into the attack.
        expect(session.wait_for(combat_first->first_player_hit > 60 ? combat_first->first_player_hit - 60 : 0, std::chrono::minutes(5)),
               "the paused run reaches the approach");
        session.set_target_rate(120);
        expect(session.wait_for(pause_tick, std::chrono::minutes(5)), "the paused run reaches the attack");
        session.set_paused(true);
        std::this_thread::sleep_for(100ms);
        const std::uint64_t paused_at = session.completed_tick();
        const auto services = session.script_report().ticks;
        const auto frame = session.frame().latest;
        expect(paused_at >= pause_tick && paused_at < halt, "the battle pauses mid-attack");
        expect(services == paused_at, "the AI serviced every tick up to the pause");
        std::this_thread::sleep_for(500ms);
        expect(session.completed_tick() == paused_at, "no tick runs while the AI battle is paused");
        expect(session.script_report().ticks == services, "the AI never services while paused");
        expect(session.frame().latest == frame, "the paused battle publishes no new frame");
        session.set_paused(false);
        expect(session.wait_for(halt, std::chrono::minutes(5)), "the resumed run reaches the halt tick");
        expect(!session.wait_for(halt + 1, 300ms), "the resumed run stops at the halt tick");
        session.stop();

        unpaused.value()->advance_to(halt);
        expect(unpaused.value()->wait_for(halt, std::chrono::minutes(5)), "the unpaused run reaches the halt tick");
        unpaused.value()->stop();

        expect(!session.failure() && !unpaused.value()->failure(), "neither AI run failed");
        const auto hashes = session.tick_hashes();
        expect(hashes.size() == halt && std::equal(hashes.begin(), hashes.end(), combat_first->world_hashes.begin()),
               "pausing mid-attack leaves every world hash unchanged");
        expect(hashes == unpaused.value()->tick_hashes(), "the paused and unpaused runs hash alike");
        expect(session.script_report().ticks == halt
                   && session.script_report().max_instructions == unpaused.value()->script_report().max_instructions
                   && session.script_report().total_instructions == unpaused.value()->script_report().total_instructions,
               "the AI serviced once per tick on fast forward, as unpaused");
        const auto replay = session.record();
        const auto plain_replay = unpaused.value()->record();
        const auto bytes = tactical::write_replay(replay);
        const auto plain_bytes = tactical::write_replay(plain_replay);
        expect(bytes && plain_bytes && bytes.value() == plain_bytes.value(),
               "the paused run's replay equals the unpaused run's");
        std::size_t around_pause = 0;
        for (const auto& command : replay.commands) {
            if (command.key.tick > paused_at && command.key.tick <= paused_at + 60) ++around_pause;
        }
        std::cout << "paused at tick " << paused_at << " (first AI hit on player " << combat_first->first_player_hit << "), halted at "
                  << halt << ", " << replay.commands.size() << " commands, " << around_pause
                  << " in the 60 ticks after the pause\n";
    }

    // Every selected plan also loads its definitions through the same production bindings.
    auto files = content->plan_modules;
    auto plans = foc::inspect_plans(content->ai, files, foc::selected_plans());
    expect(static_cast<bool>(plans), "the selected plans are inspected");
    if (plans) {
        expect(plans.value().size() == 25, "AI-30/WSW-37/38: 25 selected combat, economy and planetary weapon plans");
        for (const std::string name : {"groundtospacedamage", "groundtospacedisable"}) {
            expect(std::any_of(plans.value().begin(), plans.value().end(), [&](const auto& plan) {
                return plan.path.ends_with("/" + name + ".lua") && plan.loaded;
            }), "WSW-37/38: installed planetary plan loads: " + name);
        }
        for (const auto& plan : plans.value()) {
            std::cout << "plan " << plan.path << ": category " << (plan.category.empty() ? "(none)" : plan.category)
                      << (plan.loaded ? "" : " NOT LOADED") << '\n';
            for (const auto& diagnostic : plan.diagnostics) std::cout << "  " << diagnostic << '\n';
            expect(plan.loaded && !plan.category.empty(), "the plan " + plan.path + " loads and names its goal category");
        }
    }
    for (const auto& line : foc::unsupported_plan_calls()) std::cout << "unsupported: " << line << '\n';

    hutt_nearest_probe(*content);
    ability_probe(*content);
    vengeance_probe(*root);
    crusader_probe(*root);

    if (failures != 0) {
        std::cerr << failures << " FoC AI check(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
