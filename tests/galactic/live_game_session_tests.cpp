#include "galactic_support.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/campaign_battle_view.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
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
        });
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
        options.entries.push_back({1, entry, content});
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
        options.entries.push_back({1, entry, content});
        auto next_entry = entry;
        next_entry.id += 10;
        next_entry.tactical.first_free_entity_id = next_entry.id + 1;
        options.entries.push_back({8, next_entry, content});
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
        options.entries.back() = {8, next_entry, next_content};
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
        }).value();
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
        options.entries.push_back({1 + cycle * 20, entry, content});
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

}

int main() {
    retained_campaign_root();
    intervals();
    configured_services();
    campaign_end_owner();
    campaign_end_movies();
    observer_story_publication();
    pending_choice_publication();
    replay_parity();
    pause_and_sequences(LiveGameSession::Pacing::driven);
    pause_and_sequences(LiveGameSession::Pacing::real_time);
    resume_and_failure();
    child_untimed_input_after_drain();
    child_readiness_and_return();
    repeated_child_lifetimes();
    loading_failure_latch();
    if (failures != 0) return 1;
    std::cout << "live game session contracts passed\n";
    return 0;
}
