// The AI's per-cell "time since seen" (docs/behaviour/foc-tactical-ai.md PE-25, PG-01, PG-03): a
// threat cell is stamped when one of its fog cells fully fogs out, and a region's age averages the
// fogged cells it touches. The grid reads the session's real fog, also for an AI player. These
// tests check the formulas against the documented rules (no retail table cell is pinned here; the
// scouting desire values are checked by the stock-after measurement of the PR).

#include <cstddef>
#include <cstdint>
#include <utility>
#include <string_view>
#include <stdexcept>
#include "../../support/check.hpp"
#include "ai_engine.hpp"
#include "tactical_ai_world.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/tactical/land_session.hpp"
#include "eawr/skirmish/land_fog.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "../../skirmish/land_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace ai = eawr::script::foc::ai;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace tactical = eawr::sim::tactical;
using eawr::sim::math::Fixed;
using ai::Real;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string& message) {
    local_checker.check(condition, message, std::cerr);
}

constexpr tactical::PlayerId rebel = 11;
constexpr tactical::PlayerId empire = 12;
constexpr tactical::TypeId scout_type = 20;   // reveals a single fog cell and can be killed
constexpr tactical::TypeId plain_type = 30;   // no sensor of its own
constexpr tactical::TypeId station_type = 40; // a DUMMY_STAR_BASE type with UNIT_AI (view only)
constexpr tactical::TypeId ship_type = 50;    // an ordinary UNIT_AI type (view only)

Fixed units(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

// The Coruscant extent: 130 x 130 fog cells of 100 from (-6500, 6500) make a 33 x 33 threat grid
// whose cells are 13000 / 33 = 393.9 wide, not 400.
tactical::FogRules fog_rules() {
    tactical::FogRules rules;
    rules.map_left = units(-6500);
    rules.map_top = units(6500);
    rules.cell_size = units(100);
    rules.cells_wide = 130;
    rules.cells_tall = 130;
    rules.ramp_down_step = tactical::fog_ramp_down_step(units(6)).value();
    return rules;
}

// Unit 1 sits in fog column 39, row 65 (x in [-2600, -2500], y in [-100, 0]) and dies at tick 5.
// Unit 3 sits in fog cell (21, 21), which holds the centre of threat cell (5, 5), and lives.
constexpr std::int64_t revealer_x = -2550;
constexpr std::int64_t revealer_y = -50;
constexpr std::int64_t anchored_x = -4333;
constexpr std::int64_t anchored_y = 4333;

tactical::TacticalSetup setup(const bool both_players = true) {
    tactical::TacticalSetup result;
    result.seed = 12345;
    result.players = {{rebel, 0, 1, tactical::player_flag_commandable}};
    if (both_players) result.players.push_back({empire, 1, 2, tactical::player_flag_commandable});
    const auto unit = [](eawr::sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, std::int64_t x, std::int64_t y) {
        return tactical::UnitState{id, type, owner, {units(x), units(y), Fixed{}}, eawr::sim::math::identity_quat(), {}};
    };
    result.units = {unit(1, scout_type, rebel, revealer_x, revealer_y)};
    if (both_players) result.units.push_back(unit(2, plain_type, empire, 3000, 3000));
    result.units.push_back(unit(3, scout_type, rebel, anchored_x, anchored_y));
    return result;
}

tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(Fixed::scale / 5), Fixed::from_raw(Fixed::scale * 2 / 5), Fixed::from_raw(Fixed::scale / 3)};
    table.profiles.push_back(tactical::DurabilityProfile{scout_type, units(90), units(4), false, {}});
    return table;
}

const std::vector<tactical::SensorProfile>& sensors() {
    static const std::vector<tactical::SensorProfile> table{{scout_type, units(10)}}; // radius 0 cells: one fog cell
    return table;
}

// A host over a live session; the view is ours, the fog is the session's.
struct Rig {
    Rig(tactical::TacticalSession& session_ref, const std::size_t workers, const bool land = false)
        : session(session_ref), executor(workers) {
        host = std::make_shared<foc::detail::Host>();
        host->world = &session;
        // Space: the Coruscant extent centred on the origin. Land (#6942): declared extents that are NOT the land fog rectangle
        // (the fog is the 96-sample, 20-spacing field from the origin, 0 to 1920), as a shared setup's extents are.
        host->setup.bounds = land
            ? foc::AiBounds{ai::real(-480), ai::real(2400), ai::real(2400), ai::real(-480)}
            : foc::AiBounds{ai::real(-6500), ai::real(6500), ai::real(6500), ai::real(-6500)};
        if (land) host->setup.domain = tactical::BattleDomain::land;
        foc::AiType station;
        station.type_id = station_type; station.name = "STATION"; station.unit_ai = true; station.star_base = true;
        foc::AiType ship;
        ship.type_id = ship_type; ship.name = "SHIP"; ship.unit_ai = true;
        host->setup.content.types = {station, ship};
        for (const foc::AiType& entry : host->setup.content.types) host->types.emplace(entry.type_id, &entry);
        view = std::make_shared<ai::WorldView>();
        view->players = {foc::detail::ViewPlayer{rebel, 1, 0}, foc::detail::ViewPlayer{empire, 2, 1}};
        add_unit(1, ship_type, rebel);
        add_unit(2, station_type, empire); // created with the battle
        host->view = view;
        host->ai_world = std::make_shared<ai::TacticalWorld>(*host);
        ai::AiData data;
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto equations = ai::EquationSet::parse({{"age.xml",
            "<Root><Age>Variable_Target.TimeLastSeenUnnormalized</Age><Fraction>Variable_Target.TimeLastSeen</Fraction></Root>"}},
            converters);
        expect(equations.has_value(), "the age equations parse");
        if (equations) data.equations = std::move(equations).value();
        engine = std::make_unique<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{});
        auth::ModuleManifest modules;
        auto made = auth::ScriptScheduler::create({}, std::move(modules));
        expect(made.has_value(), "the script scheduler creates");
        if (made) scripts.emplace(std::move(made).value());
    }

    void add_unit(eawr::sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner) {
        ai::ViewUnit unit;
        unit.id = id; unit.type = type; unit.owner = owner;
        view->units.push_back(unit);
    }

    // One tick: the session steps, then the AI barrier runs on it.
    void tick() {
        expect(session.step(executor).has_value(), "the fog fixture steps");
        view->tick = session.completed_tick();
        auto before = engine->before_service(*host->ai_world, *scripts, sequence, &executor);
        expect(before.has_value(), "the AI barrier runs");
        auto report = scripts->service(executor);
        expect(report.has_value(), "the script barrier runs");
        if (report) expect(engine->after_service(report.value()).has_value(), "the AI barrier closes");
    }

    // The ages of a world rectangle as the stock equations read them.
    std::pair<std::optional<Real>, std::optional<Real>> age(const std::int64_t x, const std::int64_t y,
        const std::int64_t width, const std::int64_t height, const tactical::PlayerId player) const {
        ai::Target target;
        target.name = "REGION";
        target.region = ai::Rect{ai::real(x), ai::real(y), ai::real(width), ai::real(height)};
        return {engine->evaluate("Age", player, &target), engine->evaluate("Fraction", player, &target)};
    }

    tactical::TacticalSession& session;
    eawr::platform::ThreadWorkerAdapter executor;
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<ai::WorldView> view;
    std::unique_ptr<ai::Engine> engine;
    std::optional<auth::ScriptScheduler> scripts;
    std::uint64_t sequence{};
};

// The mean age in seconds of `cells` cells that are each `frames` old.
Real seconds(const std::int64_t frames, const std::int64_t cells) {
    const Real fps = ai::real(static_cast<std::int64_t>(tactical::logical_frames_per_second));
    return ai::to_single(ai::real(frames * cells) / ai::to_single(ai::real(cells) * fps));
}

// The age over the game age, as the plain token computes it.
Real fraction(const Real age_seconds, const std::int64_t frame) {
    const Real fps = ai::real(static_cast<std::int64_t>(tactical::logical_frames_per_second));
    return ai::to_single(ai::to_single(age_seconds * fps) / ai::real(frame));
}

const std::string identity(64, 'a');

// Threat cell (column 9, row 16) is the one the original stamps for fog column 39, row 65 (index
// 39 / 4 = 9, 65 / 4 = 16), although the grid's cell 393.9 wide holds that fog cell in column 10.
// Its region [-2808, -2708] x [-50, 50] touches only it; (10, 16) is the next one.
constexpr std::int64_t stamped_x = -2808, neighbour_x = -2414, region_y = -50, region_size = 100;
// Threat cell (5, 5): [-4383, -4283] x [4283, 4383] touches only it.
constexpr std::int64_t anchored_region_x = -4383, anchored_region_y = 4283;

// One battle: the rebel scout reveals one fog cell until it dies at tick 5. Returns the
// observations that must not depend on the worker count.
std::vector<std::int64_t> run(const std::size_t workers) {
    std::vector<std::int64_t> observed;
    auto made = tactical::TacticalSession::create(setup(), sensors(), durability(), {}, fog_rules());
    expect(made.has_value(), "the fog fixture starts");
    if (!made) return observed;
    auto session = std::make_unique<tactical::TacticalSession>(std::move(made).value());
    expect(session->submit(tactical::PlayerCommand{{5, rebel, 1}, {1}, tactical::DamagePayload{units(1000)}}).has_value(),
        "the kill command submits");
    Rig rig(*session, workers);
    const eawr::sim::math::Vec3 revealer{units(revealer_x), units(revealer_y), Fixed{}};
    std::int64_t fogged_at = 0;
    for (std::int64_t frame = 1; frame <= 400; ++frame) {
        rig.tick();
        const tactical::FogCells* fog = rig.session.fog_cells();
        if (frame == 4) {
            const auto [age, plain] = rig.age(anchored_region_x, anchored_region_y, region_size, region_size, rebel);
            expect(age && *age == Real{}, "PE-25: a cell whose centre the player's unit reveals adds no age");
            expect(plain && *plain == Real{}, "PE-25: the plain token of an unaged region is 0");
        }
        if (fogged_at == 0 && frame > 5 && fog != nullptr && !fog->revealed(0, revealer)) fogged_at = frame;
        if (fogged_at != 0 && frame == fogged_at + 30) {
            // The cell fully fogged at `fogged_at`: thirty ticks later its threat cell is one second old.
            const auto [age, plain] = rig.age(stamped_x, region_y, region_size, region_size, rebel);
            expect(age && *age == seconds(30, 1), "PE-25: a fogged cell's age counts from the tick its fog cell reached 0");
            expect(plain && *plain == fraction(seconds(30, 1), frame), "PE-25: TimeLastSeen is that age over the game age");
            const auto [never, never_plain] = rig.age(neighbour_x, region_y, region_size, region_size, rebel);
            expect(never && *never == seconds(frame, 1), "PE-25: a never-seen fogged cell is as old as the game");
            expect(never_plain && *never_plain == ai::real(1), "PE-25: a never-seen cell's plain token is 1");
            const auto enemy = rig.age(stamped_x, region_y, region_size, region_size, empire).first;
            expect(enemy && *enemy == seconds(frame, 1), "PE-25: the age is per player: the empire never saw that cell");
            const auto anchored = rig.age(anchored_region_x, anchored_region_y, region_size, region_size, rebel).first;
            expect(anchored && *anchored == Real{}, "PE-25: the living unit's cell is still unaged");
            observed.push_back(static_cast<std::int64_t>(age->repr));
            observed.push_back(static_cast<std::int64_t>(never->repr));
            // #6942: the space box is still the declared extents, centred on the origin, with 33 x 33 cells.
            expect(rig.engine->grid().left() == ai::real(-6500) && rig.engine->grid().right() == ai::real(6500)
                    && rig.engine->grid().top() == ai::real(6500) && rig.engine->grid().bottom() == ai::real(-6500)
                    && rig.engine->grid().x_cells() == 33 && rig.engine->grid().y_cells() == 33,
                "#6942: the space grid's box is unchanged: the declared extents");
            // PG-01: the stamp lands by fog index, not by position.
            expect(rig.engine->grid().last_seen(0, 9, 16) == fogged_at && rig.engine->grid().last_seen(0, 10, 16) == 0,
                "PG-01: fog column 39 stamps threat column 9 (index / 4), not column 10 (position)");
            expect(rig.engine->grid().last_seen(1, 9, 16) == 0, "PE-25: the other player's grid is unstamped");

            // PG-03: the setup station is not a member, the ship is, and an upgrade created later joins.
            expect(rig.engine->grid().objects() == std::vector<eawr::sim::EntityId>{1}, "PG-03: the setup station is not a grid member");
            rig.add_unit(7, station_type, empire);
            rig.tick();
            expect(rig.engine->grid().objects() == (std::vector<eawr::sim::EntityId>{1, 7}),
                "PG-03: a station created later (an upgrade) joins the grid");
            const auto limit = rig.engine->grid().setup_limit();
            expect(limit > 0 && rig.engine->grid().setup_scanned(), "PG-03: the first service recorded the setup limit");

            // Save with the stamps present and restore into a fresh rig on the same session.
            auto saved = rig.engine->save_tactical_native(identity, *rig.scripts);
            expect(saved.has_value(), "the engine state saves");
            if (!saved) return observed;
            Rig again(*session, workers);
            again.view->tick = rig.view->tick;
            again.view->units = rig.view->units;
            expect(again.engine->grid().last_seen(0, 9, 16) == 0 && again.engine->grid().setup_limit() == 0
                    && !again.engine->grid().setup_scanned(),
                "restore control: a fresh engine knows neither the stamps nor the setup limit");
            // The scheduler barrier must match: bring the second scheduler to the same completed tick.
            for (std::uint64_t i = 0; i < rig.scripts->completed_tick(); ++i) {
                expect(again.scripts->service(again.executor).has_value(), "the second scheduler advances");
            }
            const auto restored = again.engine->restore_tactical_native(saved.value(), identity, *again.scripts);
            expect(restored.has_value(), "the engine state restores");
            if (restored) {
                expect(again.engine->grid().last_seen(0, 9, 16) == fogged_at,
                    "PE-25: the restored grid keeps the stamps");
                expect(again.engine->grid().setup_limit() == limit && again.engine->grid().setup_scanned(),
                    "PG-03: the restored grid keeps the setup limit");
                expect(again.engine->grid().objects() == rig.engine->grid().objects(), "PG-03: the restored members agree");
                const auto [restored_age, restored_plain] = again.age(stamped_x, region_y, region_size, region_size, rebel);
                const auto [later_age, later_plain] = rig.age(stamped_x, region_y, region_size, region_size, rebel); // one tick on
                expect(restored_age == later_age && restored_plain == later_plain && later_age && *later_age == seconds(31, 1),
                    "PE-25: the restored engine answers the same ages as the engine that saved");
                const auto resaved = again.engine->save_tactical_native(identity, *again.scripts);
                expect(resaved && resaved.value() == saved.value(), "the restored state saves the same bytes");
            }
            // A restore into a world with a different player count is refused (stamps per session player).
            auto lone = tactical::TacticalSession::create(setup(false), sensors(), durability(), {}, fog_rules());
            expect(lone.has_value(), "the one-player fixture starts");
            if (lone) {
                auto other = std::make_unique<tactical::TacticalSession>(std::move(lone).value());
                Rig mismatched(*other, workers);
                mismatched.view->tick = rig.view->tick;
                for (std::uint64_t i = 0; i < rig.scripts->completed_tick(); ++i) static_cast<void>(mismatched.scripts->service(mismatched.executor));
                expect(!mismatched.engine->restore_tactical_native(saved.value(), identity, *mismatched.scripts).has_value(),
                    "restore: stamps for two players do not load into a one-player world");
            }
        }
    }
    expect(fogged_at >= 200 && fogged_at <= 260,
        "V-15: the cell ramps down in thirteen services after the unit dies");
    observed.push_back(fogged_at);
    return observed;
}

// The fog-only part: a grid that re-records the fog mid-game agrees with one that watched all along.
void restart_mid_game() {
    auto made = tactical::TacticalSession::create(setup(), sensors(), durability(), {}, fog_rules());
    expect(made.has_value(), "the restart fixture starts");
    if (!made) return;
    auto session = std::make_unique<tactical::TacticalSession>(std::move(made).value());
    expect(session->submit(tactical::PlayerCommand{{5, rebel, 1}, {1}, tactical::DamagePayload{units(1000)}}).has_value(),
        "the kill command submits");
    auto host = std::make_shared<foc::detail::Host>();
    host->world = session.get();
    ai::ThreatGrid watching, restarted;
    const foc::AiBounds bounds{ai::real(-6500), ai::real(6500), ai::real(6500), ai::real(-6500)};
    watching.partition(bounds, 33, 33, ai::Constants{});
    restarted.partition(bounds, 33, 33, ai::Constants{});
    const eawr::platform::ThreadWorkerAdapter executor(2);
    std::chrono::nanoseconds sweep{};
    std::chrono::nanoseconds slowest{};
    for (std::int64_t frame = 1; frame <= 400; ++frame) {
        expect(session->step(executor).has_value(), "the restart fixture steps");
        const auto begin = std::chrono::steady_clock::now();
        watching.observe_fog(*host, frame);
        const auto spent = std::chrono::steady_clock::now() - begin;
        sweep += std::chrono::duration_cast<std::chrono::nanoseconds>(spent);
        slowest = std::max(slowest, std::chrono::duration_cast<std::chrono::nanoseconds>(spent));
        if (frame == 150) restarted.baseline_fog(*host);
        else if (frame > 150) restarted.observe_fog(*host, frame);
    }
    const ai::Rect region{ai::real(stamped_x), ai::real(region_y), ai::real(region_size), ai::real(region_size)};
    expect(watching.last_seen(0, 9, 16) != 0 && watching.last_seen(0, 9, 16) == restarted.last_seen(0, 9, 16),
        "PE-25: a grid that re-records the fog before the ramp ends agrees with one that watched all along");
    // Informational (not asserted): the serial sweep over two 130 x 130 fog grids, one ramp-down in the run.
    std::cout << "fog sweep: mean " << sweep.count() / 400 << " ns/tick, slowest " << slowest.count() << " ns\n";
    expect(watching.time_since_seen(*host, region, rebel, 400) == restarted.time_since_seen(*host, region, rebel, 400),
        "PE-25: and so do their ages");
}

// #6182: a land session. The terrain is 96 x 96 samples of 20 (1920 a side), so the land fog is 128 x 128 cells of 15 and,
// with four fog cells per threat cell, the threat grid is 32 x 32 cells of 60. A grid sized from the space cell size
// (1920 / 100 = 20 fog cells, 5 threat cells) would drop every fog row past 19 and stamp the wrong cells.
constexpr tactical::TypeId land_company = 61;
constexpr tactical::TypeId land_member = 62;

// The fog the production land start builds for the terrain (skirmish::land_fog_rules, DesiredLandFOWCellSize 15, 6 s regrow),
// not a hand-written grid: the 128 x 128 cells of the comment above are asserted from it below.
tactical::FogRules land_fog(const tactical::LandTerrain& terrain) {
    auto rules = eawr::skirmish::land_fog_rules(terrain.width(), terrain.height(), terrain.spacing(), units(15), units(6));
    if (!rules) throw std::runtime_error(rules.error().message);
    return rules.value();
}

tactical::TacticalContent land_content() {
    namespace lt = eawr::land_test;
    tactical::TacticalContent content;
    content.domain = tactical::BattleDomain::land;
    content.land_terrain = lt::make_field(std::vector<std::string>(96, std::string(96, '.'))).terrain;
    content.land.clear_bit = lt::clear;
    content.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), lt::whole(1)};
    tactical::LandCompanyBinding binding;
    binding.type = land_company; binding.company.id = "synthetic-company";
    binding.company.team_max_speed = lt::whole(60); binding.company.max_squad_size = 1;
    binding.company.team_collision_class = eawr::units::LandCollisionGroup::infantry;
    binding.effective_team_max_speed = lt::whole(60);
    binding.formation_spacing = Fixed::from_ratio(1, 5).value();
    eawr::units::LandCompanyMember member;
    member.type = "synthetic-member"; member.kind = eawr::units::LandObjectKind::infantry;
    member.create_team = true; member.movement_class = "walker"; member.max_speed = lt::whole(80);
    member.locomotor = "synthetic-walking";
    binding.company.members.push_back(member);
    binding.members.push_back({land_member, {lt::whole(80), lt::whole(120), lt::whole(150), lt::whole(2), lt::whole(1), false,
        eawr::units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
    content.land.companies.push_back(std::move(binding));
    content.fog = land_fog(*content.land_terrain);
    content.sensors = {tactical::SensorProfile{land_company, units(100)}};
    return content;
}

tactical::TacticalSetup land_setup() {
    tactical::TacticalSetup result;
    result.domain = tactical::BattleDomain::land;
    result.seed = 49;
    result.content_identity[0] = 7;
    result.players = {{rebel, 0, 1, tactical::player_flag_commandable}, {empire, 1, 2, tactical::player_flag_commandable}};
    const auto place = [&](eawr::sim::EntityId id, tactical::PlayerId owner, std::int64_t x, std::int64_t y) {
        tactical::UnitState unit;
        unit.entity_id = id; unit.type_id = land_company; unit.owner = owner;
        unit.position = {units(x), units(y), Fixed{}};
        result.units.push_back(unit);
    };
    place(1, rebel, 200, 200);    // the scout: walks east along the bottom of the map
    place(2, empire, 1800, 1800); // the far corner, which only the empire sees
    return result;
}

// The rebel scout walks from (200, 200) to (1500, 200). Threat cell (column 3, row 28) holds its start and (25, 28) its goal.
std::vector<std::int64_t> run_land(const std::size_t workers) {
    std::vector<std::int64_t> observed;
    auto made = tactical::TacticalSession::create(land_setup(), land_content());
    expect(made.has_value(), "the land fog fixture starts");
    if (!made) return observed;
    auto session = std::make_unique<tactical::TacticalSession>(std::move(made).value());
    const auto moved = session->submit(tactical::PlayerCommand{{2, rebel, 1}, {1},
        tactical::MovePayload{{units(1500), units(200), Fixed{}}, false, false}});
    expect(moved.has_value(), "the land move command submits");
    Rig rig(*session, workers, true);
    constexpr std::int64_t frames = 900;
    for (std::int64_t frame = 1; frame <= frames; ++frame) rig.tick();
    const auto scout = rig.session.unit_state(1);
    expect(scout && scout->position.x.raw() >= units(1450).raw(), "the land scout crossed the map");
    expect(rig.engine->grid().x_cells() == 32 && rig.engine->grid().y_cells() == 32,
        "#6182: a land threat grid is sized from the land fog grid: 128 fog cells / 4 = 32 cells a side");
    // #6942: the grid's box is the land fog rectangle (corners from the session's fog rules), not the setup's declared extents.
    const tactical::FogRules& fog_rules_land = rig.session.fog_cells()->rules();
    const Real fog_right = ai::real(static_cast<std::int64_t>(fog_rules_land.cells_wide) * 15);
    const Real fog_top = ai::real(static_cast<std::int64_t>(fog_rules_land.cells_tall) * 15);
    expect(rig.engine->grid().left() == Real{} && rig.engine->grid().bottom() == Real{}
            && rig.engine->grid().right() == fog_right && rig.engine->grid().top() == fog_top
            && rig.engine->grid().right() == ai::real(1920) && rig.engine->grid().top() == ai::real(1920),
        "#6942: the land threat grid's box is the land fog rectangle: (0, 0) to (width x spacing, height x spacing)");
    expect(rig.engine->grid().cell_width() == ai::real(60) && rig.engine->grid().cell_height() == ai::real(60),
        "#6942: a land threat cell is 60: the fog rectangle over 32 cells");
    expect(rig.host->setup.bounds && rig.host->setup.bounds->left == ai::real(-480) && rig.host->setup.bounds->top == ai::real(2400),
        "#6942: the setup's bounds (clamp, ambush walk, CELL regions) are not changed by the grid's box");
    const auto goal = rig.age(1510, 190, 20, 40, rebel);
    expect(goal.first && *goal.first == Real{} && goal.second && *goal.second == Real{},
        "#6182: TimeLastSeen is 0 where the scout is now");
    const auto start = rig.age(190, 190, 40, 40, rebel);
    const auto stamped = rig.engine->grid().last_seen(0, 3, 28);
    expect(stamped > 0 && stamped < frames, "#6182: the start cell is stamped by the land fog row / column index");
    expect(start.first && *start.first == seconds(frames - stamped, 1),
        "#6182: the cell the scout left is aged from the tick its fog cells reached 0, not from the game start");
    expect(start.second && *start.second == fraction(seconds(frames - stamped, 1), frames),
        "#6182: its plain TimeLastSeen is that age over the game age");
    const auto corner = rig.age(1810, 1870, 20, 20, rebel);
    expect(corner.first && *corner.first == seconds(frames, 1), "#6182: a never-seen land cell is as old as the game");
    const auto enemy = rig.age(1810, 1870, 20, 20, empire);
    expect(enemy.first && *enemy.first == Real{}, "#6182: the age is per player: the empire sees its own corner");
    expect(rig.engine->grid().last_seen(1, 3, 28) == 0, "#6182: the other player's grid is unstamped there");
    // #6942: every cell the index stamp reached is a cell the position read names for the scout's walk along y = 200 with its
    // sensor radius of 100: rows 27 to 30 (1920 - 300 to 1920 - 100, over 60), columns 1 to 26 (100 to 1600 over 60), one cell
    // of slack on each side for the cell edges of the reveal circle.
    std::int32_t off_walk = 0, on_walk = 0;
    for (std::int32_t row = 0; row < 32; ++row) for (std::int32_t column = 0; column < 32; ++column) {
        if (rig.engine->grid().last_seen(0, column, row) == 0) continue;
        const bool near_walk = row >= 26 && row <= 31 && column >= 0 && column <= 27;
        (near_walk ? on_walk : off_walk) += 1;
        if (!near_walk) std::cout << "off-walk stamped cell (" << column << ", " << row << ")\n";
    }
    expect(rig.engine->grid().last_seen(0, 3, 28) != 0 && on_walk > 0 && off_walk == 0,
        "#6942: the stamped cells are the ones the position read names along the scout's walk");
    // #6942: for the four corner cells and the centre cell, the cell a position at the cell's centre reads (box / count, the box
    // pinned above: left 0, top 1920, cells of 60) is the cell the index stamp names for the fog cell holding that position
    // (fog row / 4, fog column / 4; fog cells of 15 from the same corner).
    {
        const tactical::FogRules& fog = rig.session.fog_cells()->rules();
        expect(fog.cell_size == units(15) && fog.cells_wide == 128 && fog.cells_tall == 128 && fog.map_left == Fixed{}
                && fog.map_top == units(1920),
            "#6942: the land fog rectangle is 128 x 128 cells of 15 from (0, 1920)");
        const std::int32_t last = 31;
        const std::pair<std::int32_t, std::int32_t> probes[] = {{0, 0}, {last, 0}, {0, last}, {last, last}, {16, 16}};
        for (const auto& [column, row] : probes) {
            const std::int64_t x = static_cast<std::int64_t>(column) * 60 + 30;
            const std::int64_t y = 1920 - (static_cast<std::int64_t>(row) * 60 + 30);
            const std::int64_t position_column = x / 60, position_row = (1920 - y) / 60;
            const std::int64_t fog_column = x / 15, fog_row = (1920 - y) / 15;
            expect(position_column == column && position_row == row && fog_column / 4 == column && fog_row / 4 == row,
                "#6942: the index stamp names the cell the position read names, at the four corners and the centre");
        }
    }
    // #6942: a land save keeps its box. A fresh engine on the same session has no box until it restores; after the restore the
    // box is the fog rectangle again, the stamps are kept and the state saves the same bytes.
    {
        auto saved = rig.engine->save_tactical_native(identity, *rig.scripts);
        expect(saved.has_value(), "#6942: the land engine state saves");
        if (saved) {
            Rig again(*session, workers, true);
            again.view->tick = rig.view->tick;
            again.view->units = rig.view->units;
            expect(!again.engine->grid().ready() && again.engine->grid().last_seen(0, 3, 28) == 0,
                "#6942: a fresh land engine has neither the box nor the stamps");
            for (std::uint64_t i = 0; i < rig.scripts->completed_tick(); ++i)
                expect(again.scripts->service(again.executor).has_value(), "the second land scheduler advances");
            const auto restored = again.engine->restore_tactical_native(saved.value(), identity, *again.scripts);
            expect(restored.has_value(), "#6942: the land engine state restores");
            if (restored) {
                const auto& grid = again.engine->grid();
                expect(grid.left() == Real{} && grid.bottom() == Real{} && grid.right() == ai::real(1920)
                        && grid.top() == ai::real(1920) && grid.x_cells() == 32 && grid.y_cells() == 32,
                    "#6942: the restored land grid keeps the fog rectangle as its box");
                expect(grid.last_seen(0, 3, 28) == stamped, "#6942: the restored land grid keeps the stamps");
                const auto resaved = again.engine->save_tactical_native(identity, *again.scripts);
                expect(resaved && resaved.value() == saved.value(), "#6942: the restored land state saves the same bytes");
            }
        }
    }
    if (start.first && corner.first) {
        observed.push_back(static_cast<std::int64_t>(start.first->repr));
        observed.push_back(static_cast<std::int64_t>(corner.first->repr));
    }
    observed.push_back(stamped);
    return observed;
}

} // namespace

int main() {
    std::vector<std::int64_t> land_reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto observed = run_land(workers);
        if (land_reference.empty()) land_reference = observed;
        expect(!observed.empty() && observed == land_reference, "#6182: the land ages are equal on 1, 2, 4 and 8 workers");
    }
    std::vector<std::int64_t> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto observed = run(workers);
        if (reference.empty()) reference = observed;
        expect(!observed.empty() && observed == reference, "PE-25: the ages are equal on 1, 2, 4 and 8 workers");
    }
    restart_mid_game();
    std::cout << "AI cell fog tests: " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
