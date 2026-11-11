// LC-86 (#6183): placed and pre-built land structures reveal fog from their own rows. The sensor rows come from
// land_static_sensor_table; the pad gates (pad_build_allowed and the fog cells they read) stay as they are.
// Synthetic content on a plain field; the stock-map witness is in land_start_tests (installed).
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/skirmish/land_fog.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <iostream>
#include <map>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using Bytes = std::vector<std::uint8_t>;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template <class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

constexpr TypeId company_type = 11, member_type = 12;
const TypeId hq_type = skirmish::type_id("synthetic-hq");
const TypeId pad_type = skirmish::type_id("synthetic-pad");
const TypeId site_type = skirmish::type_id("synthetic-site");
const TypeId turret_type = skirmish::type_id("synthetic-turret");

units::LandStaticObject structure_row(const std::string& id, const bool reveal, const std::optional<Fixed> range) {
    units::LandStaticObject row;
    row.id = id; row.fog.reveal = reveal; row.fog.range = range;
    return row;
}

// The sensor rows of the placed statics: range, floor input, mod value, census-only and sampling-only rows, ordering.
void sensor_rows() {
    auto hq = structure_row("synthetic-hq", true, whole(300));
    hq.fog.dense_multiplier = Fixed::from_ratio(1, 4).value();
    hq.fog.multisample = true; hq.fog.box_x_offset = whole(3); hq.fog.box_y_offset = whole(-2);
    hq.fog.custom_hard_x = whole(12); hq.fog.custom_hard_y = whole(8);
    hq.scale_factor = whole(2);
    const auto bare = structure_row("synthetic-bare", true, {});                       // REVEAL without a range
    auto sampler = structure_row("synthetic-sampler", false, whole(200)); sampler.fog.multisample = true;
    auto silent = structure_row("synthetic-silent", false, whole(200));
    silent.hide_when_fogged = true;
    const auto mod = structure_row("synthetic-mod", true, whole(450));
    const auto census = structure_row("synthetic-census", true, whole(300));           // never a session object
    // An obstacle with a model and no authored hard extents (every stock multisample structure): the box is the model's.
    auto boxed = structure_row("synthetic-boxed", false, {}); boxed.fog.multisample = true;
    boxed.collision_half_x = whole(10); boxed.collision_half_y = whole(6); boxed.scale_factor = whole(2);
    // One authored hard extent overrides only its own axis, as for a unit.
    auto mixed = structure_row("synthetic-mixed", false, {}); mixed.fog.multisample = true;
    mixed.collision_half_x = whole(10); mixed.collision_half_y = whole(6); mixed.fog.custom_hard_x = whole(15);
    const std::vector rows{hq, bare, sampler, silent, mod, census, boxed, mixed};
    const std::vector<TypeId> session{skirmish::type_id("synthetic-mod"), skirmish::type_id("synthetic-hq"),
        skirmish::type_id("synthetic-silent"), skirmish::type_id("synthetic-bare"), skirmish::type_id("synthetic-sampler"),
        skirmish::type_id("synthetic-boxed"), skirmish::type_id("synthetic-mixed")};
    const auto table = skirmish::land_static_sensor_table(rows, session);
    const auto find = [&](const std::string_view id) -> const SensorProfile* {
        const auto type = skirmish::type_id(id);
        const auto found = std::find_if(table.begin(), table.end(), [&](const auto& row) { return row.type_id == type; });
        return found == table.end() ? nullptr : &*found;
    };
    const auto* hq_row = find("synthetic-hq");
    expect(hq_row && hq_row->reveals && hq_row->reveal_range == whole(300) && hq_row->multisample
            && hq_row->dense_multiplier == Fixed::from_ratio(1, 4).value() && hq_row->box_offset.x == whole(3)
            && hq_row->box_offset.y == whole(-2) && hq_row->half_extents.x == whole(24) && hq_row->half_extents.y == whole(16),
        "LC-86: a placed structure reveals its authored range; its fog box follows the authored extents and offsets");
    const auto* bare_row = find("synthetic-bare");
    expect(bare_row && bare_row->reveals && bare_row->reveal_range == Fixed{},
        "LC-86: a REVEAL structure without a range reveals with range 0 (the sensor floors it)");
    const auto* sampler_row = find("synthetic-sampler");
    expect(sampler_row && !sampler_row->reveals && sampler_row->multisample && sampler_row->reveal_range == Fixed{},
        "LC-86: a structure that only samples the fog gets a row that does not reveal");
    expect(sampler_row && sampler_row->half_extents.x == Fixed{} && sampler_row->half_extents.y == Fixed{},
        "LC-89: a sampling structure with neither a model nor hard extents has no fog box");
    const auto* boxed_row = find("synthetic-boxed");
    expect(boxed_row && boxed_row->multisample && boxed_row->half_extents.x == whole(20) && boxed_row->half_extents.y == whole(12),
        "LC-89: without authored hard extents the fog box is the model's collision half extents times the scale");
    const auto* mixed_row = find("synthetic-mixed");
    expect(mixed_row && mixed_row->half_extents.x == whole(15) && mixed_row->half_extents.y == whole(6),
        "LC-89: an authored hard extent replaces only its own axis of the model box");
    const auto* hidden = find("synthetic-silent");
    expect(hidden && hidden->hide_when_fogged && !hidden->reveals,
        "a fog-bound marker publishes its hide flag without becoming a revealer");
    const auto* mod_row = find("synthetic-mod");
    expect(mod_row && mod_row->reveal_range == whole(450), "LC-86: a mod-authored range is used as authored");
    expect(!find("synthetic-census"), "LC-86: a census-only type (not a session object) gets no row");
    expect(table.size() == 7 && std::is_sorted(table.begin(), table.end(),
            [](const auto& a, const auto& b) { return a.type_id < b.type_id; })
        && std::adjacent_find(table.begin(), table.end(), [](const auto& a, const auto& b) { return a.type_id == b.type_id; }) == table.end(),
        "LC-86: the static sensor rows are strictly ascending by type id");
}

struct Layout {
    std::int64_t hq_x{410}, hq_y{190};
    std::int64_t pad_x{310}, pad_y{190};
    bool hq_reveals{true}, pad_reveals{false};
    std::int64_t hq_range{300}, pad_range{100};
    bool static_sensors{true};   // false = the table main has before this change: static types have no sensor row
};

struct Scene { TacticalSetup setup; TacticalContent content; };

// Player 1's base: a headquarters, a build pad and a company far from both (it reveals nothing: its type has no sensor row,
// so only the structure rows can reveal). Player 2 has one company far away.
Scene make_scene(const std::shared_ptr<const LandTerrain>& terrain, const Layout& layout) {
    Scene result;
    auto& content = result.content;
    content.domain = BattleDomain::land; content.land_terrain = terrain;
    content.land.clear_bit = clear;
    content.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    LandCompanyBinding binding;
    binding.type = company_type; binding.company.id = "synthetic-company";
    binding.company.team_max_speed = whole(60); binding.company.max_squad_size = 1;
    binding.company.team_collision_class = units::LandCollisionGroup::infantry;
    binding.effective_team_max_speed = whole(60);
    binding.formation_spacing = Fixed::from_ratio(1, 5).value();
    units::LandCompanyMember member;
    member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
    member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
    member.locomotor = "synthetic-walking";
    binding.company.members.push_back(member);
    binding.members.push_back({member_type, {whole(80), whole(120), whole(150), whole(2), whole(1), false,
        units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
    content.land.companies.push_back(std::move(binding));
    content.fog = take(skirmish::land_fog_rules(terrain->width(), terrain->height(), terrain->spacing(), whole(15), whole(6)));
    auto& setup = result.setup;
    setup.domain = BattleDomain::land; setup.seed = 49; setup.content_identity[0] = 7;
    setup.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}, {3, 3, 3, 0}};
    const auto place = [&](sim::EntityId id, TypeId type, PlayerId owner, std::int64_t x, std::int64_t y, bool structure) {
        UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = owner;
        unit.position = {whole(x), whole(y), {}};
        setup.units.push_back(unit);
        if (!structure) return;
        content.land.static_objects.push_back(type);
        LandOccupation occupation; occupation.object = id; occupation.primary_passability = wall;
        const auto column = static_cast<std::uint32_t>(x / 20), row = static_cast<std::uint32_t>(y / 20);
        for (std::uint32_t cy = row - 1; cy <= row + 1; ++cy) for (std::uint32_t cx = column - 1; cx <= column + 1; ++cx)
            occupation.primary_cells.push_back(cy * terrain->width() + cx);
        content.land.occupations.push_back(std::move(occupation));
    };
    place(1, company_type, 1, 150, 600, false);
    place(2, company_type, 2, 1100, 700, false);
    place(3, hq_type, 1, layout.hq_x, layout.hq_y, true);
    place(4, pad_type, 1, layout.pad_x, layout.pad_y, true);
    // The pad economy of the land economy tests: a pad profile, its menu, the construction site and its child.
    for (const TypeId type : {pad_type, hq_type, site_type, turret_type}) {
        DurabilityProfile health; health.type_id = type; health.max_hull = whole(300);
        content.durability.profiles.push_back(health);
    }
    auto& economy = content.economy;
    economy.players = {{1, whole(1000), 0}, {2, whole(1000), 0}};
    StationMenu pad_menu; pad_menu.station = pad_type; pad_menu.faction = 1; pad_menu.station_producer = false;
    pad_menu.options = {{site_type, BuildKind::structure, BuildQueue::units, whole(100), 30, 30, 0, true}};
    economy.menus = {pad_menu};
    economy.pads.neutral = 3;
    economy.pads.capture = {{pad_type, whole(100), whole(5), {1, 2}, false, true, true}};
    economy.pads.construction = {{site_type, turret_type, whole(100), 1, 1}};
    economy.pads.influence = {{pad_type, false}, {site_type, false}, {turret_type, false}};
    // The rows are strictly ascending by type id; the synthetic ids are hashes of their names.
    std::sort(economy.pads.influence.begin(), economy.pads.influence.end(), [](const auto& left, const auto& right) { return left.type < right.type; });
    std::sort(content.durability.profiles.begin(), content.durability.profiles.end(),
        [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    // The sensor rows: what the structures' own REVEAL rows give them (or nothing, as before this change).
    if (layout.static_sensors) {
        const std::vector rows{structure_row("synthetic-hq", layout.hq_reveals, whole(layout.hq_range)),
            structure_row("synthetic-pad", layout.pad_reveals, whole(layout.pad_range))};
        content.sensors = skirmish::land_static_sensor_table(rows, content.land.static_objects);
    }
    return result;
}

struct Gate { bool allowed{}, revealed_owner{}, revealed_enemy{}; Bytes state; };
Gate gate(const Scene& scene, const sim::PartitionExecutor& executor, const int ticks) {
    auto session = take(TacticalSession::create(scene.setup, scene.content));
    for (int tick = 0; tick < ticks; ++tick) take(session.step(executor));
    Gate result;
    const auto pad = session.unit_state(4);
    result.allowed = take(session.pad_build_allowed(1, 4));
    result.revealed_owner = session.fog_cells()->revealed(0, pad->position);
    result.revealed_enemy = session.fog_cells()->revealed(1, pad->position);
    result.state = session.canonical_state_bytes();
    return result;
}

void pad_gates() {
    auto field = make_field(std::vector<std::string>(64, std::string(64, '.')));
    const sim::InlineExecutor serial;
    // In the base: the headquarters (range 300) reveals the pad 100 away; nothing else reveals it.
    const auto inside = gate(make_scene(field.terrain, {}), serial, 1);
    expect(inside.allowed && inside.revealed_owner && !inside.revealed_enemy,
        "LC-86: a pad inside a headquarters' reveal circle is revealed to its owner (and not to the enemy), so its build is allowed");
    // The case that fails without the structure rows: the same base, the table static types have without this change.
    Layout none; none.static_sensors = false;
    const auto before = gate(make_scene(field.terrain, none), serial, 1);
    expect(!before.allowed && !before.revealed_owner,
        "LC-86: without sensor rows for placed structures the same pad is not revealed and its build is refused");
    // Outside every revealer: the pad 600 away from the headquarters, and nothing of its own to reveal it.
    Layout far; far.pad_x = 1010; far.pad_y = 590;
    const auto outside = gate(make_scene(field.terrain, far), serial, 1);
    expect(!outside.allowed && !outside.revealed_owner,
        "LC-86: a pad outside every reveal circle is refused even with the structure rows");
    // A pad with its own REVEAL (stock build pads carry range 100) reveals its own cell to its owner.
    Layout own = far; own.pad_reveals = true; own.hq_reveals = false;
    const auto reveals = gate(make_scene(field.terrain, own), serial, 1);
    expect(reveals.allowed && reveals.revealed_owner && !reveals.revealed_enemy,
        "LC-86: a pad that reveals reveals its own cell to its owner");
    // The circle is bounded: a pad just beyond the range stays fogged (range 300 from (410,190); the pad's cell is 340 away).
    Layout edge; edge.pad_x = 750; edge.pad_y = 190; edge.hq_range = 300;
    const auto beyond = gate(make_scene(field.terrain, edge), serial, 1);
    expect(!beyond.revealed_owner, "LC-86: a structure reveals out to its range and not beyond");
    // 1/2/4/8 workers and reversed partitions end in one state.
    const auto reference = gate(make_scene(field.terrain, {}), serial, 40);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        expect(gate(make_scene(field.terrain, {}), threads, 40).state == reference.state,
            "a base with structure reveal ends in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(gate(make_scene(field.terrain, {}), reverse, 40).state == reference.state,
            "a base with structure reveal ends in one state with reversed partitions");
    }
}

void marker_fog_publication() {
    auto field = make_field(std::vector<std::string>(64, std::string(64, '.')));
    auto scene = make_scene(field.terrain, {});
    auto marker = structure_row("synthetic-pad", false, {});
    marker.hide_when_fogged = true;
    scene.content.sensors = skirmish::land_static_sensor_table({marker}, scene.content.land.static_objects);
    auto session = take(TacticalSession::create(scene.setup, scene.content));
    const auto check = [&] {
        const auto instances = session.snapshot()->instances();
        const auto pad = std::find_if(instances.begin(), instances.end(), [](const auto& row) { return row.entity_id == 4; });
        const auto hq = std::find_if(instances.begin(), instances.end(), [](const auto& row) { return row.entity_id == 3; });
        return pad != instances.end() && pad->hide_when_fogged && hq != instances.end() && !hq->hide_when_fogged;
    };
    expect(check(), "initial land snapshot publishes marker hide behavior");
    take(session.step(sim::InlineExecutor{}));
    expect(check(), "later land snapshots retain marker hide behavior");
}
}

int main() {
    try {
        sensor_rows();
        pad_gates();
        marker_fog_publication();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
