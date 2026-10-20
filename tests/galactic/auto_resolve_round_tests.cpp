#include "../support/env.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"
#include "eawr/campaign/auto_resolve_round.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../../src/script/foc/host.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace eawr::campaign;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using eawr::units::GalacticType;
using eawr::units::GalacticTypeTable;
using eawr::sim::EntityId;
Fixed f(const std::int64_t value) { return Fixed::from_integer(value).value(); }
Fixed d(const std::string_view value) { return Fixed::from_decimal(value).value(); }
template <typename T> eawr::core::Result<T> ok(T value) { return eawr::core::Result<T>::success(std::move(value)); }
const std::vector<eawr::units::EnumValue> bits{{"Fighter", 1}, {"Bomber", 2}, {"Capital", 4}, {"Infantry", 8}, {"Super", 16}};
const std::vector<AutoResolveCategory> categories{{"Fighter", false}, {"Bomber", false}, {"Capital", false}, {"Infantry", true}};
GalacticType unit(std::string name, const std::int64_t power, std::string category = "Fighter") {
    GalacticType result; result.name = std::move(name); result.ai_combat_power = f(power);
    result.category_mask = {std::move(category)}; return result;
}
GalacticTypeTable types() {
    GalacticTypeTable result;
    result.types = {unit("Ten", 10), unit("Twenty", 20), unit("Hundred", 100), unit("Zero", 0),
        unit("Carrier", 100), unit("Base3", 90), unit("Base2", 60), unit("Base1", 30), unit("Building", 30, "Infantry"), unit("Super", 0, "Super")};
    result.types[4].starting_spawned_units[0] = {{"Ten", 2}, {"Twenty", 4}};
    return result;
}
AutoResolveCombatant object(const EntityId id, std::string name, const bool hero = false, const bool planet = false, const bool transport = false) {
    AutoResolveCombatant value; value.id = id; value.type = std::move(name);
    value.named_hero = hero; value.planet = planet; value.transport = transport; return value;
}
AutoResolveStrength strength(std::vector<AutoResolveCombatant> a = {object(1, "Ten")},
    std::vector<AutoResolveCombatant> b = {object(2, "Ten")}, const BattleLayer layer = BattleLayer::space) {
    AutoResolveStrength value; value.gathered.layer = layer; value.gathered.aggressor = 20;
    value.gathered.sides[0].owner = 10; value.gathered.sides[1].owner = 20;
    value.gathered.sides[0].combatants = std::move(a); value.gathered.sides[1].combatants = std::move(b);
    value.categories = categories;
    for (auto& side : value.sides) side.force = {f(0), f(100), f(100), f(0), f(0), f(0)};
    return value;
}
AutoResolveRoundRules rules() { return auto_resolve_round_rules({}).value(); }
AutoResolveRoundContext humans() {
    AutoResolveRoundContext value; value.players = {{{true, true, true}, {true, true, true}}}; return value;
}
AutoResolveAttackServices services(const Fixed contrast = f(1)) {
    AutoResolveAttackServices result;
    result.contrast = [contrast](const GalacticType&, const AutoResolveCategory&) { return ok(contrast); };
    result.terrain = [](const GalacticType&, std::uint32_t) { return ok(f(1)); };
    result.ground_attack = [](const GalacticType& type) { return type.name == "Building"; };
    result.contains_named_hero = [](const AutoResolveCombatant& combatant) { return ok(combatant.named_hero); };
    result.is_superweapon = [](const AutoResolveCombatant& combatant) { return ok(combatant.type == "Super"); };
    result.learning_type = [](const AutoResolveCombatant& combatant) { return ok(combatant.type); };
    result.post_transport_death_contains_hero = [](const AutoResolveCombatant&) { return ok(false); };
    return result;
}
AutoResolvePick first(std::vector<std::size_t>* counts = nullptr) {
    return [counts](const std::size_t count) { if (counts) counts->push_back(count); return ok(std::size_t{0}); };
}
const AutoResolveAllocation& allocation(const AutoResolveRoundOutcome& result, const EntityId id) {
    const auto found = std::find_if(result.allocations.begin(), result.allocations.end(), [&](const auto& value) { return value.id == id; });
    if (found == result.allocations.end()) throw std::runtime_error("missing allocation " + std::to_string(id));
    return *found;
}
void defaults() {
    const auto defaults = rules();
    expect(defaults.display_seconds == f(10) && defaults.winner_attrition == f(1) && defaults.loser_attrition == d("0.9")
        && defaults.retreat_loser_attrition == d("0.75") && defaults.allowance == d("0.33333")
        && defaults.transport_losses == d("0.333333") && defaults.tactical_multiplier == f(1), "WAR code defaults differ from stock");
    eawr::data::galactic::GalacticData data;
    data.constants = {{"AutoResolveWinnerAttrition", "0.75", d("0.75")}, {"AutoResolveDisplayTime", "0.01", d("0.01")}};
    const auto loaded = auto_resolve_round_rules(data);
    expect(loaded && loaded.value().winner_attrition == d("0.75") && loaded.value().display_seconds == d("0.01"), "authored constants win");
    data.constants[0].number.reset(); expect(!auto_resolve_round_rules(data), "malformed authored constant rejected");
    auto tactical_rules = defaults; tactical_rules.tactical_multiplier = d("0.5");
    expect(auto_resolve_survival_budget(f(100), tactical_rules, false).value() == f(100)
        && auto_resolve_survival_budget(f(100), tactical_rules, true).value() == f(50),
        "WAR-40 multiplier applies only to the mid-tactical survival budget kernel");
}
void attack_cases() {
    const auto table = types();
    auto value = strength({object(1, "Twenty")}, {object(2, "Zero")});
    value.sides[1].force = {f(0), f(20), f(10), f(10), f(0), f(0)};
    value.sides[0].category_bonuses[0] = f(1);
    const auto hit = auto_resolve_attacks(value, table, bits, humans(), services());
    expect(hit && hit.value()[1] == std::vector<Fixed>{f(0), f(-5), f(0), f(10), f(0), f(0)},
        "WAR-32/33/34 first tied category, doubled damage, unboosted spill 15");
    auto none = services(f(0));
    none.ground_attack = [](const GalacticType&) { return true; };
    const auto fallback = auto_resolve_attacks(value, table, bits, humans(), none);
    expect(fallback && fallback.value()[1][0] == f(-40) && fallback.value()[1][1] == f(20), "WAR-34 no category uses attacking type layer");
    value = strength({object(1, "Carrier")}, {object(2, "Zero")});
    const auto garrison = auto_resolve_attacks(value, table, bits, humans(), services());
    expect(garrison && garrison.value()[1][1] == f(90), "WAR-31 only first spawned type attacks once, no count or own power");
    auto sparse = table;
    sparse.types[4].starting_spawned_units[0] = {{"Zero", 3}, {"Twenty", 0}, {"Ten", 1}};
    const auto positive = auto_resolve_attacks(value, sparse, bits, humans(), services());
    expect(positive && positive.value()[1][1] == f(90), "first positive-power contribution with positive selected count attacks");
    value.gathered.sides[0].combatants[0].tactical = true;
    const auto tactical = auto_resolve_attacks(value, table, bits, humans(), services());
    expect(tactical && tactical.value()[1][1] == f(0), "tactical combatant attacks with own power without galactic garrison");
    value = strength({object(1, "Base3", false, true)}, {object(2, "Zero")}, BattleLayer::land);
    value.gathered.built_structures = {"Building", "Hundred"};
    value.sides[1].force = {f(100), f(0), f(0), f(0), f(0), f(100)};
    const auto land = auto_resolve_attacks(value, table, bits, humans(), services());
    expect(land && land.value()[1][0] == f(70), "WAR-31 land planet only first built structure attacks");
    value.gathered.sides[0].combatants[0].type.clear();
    const auto baseless = auto_resolve_attacks(value, table, bits, humans(), services());
    expect(baseless && baseless.value() == land.value(), "land planet without a base keeps its structure attack");
    value.gathered.bomber_type = "Twenty"; value.gathered.bombing_side = 10;
    value.sides[1].force = {f(100), f(0), f(10), f(0), f(0), f(90)};
    const auto bomb = auto_resolve_attacks(value, table, bits, humans(), services());
    expect(bomb && bomb.value()[1][2] == f(0) && bomb.value()[1][5] == f(60), "WAR-30 bombing precedes first structure");
    auto terrain_services = services();
    terrain_services.terrain = [](const GalacticType&, std::uint32_t index) { expect(index == 7, "terrain index forwarded"); return ok(f(2)); };
    value.gathered.bomber_type.clear(); value.gathered.terrain_index = 7;
    auto context = humans(); context.terrain_known = true;
    const auto terrain = auto_resolve_attacks(value, table, bits, context, terrain_services);
    expect(terrain && terrain.value()[1][5] == f(30), "WAR-35 mod terrain scales ground damage");
}
void winner_cases() {
    const auto table = types();
    struct Case { std::int64_t a, b; bool ai, playable_a, playable_b; std::uint32_t winner; };
    const std::vector<Case> cases{{10, 10, false, true, true, 20}, {20, 10, false, true, true, 10},
        {20, 10, true, false, true, 20}, {10, 20, true, true, false, 10}, {20, 10, true, false, false, 20},
        {0, 10, true, true, true, 20}, {10, 0, false, true, true, 10}, {0, 0, false, true, true, 20}};
    for (const auto& row : cases) {
        auto value = strength({object(1, "Zero")}, {object(2, "Zero")}, BattleLayer::land);
        value.sides[0].force = {f(row.a), f(0), f(0), f(0), f(0), f(0)};
        value.sides[1].force = {f(row.b), f(0), f(0), f(0), f(0), f(0)};
        auto context = humans(); context.players = {{{true, !row.ai, row.playable_a}, {true, !row.ai, row.playable_b}}};
        const auto result = auto_resolve_combat_round(value, table, bits, rules(), context, services(), first());
        expect(result && result.value().winner == row.winner, "WAR-37 winner table");
    }
    auto value = strength({object(1, "Zero")}, {object(2, "Zero")});
    value.sides[0].force.assign(6, f(0)); value.sides[1].force.assign(6, f(0));
    auto result = auto_resolve_combat_round(value, table, bits, rules(), humans(), {}, {});
    expect(result && result.value().winner == 10 && result.value().retreating_player == 20 && result.value().random_picks == 0,
        "WAR-27 count tie goes to defender without attacks or allocation");
    expect(result && result.value().retained_queues[0] == value.gathered.sides[0].combatants
        && result.value().retained_queues[1] == value.gathered.sides[1].combatants,
        "WAR-120 zero-power shortcut retains both complete gathered queues");
    value.gathered.sides[1].combatants.push_back(object(3, "Zero"));
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), {}, {});
    expect(result && result.value().winner == 20 && result.value().retreating_player == 10, "space shortcut larger count");
}
void allocation_cases() {
    const auto table = types();
    auto value = strength({object(1, "Hundred"), object(3, "Ten")}, {object(2, "Hundred")});
    value.sides[0].force = {f(0), f(110), f(110), f(0), f(0), f(0)};
    value.sides[1].force = {f(0), f(100), f(100), f(0), f(0), f(0)};
    value.sides[0].weakest = 3;
    auto result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(), first());
    expect(result && result.value().winner == 10 && allocation(result.value(), 1).killed && !allocation(result.value(), 3).killed,
        "WAR-45 winner allocation retains the cheap member within its budget");
    expect(result && result.value().random_picks == 3 && result.value().allocations.back().id == 2, "WAR-41 A then B singleton draws");
    expect(result && result.value().retained_queues[0] == std::vector<AutoResolveCombatant>{object(3, "Ten")},
        "WAR-118 retained queue excludes killed picks");
    auto rescue_rules = rules(); rescue_rules.allowance = f(1);
    auto rescue_context = humans(); rescue_context.players[0].human = false;
    const auto rescued = auto_resolve_combat_round(value, table, bits, rescue_rules, rescue_context, services(), first());
    expect(rescued && allocation(rescued.value(), 1).killed && !allocation(rescued.value(), 3).killed
        && rescued.value().allocations[1].id == 3, "WAR-45 deferred weakest is rescued when all other winners die");
    if (rescued) {
        const auto learned = std::find_if(rescued.value().learning.begin(), rescued.value().learning.end(),
            [](const auto& row) { return row.id == 3; });
        expect(learned != rescued.value().learning.end() && !learned->survived,
            "WAR-48 learning observes the pick's death decision before weakest rescue");
    }
    value.sides[0].force = {f(0), f(160), f(160), f(0), f(0), f(0)};
    // Winner can retain Hundred but not its weakest when the allowance is raised.
    auto high_allowance = rules(); high_allowance.allowance = d("0.99");
    value.gathered.sides[0].combatants[1].type = "Hundred";
    result = auto_resolve_combat_round(value, table, bits, high_allowance, humans(), services(f(0)), first(), 20);
    expect(result && !allocation(result.value(), 1).killed && allocation(result.value(), 3).killed, "weakest deferred death when another winner survives");
    value = strength({object(1, "Zero", true), object(3, "Base3", false, true), object(4, "Zero")}, {object(2, "Zero")});
    value.sides[0].force = {f(0), f(21), f(21), f(0), f(0), f(0)};
    value.sides[1].force = {f(0), f(1), f(1), f(0), f(0), f(0)};
    std::vector<std::size_t> counts;
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(f(0)), first(&counts));
    expect(result && allocation(result.value(), 3).retained_base_type == "Base2" && allocation(result.value(), 3).base_changed
        && allocation(result.value(), 3).final_blow_player == 20, "WAR-44 last-character base downgrade");
    if (result) {
        AutoResolveRoundState fought; fought.outcome = result.value();
        const auto publication = live_auto_resolve_progress_roster(value, {}, fought);
        const auto base = std::find_if(publication[0].queue.begin(), publication[0].queue.end(),
            [](const auto& row) { return row.current_type == "Base2"; });
        expect(base != publication[0].queue.end() && base->original_type == "Base3",
            "downgraded surviving planet publishes pre-round original and retained current base types");
    }
    expect(result && result.value().allocations[0].id == 1 && result.value().allocations[1].id == 3 && counts == std::vector<std::size_t>{1, 1},
        "heroes/planet first without draws then ordinary picks A and B");
    auto exact = value;
    exact.sides[0].force = {f(0), f(90), f(90), f(0), f(0), f(0)};
    auto full_allowance = rules(); full_allowance.allowance = f(1);
    const auto equality = auto_resolve_combat_round(exact, table, bits, full_allowance, humans(), services(f(0)), first());
    expect(equality && allocation(equality.value(), 3).retained_base_type == "Base3" && !allocation(equality.value(), 3).base_changed,
        "base threshold equality retains the base, unlike ordinary unit strict survival");
    value = strength({object(1, "Zero")}, {object(2, "Zero", false, false, true), object(3, "Zero", true, false, true), object(4, "Zero", false, false, true)});
    value.sides[0].force[1] = f(100); value.sides[1].force[1] = f(1);
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(f(0)), first());
    expect(result && !allocation(result.value(), 3).killed && !allocation(result.value(), 2).killed && allocation(result.value(), 4).killed
        && result.value().retreating_player == 20 && result.value().random_picks == 3, "WAR-38 hero escapes, both retained transports still consume ordered picks");
    auto live_contents = services(f(0));
    value.gathered.sides[1].combatants[1].named_hero = false;
    live_contents.contains_named_hero = [](const AutoResolveCombatant& combatant) { return ok(combatant.id == 3); };
    counts.clear();
    const auto hero_contents = auto_resolve_combat_round(value, table, bits, rules(), humans(), live_contents, first(&counts));
    expect(hero_contents && allocation(hero_contents.value(), 3).escaped_transport && counts == std::vector<std::size_t>{1, 2, 1},
        "live contained hero escapes independently of named-hero type flag; killed transport retains its pick position");
    value.gathered.sides[1].combatants.resize(1);
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(f(0)), first(), 20);
    expect(result && allocation(result.value(), 2).killed && result.value().retreating_player == 0, "single transport always dies and empty retreat cancels");
    auto missing_roster = services(); missing_roster.post_transport_death_contains_hero = {};
    expect(!auto_resolve_combat_round(value, table, bits, rules(), humans(), missing_roster, first()), "transport mortality requires explicit post-death roster seam");
    value = strength({object(1, "Zero")}, {object(2, "Zero")});
    value.sides[0].superweapon_killer = 1; value.sides[1].superweapon = 2;
    value.gathered.sides[1].combatants[0].type = "Super";
    value.sides[0].force[1] = f(100); value.sides[1].force[1] = f(1);
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(f(0)), first());
    expect(result && allocation(result.value(), 2).killed && result.value().superweapon_victory, "WAR-42/47 superweapon killer emits pending victory seam");
    value.sides[0].superweapon_killer = 0;
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(f(0)), first());
    expect(result && result.value().winner == 20 && result.value().retreating_player == 10 && !allocation(result.value(), 2).killed, "WAR-36 superweapon pressure before winner");
    auto context = humans(); context.players[0].human = false;
    result = auto_resolve_combat_round(value, table, bits, rules(), context, services(f(0)), first());
    expect(result && result.value().learning.size() == 1 && result.value().learning[0].player == 10, "WAR-48 AI-only learning rows");
    context.mid_tactical = true;
    expect(!auto_resolve_combat_round(value, table, bits, rules(), context, services(), first()), "mid-tactical allocation has a named #2026 seam");
    value = strength({object(1, "Super"), object(3, "Super")}, {object(2, "Hundred")});
    value.sides[0].superweapon = 3;
    value.sides[0].force = {f(0), f(50), f(50), f(0), f(0), f(0)};
    result = auto_resolve_combat_round(value, table, bits, rules(), humans(), services(), first(), 10);
    expect(result && result.value().winner == 20 && !allocation(result.value(), 1).killed && !allocation(result.value(), 3).killed,
        "WAR-42 protects every superweapon even when only the last is the pressure mark");
}
void timing() {
    auto ordered = strength({object(1, "Ten"), object(3, "Ten"), object(4, "Ten"), object(5, "Ten")}, {object(2, "Ten")});
    ordered.sides[0].force[1] = f(1000);
    const auto last = [](std::size_t count) { return ok(count - 1); };
    const auto reordered = auto_resolve_combat_round(ordered, types(), bits, rules(), humans(), services(f(0)), last);
    expect(reordered && reordered.value().retained_queues[0]
        == std::vector<AutoResolveCombatant>{object(5, "Ten"), object(4, "Ten"), object(3, "Ten"), object(1, "Ten")},
        "WAR-118 survivor queue keeps pick order without a three-entry cap");
    AutoResolveRoundState late; late.start_seconds = f(100); late.outcome = reordered.value();
    late.outcome->retreating_player = 0;
    const auto retained = *late.outcome;
    expect(request_auto_resolve_retreat(late, ordered, 10) && !request_auto_resolve_retreat(late, ordered, 20),
        "WAR-123 late first request remains admitted after allocation");
    const auto completed = step_auto_resolve_round(late, f(110), false, ordered, types(), bits, rules(), humans(), services(),
        [](std::size_t) { expect(false, "late withdrawal must not allocate again"); return ok(std::size_t{0}); });
    auto expected = retained; expected.retreating_player = 10;
    expect(completed && completed.value().progress == AutoResolveRoundProgress::done_with_retreat
        && completed.value().state.outcome == expected,
        "WAR-123 late completion changes only retreat flag, retaining winner, allocations, queues and picks");
    const auto table = types(); const auto value = strength(); const auto config = rules();
    AutoResolveRoundState initial; initial.start_seconds = f(100);
    auto before = step_auto_resolve_round(initial, d("106.665"), false, value, table, bits, config, humans(), services(), first());
    expect(before && !before.value().state.outcome, "WAR-28 not fought before display threshold");
    auto threshold = eawr::sim::math::add(f(100), eawr::sim::math::multiply(f(10), d("0.6666")).value()).value();
    auto fought = step_auto_resolve_round(initial, threshold, false, value, table, bits, config, humans(), services(), first());
    expect(fought && fought.value().state.outcome && fought.value().progress == AutoResolveRoundProgress::not_done, "round runs at threshold, display still pending");
    if (!fought) return;
    auto done = step_auto_resolve_round(fought.value().state, f(110), false, value, table, bits, config, humans(), services(),
        [](std::size_t) { expect(false, "second step never draws"); return ok(std::size_t{0}); });
    expect(done && done.value().progress == AutoResolveRoundProgress::done_with_retreat && done.value().state.outcome == fought.value().state.outcome,
        "display completion does not fight twice");
    expect(request_auto_resolve_retreat(initial, value, 10) && !request_auto_resolve_retreat(initial, value, 20), "WAR-46 first retreat wins");
    auto early = step_auto_resolve_round(initial, f(101), false, value, table, bits, config, humans(), services(), first());
    expect(early && early.value().state.start_seconds < f(100) && early.value().state.outcome->winner == 20,
        "early retreat fights and moves display start backwards");
    auto forced = step_auto_resolve_round({}, f(0), true, value, table, bits, config, humans(), services(), first());
    expect(forced && forced.value().progress != AutoResolveRoundProgress::not_done, "forced round completes immediately");
}
void fh30() {
    eawr::script::foc::detail::Host host;
    using eawr::script::numeric::LuaNumber;
    host.contrast[1] = {{1, {}, LuaNumber(1)}, {1, {}, LuaNumber(3)}, {1, {}, LuaNumber(5)}};
    auto adapters = services();
    adapters.contrast = [&](const GalacticType& type, const AutoResolveCategory&) {
        eawr::script::foc::AiType ai_type; ai_type.type_id = eawr::assets::object_type_crc(type.name); ai_type.category_bits = 1;
        return eawr::script::numeric::to_fixed(eawr::script::foc::detail::average_contrast(host, ai_type, 1));
    };
    auto value = strength({object(1, "Ten")}, {object(2, "Zero")});
    auto hit = auto_resolve_attacks(value, types(), bits, humans(), adapters);
    expect(hit && hit.value()[1][1] == f(60), "actual FH-30 excludes neutral factors and averages 3/5 to 4");
    host.contrast[1].push_back({0, eawr::assets::object_type_crc("Ten"), LuaNumber(7)});
    hit = auto_resolve_attacks(value, types(), bits, humans(), adapters);
    expect(hit && hit.value()[1][1] == f(30), "FH-30 exact type overrides category average");
}
void stock() {
    std::string root = eawr::testing::game_root().value_or("");
    if (root.empty()) { eawr::testing::skip_installed("SKIPPED: stock round needs EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"}) {
        const auto manifest = eawr::vfs::resolve_manifest_mount(folder, std::filesystem::path(root) / folder / "Data");
        if (!manifest) throw std::runtime_error(manifest.error().message);
        mounts.push_back(manifest.value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto table = eawr::units::galactic_type_table(catalog.catalog);
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto config = auto_resolve_round_rules(data).value();
    expect(config.display_seconds == d("0.01") && config.winner_attrition == d("0.75") && config.loser_attrition == d("0.8"), "stock rules loaded from effective XML");
    eawr::units::LoadInput input; input.catalog = &catalog.catalog; input.filesystem = &filesystem; input.types = {"Rebel_X-Wing"};
    const auto stock_bits = eawr::units::load_unit_tables(input).value().categories;
    AutoResolveSides sides; sides.aggressor = 20; sides.layer = BattleLayer::space;
    sides.sides[0].owner = 10; sides.sides[1].owner = 20;
    sides.sides[0].combatants = {object(40, "Calamari_Cruiser"), object(1, "Empire_Star_Base_1", false, true)};
    sides.sides[1].combatants = {object(30, "Rebel_X-Wing_Squadron")};
    const std::vector<AutoResolveCategory> stock_categories{{"Fighter", false}, {"Bomber", false}, {"Capital", false}};
    std::optional<AutoResolveRoundOutcome> reference;
    std::optional<std::uint64_t> rng_position;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto power = auto_resolve_strength(sides, table, stock_categories, stock_bits, executor).value();
        eawr::script::authoritative::RandomStream stream(123, 7, 99);
        const auto step = step_auto_resolve_round_transaction({}, f(0), true, power, table, stock_bits,
            config, humans(), services(), stream);
        expect(step && step.value().state.outcome, "stock production chooser succeeds");
        if (!step || !step.value().state.outcome) return;
        const auto result = ok(*step.value().state.outcome);
        expect(static_cast<bool>(result), "stock round succeeds"); if (!result) return;
        expect(result.value().winner == 10 && allocation(result.value(), 30).killed && !allocation(result.value(), 40).killed
            && allocation(result.value(), 1).retained_base_type == "Empire_Star_Base_1" && result.value().random_picks == 2 && stream.draws() == 0,
            "stock loss table and exact synchronized-stream draw count");
        if (!reference) { reference = result.value(); rng_position = stream.draws(); }
        else expect(result.value() == *reference && stream.draws() == *rng_position, "stock allocations and RNG position equal on 1/2/4/8 workers");
    }
}
} // namespace
void transaction_and_initiate() {
    auto value = strength();
    auto config = rules();
    eawr::script::authoritative::RandomStream random(123, 7, 99);
    auto untouched = random;
    auto context = humans(); context.players[1].human = false;
    auto callbacks = services(f(0));
    callbacks.learning_type = [](const AutoResolveCombatant&) {
        eawr::core::Diagnostic error; error.code = "test-learning"; error.message = "injected failure";
        return eawr::core::Result<std::string>::failure(std::move(error));
    };
    auto failed = step_auto_resolve_round_transaction({}, f(0), true, value, types(), bits,
        config, context, callbacks, random);
    expect(!failed && random.draws() == 0 && random.next_word() == untouched.next_word(),
        "WAR-41 failed round leaves the caller RNG sequence unchanged after staged draws");
    random = eawr::script::authoritative::RandomStream(123, 7, 99);
    auto result = step_auto_resolve_round_transaction({}, f(0), true, value, types(), bits,
        config, humans(), services(f(0)), random);
    expect(result && result.value().state.outcome && result.value().state.outcome->random_picks == 2 && random.draws() == 0,
        "WAR-41/111 singleton requests remain counted without advancing the stream");
    auto singleton_reference = eawr::script::authoritative::RandomStream(123, 7, 99);
    expect(random.next_word() == singleton_reference.next_word(), "singleton preserves the next generator word");
    std::optional<AutoResolveRoundOutcome> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto mixed = value;
        mixed.gathered.sides[0].combatants.push_back(object(3, "Ten"));
        mixed = auto_resolve_strength(mixed.gathered, types(), categories, bits, executor).value();
        auto stream = eawr::script::authoritative::RandomStream(123, 7, 99);
        auto expected = stream;
        (void)expected.next_in_range(0, 1);
        const auto wider = step_auto_resolve_round_transaction({}, f(0), true, mixed, types(), bits,
            config, humans(), services(f(0)), stream);
        expect(wider && wider.value().state.outcome && wider.value().state.outcome->random_picks == 3
            && stream.draws() == expected.draws(), "two-choice request advances; remaining singleton requests do not");
        expect(stream.next_word() == expected.next_word(), "wider request preserves the existing numerical mapping");
        if (wider && wider.value().state.outcome) {
            if (!reference) reference = *wider.value().state.outcome;
            else expect(*wider.value().state.outcome == *reference, "production choices equal on 1/2/4/8 workers");
        }
    }
    auto ordinary = initiate_auto_resolve_round(f(100), value, types(), bits, config, humans(), services(), random);
    expect(ordinary && !ordinary.value().state.outcome && ordinary.value().state.start_seconds == f(100),
        "WAR-28 ordinary initiate preserves the display deadline without fighting");
    for (auto& side : value.sides) side.force[1] = f(0);
    const auto before = random.draws();
    auto shortcut_result = initiate_auto_resolve_round(f(100), value, types(), bits, config, humans(), services(), random);
    expect(shortcut_result && shortcut_result.value().state.outcome
        && shortcut_result.value().progress == AutoResolveRoundProgress::done_with_retreat
        && shortcut_result.value().state.outcome->winner == 10 && random.draws() == before,
        "WAR-27 shortcut is fought and complete at initiate, defender wins equal counts without draws");
    value.sides[0].force.clear();
    expect(!initiate_auto_resolve_round(f(100), value, types(), bits, config, humans(), services(), random),
        "initiate rejects malformed strength before inspecting shortcut totals");
    expect(auto_resolve_service_delay(f(3), true).value() == f(1)
        && auto_resolve_service_delay(f(3), false).value() == f(3)
        && !auto_resolve_service_delay(f(-1), true), "WAR-51 rearm uses the land delay and retreat divisor three");
    value = strength();
    const auto overrun = step_auto_resolve_round_transaction({}, f(0), false, value, types(), bits,
        config, humans(), services(f(0)), random, 20);
    expect(overrun && overrun.value().state.outcome && overrun.value().state.outcome->winner == 10,
        "WAR-50 overrun loser requests retreat at every round invocation");
}
int main(int argc, char**) {
    try { if (argc > 1) stock(); else { defaults(); attack_cases(); winner_cases(); allocation_cases(); timing(); fh30(); transaction_and_initiate(); } }
    catch (const std::exception& error) { expect(false, error.what()); }
    return galactic_test::failures ? 1 : 0;
}
