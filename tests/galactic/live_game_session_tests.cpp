#include "galactic_support.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/campaign_battle_view.hpp"
#include "eawr/presentation/audio/health_warning.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace {
using namespace galactic_test;
using eawr::platform::LiveGameSession;
using eawr::platform::LiveGameOrder;
using eawr::sim::math::Fixed;
using namespace std::chrono_literals;

galactic::GalacticSetup setup() {
    return galactic::synthetic_galaxy({.seed = 91, .planets = 16, .players = 3,
        .fleets_per_player = 2, .members_per_fleet = 2, .debug_commands = true, .coop_seat = true});
}
void retained_campaign_root() {
    {
        eawr::campaign::ConflictRecord flattened;
        flattened.forces = {{20, 202, 1, 10}}; // ordinary parent 10 was flattened away
        const std::map<galactic::TypeId, bool> flags{{101, true}, {202, false}};
        expect(!eawr::platform::campaign_story_prevention(true, flattened, flags).has_value(),
            "WAR-10: flagged parent with unflagged child cannot become a proven negative from the child manifest");
        flattened.forces.push_back({10, 101, 1});
        expect(eawr::platform::campaign_story_prevention(true, flattened, flags) == true,
            "WAR-10: a represented flagged parent proves prevention");
        expect(eawr::platform::campaign_story_prevention(false, flattened, flags) == false,
            "WAR-10: ordinary campaigns do not consult the story-only prevention flag");
    }
    namespace cp = eawr::campaign;
    eawr::data::galactic::GalacticData data;
    eawr::data::galactic::Faction human; human.name = "Human";
    eawr::data::galactic::Faction neutral; neutral.name = "Neutral"; neutral.is_neutral = true;
    data.factions = {human, neutral};
    eawr::data::galactic::Planet planet; planet.name = "Alpha";
    planet.galactic_position = eawr::data::galactic::Position{}; planet.galactic_behaviors = {"PLANET"}; data.planets = {planet};
    eawr::data::galactic::Campaign authored; authored.name = "Retained_Root"; authored.planets = {"Alpha"}; authored.locations = {"Alpha"};
    data.campaigns = {authored};
    data.constants = {{"MaximumFleetMovementDistance", {}, Fixed::from_raw(100 * Fixed::scale)},
        {"Fiscal_Cycle_Time_In_Secs", {}, Fixed::from_raw(2 * Fixed::scale)}};
    cp::CampaignSetupOptions options; options.seed = 91;
    options.players = {{{1, 1}, "Human", true}, {{2, 2}, "Neutral", true}};
    options.seats = {{{1}, 1, eawr::sim::SeatKind::human, 1, true, 1}};
    auto plan = cp::plan_campaign_setup(data, {}, authored.name, options);
    if (!plan) std::cerr << plan.error().message << '\n';
    expect(static_cast<bool>(plan), "retained live root plans"); if (!plan) return;
    unsigned locations{}, assignments{};
    cp::CampaignSetupServices services;
    services.location = [&](const auto&) { ++locations; return eawr::core::Result<void>::success(); };
    services.home = [](const auto&) { return eawr::core::Result<void>::success(); };
    services.political_control = [](const auto&, auto, const auto&) { return eawr::core::Result<galactic::PlayerId>::success(1); };
    services.credit_cap = [](const auto&, const auto&) { return eawr::core::Result<Fixed>::success(Fixed::from_raw(100 * Fixed::scale)); };
    services.assign_tech = [&](const auto& request, const auto&) { ++assignments; return eawr::core::Result<std::int64_t>::success(request.requested_current); };
    auto root = cp::CampaignRoot::create(plan.value(), services);
    expect(root && locations == 1 && assignments == 2, "root startup assigns each player once"); if (!root) return;
    const auto initial = root.value().game()->record().setup;
    auto live = LiveGameSession::start(std::move(root).value(), {.workers = 2, .pacing = LiveGameSession::Pacing::driven});
    expect(static_cast<bool>(live), "initialized campaign root transfers to the live pacer"); if (!live) return;
    expect(live.value()->record().setup == initial && live.value()->frame().latest->planets()[0].owner == 1,
        "root transfer preserves initialized ownership, economy and seed without recreation");
    live.value()->advance_to(3); expect(live.value()->wait_for(3, 5s), "retained root advances through ordinary live pacing");
    live.value()->stop();
    expect(locations == 1 && assignments == 2 && !live.value()->failure(), "live ownership never reruns startup providers");
    root = cp::CampaignRoot::create(plan.value(), services); root.value().stop_root();
    expect(!LiveGameSession::start(std::move(root).value(), {}), "stopped root starts no thread");
}

void replay_parity() {
    const auto initial = setup();
    const auto input = script(initial, 50);
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto started = LiveGameSession::start(initial, {.workers = workers, .pacing = LiveGameSession::Pacing::driven, .entries = {}});
        expect(static_cast<bool>(started), "live session starts");
        if (!started) continue;
        auto& live = *started.value();
        const auto zero = live.frame();
        expect(zero.latest && zero.latest == zero.previous && zero.latest->completed_tick() == 0, "tick zero published");
        // Reverse across ticks: future exact commands wait without blocking earlier seat input.
        for (auto at = input.rbegin(); at != input.rend(); ++at) live.submit(*at);
        expect(!live.wait_for(1, 20ms), "driven input waits for an advance target");
        std::atomic<bool> done{false};
        std::atomic<bool> ordered{true};
        std::thread reader([&] {
            std::uint64_t last = 0;
            while (!done.load()) {
                const auto frame = live.frame();
                if (!frame.latest || !frame.previous || frame.previous->completed_tick() > frame.latest->completed_tick()
                    || frame.latest->completed_tick() < last) ordered = false;
                last = frame.latest->completed_tick();
                std::this_thread::yield();
            }
        });
        live.advance_to(50);
        expect(live.wait_for(50, 10s), "driven live run reaches target");
        live.stop();
        live.stop();
        done = true;
        reader.join();
        expect(ordered.load(), "concurrent presentation reads ordered immutable frames");
        expect(!live.failure() && live.rejected_orders().empty(), "valid commands accepted");
        const auto frame = live.frame();
        expect(frame.latest->completed_tick() == 50 && frame.previous->completed_tick() == 49, "two newest snapshots retained");
        expect(zero.latest->completed_tick() == 0, "retained old snapshot stays immutable");
        const auto record = live.record();
        expect(record.commands == input && record.final_tick_count == 50, "live recording preserves canonical input");
        const auto bytes = galactic::write_replay(record);
        expect(static_cast<bool>(bytes), "live record serializes");
        const auto parsed = galactic::parse_replay(bytes.value());
        expect(parsed && parsed.value() == record, "recorded replay round trips");
        auto replayed = eawr::campaign::GameSession::from_replay(parsed.value()).value();
        const eawr::sim::InlineExecutor executor;
        std::vector<std::string> headless;
        while (replayed.completed_tick() < record.final_tick_count) headless.push_back(replayed.step(executor).value().state_sha256);
        expect(live.tick_hashes() == headless, "live equals headless replay");
        if (baseline.empty()) baseline = headless;
        expect(headless == baseline, "1/2/4/8 live worker parity");
    }
}

void pause_and_sequences(const LiveGameSession::Pacing pacing) {
    auto started = LiveGameSession::start(setup(), {.workers = 2, .pacing = pacing,
        .command_source = [](const std::uint64_t tick) {
            if (tick == 0) return std::vector<galactic::GameCommand>{{{0, {1}, 1}, galactic::PauseGalaxy{}}};
            return std::vector<galactic::GameCommand>{};
        }, .entries = {}});
    expect(static_cast<bool>(started), "pause test starts");
    if (!started) return;
    auto& live = *started.value();
    live.advance_to(1);
    expect(live.wait_for(1, 5s), "pause command executes");
    const auto paused = live.frame().latest;
    expect(paused->clock().pause_depth == 1, "pause is authoritative snapshot state");
    expect(std::any_of(paused->events().begin(), paused->events().end(), [](const auto& event) {
        return event.kind == galactic::GalacticEventKind::galaxy_paused && event.producer == eawr::sim::SeatId{1};
    }), "presentation receives the pause event in its immutable snapshot");
    const auto tick = paused->completed_tick();
    live.advance_to(tick + 100);
    expect(!live.wait_for(tick + 1, 80ms), "paused session produces no empty ticks even with a driven target");
    // A future command cannot make a paused session manufacture empty ticks.
    live.submit(LiveGameOrder{{1}, galactic::DebugSetPlanetOwner{setup().planets[0].id, 2}, tick + 3});
    expect(!live.wait_for(tick + 1, 40ms), "future paused order stays held");
    live.submit(LiveGameOrder{{1}, galactic::PauseGalaxy{}, {}});
    expect(live.wait_for(tick + 1, 5s), "nested pause command drives one paused tick");
    expect(live.frame().latest->clock().pause_depth == 2, "pause depth nests");
    live.submit(LiveGameOrder{{1}, galactic::ResumeGalaxy{}, {}});
    expect(live.wait_for(tick + 2, 5s), "resume command drives one paused tick");
    expect(live.frame().latest->clock().pause_depth == 1, "one resume keeps nested pause");
    // Coop seat 4 shares faction 1 but owns an independent sequence.
    live.submit(LiveGameOrder{{4}, galactic::SetSpeed{false, Fixed::from_raw(2 * Fixed::scale)}, {}});
    expect(live.wait_for(tick + 3, 5s), "coop seat speed command executes while paused");
    expect(live.frame().latest->clock().strategic_speed.raw() == 2 * Fixed::scale, "speed reaches snapshot");
    expect(live.wait_for(tick + 4, 5s), "held future order executes when its paused tick becomes current");
    live.submit(LiveGameOrder{{1}, galactic::DebugSetPlanetOwner{setup().planets[1].id, 2}, {}});
    expect(live.wait_for(tick + 5, 5s), "untimed order executes after held order");
    expect(live.frame().latest->clock().galactic_frame == paused->clock().galactic_frame, "paused commands do not service galaxy");
    expect(!live.wait_for(tick + 6, 40ms), "after paused input session parks again");
    live.submit(galactic::GameCommand{{tick - 1, {1}, 500}, galactic::ResumeGalaxy{}});
    expect(!live.wait_for(tick + 6, 40ms), "late rejected input creates no tick");
    expect(!live.rejected_orders().empty() && live.rejected_orders().back().code == galactic::diagnostic_codes::late_command,
        "late command reports refusal");
    live.submit(LiveGameOrder{{999}, galactic::ResumeGalaxy{}, {}});
    expect(!live.wait_for(tick + 6, 40ms), "unknown seat input creates no paused tick");
    expect(live.rejected_orders().back().code == galactic::diagnostic_codes::unknown_issuer, "unknown seat reports refusal");
    live.stop();
    const auto record = live.record();
    expect(record.commands.size() == 6, "only accepted commands recorded");
    std::uint64_t seat_one_sequence = 0;
    for (const auto& command : record.commands) {
        if (command.key.seat == eawr::sim::SeatId{1}) {
            expect(command.key.sequence == ++seat_one_sequence, "future orders allocate sequence only when due");
        }
    }
    auto replayed = eawr::campaign::GameSession::from_replay(record).value();
    const eawr::sim::InlineExecutor executor;
    std::vector<std::string> hashes;
    while (replayed.completed_tick() < record.final_tick_count) hashes.push_back(replayed.step(executor).value().state_sha256);
    expect(hashes == live.tick_hashes(), "paused live recording replays all control ticks");
}

void resume_and_failure() {
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> on_sim_thread{false};
    auto live = LiveGameSession::start(setup(), {.pacing = LiveGameSession::Pacing::driven,
        .command_source = [&](const std::uint64_t tick) {
            on_sim_thread = std::this_thread::get_id() != caller;
            if (tick == 0) return std::vector<galactic::GameCommand>{{{0, {1}, 10}, galactic::PauseGalaxy{}}};
            return std::vector<galactic::GameCommand>{};
        }, .entries = {}}).value();
    live->advance_to(5);
    expect(live->wait_for(1, 5s) && !live->wait_for(2, 40ms), "sourced pause stops driven pacing");
    expect(on_sim_thread.load(), "command source executes on simulation thread");
    live->submit(LiveGameOrder{{1}, galactic::ResumeGalaxy{}, {}});
    expect(live->wait_for(5, 5s), "resume releases existing driven target");
    live->stop();
    expect(live->record().commands.back().key.sequence == 11, "untimed command follows sourced seat sequence");

    for (const bool standard : {true, false}) {
        auto broken = LiveGameSession::start(setup(), {.pacing = LiveGameSession::Pacing::driven,
            .command_source = [standard](const std::uint64_t tick) -> std::vector<galactic::GameCommand> {
                if (tick == 2) {
                    if (standard) throw std::runtime_error("injected failure");
                    throw 42;
                }
                return {};
            }, .entries = {}}).value();
        broken->advance_to(10);
        expect(!broken->wait_for(10, 5s), "failure wakes waiters without completing target");
        expect(broken->failure() && broken->failure()->code == galactic::diagnostic_codes::worker_failure, "thread exception isolated");
        expect(broken->completed_tick() == 2 && broken->frame().previous->completed_tick() == 1, "last successful frames survive failure");
        broken->stop();
        expect(broken->tick_hashes().size() == 2 && broken->record().final_tick_count == 2, "failed live run retains completed recording");
    }
    auto stopped = LiveGameSession::start(setup(), {.pacing = LiveGameSession::Pacing::driven, .entries = {}}).value();
    std::atomic<bool> woke{false};
    std::thread waiter([&] { woke = !stopped->wait_for(1, 5s); });
    stopped->stop();
    waiter.join();
    expect(woke.load() && stopped->completed_tick() == 0, "stop wakes waiters while parked");
    expect(!LiveGameSession::start(setup(), {.workers = 0, .entries = {}}), "zero workers rejected");
    expect(!LiveGameSession::start(setup(), {.workers = 257, .entries = {}}), "excess workers rejected");
    auto invalid = setup();
    invalid.planets[0].owner = 999;
    expect(!LiveGameSession::start(invalid, {}), "bad setup starts no thread");
}

void intervals() {
    expect(LiveGameSession::tick_interval(Fixed::from_raw(Fixed::scale)) == 33'333'333ns, "base 30 Hz interval");
    expect(LiveGameSession::tick_interval(Fixed::from_raw(2 * Fixed::scale)) == 16'666'666ns, "double speed only changes pacing");
    expect(LiveGameSession::tick_interval(Fixed::from_raw(Fixed::scale / 2)) == 66'666'666ns, "half speed pacing");
    expect(LiveGameSession::tick_interval(Fixed::from_raw(std::numeric_limits<std::int64_t>::max()))
        == LiveGameSession::tick_interval(Fixed::from_raw(galactic::max_speed_factor_raw)), "interval arithmetic clamps without overflow");
}

// Ticks a pacer completes in `seconds` of simulated wall time when each wake-up arrives `latency`
// after the due time and each tick costs `cost`. Pure arithmetic over LivePaceSchedule.
std::uint64_t scheduled_ticks(const Fixed speed, const std::chrono::nanoseconds latency,
    const std::chrono::nanoseconds cost, const std::chrono::seconds seconds) {
    using Clock = eawr::platform::LivePaceSchedule::Clock;
    const Clock::time_point begin{};
    eawr::platform::LivePaceSchedule schedule;
    schedule.rebase(begin, LiveGameSession::tick_interval(speed));
    auto now = begin;
    std::uint64_t ticks = 0;
    while (now - begin < seconds) {
        now = std::max(now, schedule.next_due() + latency);
        schedule.drop_backlog(now, 5);
        now += cost;
        schedule.stepped();
        ++ticks;
    }
    return ticks;
}

void applied_speed_schedule() {
    // The applied factor, not the setting: ticks per real second at 1x versus the fast-forward factor.
    const Fixed one = Fixed::from_raw(Fixed::scale);
    const Fixed fast = Fixed::from_raw(galactic::fast_forward_factor_raw);
    for (const std::chrono::nanoseconds latency : {0ns, std::chrono::nanoseconds(1ms), std::chrono::nanoseconds(2ms)}) {
        const auto slow_ticks = scheduled_ticks(one, latency, 2ms, 10s);
        const auto fast_ticks = scheduled_ticks(fast, latency, 2ms, 10s);
        expect(slow_ticks >= 299 && slow_ticks <= 301, "1x schedule runs 30 ticks per real second");
        expect(fast_ticks >= 1199 && fast_ticks <= 1201, "fast forward schedule runs 120 ticks per real second whatever the wake latency");
    }
    // Tick cost above the interval: bound by cost, no catch-up burst beyond max_catch_up, no stall.
    const auto bound = scheduled_ticks(fast, 0ns, 12ms, 10s);
    expect(bound >= 800 && bound <= 840, "a 12 ms tick caps fast forward near 83 ticks per second");
    // A speed change starts a fresh interval.
    eawr::platform::LivePaceSchedule schedule;
    const auto begin = eawr::platform::LivePaceSchedule::Clock::time_point{};
    schedule.rebase(begin, LiveGameSession::tick_interval(one));
    schedule.stepped();
    schedule.rebase(begin + 1s, LiveGameSession::tick_interval(fast));
    expect(schedule.next_due() == begin + 1s + LiveGameSession::tick_interval(fast), "rebase restarts at the new interval");
    expect(schedule.behind(begin + 1s) == 0 && schedule.behind(begin + 1s + 2 * LiveGameSession::tick_interval(fast)) == 2,
        "behind counts the steps due and not yet run");
}

// Live thread, observation only: prints the measured ticks per real second at 1x and fast forward.
void applied_speed_live() {
    // One human seat: TM-04 refuses the fast-forward factor when several seats are active.
    auto single = galactic::synthetic_galaxy({.seed = 91, .planets = 16, .players = 3, .fleets_per_player = 2, .members_per_fleet = 2});
    for (auto& seat : single.seats) {
        if (seat.seat_id == eawr::sim::SeatId{1}) continue;
        seat.kind = galactic::SeatKind::ai;
        seat.human_side_id = 0;
    }
    auto started = LiveGameSession::start(single, {.workers = 2, .pacing = LiveGameSession::Pacing::real_time, .entries = {}});
    expect(static_cast<bool>(started), "applied speed session starts");
    if (!started) return;
    auto& live = *started.value();
    const auto rate = [&](const std::chrono::milliseconds window) {
        const auto before = live.completed_tick();
        const auto at = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(window);
        const auto ticks = live.completed_tick() - before;
        return static_cast<double>(ticks) / std::chrono::duration<double>(std::chrono::steady_clock::now() - at).count();
    };
    std::this_thread::sleep_for(100ms);
    const double normal = rate(1500ms);
    live.submit(LiveGameOrder{{1}, galactic::SetSpeed{false, Fixed::from_raw(galactic::fast_forward_factor_raw)}, {}});
    expect(live.wait_for(live.completed_tick() + 2, 5s), "fast forward command executes");
    expect(live.frame().latest->clock().strategic_speed.raw() == galactic::fast_forward_factor_raw, "fast forward reaches the snapshot");
    std::this_thread::sleep_for(100ms);
    const double fast = rate(1500ms);
    std::cout << "live pacing ticks/s: 1x " << normal << ", fast-forward " << fast << ", ratio " << fast / normal << '\n';
    const auto costs = live.tick_costs_after(0);
    expect(!costs.empty() && costs.back().phases.size() == 2 && costs.back().total_ms >= 0.0, "each parent step records its cost");
    live.stop();
    // Observation only: wall-clock rates on a loaded host are not a gate. The gate is the schedule arithmetic
    // above (applied_speed_schedule), which runs on injected time.
    static_cast<void>(normal);
    static_cast<void>(fast);
}

void observer_story_publication() {
    galactic::GalacticSetup initial;
    initial.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}, {4, 4, 0}, {5, 5, 0}};
    initial.seats = {{{1}, 1, galactic::SeatKind::human, 0, true, 1},
        {{2}, 2, galactic::SeatKind::human, 1, true, 2}, {{3}, 3, galactic::SeatKind::human, 2, true, 2},
        {{4}, 4, galactic::SeatKind::ai, 3, true, 0}, {{5}, 1, galactic::SeatKind::human, 0, false, 1}};
    initial.planets = {{1, {}, 1}, {2, {}, 2}};
    auto& invader = initial.fleets.emplace_back();
    invader.id = 10; invader.owner = 1; invader.orbit = 2; invader.members = {{20, 2}};
    initial.rules.maximum_fleet_movement_distance = Fixed::from_raw(Fixed::scale);
    initial.fleet_control.emplace(); initial.fleet_control->types = {{2, true, true, false}};
    initial.control.emplace(); auto& control = *initial.control;
    control.change_seconds = Fixed::from_raw(7 * Fixed::scale); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, false, true, 3}, {4, false, true, 4}, {5, true, false, 5}};
    for (const auto& a : control.players) for (const auto& b : control.players)
        control.relations.push_back({a.player, b.player, a.player == b.player});
    control.types = {{2, 0, false, true}};
    for (const auto& planet : initial.planets) {
        galactic::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id; row.surface_accessible = true;
        control.planets.push_back(row);
        galactic::PlanetControl retained; retained.planet = planet.id; retained.initialized = true;
        retained.current_player = planet.owner; retained.target_player = planet.owner;
        retained.current = Fixed::from_raw(5 * Fixed::scale); retained.target = retained.current;
        control.state.push_back(retained);
    }
    control.planets[0].ground_base = {1, 1};
    auto started = LiveGameSession::start(initial, {.pacing = LiveGameSession::Pacing::driven, .entries = {}});
    expect(static_cast<bool>(started), "story publication fixture admits its two human sides");
    if (!started) { std::cerr << started.error().message << '\n'; return; }
    auto live = std::move(started).value();
    live->submit(LiveGameOrder{{1}, galactic::LandFleet{10, 2}, {}});
    live->advance_to(1); expect(live->wait_for(1, 5s), "native conquest publication arrives");
    const auto winner = live->frame({1}); const auto loser = live->frame({2});
    bool owner_context = false;
    for (const auto& published : live->galactic_events_after(0).events)
        if (published.event.kind == galactic::GalacticEventKind::planet_owner_changed && published.event.subject == 2) {
            owner_context = true;
            expect(published.previous_owner == galactic::PlayerId{2} && published.new_owner_playable,
                "owner event retains committed playable-new-owner context for WGM-194");
        }
    expect(owner_context, "native owner change reaches the event boundary");
    expect(winner.story_notices && winner.story_notices->size() == 2
        && loser.story_notices && loser.story_notices->size() == 2,
        "conquest observer publication reaches exact old and new active human owners");
    expect(winner.story_notices == live->frame({1}).story_notices && loser.story_notices == live->frame({2}).story_notices,
        "repeated frame reads reuse immutable qualified notices");
    expect(!live->frame({3}).story_notices && !live->frame({4}).story_notices
        && !live->frame({5}).story_notices && !live->frame({999}).story_notices,
        "unrelated human, AI, inactive and unknown seats cannot read local notice rows");
    live->advance_to(2); expect(live->wait_for(2, 5s), "quiet following tick arrives");
    expect(!live->frame({1}).story_notices && !live->frame({2}).story_notices,
        "quiet publications retain no stale notice storage");
    expect(winner.story_notices && winner.story_notices->size() == 2,
        "older immutable publication remains valid after quiet tick");
    live->stop();
}
void local_hero_return_publication() {
    namespace cp = eawr::campaign;
    galactic::GalacticSetup initial;
    initial.players = {{1, 1, 0}, {2, 2, 0}};
    initial.seats = {{{1}, 1, galactic::SeatKind::human, 0, true, 1},
        {{2}, 2, galactic::SeatKind::human, 1, true, 2}};
    initial.planets = {{1, {}, 1}, {2, {}, 2}};
    initial.rules.maximum_fleet_movement_distance = Fixed::from_raw(Fixed::scale);
    initial.observer.emplace(); initial.observer->human_observers = {1, 2}; initial.observer->solo_reference_human = 0;
    initial.observer->players = {{1}, {2}};
    for (const auto& planet : initial.planets) {
        galactic::ObserverPlanet facts; facts.planet = planet.id;
        facts.current_owner = facts.current_controller = facts.allegiance = facts.target_owner = planet.owner;
        initial.observer->planets.push_back(facts);
        for (const auto& player : initial.players)
            initial.observer->records.push_back({player.player_id, planet.id});
    }
    std::sort(initial.observer->records.begin(), initial.observer->records.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    auto owner = cp::GameSession::create(initial).value();
    const eawr::sim::InlineExecutor executor;
    for (unsigned i = 0; i < 5; ++i) expect(static_cast<bool>(owner.step(executor)), "hero publication advances to real schedule frame");
    for (const auto player : {1U, 2U}) {
        galactic::HeroDefeat defeat; defeat.id = 100 + player; defeat.type = defeat.respawn_type = 10 + player;
        defeat.owner = player; defeat.planet = player; defeat.named = true;
        defeat.seconds = Fixed::from_raw(10 * Fixed::scale);
        expect(owner.schedule_hero_defeat(defeat, executor).value().scheduled, "actual owner schedules each hero");
    }
    const auto saved = owner.save_return_state().value();
    const auto tick = owner.step(executor).value();
    const std::vector<galactic::HeroReturnView> expected{{11, 1, 1, 5, 305}};
    const std::vector<galactic::HeroReturnView> guest_expected{{12, 2, 2, 5, 305}};
    expect(tick.hero_returns == std::vector<galactic::HeroReturnView>{{11, 1, 1, 5, 305}, {12, 2, 2, 5, 305}},
        "GameTick carries both canonical human schedules");
    auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
    services->configure = [saved](auto& game) { return game.restore_return_state(saved); };
    LiveGameSession::Options options; options.pacing = LiveGameSession::Pacing::driven; options.services = services;
    auto live = LiveGameSession::start(initial, options).value();
    const auto restored = live->frame({1}).hero_returns;
    expect(restored && *restored == expected, "initial restored frame publishes copied local schedule before any step");
    const auto guest = live->frame({2}).hero_returns;
    expect(guest && *guest == guest_expected && live->frame({1}).hero_returns == restored
        && !live->frame({999}).hero_returns && !live->frame().hero_returns,
        "seat switch publishes each human's schedule without a hidden default observer");
    expect(live->frame({1}).latest->view_player() == 1 && live->frame({2}).latest->view_player() == 2,
        "frame seat binding selects the private snapshot");
    live->submit(LiveGameOrder{{1}, galactic::PauseGalaxy{}, {}});
    live->advance_to(1); expect(live->wait_for(1, 5s), "hero publication pause reaches boundary");
    const auto paused = live->frame({1});
    expect(paused.hero_returns == restored && *restored == expected,
        "paused publication preserves the retained interval and older immutable frame");
    expect(live->frame({2}).hero_returns == guest, "guest return cache survives a host query and a paused tick");
    live->stop();
    initial.observer.reset();
    auto headless = LiveGameSession::start(initial, options).value();
    expect(!headless->frame().hero_returns, "observer-free owner publishes no private schedule");
    headless->stop();
}
void configured_services() {
    unsigned calls = 0;
    const auto caller = std::this_thread::get_id();
    LiveGameSession::Options options; options.pacing = LiveGameSession::Pacing::driven;
    auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
    options.services = services;
    services->configure = [&](auto& game) {
        ++calls;
        expect(std::this_thread::get_id() == caller && game.completed_tick() == 0,
            "campaign services install before the live simulation thread starts");
        return game.submit({{0, {1}, 1}, galactic::PauseGalaxy{}});
    };
    auto live = LiveGameSession::start(setup(), options).value();
    live->advance_to(1);
    expect(live->wait_for(1, 5s) && live->frame().latest->clock().pause_depth == 1,
        "configured campaign input participates in the first canonical live step");
    live->stop();
    expect(calls == 1 && live->record().commands.size() == 1,
        "service configuration runs once and retains synchronized input");
    services->configure = [](auto&) {
        eawr::core::Diagnostic error; error.code = "fixture.configuration"; error.message = "refused service";
        return eawr::core::Result<void>::failure(error);
    };
    auto refused = LiveGameSession::start(setup(), options);
    expect(!refused && refused.error().code == "fixture.configuration",
        "configuration refusal preserves its diagnostic and starts no live session");
}

galactic::GalacticSetup ending_setup(bool multiplayer) {
    auto initial = setup();
    initial.seats = {{{1}, 1, galactic::SeatKind::human, 1, true, 1},
        {{2}, 2, galactic::SeatKind::human, 2, true, 2}, {{3}, 1, galactic::SeatKind::human, 1, true, 1}};
    galactic::VictoryContext context; context.multiplayer = multiplayer;
    context.players = {{1, "Local", false, true, true}, {2, "Winner", false, true, true}, {3, "Neutral", true, false}};
    for (const auto& planet : initial.planets)
        context.planets.push_back({planet.id, 1001, planet.owner, planet.owner, planet.owner, false});
    context.types = {{"Fixture_Planet", 1001}};
    for (const auto& a : context.players) for (const auto& b : context.players)
        context.relations.push_back({a.player, b.player, a.player == b.player || (a.player <= 2 && b.player <= 2)});
    galactic::VictoryAuthoring authored; authored.human = {galactic::VictoryKind::credits};
    authored.ai = authored.human;
    initial.victory = galactic::install_victory(authored, context, 1);
    expect(galactic::register_victory(*initial.victory, galactic::VictoryKind::credits, 2, 0, 0), "ending fixture has an exactly identified mature winner");
    return initial;
}
void campaign_end_owner() {
    using namespace eawr::platform;
    for (const std::size_t n : {1U, 2U, 4U, 8U}) {
        std::weak_ptr<int> script_owner;
        unsigned copies{};
        std::atomic<unsigned> commands{};
        auto services = std::make_shared<CampaignServiceConfiguration>();
        services->configure = [&](auto& game) {
            auto anchor = std::make_shared<int>(1); script_owner = anchor;
            game.set_hero_placement([anchor](const auto&, auto) { return eawr::core::Result<bool>::success(*anchor == 1); });
            return eawr::core::Result<void>::success();
        };
        services->campaign_end_inputs = [&](const auto& game, const auto& result, auto local) {
            expect(!script_owner.expired() && game.completed_tick() == 0 && result.local_won == (local == 2),
                "WGP-111 local scoring/movie inputs copy while their actual owner still exists");
            ++copies;
            CampaignEndInputs inputs; inputs.title_key = local == 1 ? "Local_Title" : "Winner_Title";
            inputs.efficiencies = {0.0, std::nullopt, 0.129};
            inputs.victory_text = "Local_Victory"; inputs.defeat_text = "Local_Defeat";
            inputs.space_lose_image = "Local_Space"; inputs.land_lose_image = "Local_Land";
            inputs.generic_win_movie = "Winner_Movie"; inputs.finale_movie = "Winner_Finale";
            inputs.victory_movies_enabled = true;
            return eawr::core::Result<CampaignEndInputs>::success(std::move(inputs));
        };
        LiveGameSession::Options options; options.workers = n; options.pacing = LiveGameSession::Pacing::driven;
        options.services = services; options.saves.directory = "out/campaign-end-unadmitted-save";
        options.await_presentation = true; // queued-save ending is deterministic before the startup barrier opens.
        options.command_source = [&](auto) { ++commands; return std::vector<galactic::GameCommand>{}; };
        auto live = LiveGameSession::start(ending_setup(false), options).value();
        expect(!live->consume_campaign_result({99}, false, false), "unadmitted observer cannot consume or tear down campaign result");
        auto gated = live->consume_campaign_result({1}, true, false);
        expect(gated && !gated.value() && !script_owner.expired() && copies == 0, "WGS-82 dialog blocks solo outer admission");
        gated = live->consume_campaign_result({1}, false, true);
        expect(gated && !gated.value() && !script_owner.expired(), "WGS-82 movie blocks solo outer admission");
        live->submit(LiveGameOrder{{1}, galactic::PauseGalaxy{}, 100});
        const auto save = live->request_save({{1}, "pending", false, false, false, false, {}});
        auto ended = live->consume_campaign_result({1}, false, false);
        expect(ended && ended.value() && ended.value()->local_player == 1 && !ended.value()->local_won
            && campaign_heading(*ended.value()) == "TEXT_DEFEAT" && campaign_score_text(*ended.value()) == "Local_Defeat",
            "WGS-82 allied winning player still yields exact-local defeat and copied local text");
        expect(script_owner.expired() && copies == 2 && commands == 0,
            "WGS-176 all local inputs copy once before joined session releases script/AI ownership");
        expect(live->frame({1}).campaign_end == ended.value() && live->frame({3}).campaign_end == ended.value()
            && live->frame({2}).campaign_end && live->frame({2}).campaign_end->local_won
            && !live->frame({99}).campaign_end, "observer end publication selects only its copied local result, sharing co-op identity");
        expect(live->consume_campaign_result({1}, false, false).value() == ended.value() && copies == 2,
            "end consumption is idempotent after actual teardown");
        const auto outcome = live->terminal_save_outcome(save);
        expect(outcome && outcome->phase == LiveSavePhase::failed, "unadmitted queued save reaches terminal refusal at ending; no autosave is inserted");
        const auto tick = live->completed_tick(); const auto record = live->record();
        const auto hash = live->frame().latest->sha256();
        expect(campaign_score_picture(*ended.value(), true) == "Local_Space"
            && campaign_score_picture(*ended.value(), false) == "Local_Land"
            && live->frame().latest->sha256() == hash, "WGP-38 both presentation image draws retain the identical simulation hash");
        live->submit(LiveGameOrder{{1}, galactic::ResumeGalaxy{}, {}}); live->advance_to(tick + 1);
        expect(!live->wait_for(tick + 1, 100ms) && live->completed_tick() == tick && commands == 0
            && live->record().commands == record.commands, "post-end input cannot mutate the released session or invoke its command/AI source");
        live->stop(); live->stop();
        auto next = LiveGameSession::start(setup(), {.workers = n, .pacing = LiveGameSession::Pacing::driven}).value();
        next->advance_to(1); expect(next->wait_for(1, 5s) && !next->frame({1}).campaign_end, "fresh second campaign has no old result or script/input ownership");
        next->stop();

        auto multi = LiveGameSession::start(ending_setup(true), {.workers = n, .pacing = LiveGameSession::Pacing::driven}).value();
        auto quit = multi->consume_campaign_result({1}, true, true).value();
        expect(quit && quit->route == CampaignEndRoute::multiplayer_quit && !quit->local_won
            && multi->consume_campaign_result({1}, true, true).value() == quit
            && multi->consume_campaign_result({3}, false, false).value() == quit,
            "WGS-177 ordinary multiplayer bypasses dialog/movie gates and deduplicates local quit independent of winner");
        multi->advance_to(1); expect(multi->wait_for(1, 5s), "ordinary multiplayer insertion alone does not invent network shutdown"); multi->stop();
    }
}

void campaign_end_movies() {
    using namespace eawr::platform;
    CampaignEndView view; view.local_player = 1; view.condition.player = 2; view.inputs.emplace();
    auto& in = *view.inputs;
    in.victory_movies_enabled = true; in.finale_movie = "Finale"; in.second_finale_movie = "Second"; in.post_credits_movie = "Post";
    expect(campaign_efficiency_text(std::nullopt) == "0%" && campaign_efficiency_text(0.0) == "0"
        && campaign_efficiency_text(0.129) == "12" && campaign_efficiency_text(-1.0) == "0"
        && campaign_efficiency_text(3.0) == "100", "WGP-97 absent versus present-zero controls, truncation and clamp");
    CampaignEndPresentation movie(view);
    const auto initial = movie.movie_request()->token;
    expect(movie.movie_request()->name.empty(), "WGP-106 empty authored initial movie remains a real request");
    movie.movie_open_result(initial, false, false);
    expect(movie.movie_request()->token == initial, "WGP-108 uninitialized scene does not invent a continuation");
    movie.movie_open_result(initial, false, true);
    expect(movie.movie_request()->name == "Finale" && movie.take_stop_music_and_speech()
        && !movie.take_stop_music_and_speech(), "WGP-107/108 initialized empty/failing initial continues synchronously and stops audio once");
    const auto first = movie.movie_request()->token;
    movie.movie_open_result(initial, false, true);
    expect(movie.movie_request()->token == first, "a stale duplicate movie callback cannot advance the next movie");
    expect(movie.movie_skip_key(true, true, true, false, true) && movie.movie_request()->token == first,
        "WGP-109 authored Cannot_Skip consumes the key without stopping or also dismissing score");
    movie.movie_stopped(first);
    expect(movie.movie_request()->name == "Second", "WGP-107 retained second finale follows first");
    movie.movie_open_result(movie.movie_request()->token, false, true);
    expect(movie.movie_request()->name == "Credits" && movie.movie_request()->looping, "WGP-107 failing second continues to looping Credits");
    const auto credits = movie.movie_request()->token;
    movie.credits_completed(credits, false);
    expect(movie.movie_request()->token == credits, "WGP-110 credits completion waits for operational rendering device");
    movie.credits_completed(credits, true);
    expect(movie.movie_request()->name == "Post" && !movie.movie_request()->looping && movie.active(),
        "WGP-110 natural credits completion requests copied post movie without menu transition");
    movie.movie_stopped(movie.movie_request()->token);
    expect(!movie.movie_request() && movie.active(), "post movie supplies no automatic score deactivation");
    expect(movie.score_exit(false, true) == CampaignScoreDestination::none
        && movie.score_exit(true, true) == CampaignScoreDestination::main_menu
        && movie.score_exit(true, true) == CampaignScoreDestination::none, "WGS-116/117 topmost score exits exactly once");
    in.victory_movies_enabled = false;
    CampaignEndPresentation disabled(view);
    disabled.movie_open_result(disabled.movie_request()->token, false, true);
    expect(disabled.movie_request()->name.empty(), "disabled movies do not populate authored finale names");
    disabled.movie_open_result(disabled.movie_request()->token, false, true);
    expect(disabled.movie_request()->name == "Credits", "WGP-105/107 suppression can still reach Credits");
    expect(disabled.movie_skip_key(true, true, true, false, false) && !disabled.movie_request()
        && disabled.active(), "WGP-109/110 skip has no post-credits or score-exit continuation");
    view.explicit_quit = true; view.ending_mode = CampaignEndingMode::lan;
    expect(CampaignEndPresentation(view).score_exit(true, true) == CampaignScoreDestination::lan_lobby,
        "WGS-117 explicit LAN score exits to lobby");
    view.ending_mode = CampaignEndingMode::other_multiplayer; view.tutorial_queued = true;
    expect(CampaignEndPresentation(view).score_exit(true, true) == CampaignScoreDestination::none,
        "WGS-117 queued tutorial supplies no menu/lobby activation or invented retry");
}

// WGP-81/89: session ticks may service input, but an open summary freezes campaign time.
void summary_clock_hold() {
    const galactic::CommandPayload close = galactic::BattleSummaryAcknowledge{900};
    const auto encoded_close = galactic::encode_payload(close);
    expect(galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::dialog_acknowledge, encoded_close)
        == std::optional(close), "summary close has a canonical conflict-only session payload");
    auto short_close = encoded_close; short_close.pop_back();
    auto long_close = encoded_close; long_close.push_back(0);
    for (const auto& invalid : {short_close, long_close, std::vector<std::uint8_t>(8)})
        expect(!galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::dialog_acknowledge, invalid),
            "summary command rejects truncated, trailing and zero-conflict payloads");
    namespace cp = eawr::campaign;
    using eawr::core::Result;
    const eawr::sim::InlineExecutor executor;
    std::string multiplayer_initial;
    std::vector<std::string> multiplayer_ticks;
    std::vector<std::uint8_t> multiplayer_record;
    for (const auto observer : {eawr::sim::SeatId{}, eawr::sim::SeatId{1}, eawr::sim::SeatId{2}, eawr::sim::SeatId{3}}) {
        auto initial = ending_setup(true); initial.interactive_seat = observer;
        auto game = cp::GameSession::create(initial).value();
        if (observer.value) expect(bool(game.set_interactive_seat(observer)), "multiplayer accepts the valid local observer without synchronized policy");
        expect(!game.galaxy().setup().interactive_seat.value && !game.record().setup.interactive_seat.value,
            "multiplayer owner setup and recordings omit the local interactive-seat policy");
        const auto initial_hash = game.state_sha256();
        cp::AutoResolveOutcomeApplication outcome; outcome.planet_type = "Fixture_Planet";
        outcome.planet = initial.planets.front().id; outcome.invader = 1; outcome.defender = 2;
        outcome.result.conflict = 902; outcome.result.winner = 1;
        expect(bool(game.stage_auto_resolve_outcome(outcome)), "multiplayer peer fixture stages the same outcome");
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 4; ++tick) hashes.push_back(game.step(executor).value().state_hash.get());
        const auto bytes = galactic::write_replay(game.record()).value();
        if (!observer.value) { multiplayer_initial = initial_hash; multiplayer_ticks = hashes; multiplayer_record = bytes; }
        else expect(initial_hash == multiplayer_initial && hashes == multiplayer_ticks && bytes == multiplayer_record,
            "multiplayer peers and headless owner retain identical hashes and replay bytes despite different local observers");
    }
    for (unsigned policy = 0; policy < 3; ++policy) {
        auto initial = policy == 2 ? ending_setup(true) : setup();
        if (policy == 1) {
            initial.interactive_seat = {3}; initial.seats[2].kind = galactic::SeatKind::human;
            initial.seats[2].human_side_id = 3;
        }
        if (policy == 2) initial.interactive_seat = {1};
        auto game = cp::GameSession::create(initial).value();
        cp::AutoResolveOutcomeApplication outcome; outcome.planet_type = "Fixture_Planet";
        outcome.planet = initial.planets.front().id; outcome.invader = 1; outcome.defender = 2;
        outcome.result.conflict = 901; outcome.result.winner = 1;
        expect(bool(game.stage_auto_resolve_outcome(outcome)), "nonmodal fixture stages outcome");
        (void)game.step(executor).value(); const auto frame = game.galaxy().clock().galactic_frame;
        (void)game.step(executor).value();
        expect(!game.battle_summary_hold() && !game.galaxy().clock().pause_depth
            && game.galaxy().clock().galactic_frame > frame, "headless, uninvolved and pinned multiplayer policies add no summary pause");
    }
    for (const bool land : {false, true}) for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup(); initial.interactive_seat = {1};
        cp::AutoResolveOutcomeApplication outcome; outcome.planet_type = "Fixture_Planet";
        outcome.planet = initial.planets.front().id; outcome.invader = 1; outcome.defender = 2;
        outcome.land = land; outcome.result.conflict = 900; outcome.result.winner = 1;
        auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
        services->configure = [outcome](auto& game) {
            cp::AutoResolveOutcomeServices adapters;
            adapters.control = [](auto&, const auto&) { return Result<void>::success(); };
            game.set_auto_resolve_outcome_services(std::move(adapters));
            return game.stage_auto_resolve_outcome(outcome);
        };
        LiveGameSession::Options options;
        options.workers = workers; options.pacing = LiveGameSession::Pacing::driven; options.services = services;
        auto live = LiveGameSession::start(initial, options).value();
        live->advance_to(30);
        expect(live->wait_for(1, 5s), "outcome is published");
        const auto published = live->frame(); const auto frozen = published.latest->clock().galactic_frame;
        expect(published.result && published.summary_hold == 900 && published.latest->clock().pause_depth == 1,
            "auto outcome publication already owns the matching solo summary hold in both modes");
        expect(!live->wait_for(2, 30ms), "driven demand cannot advance an open summary");
        live->submit(LiveGameOrder{{1}, galactic::BattleSummaryAcknowledge{901}, {}});
        expect(live->wait_for(2, 5s) && live->frame().summary_hold == 900
            && live->frame().latest->clock().galactic_frame == frozen, "stale close leaves actual galactic frame frozen");
        live->submit(LiveGameOrder{{1}, galactic::PauseGalaxy{}, {}});
        expect(live->wait_for(3, 5s) && live->frame().latest->clock().pause_depth == 2, "player pause nests over summary hold");
        live->submit(LiveGameOrder{{2}, galactic::BattleSummaryAcknowledge{900}, {}});
        expect(live->wait_for(4, 5s) && live->frame().summary_hold == 900, "another seat cannot close the owning observer's summary");
        live->submit(LiveGameOrder{{1}, galactic::BattleSummaryAcknowledge{900}, {}});
        expect(live->wait_for(5, 5s) && !live->frame().summary_hold
            && live->frame().latest->clock().pause_depth == 1
            && live->frame().latest->clock().galactic_frame == frozen, "matching close preserves independently owned player pause");
        expect(!live->wait_for(6, 30ms), "remaining player hold still parks the pacer");
        live->submit(LiveGameOrder{{1}, galactic::ResumeGalaxy{}, {}});
        expect(live->wait_for(30, 5s) && live->frame().latest->clock().galactic_frame > frozen, "campaign frame advances after both owners release");
        live->stop();
        const auto hashes = live->tick_hashes(); const auto recorded = live->record();
        const auto bytes = galactic::write_replay(recorded).value();
        const auto parsed = galactic::parse_replay(bytes).value();
        expect(parsed.setup.interactive_seat == eawr::sim::SeatId{1}, "interactive policy survives replay wire encoding");
        auto replayed = cp::GameSession::from_replay(parsed).value();
        cp::AutoResolveOutcomeServices adapters; adapters.control = [](auto&, const auto&) { return Result<void>::success(); };
        replayed.set_auto_resolve_outcome_services(std::move(adapters));
        std::vector<std::string> replay_hashes;
        for (std::size_t tick = 0; tick < hashes.size(); ++tick) replay_hashes.push_back(replayed.step(executor).value().state_hash.get());
        expect(hashes == replay_hashes, "summary policy and recorded close replay identically to the threaded live owner");

        // Direct paused-owner checkpoint and player-load normalization retain the hold.
        auto game = cp::GameSession::create(initial).value();
        cp::AutoResolveOutcomeServices restore_adapters; restore_adapters.control = [](auto&, const auto&) { return Result<void>::success(); };
        game.set_auto_resolve_outcome_services(std::move(restore_adapters));
        expect(bool(game.stage_auto_resolve_outcome(outcome)), "checkpoint fixture stages outcome");
        (void)game.step(executor).value();
        auto restored = cp::GameSession::create(game.record().setup).value();
        expect(bool(restored.load_checkpoint_state(game.save_checkpoint_state().value()))
            && restored.state_sha256() == game.state_sha256() && restored.battle_summary_hold() == 900,
            "checkpoint retains the conflict-scoped hold and its state hash");
        expect(bool(restored.apply_load_fixup({{restored.completed_tick(), {1}, 0}, galactic::PostLoadFixup{0, true, false, false}}))
            && restored.galaxy().clock().pause_depth == 1, "player-load normalization cannot clear an unacknowledged summary hold");
        const auto before = restored.galaxy().clock().galactic_frame;
        for (unsigned i = 0; i < 20; ++i) (void)restored.step(executor).value();
        expect(restored.galaxy().clock().galactic_frame == before, "twenty requested headless frames do not advance held campaign time");
        expect(bool(restored.submit({{restored.completed_tick(), {1}, 1}, galactic::BattleSummaryAcknowledge{900}})), "restored close is admitted");
        (void)restored.step(executor).value();
        expect(restored.galaxy().clock().galactic_frame > before, "restored summary close permits the next galactic frame");
    }
}

void summary_startup_reconciliation() {
    namespace cp = eawr::campaign;
    auto initial = setup(); initial.interactive_seat = {1};
    auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
    services->restore.script_requirement = cp::ScriptRequirement::none;
    services->configure = [planet = initial.planets.front().id](auto& game) {
        cp::AutoResolveOutcomeApplication outcome; outcome.planet_type = "Fixture_Planet"; outcome.planet = planet;
        outcome.invader = 1; outcome.defender = 2; outcome.result.conflict = 900; outcome.result.winner = 1;
        return game.stage_auto_resolve_outcome(outcome);
    };
    const auto directory = std::filesystem::temp_directory_path()
        / ("eawr-summary-clock-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LiveGameSession::Options options; options.pacing = LiveGameSession::Pacing::driven; options.services = services;
    const auto utf8 = directory.u8string(); options.saves.directory.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    auto live = LiveGameSession::start(initial, options).value();
    live->advance_to(1); expect(live->wait_for(1, 5s), "saved summary reaches its held outcome");
    eawr::platform::LiveSaveRequest request; request.seat = {1}; request.name = "summary";
    const auto token = live->request_save(request);
    expect(live->wait_save(token, 5s) && live->terminal_save_outcome(token)->phase == eawr::platform::LiveSavePhase::completed,
        "open-summary checkpoint completes under its hold");
    const auto barrier = live->completed_tick();
    for (const bool acknowledged : {false, true}) {
        auto prepared = live->prepare_load("summary").value();
        auto restore_options = options; restore_options.await_presentation = true;
        auto restored = LiveGameSession::start(std::move(prepared), restore_options).value();
        const auto first = restored->frame();
        expect(first.summary_hold == 900 && first.latest->clock().pause_depth == 1,
            "restored first frame retains the summary hold before startup readiness");
        restored->advance_to(barrier + 10);
        if (acknowledged) restored->reconcile_battle_summary({1}, 900);
        else restored->reconcile_battle_summary({1}, 901);
        expect(!restored->wait_for(barrier + 1, 20ms), "restore acknowledgement reconciliation waits behind startup readiness");
        restored->presentation_ready();
        if (acknowledged) expect(restored->wait_for(barrier + 10, 5s) && !restored->frame().summary_hold
            && restored->frame().latest->clock().galactic_frame > first.latest->clock().galactic_frame,
            "acknowledged restored summary releases before the first resumed galactic frame");
        else expect(!restored->wait_for(barrier + 1, 30ms) && restored->frame().summary_hold == 900
            && restored->frame().latest->clock().galactic_frame == first.latest->clock().galactic_frame,
            "unacknowledged restored summary remains frozen after startup readiness");
        restored->stop();
    }
    live->stop(); std::filesystem::remove_all(directory);
}

void tactical_summary_clock_hold() {
    namespace cp = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    for (const auto workers : {1U, 4U}) {
        const auto initial = setup();
        auto game = cp::GameSession::create(initial).value();
        cp::ConflictRecord entry; entry.id = game.galaxy().next_entity_id();
        entry.planet = initial.planets.front().id; entry.invader = 1; entry.defender = 2;
        entry.tactical.content_identity = initial.content_identity; entry.tactical.first_free_entity_id = entry.id + 1;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        tactical::TacticalContent content;
        for (const auto owner : {1U, 2U}) {
            const auto fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(), [owner](const auto& row) { return row.owner == owner; });
            const auto member = fleet->members.front();
            entry.forces.push_back({member.id, member.type, owner, fleet->id});
            tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = owner;
            entry.tactical.units.push_back(unit);
        }
        for (const auto& seat : initial.seats) if (seat.player <= 2) entry.seats.push_back({seat.seat_id, seat.player});
        tactical::DurabilityProfile hull; hull.type_id = entry.forces.front().type; hull.max_hull = Fixed::from_integer(100).value();
        content.durability.profiles.push_back(hull);
        content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
        content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2}; content.victory.humans = {1};
        content.victory.countdown_frames = 2; content.victory.relevant_types = {hull.type_id};
        LiveGameSession::Options options; options.workers = workers; options.pacing = LiveGameSession::Pacing::driven;
        options.interactive_seat = {1}; options.entries.push_back({0, entry, content, cp::ScriptRequirement::none});
        auto live = LiveGameSession::start(initial, options).value();
        live->advance_to(1); expect(live->wait_for(1, 5s), "summary fixture enters tactical child");
        auto feed = live->battle_feed(entry.id, {1}); live->battle_ready(entry.id); feed->set_paused(false);
        feed->submit({1, {entry.forces.back().id}, tactical::DamagePayload{Fixed::from_integer(1000).value()}, {}});
        live->advance_to(30);
        for (unsigned tick = 2; tick <= 20 && !live->frame().result; ++tick) (void)live->wait_for(tick, 100ms);
        const auto returned = live->frame();
        expect(returned.result && !returned.conflict && returned.summary_hold == entry.id
            && returned.latest->clock().pause_depth == 1, "tactical return publishes the summary with its owner pause already acquired");
        const auto frozen = returned.latest->clock().galactic_frame;
        expect(!live->wait_for(returned.session_tick + 1, 30ms), "tactical summary parks driven demand immediately");
        live->submit(LiveGameOrder{{1}, galactic::ResumeGalaxy{}, {}});
        expect(live->wait_for(returned.session_tick + 1, 5s) && live->frame().latest->clock().galactic_frame == frozen,
            "generic ResumeGalaxy cannot consume summary ownership");
        live->submit(LiveGameOrder{{1}, galactic::BattleSummaryAcknowledge{entry.id}, {}});
        expect(live->wait_for(30, 5s) && live->frame().latest->clock().galactic_frame > frozen,
            "tactical Continue releases its matching hold and advances the galaxy");
        live->stop(); feed->stop();
        cp::GameRecording recording{live->record(), {{0, entry}}};
        auto replayed = cp::GameSession::from_recording(recording, [content](const auto&) {
            return eawr::core::Result<tactical::TacticalContent>::success(content);
        }, {}, cp::ScriptRequirement::none).value();
        const eawr::sim::InlineExecutor executor;
        std::vector<std::string> replay_hashes;
        for (std::size_t tick = 0; tick < live->tick_hashes().size(); ++tick) replay_hashes.push_back(replayed.step(executor).value().state_hash.get());
        expect(replay_hashes == live->tick_hashes(), "interactive tactical outcome hold and acknowledgement reproduce headless");
    }
}

void pending_choice_publication() {
    namespace cp = eawr::campaign;
    const auto initial = setup();
    auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
    cp::ConflictRecord entry;
    unsigned content_attempts{};
    services->tactical_capability = [](const auto&) {
        eawr::core::Diagnostic refusal;
        refusal.code = "fixture.space.capability";
        refusal.message = "reachable ship has an unsupported rocket curve";
        return eawr::core::Result<void>::failure(std::move(refusal));
    };
    services->configure = [&](auto& game) {
        entry.id = game.galaxy().next_entity_id(); entry.planet = initial.planets.front().id;
        entry.invader = 1; entry.defender = 2;
        entry.tactical.content_identity = initial.content_identity;
        entry.tactical.first_free_entity_id = entry.id + 1;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        entry.seats = {{{1}, 1}, {{2}, 2}};
        game.set_conflict_service([](const auto&, const auto&, eawr::sim::EntityId) {
            return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
        }, [&](const auto&) {
            if (++content_attempts <= 2) {
                eawr::core::Diagnostic refusal;
                refusal.message = "tactical content unavailable";
                return eawr::core::Result<eawr::sim::tactical::TacticalContent>::failure(refusal);
            }
            return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success({});
        }, {}, eawr::campaign::ScriptRequirement::none);
        return game.queue_conflict(entry, {cp::ConflictRoute::pending_tactical, false, false});
    };
    LiveGameSession::Options options; options.workers = 2;
    options.pacing = LiveGameSession::Pacing::driven; options.services = services;
    auto created = LiveGameSession::start(initial, options);
    expect(bool(created), "pending view fixture starts"); if (!created) return;
    auto& live = *created.value();
    live.submit(LiveGameOrder{{1}, galactic::PauseGalaxy{}, {}});
    live.advance_to(1); expect(live.wait_for(1, 5s), "announcement publication arrives");
    const auto announcement = live.frame();
    expect(announcement.pending_conflict && announcement.pending_conflict->conflict.id == entry.id
        && !announcement.pending_conflict->choice_ready && announcement.latest->clock().pause_depth == 1
        && !announcement.conflict && !announcement.battle,
        "player pause does not turn announcement into a ready choice or active tactical child");
    expect(!announcement.tactical_available && announcement.tactical_refusal_code == "fixture.space.capability"
        && announcement.tactical_refusal == "reachable ship has an unsupported rocket curve",
        "native refused publication retains the provider code and reason instead of generic unavailable");
    live.submit(LiveGameOrder{{1}, galactic::SetSpeed{false, Fixed::from_raw(Fixed::scale)}, {}});
    live.advance_to(2); expect(live.wait_for(2, 5s), "next eligible tail publishes pending choice");
    const auto pending = live.frame();
    expect(pending.pending_conflict && pending.pending_conflict->choice_ready
        && pending.pending_conflict->arbitration.route == cp::ConflictRoute::pending_tactical
        && pending.latest->clock().pause_depth == 2,
      "choice readiness names conflict-owned pause independently of a nested player pause");
    expect(pending.tactical_refusal_code == announcement.tactical_refusal_code
        && pending.tactical_refusal == announcement.tactical_refusal,
        "refusal diagnostic is republished with the same pending conflict");
    if (!pending.pending_conflict || !announcement.pending_conflict) { live.stop(); return; }
    expect(!eawr::platform::battle_choice_order(announcement, {1}, entry.id, true)
        && !eawr::platform::battle_choice_order(pending, {1}, entry.id + 1, true)
        && !eawr::platform::battle_choice_order(pending, {3}, entry.id, true)
        && !eawr::platform::battle_choice_order(pending, {2}, entry.id, true),
        "dialog dispatcher refuses announcement, stale identity, unrelated faction and AI seat");
    // Neither refusal advances a paused session, but both must publish a response.
    live.submit(LiveGameOrder{{999}, galactic::BattleChoice{entry.id, true}, {}});
    expect(!live.wait_for(3, 80ms), "pre-step seat rejection produces no tick");
    const auto seat_refused = live.frame();
    expect(seat_refused.battle_choice_responses.size() == 1
        && seat_refused.battle_choice_responses.front().seat == eawr::sim::SeatId{999}
        && seat_refused.battle_choice_responses.front().conflict == entry.id
        && !seat_refused.battle_choice_responses.front().refusal.empty(),
        "pre-step unknown seat refusal publishes identity and reason without a view transition");
    live.submit(galactic::GameCommand{{1, {1}, 50}, galactic::BattleChoice{entry.id, true}});
    expect(!live.wait_for(3, 80ms), "pre-step late choice rejection produces no tick");
    const auto late_refused = live.frame();
    expect(late_refused.battle_choice_responses.size() == 2
        && late_refused.battle_choice_responses.front().seat == eawr::sim::SeatId{1}
        && late_refused.battle_choice_responses.front().revision > seat_refused.battle_choice_responses.front().revision,
        "pre-step late refusal publishes a fresh local response");
    auto forced = pending;
    auto forced_view = std::make_shared<cp::PendingConflictView>(*pending.pending_conflict);
    forced_view->arbitration.forced_tactical = true; forced.pending_conflict = forced_view;
    expect(!eawr::platform::battle_choice_order(forced, {1}, entry.id, false)
        && eawr::platform::battle_choice_order(forced, {1}, entry.id, true),
        "dialog dispatcher preserves the sourced tactical forcing gate");
    live.submit(LiveGameOrder{{1}, galactic::BattleChoice{entry.id + 1, true}, {}});
    live.advance_to(3); expect(live.wait_for(3, 5s), "stale dialog input is serviced");
    expect(live.frame().pending_conflict && !live.frame().conflict,
        "stale conflict choice cannot start a child");
    const auto choice = eawr::platform::battle_choice_order(pending, {1}, entry.id, true);
    expect(bool(choice), "involved human receives a seat-issued choice order");
    if (!choice) { live.stop(); return; }
    std::uint64_t revision{};
    for (const auto tick : {4U, 5U}) {
        live.submit(*choice);
        live.advance_to(tick); expect(live.wait_for(tick, 5s), "choice refusal is published");
        const auto refused = live.frame();
        expect(refused.pending_conflict && refused.pending_conflict->choice_ready && !refused.battle
            && refused.battle_choice_responses.size() == 2,
            "refused content selection leaves an authoritative ready choice");
        if (!refused.battle_choice_responses.empty()) {
            const auto& response = refused.battle_choice_responses.front();
            expect(response.conflict == entry.id && response.seat == eawr::sim::SeatId{1}
                && response.revision > revision && response.refusal == "tactical content unavailable",
                "identical repeated refusals each publish a fresh choice response revision");
            revision = response.revision;
        }
    }
    live.submit(*choice);
    live.advance_to(6); expect(live.wait_for(6, 5s), "current dialog choice enters its child");
    const auto selected = live.frame();
    expect(!selected.pending_conflict && selected.conflict && selected.conflict->id == entry.id && selected.battle,
        "child selection clears pending publication and exposes the separate tactical record");
    expect(selected.tactical_refusal_code.empty() && selected.tactical_refusal.empty(),
        "a selected child does not inherit a stale pending capability refusal");
    expect(!announcement.pending_conflict->choice_ready && pending.pending_conflict->choice_ready,
        "retained dialog publications stay immutable across readiness and selection");
    live.stop(); expect(!live.failure(), "pending choice fixture stops cleanly");
}

void child_untimed_input_after_drain() {
    namespace tactical = eawr::sim::tactical;
    namespace campaign = eawr::campaign;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto initial = setup();
        auto game = campaign::GameSession::create(initial).value();
        campaign::ConflictRecord entry;
        entry.id = game.galaxy().next_entity_id();
        entry.planet = initial.planets.front().id;
        entry.invader = 1; entry.defender = 2;
        entry.tactical.content_identity = initial.content_identity;
        entry.tactical.first_free_entity_id = entry.id + 1;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        tactical::TacticalContent content;
        for (const auto owner : {1U, 2U}) {
            const auto fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(),
                [owner](const auto& row) { return row.owner == owner; });
            const auto member = fleet->members.front();
            entry.forces.push_back({member.id, member.type, owner, fleet->id});
            tactical::UnitState unit;
            unit.entity_id = member.id; unit.type_id = member.type; unit.owner = owner;
            entry.tactical.units.push_back(unit);
        }
        for (const auto& seat : initial.seats) if (seat.player <= 2) entry.seats.push_back({seat.seat_id, seat.player});
        LiveGameSession::Options options;
        options.workers = workers; options.pacing = LiveGameSession::Pacing::driven;
        options.entries.push_back({1, entry, content, eawr::campaign::ScriptRequirement::none});
        auto started = LiveGameSession::start(initial, options);
        expect(bool(started), "child inbox regression starts"); if (!started) continue;
        auto& live = *started.value();
        live.advance_to(2);
        expect(live.wait_for(2, 5s), "child inbox regression publishes its gate");
        auto feed = live.battle_feed(entry.id, {1});
        expect(bool(feed), "child inbox regression binds its feed"); if (!feed) { live.stop(); continue; }
        feed->command_source([attacker = entry.forces.front().id, pending_feed = std::weak_ptr(feed)](const auto tick) {
            if (tick == 0) {
                // The inbox has already drained, but its published tick is still
                // current. This input must execute on the next admitted tick.
                if (const auto current = pending_feed.lock())
                    current->submit({1, {attacker}, tactical::StopPayload{}, {}});
            }
            return std::vector<tactical::PlayerCommand>{};
        });
        feed->set_paused(false); live.battle_ready(entry.id);
        live.advance_to(4);
        expect(live.wait_for(4, 5s), "child inbox regression advances two child ticks");
        const auto refusals = live.rejected_orders().size();
        expect(refusals == 0, "untimed child input after inbox drain is not rejected as late");
        feed->submit({1, {entry.forces.front().id}, tactical::StopPayload{}, 3});
        live.advance_to(6);
        expect(live.wait_for(6, 5s), "explicit future child input reaches its tick");
        expect(live.rejected_orders().size() == refusals, "explicit future child input remains accepted");
        feed->submit({1, {entry.forces.front().id}, tactical::StopPayload{}, 0});
        expect(live.rejected_orders().size() == refusals + 1, "explicit past child input remains refused");
        live.stop(); feed->stop();
        const auto record = feed->record();
        expect(record.commands.size() == 2 && record.commands.front().key.tick == 1
            && record.commands.back().key.tick == 3, "untimed and timed child orders record their admitted ticks");
        const auto replayed = eawr::platform::headless_tick_hashes(record, content);
        expect(replayed && replayed.value() == feed->tick_hashes(), "child inbox input replays at its recorded ticks");
        const auto hashes = live.tick_hashes();
        if (reference.empty()) reference = hashes;
        expect(reference == hashes, "child inbox input remains equal on 1/2/4/8 workers");
    }
}

void child_health_warning_publication() {
    namespace tactical = eawr::sim::tactical;
    namespace campaign = eawr::campaign;
    namespace audio = eawr::presentation::audio;
    std::vector<std::vector<tactical::DamageChange>> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto initial = setup();
        auto game = campaign::GameSession::create(initial).value();
        campaign::ConflictRecord entry;
        entry.id = game.galaxy().next_entity_id(); entry.planet = initial.planets.front().id;
        entry.invader = 1; entry.defender = 2;
        entry.tactical.content_identity = initial.content_identity;
        entry.tactical.first_free_entity_id = entry.id + 1;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        tactical::TacticalContent content;
        for (const auto owner : {1U, 2U}) {
            const auto fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(),
                [owner](const auto& row) { return row.owner == owner; });
            const auto member = fleet->members.front();
            entry.forces.push_back({member.id, member.type, owner, fleet->id});
            tactical::UnitState unit;
            unit.entity_id = member.id; unit.type_id = member.type; unit.owner = owner;
            entry.tactical.units.push_back(unit);
            if (std::none_of(content.durability.profiles.begin(), content.durability.profiles.end(),
                [type = member.type](const auto& row) { return row.type_id == type; })) {
                tactical::DurabilityProfile hull; hull.type_id = member.type;
                hull.max_hull = Fixed::from_integer(100).value();
                content.durability.profiles.push_back(hull);
            }
        }
        std::sort(content.durability.profiles.begin(), content.durability.profiles.end(),
            [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        for (const auto& seat : initial.seats) if (seat.player <= 2) entry.seats.push_back({seat.seat_id, seat.player});
        LiveGameSession::Options options;
        options.workers = workers; options.pacing = LiveGameSession::Pacing::driven;
        options.entries.push_back({1, entry, content, campaign::ScriptRequirement::none});
        auto started = LiveGameSession::start(initial, options);
        expect(bool(started), "SND-33: campaign health-warning fixture starts");
        if (!started) continue;
        auto& live = *started.value();
        live.advance_to(2);
        expect(live.wait_for(2, 5s), "campaign health-warning child reaches readiness");
        auto feed = live.battle_feed(entry.id, {1});
        expect(bool(feed), "campaign health-warning observer binds the real child feed");
        if (!feed) { live.stop(); continue; }
        const auto target = entry.forces.front();
        feed->command_source([id = target.id](const std::uint64_t tick) {
            if (tick != 0) return std::vector<tactical::PlayerCommand>{};
            return std::vector<tactical::PlayerCommand>{{{tick, 1, 1}, {id},
                tactical::DamagePayload{Fixed::from_integer(75).value()}}};
        });
        feed->set_paused(false); live.battle_ready(entry.id);
        live.advance_to(4);
        expect(live.wait_for(4, 5s), "campaign child completes the surviving damage delivery");
        live.stop();
        const auto published = feed->events_after(0, feed->completed_tick());
        std::vector<tactical::DamageChange> changes;
        for (const auto& tick : published.ticks)
            changes.insert(changes.end(), tick.damage_changes.begin(), tick.damage_changes.end());
        expect(changes.size() == 1, "campaign lifecycle and live journal forward one immutable damage fact");
        if (changes.size() == 1) {
            const auto& change = changes.front();
            expect(change.target == tactical::DamageIdentity{target.id, target.type, 1}
                && change.hull_before == Fixed::from_integer(100).value()
                && change.hull_after == Fixed::from_integer(25).value(), "campaign fact retains target identity and exact hull crossing");
            const audio::SfxEvent low, critical;
            const audio::HealthWarningCues cues{&low, &critical, nullptr, nullptr};
            const auto before = static_cast<double>(change.hull_before.raw()) / static_cast<double>(change.max_hull.raw());
            const auto after = static_cast<double>(change.hull_after.raw()) / static_cast<double>(change.max_hull.raw());
            expect(audio::health_warning_cue(before, after, 0.5, 0.1, change.target.owner == 1,
                false, cues, {}) == &low, "real campaign feed selects the local owner's authored low warning");
        }
        reference.push_back(changes);
        expect(reference.back() == reference.front(), "campaign damage publication agrees on 1/2/4/8 workers");
        const auto replayed = eawr::platform::headless_tick_hashes(feed->record(), content);
        expect(replayed && replayed.value() == feed->tick_hashes(), "campaign journal forwarding leaves standalone child replay hashes unchanged");
        feed->stop();
    }
}

void child_readiness_and_return() {
    namespace tactical = eawr::sim::tactical;
    namespace campaign = eawr::campaign;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        campaign::ConflictRecord entry;
        auto game = campaign::GameSession::create(initial).value();
        entry.id = game.galaxy().next_entity_id();
        entry.planet = initial.planets.front().id;
        entry.invader = 1; entry.defender = 2;
        entry.tactical.content_identity = initial.content_identity;
        entry.tactical.first_free_entity_id = entry.id + 1;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        tactical::TacticalContent content;
        for (const auto owner : {1U, 2U}) {
            const auto fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(), [owner](const auto& row) { return row.owner == owner; });
            const auto member = fleet->members.front();
            entry.forces.push_back({member.id, member.type, owner, fleet->id});
            tactical::UnitState unit;
            unit.entity_id = member.id; unit.type_id = member.type; unit.owner = owner;
            entry.tactical.units.push_back(unit);
        }
        for (const auto& seat : initial.seats) if (seat.player <= 2) entry.seats.push_back({seat.seat_id, seat.player});
        tactical::DurabilityProfile hull;
        hull.type_id = entry.forces.front().type;
        hull.max_hull = Fixed::from_integer(100).value();
        content.durability.profiles.push_back(hull);
        content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
        content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2};
        content.victory.humans = {1}; content.victory.countdown_frames = 2;
        content.victory.relevant_types = {hull.type_id};
        LiveGameSession::Options options;
        options.workers = workers; options.pacing = LiveGameSession::Pacing::driven;
        options.entries.push_back({1, entry, content, eawr::campaign::ScriptRequirement::none});
        auto next_entry = entry;
        next_entry.id += 10;
        next_entry.tactical.first_free_entity_id = next_entry.id + 1;
        options.entries.push_back({8, next_entry, content, eawr::campaign::ScriptRequirement::none});
        const auto stale_options = options;
        const auto defender_fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(),
            [&](const auto& row) { return row.id == next_entry.forces.back().parent; });
        const auto replacement = defender_fleet->members.back();
        next_entry.forces.back().id = next_entry.tactical.units.back().entity_id = replacement.id;
        next_entry.forces.back().type = next_entry.tactical.units.back().type_id = replacement.type;
        auto next_content = content;
        auto next_hull = hull; next_hull.type_id = replacement.type;
        next_content.durability.profiles.push_back(next_hull);
        next_content.victory.relevant_types.push_back(replacement.type);
        options.entries.back() = {8, next_entry, next_content, eawr::campaign::ScriptRequirement::none};
        std::atomic<bool> preparation_entered{}, release_preparation{};
        auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
        services->loading_descriptor = [](const auto& record, const auto&) {
            eawr::platform::CampaignLoadingDescriptor descriptor;
            descriptor.location_type = "FixturePlanet";
            descriptor.actual_owner = 3; descriptor.map_owner = 2;
            descriptor.basic_income = Fixed::from_raw(77 * Fixed::scale);
            descriptor.dialog_map = "dialog.ted"; descriptor.original_map = "original.ted";
            descriptor.base_level = 4; descriptor.kind = eawr::platform::CampaignLoadingKind::corruption;
            descriptor.current_type = "CurrentFixturePlanet"; descriptor.current_model = "fixture.alo";
            descriptor.selected_map = record.map_identity;
            return descriptor;
        };
        services->retreat_policy = [](const auto& record) {
            return std::optional(eawr::platform::CampaignRetreatPolicy{record.id, false, true, true});
        };
        services->loading_presentation = [&](const auto&, const auto& report) {
            using Producer = eawr::platform::CampaignLoadingProducer;
            report({Producer::space_map, 0, 0, 0, 0.25});
            report({Producer::space_map, 0, 0, 0, 0.5});
            report({Producer::force_roster, 11, 0, 2, 0.65});
            report({Producer::force_roster, 11, 1, 2, 0.75});
            report({Producer::force_roster, 12, 0, 1, 0.65});
            preparation_entered = true;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!release_preparation && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            report({Producer::transition, 0, 0, 0, 1.0});
            return eawr::core::Result<std::shared_ptr<const eawr::platform::PreparedCampaignBattle>>::success(
                std::make_shared<const eawr::platform::PreparedCampaignBattle>());
        };
        options.services = services;
        auto live = LiveGameSession::start(initial, std::move(options)).value();
        live->advance_to(2);
        const auto loading_deadline = std::chrono::steady_clock::now() + 5s;
        while (!preparation_entered && std::chrono::steady_clock::now() < loading_deadline) std::this_thread::yield();
        const auto pending_load = live->loading({1});
        expect(preparation_entered && pending_load && pending_load->outcome == eawr::platform::CampaignLoadingOutcome::pending,
            "WGP-161: retained descriptor is readable while preparation is blocked");
        expect(pending_load && pending_load->descriptor.actual_owner == 3U && pending_load->descriptor.map_owner == 2U
            && pending_load->descriptor.basic_income == Fixed::from_raw(77 * Fixed::scale)
            && pending_load->descriptor.dialog_map == "dialog.ted" && pending_load->descriptor.original_map == "original.ted",
            "WGP-162/170: loading retains actual owner, basic income and independent map identities");
        expect(!live->loading({2}) && !live->loading({3}) && !live->loading({999}),
            "WGP-162: AI seats and nonparticipants cannot read loading facts");
        expect(live->loading({1}) == pending_load && live->loading({4}) == pending_load,
            "loading reads reuse one immutable location publication across admitted seats");
        expect(pending_load && pending_load->progress && pending_load->progress->sequence == 5 && pending_load->progress->value.list == 12
            && pending_load->progress->value.fraction == 0.65,
            "WGP-165: raw reports retain list restart and decreasing fraction");
        release_preparation = true;
        const auto terminal_deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < terminal_deadline) {
            const auto loading = live->loading({1});
            if (loading && loading->outcome != eawr::platform::CampaignLoadingOutcome::pending) break;
            std::this_thread::yield();
        }
        const auto prepared_load = live->loading({1});
        expect(prepared_load && prepared_load != pending_load
            && prepared_load->outcome == eawr::platform::CampaignLoadingOutcome::succeeded
            && pending_load->outcome == eawr::platform::CampaignLoadingOutcome::pending,
            "WGP-166: verified preparation result publishes a new immutable terminal view");
        expect(live->wait_for(2, 5s), "entry is published before scene readiness");
        const auto frozen = live->frame();
        const auto retreat = eawr::platform::campaign_tactical_view_for_observer(frozen, {1});
        expect(retreat && retreat->player == 1 && retreat->policy && retreat->policy->conflict == entry.id
            && retreat->policy->planet_excluded && retreat->policy->story_prevented,
            "WAR-10/65/75: involved observer receives retained independent solver exclusion facts");
        expect(!eawr::platform::campaign_tactical_view_for_observer(frozen, {2})
            && !eawr::platform::campaign_tactical_view_for_observer(frozen, {3})
            && !eawr::platform::campaign_tactical_view_for_observer(frozen, {999}),
            "campaign tactical projection rejects AI, uninvolved and unknown seats");
        auto stale_policy = frozen;
        stale_policy.retreat_policy = eawr::platform::CampaignRetreatPolicy{entry.id + 1, true, false, false};
        const auto stale_retreat = eawr::platform::campaign_tactical_view_for_observer(stale_policy, {1});
        expect(stale_retreat && !stale_retreat->policy, "stale conflict policy cannot qualify a current child");
        expect(frozen.conflict && frozen.battle && frozen.battle->completed_tick() == 0, "retained child publishes tick zero");
        live->advance_to(3);
        expect(!live->wait_for(3, 20ms), "viewer loading parks the live pacer");
        live->battle_ready(entry.id + 1);
        expect(!live->wait_for(3, 20ms), "wrong conflict readiness does not open the gate");
        expect(!live->battle_feed(entry.id, eawr::sim::SeatId{3}), "nonparticipant receives no command feed");
        auto feed = live->battle_feed(entry.id, eawr::sim::SeatId{1});
        expect(feed && feed->completed_tick() == 0, "local seat binds a child feed");
        const auto defender = entry.forces.back().id;
        feed->command_source([defender](std::uint64_t tick) {
            if (tick != 1) return std::vector<tactical::PlayerCommand>{};
            return std::vector<tactical::PlayerCommand>{{{tick, 1, 1}, {defender}, tactical::DamagePayload{Fixed::from_integer(1000).value()}}};
        });
        feed->set_paused(false);
        live->battle_ready(entry.id);
        live->advance_to(3);
        expect(live->wait_for(3, 5s), "ready child advances on its owner thread");
        expect(feed->completed_tick() == 1 && live->completed_tick() == 3, "child and session ticks are distinct");
        expect(live->frame().latest->sha256() == frozen.latest->sha256(), "galaxy snapshot stays frozen during battle");
        live->advance_to(4);
        expect(live->wait_for(4, 5s), "seat scheduler reaches the tactical command boundary");
        live->advance_to(8);
        expect(live->wait_for(8, 5s), "child ends and parent resumes");
        feed->stop();
        expect(!live->frame().conflict && !live->failure(), "return clears the active child");
        expect(feed->frame().latest && frozen.battle->completed_tick() == 0, "retained snapshots remain immutable after return");
        expect(!feed->events_after(0, 100).ticks.empty(), "child journal retains destruction events");
        const auto replay = feed->record();
        const auto child_hashes = eawr::platform::headless_tick_hashes(replay, content).value();
        expect(child_hashes == feed->tick_hashes(), "exported child replay runs alone with the same hashes");
        const auto hashes = live->tick_hashes();
        if (reference.empty()) reference = hashes;
        expect(reference == hashes, "readiness does not alter worker parity");
        campaign::GameRecording recording{live->record(), {{1, entry}}};
        auto replayed = campaign::GameSession::from_recording(recording, [content](const auto&) {
            return eawr::core::Result<tactical::TacticalContent>::success(content);
        }, {}, eawr::campaign::ScriptRequirement::none).value();
        std::vector<std::string> again;
        const eawr::sim::InlineExecutor executor;
        for (std::size_t tick = 0; tick < hashes.size(); ++tick) again.push_back(replayed.step(executor).value().state_hash.get());
        expect(again == hashes, "headless game replay has no viewer readiness state");
        live->advance_to(10);
        expect(live->wait_for(9, 5s), "next conflict enters after return");
        expect(live->frame().conflict && live->frame().conflict->id == next_entry.id, "next child has its own identity");
        feed->set_paused(false);
        live->battle_ready(entry.id);
        feed->submit({1, {entry.forces.front().id}, tactical::StopPayload{}, {}});
        expect(!live->wait_for(10, 20ms), "old feed and old readiness cannot advance a later child");
        auto next_feed = live->battle_feed(next_entry.id, eawr::sim::SeatId{1});
        expect(bool(next_feed), "later child provides a feed for its participating seat");
        if (!next_feed) { live->stop(); continue; }
        next_feed->set_paused(false);
        live->battle_ready(next_entry.id);
        expect(live->wait_for(10, 5s), "later child admits only its own readiness");
        live->stop();
        next_feed->stop();

        auto stale = LiveGameSession::start(initial, stale_options).value();
        stale->advance_to(2);
        expect(stale->wait_for(2, 5s), "stale-entry fixture reaches its first child");
        auto stale_feed = stale->battle_feed(entry.id, eawr::sim::SeatId{1});
        expect(bool(stale_feed), "stale-entry fixture provides its first feed");
        if (!stale_feed) { stale->stop(); continue; }
        stale_feed->command_source([defender](std::uint64_t tick) {
            if (tick != 1) return std::vector<tactical::PlayerCommand>{};
            return std::vector<tactical::PlayerCommand>{{{tick, 1, 1}, {defender},
                tactical::DamagePayload{Fixed::from_integer(1000).value()}}};
        });
        stale_feed->set_paused(false); stale->battle_ready(entry.id);
        stale->advance_to(9);
        expect(!stale->wait_for(9, 5s) && stale->failure()
            && stale->failure()->message.find("persistent forces must match") != std::string::npos,
            "destroyed-member re-entry refuses cleanly and reports its diagnostic");
        expect(!stale->frame().conflict && !stale->battle_feed(stale_options.entries.back().conflict.id, eawr::sim::SeatId{1}),
            "refused stale entry publishes no later child or feed");
        stale->stop(); stale_feed->stop();
    }
}
void loading_failure_latch() {
    namespace platform = eawr::platform;
    platform::CampaignLoadingPublication publication;
    platform::CampaignLoadingDescriptor descriptor;
    descriptor.conflict = 19; descriptor.planet = 23; descriptor.selected_map = "broken.ted";
    descriptor.base_level = 7;
    publication.begin(descriptor);
    publication.report({platform::CampaignLoadingProducer::land_file, 0, 95, 100, 0.95});
    publication.report({platform::CampaignLoadingProducer::force_roster, 1, 0, 2, 0.65});
    eawr::core::Diagnostic error; error.code = "fixture.failed.map"; error.message = "Controlled failed reader";
    publication.finish(error);
    const auto failed = publication.view();
    publication.report({platform::CampaignLoadingProducer::transition, 0, 0, 0, 1.0});
    publication.finish();
    const auto tail = publication.view();
    expect(tail && tail->outcome == platform::CampaignLoadingOutcome::failed && tail->failure
        && tail->failure->code == error.code && tail->descriptor.selected_map == "broken.ted"
        && tail->descriptor.base_level == 7 && tail->progress && tail->progress->value.fraction == 1.0,
        "WGP-167: common 1.0 tail cannot erase selected-map/base failure");
    expect(failed && failed->progress && failed->progress->sequence == 2 && failed != tail, "retained failure view is immutable");
    publication.begin(descriptor);
    expect(publication.view()->outcome == platform::CampaignLoadingOutcome::pending
        && !publication.view()->failure && !publication.view()->progress, "restart clears previous preparation facts");
}

void repeated_child_lifetimes() {
    namespace tactical = eawr::sim::tactical;
    namespace campaign = eawr::campaign;
    auto initial = setup();
    const auto first = campaign::GameSession::create(initial).value().galaxy().next_entity_id();
    tactical::TacticalContent content;
    tactical::DurabilityProfile hull;
    hull.type_id = 1; hull.max_hull = Fixed::from_integer(100).value();
    content.durability.profiles.push_back(hull);
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2};
    content.victory.humans = {1}; content.victory.countdown_frames = 2;
    content.victory.relevant_types = {1};
    LiveGameSession::Options options;
    options.pacing = LiveGameSession::Pacing::driven;
    std::array<campaign::ConflictRecord, 3> entries;
    for (std::size_t cycle = 0; cycle < entries.size(); ++cycle) {
        auto& entry = entries[cycle];
        entry.id = first + cycle * 10;
        entry.planet = initial.planets.front().id; entry.invader = 1; entry.defender = 2;
        entry.tactical.content_identity = initial.content_identity;
        entry.tactical.first_free_entity_id = entry.id + 3;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        for (const auto owner : {1U, 2U}) {
            const auto id = entry.id + owner;
            entry.forces.push_back({id, 1, owner, 0, campaign::ForceRole::escort});
            tactical::UnitState unit; unit.entity_id = id; unit.type_id = 1; unit.owner = owner;
            entry.tactical.units.push_back(unit);
        }
        for (const auto& seat : initial.seats) if (seat.player <= 2) entry.seats.push_back({seat.seat_id, seat.player});
        options.entries.push_back({1 + cycle * 20, entry, content, eawr::campaign::ScriptRequirement::none});
    }
    auto live = LiveGameSession::start(initial, options).value();
    const auto owner = live->lifetime();
    for (std::size_t cycle = 0; cycle < entries.size(); ++cycle) {
        const auto tick = 1 + cycle * 20;
        live->advance_to(tick + 1);
        expect(live->wait_for(tick + 1, 5s), "repeated conflict publishes before readiness");
        const auto child = live->frame().battle_lifetime;
        expect(!child.expired() && !owner.expired(), "child and campaign owner are alive in battle");
        auto feed = live->battle_feed(entries[cycle].id, eawr::sim::SeatId{1});
        const std::weak_ptr<eawr::platform::TacticalFeed> feed_lifetime = feed;
        const auto target = entries[cycle].forces.back().id;
        feed->command_source([target](std::uint64_t frame) {
            if (frame != 1) return std::vector<tactical::PlayerCommand>{};
            return std::vector<tactical::PlayerCommand>{{{frame, 1, 1}, {target},
                tactical::DamagePayload{Fixed::from_integer(1000).value()}}};
        });
        feed->set_paused(false); live->battle_ready(entries[cycle].id);
        live->advance_to(tick + 10);
        expect(live->wait_for(tick + 10, 5s), "repeated child completes and galaxy resumes");
        expect(live->frame().result && live->frame().result->conflict == entries[cycle].id,
            "result remains visible after parent resumes");
        feed->stop(); feed.reset();
        expect(child.expired() && feed_lifetime.expired(), "child and feed expire every cycle");
        expect(!owner.expired() && !live->failure(), "same campaign owner survives every return");
    }
    live->stop(); live.reset();
    expect(owner.expired(), "campaign owner expires after final teardown");

    options.entries.resize(1);
    options.readiness_timeout = 30ms;
    auto never_ready = LiveGameSession::start(initial, options).value();
    never_ready->advance_to(3);
    expect(!never_ready->wait_for(3, 5s), "missing readiness terminates with a diagnostic");
    expect(never_ready->failure() && never_ready->failure()->message.find("readiness timed out for conflict") != std::string::npos,
        "readiness timeout identifies the stalled conflict");
    expect(never_ready->frame().battle && never_ready->frame().battle->completed_tick() == 0,
        "timeout never steps the unready child");
    never_ready->stop();

    auto failed_services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
    failed_services->loading_presentation = [](const auto&, const auto& report) {
        report({eawr::platform::CampaignLoadingProducer::transition, 0, 0, 0, 1.0});
        eawr::core::Diagnostic error; error.code = "fixture.failed.presentation"; error.message = "Failed presentation preparation";
        return eawr::core::Result<std::shared_ptr<const eawr::platform::PreparedCampaignBattle>>::failure(error);
    };
    options.services = failed_services; options.readiness_timeout = 10s;
    auto failed_preparation = LiveGameSession::start(initial, options).value();
    failed_preparation->advance_to(2);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::shared_ptr<const eawr::platform::CampaignLoadingView> failed_view;
    while (std::chrono::steady_clock::now() < deadline) {
        failed_view = failed_preparation->loading({1});
        if (failed_view && failed_view->outcome == eawr::platform::CampaignLoadingOutcome::failed) break;
        std::this_thread::yield();
    }
    expect(failed_view && failed_view->outcome == eawr::platform::CampaignLoadingOutcome::failed
        && failed_view->failure && failed_view->progress && failed_view->progress->value.fraction == 1.0,
        "WGP-166/167: real failed provider result wins over its final numeric callback");
    failed_preparation->battle_ready(entries.front().id);
    expect(!failed_preparation->battle_is_ready(entries.front().id)
        && !failed_preparation->frame().battle_presentation,
        "failed platform preparation cannot accept readiness or publish successful content");
    failed_preparation->stop();

}


// GCMP-04 / WGP-89: invasion acceptance is a typed session command. It round-trips its canonical bytes, and the
// live owner journals it and decides it on the simulation thread at its tick, so a replay refuses it identically.
void invasion_accept_command() {
    const galactic::CommandPayload accept = galactic::InvasionAccept{900};
    const auto encoded = galactic::encode_payload(accept);
    expect(galactic::layer_of(accept) == galactic::CommandLayer::session
        && galactic::opcode_of(accept) == galactic::session_opcodes::invasion_accept && encoded.size() == 8
        && galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::invasion_accept, encoded)
            == std::optional(accept), "invasion accept has a canonical conflict-only session payload");
    auto short_accept = encoded; short_accept.pop_back();
    auto long_accept = encoded; long_accept.push_back(0);
    for (const auto& invalid : {short_accept, long_accept, std::vector<std::uint8_t>(8)})
        expect(!galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::invasion_accept, invalid),
            "invasion accept rejects truncated, trailing and zero-conflict payloads");
    expect(galactic::session_opcodes::invasion_accept == 16 && galactic::session_opcodes::first_reserved == 17,
        "the coordinator-assigned session opcode leaves the reserved walk range after it");

    auto started = LiveGameSession::start(setup(), {.pacing = LiveGameSession::Pacing::driven});
    expect(static_cast<bool>(started), "invasion accept session starts"); if (!started) return;
    auto& live = *started.value();
    live.submit(LiveGameOrder{{1}, accept, {}});
    live.advance_to(1);
    expect(live.wait_for(1, 5s), "the accept command drives one tick even though no offer exists");
    live.stop();
    const auto rejected = live.rejected_orders();
    expect(rejected.size() == 1 && rejected.front().message == "invasion offer is no longer available",
        "the simulation thread refuses an acceptance with no retained offer at its tick");
    const auto record = live.record();
    expect(record.commands.size() == 1 && record.commands.front().payload == accept
        && record.commands.front().key.seat == eawr::sim::SeatId{1},
        "a refused acceptance is still journaled, because every peer decides it at execution");
    auto replayed = eawr::campaign::GameSession::from_replay(record).value();
    const eawr::sim::InlineExecutor executor;
    std::vector<std::string> hashes;
    while (replayed.completed_tick() < record.final_tick_count) hashes.push_back(replayed.step(executor).value().state_sha256);
    expect(hashes == live.tick_hashes(), "the journaled acceptance replays to the same hashes");
}
}

int case_galactic_live_game_session_tests() {
    retained_campaign_root();
    intervals();
    applied_speed_schedule();
    applied_speed_live();
    configured_services();
    campaign_end_owner();
    campaign_end_movies();
    observer_story_publication();
    local_hero_return_publication();
    summary_clock_hold();
    tactical_summary_clock_hold();
    invasion_accept_command();
    summary_startup_reconciliation();
    pending_choice_publication();
    replay_parity();
    pause_and_sequences(LiveGameSession::Pacing::driven);
    pause_and_sequences(LiveGameSession::Pacing::real_time);
    resume_and_failure();
    child_untimed_input_after_drain();
    child_health_warning_publication();
    child_readiness_and_return();
    repeated_child_lifetimes();
    loading_failure_latch();
    if (failures != 0) return 1;
    std::cout << "live game session contracts passed\n";
    return 0;
}
