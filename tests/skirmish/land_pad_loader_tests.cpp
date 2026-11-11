// LE-LOADER: build pads that are not capture points (every stock faction pad and structure pad) become inert pad profiles on
// land: the build menu and the WBP-07 proximity radius are theirs, capture is not. Synthetic tables; the stock-map witness
// (pads and menus from data, a build end to end) is in land_start_tests (installed).
#include "eawr/skirmish/ai.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/pads.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "../support/check.hpp"

#include <algorithm>
#include <iostream>

namespace {
using namespace eawr;
using namespace sim::tactical;
using sim::math::Fixed;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }

Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

units::UnitType pad(const std::string& id, const bool capture_point, const std::optional<Fixed> radius) {
    units::UnitType type;
    type.id = id;
    type.build_pad = true;
    type.capture_point = capture_point;
    type.capture_radius = radius;
    type.capture_seconds = whole(2);
    type.affiliation = "Rebel";
    return type;
}

const CaptureProfile* find(const PadRules& rules, const std::string& id) { return rules.point(skirmish::type_id(id)); }

void profiles() {
    skirmish::StartInputs inputs;
    skirmish::StartFaction rebel; rebel.name = "Rebel"; rebel.playable = true;
    inputs.factions.push_back(rebel);
    TacticalSetup setup;
    setup.players = {{1, 1, 1, player_flag_commandable}};
    units::UnitTables land;
    land.domain = BattleDomain::land;
    land.units.push_back(pad("Pad_Faction", false, whole(0)));      // a faction pad: authored radius 0
    land.units.push_back(pad("Pad_Structure", false, whole(200)));   // a structure pad: the type default radius
    auto capturing = pad("Pad_Capturing", true, whole(100));
    capturing.capture_seconds = whole(5);
    land.units.push_back(capturing);
    units::UnitType plain; plain.id = "Not_A_Pad";
    land.units.push_back(plain);
    const auto rules = skirmish::capture_rules(setup, inputs, land);
    expect(static_cast<bool>(rules), "land pad rules build");
    if (!rules) return;
    const auto* faction = find(rules.value(), "Pad_Faction");
    const auto* structure = find(rules.value(), "Pad_Structure");
    const auto* capture = find(rules.value(), "Pad_Capturing");
    expect(faction && !faction->captures && faction->radius == Fixed{} && faction->affiliation.empty() && faction->build_pad,
        "LE-LOADER: a land faction pad without CAPTURE_POINT is an inert pad profile with its authored radius 0");
    expect(structure && !structure->captures && structure->radius == whole(200) && structure->affiliation.empty(),
        "LE-LOADER: a land structure pad keeps the type radius for the proximity test and never captures");
    expect(capture && !capture->captures && capture->radius == whole(100) && capture->transition_seconds == whole(5)
            && capture->affiliation.size() == 1,
        "LE-LOADER: a land pad with CAPTURE_POINT keeps its capture inputs but does not capture until the land capture service (LE-CAPTURE)");
    expect(!find(rules.value(), "Not_A_Pad"), "LE-LOADER: a type that is not a pad gets no profile");
    expect(std::is_sorted(rules.value().capture.begin(), rules.value().capture.end(),
               [](const auto& a, const auto& b) { return a.type < b.type; }), "LE-LOADER: the profiles stay ordered by type");
    // The space domain keeps its rule: a pad without CAPTURE_POINT is not a capture profile there.
    auto space = land;
    space.domain = BattleDomain::space;
    const auto space_rules = skirmish::capture_rules(setup, inputs, space);
    expect(space_rules && !find(space_rules.value(), "Pad_Faction") && !find(space_rules.value(), "Pad_Structure")
            && find(space_rules.value(), "Pad_Capturing") && find(space_rules.value(), "Pad_Capturing")->captures,
        "LE-LOADER: a space pad without CAPTURE_POINT still has no profile (space meaning unchanged)");
}

void inertness() {
    CaptureProfile inert;
    inert.type = 5; inert.radius = whole(200); inert.transition_seconds = whole(2); inert.captures = false;
    CaptureProfile live = inert;
    live.captures = true; live.affiliation = {1};
    PadState state{};
    const std::vector<Player> players{{1, 1, 1, player_flag_commandable}, {2, 2, 1, player_flag_commandable}, {3, 3, 2, 0}};
    const auto none_before = state.under_construction;
    // An owned pad with no one near it: a capturing pad without stickiness returns to the neutral player; an inert pad keeps
    // its owner (the faction pad of a start belongs to its player and stays so).
    const auto kept = service_capture(inert, state, 1, 3, players, {}, PadRules{}, 7, sim::math::Vec3{}, Fixed{});
    expect(kept == 1 && state.progress == Fixed{} && state.under_construction == none_before,
        "LE-LOADER: an inert pad keeps its owner and does not move its capture progress");
    PadState other{};
    const auto lost = service_capture(live, other, 1, 3, players, {}, PadRules{}, 7, sim::math::Vec3{}, Fixed{});
    expect(lost != 1 || other.target == 3 || other.progress != Fixed{},
        "LE-LOADER: the same pad as a capture point does start to leave its owner (the case the inert flag prevents)");
}

void validation() {
    const std::vector<Player> players{{1, 1, 1, player_flag_commandable}, {2, 2, 2, 0}};
    EconomyRules rules;
    EconomyPlayer one; one.player = 1;
    rules.players = {one};
    CaptureProfile inert;
    inert.type = 5; inert.radius = whole(200); inert.captures = false;
    rules.pads.capture = {inert};
    rules.pads.neutral = 0;
    // Only the neutral-player rule is under test: any other reason a bare fixture is refused would show in the message.
    const auto about_neutral = [](const core::Result<void>& result) {
        return !result && result.error().message.find("neutral") != std::string::npos;
    };
    expect(!about_neutral(validate_economy(rules, players)), "LE-LOADER: inert pads alone need no declared neutral player");
    rules.pads.capture.front().captures = true;
    expect(about_neutral(validate_economy(rules, players)), "a capturing pad still needs a declared neutral player");
    rules.pads.neutral = 2;
    expect(!about_neutral(validate_economy(rules, players)), "a capturing pad with a declared neutral player passes that rule");
}

// LC-46: where the land start and the shared builder both set an EconomyPlayer row (credits, cap, tech, multipliers), the land row wins;
// only the pad rules, menus and pad sales are taken from the builder.
void merge_rule() {
    EconomyRules land;
    EconomyPlayer own; own.player = 1; own.credits = whole(500); own.population_cap = 6; own.start_tech = 1; own.max_tech = 3;
    own.credit_multiplier = whole(2);
    land.players = {own};
    EconomyRules builder;
    EconomyPlayer theirs; theirs.player = 1; theirs.credits = whole(9999); theirs.population_cap = 40; theirs.start_tech = 2; theirs.max_tech = 5;
    builder.players = {theirs};
    CaptureProfile inert; inert.type = 5; inert.radius = whole(200); inert.captures = false;
    builder.pads.capture = {inert};
    builder.pads.neutral = 9;
    StationMenu menu; menu.station = 5; menu.faction = 1; menu.station_producer = false;
    BuildOption option; option.type = 7; option.kind = BuildKind::structure; option.price = whole(850); option.available = true;
    menu.options = {option};
    builder.menus = {menu};
    builder.pad_sales.push_back(PadSaleProfile{5, whole(1)});
    IncomeProfile income; income.source = 7; income.base_value = whole(45); income.interval_seconds = whole(5);
    builder.income.push_back(income);
    builder.disabled_types = std::vector<TypeId>{77};
    builder.max_queue = 99;
    skirmish::merge_land_pad_rules(land, std::move(builder));
    expect(land.players.size() == 1 && land.players.front() == own,
        "LC-46: the land EconomyPlayer row wins over the builder's on credits, cap, tech and multipliers");
    expect(land.pads.capture.size() == 1 && land.pads.capture.front().type == 5 && !land.pads.capture.front().captures
            && land.pads.neutral == 9 && land.menus.size() == 1 && land.menus.front().options.size() == 1
            && land.menus.front().options.front().price == whole(850) && land.pad_sales.size() == 1,
        "LC-46: the pad rules, the build menus and the pad sales come from the builder");
    expect(land.income.size() == 1 && land.income.front().source == 7 && land.income.front().base_value == whole(45),
        "LR4-15: authored pad-tree income reaches the land economy");
    expect(!land.disabled_types.contains(77) && land.max_queue != 99,
        "LC-46: nothing else the builder derived (disabled types, queue size ...) reaches the land economy");
}
}

// The land AI type registry (what GameObject.Build and the type queries resolve against) holds the map's pad rows next to the forces'
// rows, in the authored table order whatever order the pad loader produced them in, and leaves the caller's tables alone.
void ai_registry() {
    const auto row = [](const std::string& id, const std::uint64_t order) {
        units::UnitType type; type.id = id; type.table_order = order; return type;
    };
    units::UnitTables forces;
    forces.domain = BattleDomain::land;
    forces.units = {row("Company_Member", 10), row("Late_Member", 40)};
    const auto registry = [&](const std::vector<units::UnitType>& pad_rows) {
        skirmish::LandStartInputs inputs;
        inputs.pad_tables.domain = BattleDomain::land;
        inputs.pad_tables.units = pad_rows;
        const auto setup = skirmish::land_ai_setup(skirmish::LandStart{}, inputs, forces);
        std::vector<TypeId> ids;
        for (const auto& type : setup.content.types) ids.push_back(type.type_id);
        return ids;
    };
    const auto expected = [&](const std::vector<const char*>& names) {
        std::vector<TypeId> ids;
        for (const auto* name : names) ids.push_back(skirmish::type_id(name));
        return ids;
    };
    const std::vector<units::UnitType> forward{row("Pad_Faction", 20), row("Pad_Structure", 30), row("Company_Member", 10)};
    const std::vector<units::UnitType> backward{forward[2], forward[1], forward[0]};
    expect(registry(forward) == expected({"Company_Member", "Pad_Faction", "Pad_Structure", "Late_Member"}),
        "the land AI registry holds the pad rows between the forces' rows by authored table order, each type once");
    expect(registry(backward) == registry(forward), "the pad loader's row order does not reach the land AI registry");
    expect(registry({}) == expected({"Company_Member", "Late_Member"}), "a map without pads leaves the registry as the forces' tables make it");
    expect(forces.units.size() == 2, "the caller's tables are not changed by the pad merge");
}

// A pad row indexes the projectile and priority tables of the load that decoded it; merged into the company tables it must point at
// its own projectile again (a stale index ran off the table and crashed the stock start).
void merge_types() {
    units::UnitTables base;
    base.domain = BattleDomain::land;
    units::Projectile shared; shared.id = "Shared_Bolt";
    units::Projectile company_only; company_only.id = "Company_Bolt";
    base.projectiles = {company_only, shared};
    units::UnitType company; company.id = "Company_Member";
    base.units.push_back(company);
    units::UnitTables pads;
    pads.domain = BattleDomain::land;
    units::Projectile pad_bolt; pad_bolt.id = "Pad_Bolt";
    pads.projectiles = {pad_bolt, shared};
    units::UnitType turret; turret.id = "Pad_Turret";
    turret.weapon = units::Weapon{};
    turret.weapon->projectile_index = 0;           // Pad_Bolt in the pad tables
    turret.weapon->charged_projectile_index = 1;   // Shared_Bolt in both tables, at different positions
    turret.death_projectile_indices = {1};
    turret.members = {{"Company_Member", 1, {}}, {"Pad_Craft", 2, {}}, {"Missing", 99, {}}, {"Unset", units::no_index, {}}};
    turret.spawner = units::Spawner{};
    turret.spawner->starting = {{"Pad_Craft", 2, 1}};
    turret.spawner->reserves = {{"Company_Member", 1, 1}};
    turret.spawner->campaign_starting[5] = {{"Company_Member", 1, 1}};
    turret.spawner->campaign_reserves[3] = {{"Pad_Craft", 2, 1}};
    units::UnitType again = company;               // already in the base tables: the base row wins
    units::UnitType craft; craft.id = "Pad_Craft";
    craft.members = {{"Pad_Turret", 0, {}}};      // backward reference to an appended row
    pads.units = {turret, again, craft};
    skirmish::merge_land_pad_types(base, pads);
    expect(base.units.size() == 3 && base.units[1].id == "Pad_Turret" && base.units[2].id == "Pad_Craft", "LE-LOADER: missing types are appended and existing types are reused");
    if (base.units.size() != 3) return;
    const auto& merged = base.units[1];
    const auto id_at = [&](const std::uint32_t index) { return index < base.projectiles.size() ? base.projectiles[index].id : std::string("<out of range>"); };
    expect(merged.weapon && id_at(merged.weapon->projectile_index) == "Pad_Bolt",
        "LE-LOADER: a merged pad weapon still points at its own projectile (appended to the target table)");
    expect(merged.weapon && id_at(merged.weapon->charged_projectile_index) == "Shared_Bolt"
            && merged.death_projectile_indices.size() == 1 && id_at(merged.death_projectile_indices.front()) == "Shared_Bolt",
        "LE-LOADER: a projectile the target tables already hold (by id) is reused, not duplicated");
    expect(std::count_if(base.projectiles.begin(), base.projectiles.end(), [](const auto& row) { return row.id == "Shared_Bolt"; }) == 1,
        "LE-LOADER: the shared projectile exists once");
    expect(merged.members.size() == 4 && merged.members[0].craft_index == 0 && merged.members[1].craft_index == 2
            && merged.members[2].craft_index == units::no_index && merged.members[3].craft_index == units::no_index
            && base.units[2].members.front().craft_index == 1,
        "LE-LOADER: member references map to reused and appended rows in either direction; invalid and unset indices stay unset");
    expect(merged.spawner && merged.spawner->starting.front().squadron_index == 2 && merged.spawner->reserves.front().squadron_index == 0
            && merged.spawner->campaign_starting[5].front().squadron_index == 0 && merged.spawner->campaign_reserves[3].front().squadron_index == 2,
        "LE-LOADER: starting, reserve and campaign spawner references map to their target rows");
}

int main() {
    profiles();
    inertness();
    validation();
    merge_rule();
    merge_types();
    ai_registry();
    return checks.finish();
}
