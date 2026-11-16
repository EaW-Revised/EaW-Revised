// #6993: on a stock land start the pre-built Light Factory sells companies (LR4-05/06). The map, the factions, the structure, its menu, the
// company prices and the build times are all read from the installed data. The test buys the AT-ST company from the factory and checks
// the credits, the queue entry, the finished company in the reinforcement pool at the build time (the documented delivery, LC-42..51:
// the player calls it in; where retail delivers it is unverified), the prerequisite gate of the tech-two company and the disabled
// upgrade entries, and gives the same state on 1 and 4 workers.
#include "eawr/skirmish/land_content.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/text.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/scene/scene.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr;
using namespace sim::tactical;

testing::Checker checks;
void expect(const bool ok, const std::string& message) { checks.check(ok, message.c_str(), std::cerr); }
template<class T> T take(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}
void must(core::Result<void> value) { if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message); }

sim::math::Fixed whole(const std::int64_t value) { return sim::math::Fixed::from_ratio(value, 1).value(); }

struct Prepared {
    skirmish::Fixture fixture;
    units::UnitTables tables;
    skirmish::LandStartInputs inputs;
    skirmish::LandStart land;
};

const PlayerEconomy* ledger_of(const TacticalSession& session, const PlayerId player) {
    for (const auto& row : session.ledgers()) if (row.player == player) return &row;
    return nullptr;
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem = take(vfs::Vfs::mount(mounts));
    const auto loaded = take(data::load_catalog(filesystem, data::Profile::foc));
    const auto maps = take(skirmish::land_setup_maps(filesystem, loaded.catalog));
    std::vector<std::string> lobby;
    for (const auto& entry : skirmish::setup_factions(loaded.catalog)) lobby.push_back(entry.name);
    const auto has = [&](const std::string_view name) {
        return std::any_of(lobby.begin(), lobby.end(), [&](const auto& row) { return core::iequals(row, name); });
    };
    expect(has("Empire") && has("Rebel"), "the catalog has the Empire and Rebel lobby factions");
    if (!has("Empire") || !has("Rebel")) return checks.finish();
    scene::VfsAssetCache cache(filesystem);

    const auto prepare = [&](const std::string& map_path, const std::optional<sim::math::Fixed> credits = std::nullopt)
        -> std::unique_ptr<Prepared> {
        auto prepared = std::make_unique<Prepared>();
        auto& fixture = prepared->fixture;
        fixture.map = map_path; fixture.seed = 6993;
        fixture.slots = {{1, "Empire", 0, true, {}}, {2, "Rebel", 1, true, {}}};
        units::UnitTables empty;
        const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, empty));
        fixture.map_sha256 = discovered.common.map_sha256;
        fixture.match = discovered.common.match_defaults;
        fixture.match->pre_built_base = true;
        if (credits) fixture.match->credits = *credits;
        units::LoadInput load;
        load.domain = BattleDomain::land; load.catalog = &loaded.catalog;
        load.filesystem = &filesystem; const auto access = cache.access(); load.model = access.model;
        std::set<std::string> requested;
        for (const auto& slot : fixture.slots) {
            const auto faction = std::find_if(discovered.factions.begin(), discovered.factions.end(),
                [&](const auto& row) { return row.faction == slot.faction; });
            if (faction != discovered.factions.end()) requested.insert(faction->forces.begin(), faction->forces.end());
        }
        if (requested.empty()) return nullptr;
        load.types.assign(requested.begin(), requested.end());
        prepared->tables = take(units::load_unit_tables(load));
        for (const auto& company : prepared->tables.land_companies)
            for (const auto& member : company.members) requested.insert(member.type);
        load.types.assign(requested.begin(), requested.end());
        prepared->tables = take(units::load_unit_tables(load));
        prepared->inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, prepared->tables));
        const auto gates = take(skirmish::authored_land_content_inputs(prepared->tables));
        prepared->land = take(skirmish::build_land_start(fixture, prepared->inputs, gates));
        return prepared;
    };

    // The first stock two-player land map that names Naboo, else the first one that starts.
    const auto naboo = [](const std::string& path) { return core::to_lower(path).find("naboo") != std::string::npos; };
    std::vector<const skirmish::SetupMap*> candidates;
    for (const auto& map : maps) {
        if (!map.unavailable.empty() || map.metadata.capacity != 2 || map.teams.size() < 2) continue;
        candidates.push_back(&map);
    }
    std::stable_partition(candidates.begin(), candidates.end(), [&](const auto* map) { return naboo(map->path); });
    std::unique_ptr<Prepared> prepared;
    const skirmish::SetupMap* chosen = nullptr;
    sim::EntityId factory = 0;
    const TypeId at_st = skirmish::type_id("Imperial_Heavy_Scout_Squad");
    const TypeId level_two_company = skirmish::type_id("Empire_MDU_Company");
    for (const auto* map : candidates) {
        auto attempt = prepare(map->path);
        if (!attempt) continue;
        // Player 1's pre-built Light Factory: the owned structure whose menu sells the AT-ST company.
        for (const auto& unit : attempt->land.start.units) {
            if (unit.state.owner != 1 || !core::iequals(unit.type, "E_Ground_Light_Vehicle_Factory")) continue;
            const auto* menu = attempt->land.content.economy.menu(unit.state.type_id, skirmish::faction_id("Empire"));
            if (menu != nullptr && menu->find(at_st) != nullptr) { factory = unit.state.entity_id; break; }
        }
        if (factory == 0) continue;
        prepared = std::move(attempt); chosen = map; break;
    }
    expect(prepared != nullptr && factory != 0, "a stock two-player land map starts the Empire with a Light Factory that sells the AT-ST company");
    if (!prepared || factory == 0) return checks.finish();
    std::cout << "map " << chosen->path << ", factory entity " << factory << '\n';
    for (const auto& note : prepared->land.notes)
        if (note.find("not offered") != std::string::npos) std::cout << "land note: " << note << '\n';

    const auto& economy = prepared->land.content.economy;
    const auto* menu = economy.menu(prepared->land.start.units[factory - 1].state.type_id, skirmish::faction_id("Empire"));
    expect(menu != nullptr && menu->station_producer, "LR4-01: the factory has a producer menu for its owner's faction");
    if (!menu) return checks.finish();
    std::size_t sold = 0, upgrades = 0;
    for (const auto& option : menu->options) {
        if (option.kind == BuildKind::unit) { if (option.available) ++sold; }
        else if (option.kind == BuildKind::upgrade) {
            ++upgrades;
            expect(!option.available && !option.disabled_reason.empty(), "LE-BONUS: an upgrade entry of the land menu is disabled with a reason");
        }
    }
    std::cout << "factory menu: " << menu->options.size() << " entries, " << sold << " companies on sale, " << upgrades << " upgrades listed\n";
    expect(sold >= 1, "the factory sells at least one company");

    const auto* option = menu->find(at_st);
    expect(option != nullptr && option->kind == BuildKind::unit && option->available && option->queue == BuildQueue::units
            && option->price == whole(900) && option->build_frames == 25 * 30 && option->requirements.prerequisites.empty(),
        "LR4-05/06: the AT-ST company is a 900 credit unit entry of 25 seconds with no prerequisite (tech 1)");
    if (!option) return checks.finish();

    const sim::InlineExecutor serial;
    auto session = take(TacticalSession::create(prepared->land.start.setup, prepared->land.content));
    expect(session.build_allowed(1, factory, at_st), "the owner may buy the AT-ST company at the start");
    expect(!session.build_allowed(2, factory, at_st), "the other player may not buy from the owner's factory");
    // A company that names a prerequisite upgrade the player does not hold is not on sale at the start.
    const auto* gated = menu->find(level_two_company);
    expect(gated != nullptr && !gated->requirements.prerequisites.empty() && !session.build_allowed(1, factory, level_two_company),
        "the tech-two company is on the menu behind its prerequisite upgrade, which the start does not hold");

    // The structures pay income, so the credits are read against a control session that never buys: the difference is the price.
    auto control = take(TacticalSession::create(prepared->land.start.setup, prepared->land.content));
    expect(ledger_of(session, 1) != nullptr && ledger_of(control, 1) != nullptr, "player 1 has an economy account");
    if (!ledger_of(session, 1) || !ledger_of(control, 1)) return checks.finish();
    const auto spent = [&] {
        const auto* bought = ledger_of(session, 1);
        const auto* idle = ledger_of(control, 1);
        return bought && idle ? sim::math::Fixed::from_raw(idle->credits.raw() - bought->credits.raw()) : sim::math::Fixed{};
    };
    must(session.submit({{0, 1, 1}, {factory}, BuyPayload{at_st}}));
    take(session.step(serial));
    take(control.step(serial));
    const auto after = ledger_of(session, 1);
    expect(after != nullptr && spent() == whole(900), "LR4-03: the credits drop by 900 when the buy is accepted");
    expect(after != nullptr && after->queues[0].size() == 1 && after->queues[0].front().type == at_st
            && after->queues[0].front().station == factory && after->queues[0].front().frames == 25 * 30 && after->pool.empty(),
        "the company is queued at its factory for the build time and is not in the pool yet");
    std::uint64_t arrived = 0;
    for (std::uint64_t tick = 1; tick < 25 * 30 + 30; ++tick) {
        take(session.step(serial));
        take(control.step(serial));
        const auto* account = ledger_of(session, 1);
        if (account && !account->pool.empty()) { arrived = tick + 1; break; }
    }
    const auto* done = ledger_of(session, 1);
    std::cout << "AT-ST company in the pool after " << arrived << " frames\n";
    expect(arrived >= 25 * 30 && arrived <= 25 * 30 + 5 && done != nullptr && done->pool.size() == 1 && done->pool.front() == at_st
            && done->queues[0].empty(),
        "LR4-05: the finished company joins the reinforcement pool at the build time (the player calls it in)");
    expect(spent() == whole(900), "finishing the build takes no further credits");
    const auto serial_state = session.canonical_state_bytes();

    // #7190: where a player may drop the company: the half-radius ring of the owner's allied reinforcement points, the first
    // point `admits` accepts, and one point far outside every radius.
    const auto drop_points = [&](const auto& admits) {
        const auto& rules = prepared->land.content.land.reinforcement;
        const auto scale = static_cast<double>(sim::math::Fixed::scale);
        const auto at_point = [&](const double x, const double y) {
            return sim::math::Vec3{sim::math::Fixed::from_raw(static_cast<std::int64_t>(x * scale)),
                sim::math::Fixed::from_raw(static_cast<std::int64_t>(y * scale)), sim::math::Fixed{}};
        };
        std::pair<std::optional<sim::math::Vec3>, std::optional<sim::math::Vec3>> found;
        for (const auto& unit : prepared->land.start.units) {
            const auto* point = rules.point(unit.state.type_id);
            if (unit.state.owner != 1 || point == nullptr || point->enabling_radius.raw() <= 0) continue;
            const double px = static_cast<double>(unit.state.position.x.raw()) / scale;
            const double py = static_cast<double>(unit.state.position.y.raw()) / scale;
            const double radius = static_cast<double>(point->enabling_radius.raw()) / scale;
            if (!found.second) found.second = at_point(px + radius * 40.0, py + radius * 40.0);
            for (int step = 0; step < 8 && !found.first; ++step) {
                const double angle = 3.14159265358979 * 2.0 * step / 8.0;
                const auto candidate = at_point(px + radius * 0.5 * std::cos(angle), py + radius * 0.5 * std::sin(angle));
                if (admits(candidate)) found.first = candidate;
            }
        }
        return found;
    };

    // #7118: the bought company is called down (Reinforce from the pool) and then counts against the population. LC-107 ASSUMPTION,
    // unobserved in retail (#7048): the factory-bought company counts like any called-down company once it lands; the free start
    // forces count for nothing (LC-50, 0/9), so the count before the call-down is 0 and the count after is the company's value.
    {
        std::uint32_t company_population = 0;
        for (const auto& binding : prepared->land.content.land.companies)
            if (binding.type == at_st) company_population = binding.company.population_value.value_or(0);
        std::cout << "AT-ST company population value " << company_population << '\n';
        expect(company_population > 0, "the AT-ST company authors a population value to count");
        const auto owner_view = [](const TacticalSnapshot& snapshot) -> const EconomyView* {
            for (const auto& row : snapshot.economy()) if (row.player == 1) return &row;
            return nullptr;
        };
        const auto idle_step = take(session.step(serial));
        const auto* waiting = owner_view(*idle_step.snapshot);
        expect(waiting != nullptr && waiting->population == 0 && waiting->pool.size() == 1,
            "LC-107: a finished company waiting in the pool is not counted against the population");
        // #7190: the player's call-down. The drop preview and the drop check ask the session where the company may land
        // (reinforcement_point: the land rules, read-only), then the player's Reinforce command carries the click. The point
        // is found the way a player would, inside an allied reinforcement point's enabling radius; a point out of every
        // radius is refused by the query and, as a command, by the simulation with its own reason.
        const auto [good, far_away] = drop_points([&](const sim::math::Vec3& candidate) {
            const auto verdict = session.reinforcement_point(1, at_st, candidate);
            return verdict && verdict.value();
        });
        expect(good.has_value(), "#7190: the query admits the bought company inside an allied reinforcement point's radius");
        expect(far_away.has_value(), "the owner starts with an allied reinforcement point");
        if (good && far_away) {
            const auto before = session.canonical_state_bytes();
            const auto inside = session.reinforcement_point(1, at_st, *good);
            const auto outside = session.reinforcement_point(1, at_st, *far_away);
            const auto wrong_company = session.reinforcement_point(1, 1, *good);
            expect(inside && inside.value(), "#7190: the same point is admitted again (the query keeps nothing)");
            expect(outside && !outside.value(), "#7190: a point out of every allied reinforcement radius is refused by the query");
            expect(wrong_company && !wrong_company.value(), "#7190: a type that is no land company is refused by the query");
            expect(session.canonical_state_bytes() == before, "#7190: the query writes nothing to the session state");
            const auto call_tick = session.completed_tick() + 2;
            std::cout << "population cap " << (waiting != nullptr ? waiting->population_cap : 0U) << ", call-down tick " << call_tick << '\n';
            // The refused click first (the pool entry stays), then the click inside the radius.
            must(session.submit({{call_tick, 1, 99}, {}, ReinforcePayload{at_st, *far_away}}));
            must(session.submit({{call_tick + 1, 1, 100}, {}, ReinforcePayload{at_st, *good}}));
        }
        bool unloaded = false, refused_far = false, accepted_good = false;
        RejectReason far_reason = RejectReason::none;
        std::uint32_t landed_population = 0;
        for (std::uint64_t step = 0; step < 900 && !unloaded; ++step) {
            const auto stepped = take(session.step(serial));
            for (const auto& event : stepped.snapshot->events()) {
                if (event.sequence == 99 && event.kind == EventKind::order_rejected) { refused_far = true; far_reason = event.reason; }
                if (event.sequence == 100 && event.kind == EventKind::order_accepted) accepted_good = true;
                if (event.sequence == 100 && event.kind == EventKind::order_rejected)
                    std::cout << "call-down rejected, reason " << static_cast<int>(event.reason) << " at tick " << event.tick << '\n';
                if (event.kind == EventKind::reinforcement_unloaded) unloaded = true;
            }
            if (!unloaded) continue;
            const auto* view = owner_view(*stepped.snapshot);
            if (view != nullptr) landed_population = view->population;
        }
        std::cout << "call-down: accepted " << accepted_good << ", unloaded " << unloaded
            << ", landings in flight " << session.land_landings().size() << ", tick " << session.completed_tick() << '\n';
        expect(refused_far && far_reason == RejectReason::invalid_position,
            "#7190: a drop out of every radius is rejected with the simulation's invalid_position reason");
        expect(accepted_good, "LC-107: the player's drop inside the radius is accepted and the bought company is called down from the pool");
        expect(unloaded, "the called-down company unloads its members");
        const auto* after_landing = ledger_of(session, 1);
        expect(after_landing != nullptr && after_landing->pool.empty(), "the called-down company leaves the pool");
        expect(landed_population > 0 && landed_population <= company_population,
            "LC-107 (assumed, retail unobserved #7048): the called-down bought company counts against the population");
        std::cout << "population after the call-down " << landed_population << " of " << company_population << '\n';
    }

    // #7190: the same player path through the live adapter the viewer drives (LiveSession): the buy, the pool entry, the drop
    // preview and drop check (reinforcement_point), the Reinforce command and the landed company in the published snapshot.
    {
        platform::LiveSession::Options options;
        options.workers = 2;
        options.pacing = platform::LiveSession::Pacing::driven;
        auto live = take(platform::LiveSession::start(prepared->land.start.setup, prepared->land.content, options));
        must(live->submit({1, {factory}, BuyPayload{at_st}, std::nullopt}));
        const std::uint64_t built = arrived + 5;
        live->advance_to(built);
        expect(live->wait_for(built, std::chrono::seconds(120)), "live land session reaches the end of the company's build");
        const auto owner_row = [](const TacticalSnapshot& snapshot) -> const EconomyView* {
            for (const auto& row : snapshot.economy()) if (row.player == 1) return &row;
            return nullptr;
        };
        const auto frame = live->frame();
        const auto* pooled = frame.latest ? owner_row(*frame.latest) : nullptr;
        expect(pooled != nullptr && pooled->pool.size() == 1 && pooled->pool.front() == at_st,
            "#7190: the bought company is in the pool the reinforcement pane lists");
        const auto instances_before = frame.latest ? frame.latest->instances().size() : 0U;
        const auto [live_good, live_far] = drop_points([&](const sim::math::Vec3& candidate) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (const auto verdict = live->reinforcement_point(1, at_st, candidate)) return *verdict;
            }
            return false;
        });
        expect(live_good.has_value() && live_far.has_value(), "#7190: the live drop check admits a point inside an allied reinforcement radius");
        if (live_good && live_far) {
            const auto outside = live->reinforcement_point(1, at_st, *live_far);
            expect(outside.has_value() && !*outside, "#7190: the live drop check refuses a point out of every radius");
            must(live->submit({1, {}, ReinforcePayload{at_st, *live_good}, std::nullopt}));
            const std::uint64_t landed = built + 900;
            live->advance_to(landed);
            expect(live->wait_for(landed, std::chrono::seconds(120)), "live land session runs through the company's landing");
            const auto after_frame = live->frame();
            const auto* after_row = after_frame.latest ? owner_row(*after_frame.latest) : nullptr;
            expect(after_row != nullptr && after_row->pool.empty(), "#7190: the dropped company leaves the pool");
            expect(after_frame.latest && after_frame.latest->instances().size() > instances_before,
                "#7190: the dropped company's members stand in the published snapshot");
            expect(after_row != nullptr && after_row->population > 0, "#7190: the landed company counts against the population");
        }
        live->stop();
        expect(!live->failure(), "the live land session raised no failure");
    }

    // #7190: at the cap. The same company, the same open point, but a land cap one below the company's population: the query
    // refuses (no room), keeps nothing, and the Reinforce command at that point is rejected with the simulation's no-room reason.
    {
        std::uint32_t company_population = 0;
        for (const auto& binding : prepared->land.content.land.companies)
            if (binding.type == at_st) company_population = binding.company.population_value.value_or(0);
        // A point the uncapped rules admit, found on the control session (it never bought, so nothing stands near the points).
        const auto open = drop_points([&](const sim::math::Vec3& candidate) {
            const auto verdict = control.reinforcement_point(1, at_st, candidate);
            return verdict && verdict.value();
        }).first;
        expect(open.has_value() && company_population > 0, "#7190: an open point exists for the at-cap case");
        if (open && company_population > 0) {
            auto capped_content = prepared->land.content;
            for (auto& row : capped_content.land.reinforcement.caps) row.value = static_cast<std::int64_t>(company_population) - 1;
            auto capped = take(TacticalSession::create(prepared->land.start.setup, capped_content));
            must(capped.submit({{0, 1, 1}, {factory}, BuyPayload{at_st}}));
            for (std::uint64_t tick = 0; tick < arrived; ++tick) take(capped.step(serial));
            const auto* capped_ledger = ledger_of(capped, 1);
            expect(capped_ledger != nullptr && capped_ledger->pool.size() == 1, "the capped session has the bought company in its pool");
            const auto before = capped.canonical_state_bytes();
            const auto refused = capped.reinforcement_point(1, at_st, *open);
            expect(refused && !refused.value(), "#7190: the query refuses a company that does not fit under the cap");
            expect(capped.canonical_state_bytes() == before, "#7190: the refused query wrote nothing either");
            const auto call_tick = capped.completed_tick() + 2;
            must(capped.submit({{call_tick, 1, 101}, {}, ReinforcePayload{at_st, *open}}));
            bool no_room = false;
            RejectReason reason = RejectReason::none;
            for (int step = 0; step < 10; ++step) {
                const auto stepped = take(capped.step(serial));
                for (const auto& event : stepped.snapshot->events())
                    if (event.sequence == 101 && event.kind == EventKind::order_rejected) { no_room = true; reason = event.reason; }
            }
            expect(no_room && reason == RejectReason::no_population_room,
                "#7190: the Reinforce at that point is rejected with the simulation's no_population_room reason");
            const auto* kept = ledger_of(capped, 1);
            expect(kept != nullptr && kept->pool.size() == 1, "the rejected drop leaves the company in the pool");
        }
    }

    // The same buy on 4 workers is the same state.
    platform::ThreadWorkerAdapter threads(4);
    auto parallel = take(TacticalSession::create(prepared->land.start.setup, prepared->land.content));
    must(parallel.submit({{0, 1, 1}, {factory}, BuyPayload{at_st}}));
    for (std::uint64_t tick = 0; tick < arrived; ++tick) take(parallel.step(threads));
    expect(parallel.canonical_state_bytes() == serial_state, "the factory buy gives the same state on 1 and 4 workers");

    // Too few credits: the buy is refused, nothing is debited and nothing is queued or pooled.
    const auto poor = prepare(chosen->path, whole(899));
    expect(poor != nullptr && poor->land.start.units.size() >= factory
            && core::iequals(poor->land.start.units[factory - 1].type, "E_Ground_Light_Vehicle_Factory"),
        "the same map starts again with 899 credits and the same factory entity");
    if (poor && poor->land.start.units.size() >= factory) {
        auto broke = take(TacticalSession::create(poor->land.start.setup, poor->land.content));
        auto idle = take(TacticalSession::create(poor->land.start.setup, poor->land.content));
        must(broke.submit({{0, 1, 1}, {factory}, BuyPayload{at_st}}));
        bool refused = false;
        for (int tick = 0; tick < 40; ++tick) {
            const auto stepped = take(broke.step(serial));
            take(idle.step(serial));
            for (const auto& event : stepped.snapshot->events())
                if (event.kind == EventKind::order_rejected && event.reason == RejectReason::insufficient_credits) refused = true;
        }
        const auto* kept = ledger_of(broke, 1);
        const auto* untouched = ledger_of(idle, 1);
        expect(refused, "a buy with too few credits is refused for the credits");
        expect(kept != nullptr && untouched != nullptr && kept->credits == untouched->credits,
            "the refused buy takes no credits");
        expect(kept != nullptr && kept->queues[0].empty() && kept->queues[1].empty() && kept->pool.empty(),
            "the refused buy leaves no queue entry and no pool entry");
    }
    return checks.finish();
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        std::cout << "SKIPPED: the stock land factory buy test needs --installed\n";
        return 77;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
