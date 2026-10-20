// The AI's per-cell "time since seen" (docs/behaviour/foc-tactical-ai.md PE-25, PG-01, PG-03): a
// threat cell is stamped when one of its fog cells fully fogs out, and a region's age averages the
// fogged cells it touches. The grid reads the session's real fog, also for an AI player. These
// tests check the formulas against the documented rules (no retail table cell is pinned here; the
// scouting desire values are checked by the stock-after measurement of the PR).

#include <cstddef>
#include <cstdint>
#include <utility>
#include <string_view>
#include "../../support/check.hpp"
#include "ai_engine.hpp"
#include "tactical_ai_world.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/tactical/session.hpp"

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
    Rig(tactical::TacticalSession& session_ref, const std::size_t workers) : session(session_ref), executor(workers) {
        host = std::make_shared<foc::detail::Host>();
        host->world = &session;
        host->setup.bounds = foc::AiBounds{ai::real(-6500), ai::real(6500), ai::real(6500), ai::real(-6500)};
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

} // namespace

int main() {
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
