#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

// #80 part A: the viewer's live tactical session (eawr::platform::LiveSession). A session that
// steps on its own thread while a presentation reader takes its snapshots gives the same
// per-tick state hashes as the headless run of the same command stream (the committed
// tactical-motion goldens), for every worker count; a real-time run with orders stamped as they
// arrive records the replay that reproduces it headless.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using eawr::platform::LiveOrder;
using eawr::platform::LiveSession;
using math::Fixed;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {units(x), units(y), units(z)};
}

// The motion table of the tactical-motion fixture (tests/replay/motion_tests.cpp foc_table).
[[nodiscard]] tactical::MotionTable motion_table() {
    return {{units(15), units(300)},
            {{1011, decimal("3.72"), decimal("0.06"), decimal("0.06"), decimal("1.5"), units(2), decimal("0.24"), units(15)},
             {1012, decimal("2.64"), decimal("0.048"), decimal("0.048"), decimal("0.6"), units(3), decimal("0.24"), units(20)}},
        std::nullopt, {}, {}};
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// "tick,sha256" rows without the header, as sha256 values.
[[nodiscard]] std::vector<std::string> read_hashes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> hashes;
    std::string line;
    bool header = true;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) {
            header = false;
            continue;
        }
        hashes.push_back(line.substr(line.find(',') + 1));
    }
    return hashes;
}

[[nodiscard]] std::vector<std::string> headless_hashes(const tactical::TacticalReplay& replay) {
    std::vector<std::string> hashes;
    auto session = tactical::TacticalSession::from_replay(replay, {}, {}, motion_table());
    if (!session) return hashes;
    const eawr::sim::InlineExecutor executor;
    while (session.value().completed_tick() < replay.final_tick_count) {
        auto tick = session.value().step(executor);
        if (!tick) break;
        hashes.push_back(tick.value().state_sha256);
    }
    return hashes;
}

// The presentation side: reads frames and kept snapshots while the simulation thread steps,
// as the viewer does, and checks that what it reads only moves forward.
class Reader {
public:
    explicit Reader(const LiveSession& live) : thread_([this, &live] { read(live); }) {}
    ~Reader() {
        done_ = true;
        thread_.join();
    }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }
    [[nodiscard]] bool ordered() const { return ordered_; }

private:
    void read(const LiveSession& live) {
        std::uint64_t last = 0;
        while (!done_) {
            const auto frame = live.frame();
            if (!frame.latest || !frame.previous || frame.previous->completed_tick() > frame.latest->completed_tick()
                || frame.latest->completed_tick() < last) {
                ordered_ = false;
            }
            if (frame.latest) {
                last = frame.latest->completed_tick();
                static_cast<void>(frame.latest->visible_entities(1));
            }
            ++frames_;
            std::this_thread::yield();
        }
    }

    std::atomic<bool> done_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<bool> ordered_{true};
    std::thread thread_;
};

[[nodiscard]] LiveOrder order_of(const tactical::PlayerCommand& command, const bool stamped) {
    LiveOrder order{command.key.player_id, command.units, command.payload, std::nullopt};
    if (stamped) order.tick = command.key.tick;
    return order;
}

// Driven pacing: the fixture's commands at their ticks give the golden hashes, for every
// worker count, with a presentation reader attached.
void test_driven_matches_headless(const std::filesystem::path& fixtures) {
    const auto parsed = tactical::parse_replay(read_bytes(fixtures / "tactical-motion.eawr-replay"), "tactical-motion");
    expect(static_cast<bool>(parsed), "the tactical-motion fixture parses");
    if (!parsed) return;
    const auto& replay = parsed.value();
    const auto golden = read_hashes(fixtures / "tactical-motion.hashes.csv");
    expect(golden.size() == replay.final_tick_count, "one golden hash per fixture tick");
    expect(headless_hashes(replay) == golden, "the headless run gives the golden hashes");

    for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
        const std::string label = std::to_string(workers) + " workers: ";
        auto live = LiveSession::start(replay.setup, {}, {}, motion_table(), {},
            {.workers = workers, .pacing = LiveSession::Pacing::driven, .history = 8});
        expect(static_cast<bool>(live), label + "the live session starts");
        if (!live) continue;
        auto& session = *live.value();
        expect(session.completed_tick() == 0 && session.frame().latest != nullptr, label + "tick zero is published");
        {
            const Reader reader(session);
            for (const auto& command : replay.commands) session.submit(order_of(command, true));
            // Step in slices, as frames would ask for ticks.
            for (std::uint64_t tick = 7; tick < replay.final_tick_count; tick += 7) {
                session.advance_to(tick);
                expect(session.wait_for(tick, std::chrono::seconds(60)), label + "a driven slice completes");
            }
            session.advance_to(replay.final_tick_count);
            expect(session.wait_for(replay.final_tick_count, std::chrono::seconds(60)), label + "the run completes");
            expect(reader.frames() > 0 && reader.ordered(), label + "the reader sees ordered frames");
        }
        expect(session.completed_tick() == replay.final_tick_count, label + "driven pacing stops at the target");
        const auto kept = session.snapshot_at(replay.final_tick_count - 1);
        expect(kept && kept->completed_tick() == replay.final_tick_count - 1, label + "recent snapshots are kept");
        expect(session.snapshot_at(1) == nullptr, label + "old snapshots leave the history");
        session.stop();
        expect(!session.failure(), label + "no step failed");
        expect(session.rejected_orders().empty(), label + "every fixture order is accepted");
        expect(session.tick_hashes() == golden, label + "live hashes equal the headless goldens");
        const auto recorded = tactical::write_replay(session.record());
        const auto encoded = tactical::write_replay(replay);
        expect(recorded && encoded && recorded.value() == encoded.value(), label + "the live run records the fixture");
    }
}

// A two-player setup of `count` stations of type 40 (2400 hull, no shield): station 1 is player
// 1's, the others player 2's, 1000 units apart.
[[nodiscard]] tactical::TacticalSetup station_setup(const std::uint32_t count) {
    tactical::TacticalSetup setup;
    setup.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable}};
    for (std::uint32_t id = 1; id <= count; ++id) {
        tactical::UnitState station;
        station.entity_id = id;
        station.type_id = 40;
        station.owner = id == 1 ? 1U : 2U;
        station.position = at(static_cast<std::int64_t>(id) * 1000, 0, 0);
        setup.units.push_back(station);
    }
    return setup;
}

[[nodiscard]] tactical::DurabilityTable station_durability() {
    tactical::DurabilityTable durability;
    durability.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    durability.profiles = {{40, units(2400), std::nullopt, false, {}}};
    return durability;
}

[[nodiscard]] bool same_ticks(const std::vector<eawr::platform::LiveTickEvents>& a,
                              const std::vector<eawr::platform::LiveTickEvents>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (a[index].tick != b[index].tick || a[index].events != b[index].events
            || a[index].combat_events != b[index].combat_events) {
            return false;
        }
    }
    return true;
}

// #370 review 3 and re-review 1: the event log outlives the snapshot history and keeps only what
// the battle view presents. Six stations are destroyed by scripted damage at scattered ticks: a
// reader that comes back after the history has moved on gets every destruction, in order, as the
// headless run publishes it, and none of the orders that caused them; one that comes back after
// the log's tick bound is told which ticks it lost.
void test_event_log_outlives_the_history() {
    constexpr std::array<std::uint64_t, 6> kills{3, 5, 9, 14, 20, 30};
    const tactical::TacticalSetup setup = station_setup(static_cast<std::uint32_t>(kills.size()) + 1U);
    const tactical::DurabilityTable durability = station_durability();
    const std::uint64_t last = 40;
    std::vector<eawr::platform::LiveTickEvents> expected;
    for (const std::size_t kept : {std::size_t{18000}, std::size_t{4}}) {
        const std::string label = "event history " + std::to_string(kept) + ": ";
        auto live = LiveSession::start(setup, {}, durability, {}, {},
            {.workers = 2, .pacing = LiveSession::Pacing::driven, .history = 2, .event_history = kept});
        expect(static_cast<bool>(live), label + "the live session starts");
        if (!live) continue;
        auto& session = *live.value();
        for (std::size_t index = 0; index < kills.size(); ++index) {
            session.submit({1, {static_cast<eawr::sim::EntityId>(index + 2)}, tactical::DamagePayload{units(3000)}, kills[index]});
        }
        session.advance_to(last);
        expect(session.wait_for(last, std::chrono::seconds(60)), label + "the run completes");
        expect(session.snapshot_at(1) == nullptr, label + "the snapshots of the first ticks are gone");
        if (expected.empty()) {
            // The headless run of the recorded replay: its presented events, tick by tick.
            bool orders = false;
            auto headless = tactical::TacticalSession::from_replay(session.record(), {}, durability);
            const eawr::sim::InlineExecutor executor;
            while (headless && headless.value().completed_tick() < last) {
                auto tick = headless.value().step(executor);
                if (!tick) break;
                const auto& snapshot = *tick.value().snapshot;
                eawr::platform::LiveTickEvents record{snapshot.completed_tick(), {}, {}};
                for (const auto& event : snapshot.events()) {
                    if (event.kind == tactical::EventKind::order_accepted) orders = true;
                    if (event.kind == tactical::EventKind::unit_destroyed) record.events.push_back(event);
                }
                if (!record.events.empty()) expected.push_back(std::move(record));
            }
            expect(orders, "the damage orders publish order events");
        }
        expect(expected.size() == kills.size(), label + "each station's destruction is published");
        const auto events = session.events_after(0, last);
        if (kept > last) {
            expect(!events.lost_through, label + "nothing is lost");
            expect(same_ticks(events.ticks, expected), label + "every destruction comes back in order, as headless, "
                   "without the orders");
            const auto tail = session.events_after(expected.front().tick, last);
            const std::vector<eawr::platform::LiveTickEvents> later(expected.begin() + 1, expected.end());
            expect(same_ticks(tail.ticks, later), label + "only the ticks after the asked one");
            const auto bounded = session.events_after(0, expected.front().tick);
            expect(bounded.ticks.size() == 1 && bounded.ticks.front().tick == expected.front().tick,
                   label + "only the ticks up to `through`");
        } else if (!expected.empty()) {
            // Every destruction is older than the bound: all are dropped and reported, the
            // reported range ending at the asked `through`.
            expect(events.ticks.empty(), label + "only the bound is kept");
            expect(events.lost_through == expected.back().tick, label + "the newest dropped tick is reported");
            const auto early = session.events_after(0, expected[2].tick);
            expect(early.lost_through == expected[2].tick, label + "the reported loss ends at `through`");
            expect(!session.events_after(last - 1, last).lost_through, label + "a reader that kept up loses nothing");
        }
        session.stop();
        expect(!session.failure(), label + "no step failed");
    }
}

// A snapshot of `tick` whose frame fired `shots` shots (each with its acquisition) and landed
// `hits` projectile hits.
[[nodiscard]] tactical::TacticalSnapshot busy_snapshot(const std::uint64_t tick, const std::size_t shots,
                                                       const std::size_t hits) {
    std::vector<tactical::CombatEvent> combat;
    combat.reserve(2 * shots + hits);
    for (std::size_t shot = 0; shot < shots; ++shot) {
        for (const auto kind : {tactical::CombatEventKind::target_acquired, tactical::CombatEventKind::weapon_fired}) {
            tactical::CombatEvent event;
            event.tick = tick - 1;
            event.kind = kind;
            event.shooter = 1 + shot % 100;
            event.weapon = static_cast<std::uint32_t>(shot / 100);
            event.target = 1000;
            combat.push_back(event);
        }
    }
    for (std::size_t hit = 0; hit < hits; ++hit) {
        tactical::CombatEvent event;
        event.tick = tick - 1;
        event.kind = tactical::CombatEventKind::projectile_hit;
        event.shooter = 1 + hit % 100;
        event.target = 1000;
        combat.push_back(event);
    }
    std::vector<tactical::Event> events;
    tactical::Event order;
    order.tick = tick - 1;
    order.kind = tactical::EventKind::order_accepted;
    order.player = 1;
    events.push_back(order);
    return tactical::TacticalSnapshot(tick, {}, {}, std::move(events), std::move(combat));
}

// #370 re-review 1: the event log is bounded by bytes as well as ticks. A synthetic battle of
// 100 units firing ten weapons every tick (1,000 shots a tick) with 50 hits a tick would hold
// about 1.9 GB of combat events over the default ten-minute tick window; the log keeps no shots,
// acquisitions or orders, stays under its byte bound after every tick, and reports exactly the
// ticks it dropped.
void test_event_log_bounds_a_high_event_rate() {
    using eawr::platform::LiveEventLog;
    constexpr std::size_t bound = std::size_t{1} << 20U;
    LiveEventLog log({.ticks = 18000, .bytes = bound});
    constexpr std::uint64_t ticks = 2000;
    constexpr std::size_t hits = 50;
    std::size_t peak = 0;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        log.record(busy_snapshot(tick, 1000, hits));
        peak = std::max(peak, log.bytes());
    }
    expect(peak <= bound, "the log stays under its byte bound at every tick");
    const std::size_t per_tick = sizeof(eawr::platform::LiveTickEvents) + hits * sizeof(tactical::CombatEvent);
    expect(log.bytes() > bound - per_tick, "the log fills its byte bound");
    expect(log.size() == bound / per_tick, "the log keeps as many whole ticks as fit");
    const auto events = log.after(0, ticks);
    bool presented_only = !events.ticks.empty();
    for (const auto& record : events.ticks) {
        presented_only = presented_only && record.events.empty() && record.combat_events.size() == hits;
        for (const auto& event : record.combat_events) {
            presented_only = presented_only && event.kind == tactical::CombatEventKind::projectile_hit;
        }
    }
    expect(presented_only, "only the hits are kept: no shots, acquisitions or orders");
    const std::uint64_t oldest = events.ticks.empty() ? 0 : events.ticks.front().tick;
    expect(events.ticks.size() == log.size() && events.ticks.back().tick == ticks, "the newest ticks are kept");
    expect(events.lost_through == oldest - 1, "the gap ends right before the oldest kept tick");
    expect(!log.after(oldest - 1, ticks).lost_through, "a reader past the gap loses nothing");
    // One tick whose hits alone exceed the bound is dropped whole, and reported; the older
    // ticks stay.
    const std::size_t before = log.size();
    log.record(busy_snapshot(ticks + 1, 0, bound / sizeof(tactical::CombatEvent) + 1));
    expect(log.bytes() <= bound && log.size() == before, "an oversized tick is dropped alone");
    expect(log.after(ticks, ticks + 1).lost_through == ticks + 1, "the oversized tick is reported lost");
    std::cout << "event log: sizeof(LiveTickEvents) " << sizeof(eawr::platform::LiveTickEvents) << ", sizeof(Event) "
              << sizeof(tactical::Event) << ", sizeof(CombatEvent) " << sizeof(tactical::CombatEvent) << '\n';
}

// #370 re-review 3: the reported loss never runs past the asked range, and a dropped tick
// outside it is not reported at all.
void test_event_log_gap_stays_in_the_asked_range() {
    using eawr::platform::LiveEventLog;
    {
        // Only tick 10 had events; a tick bound of 5 drops it at completed tick 15.
        LiveEventLog log({.ticks = 5, .bytes = std::size_t{1} << 20U});
        for (std::uint64_t tick = 1; tick <= 15; ++tick) log.record(busy_snapshot(tick, 0, tick == 10 ? 1 : 0));
        expect(log.size() == 0, "tick 10 left the log");
        expect(!log.after(0, 2).lost_through, "events_after(0, 2) lost nothing: tick 10 is outside it");
        expect(log.after(0, 10).lost_through == 10, "events_after(0, 10) lost tick 10");
        expect(log.after(9, 15).lost_through == 10, "events_after(9, 15) lost tick 10");
        expect(!log.after(10, 15).lost_through, "events_after(10, 15) lost nothing: the boundary");
        expect(!log.after(2, 2).lost_through, "an empty range loses nothing");
    }
    {
        // Ticks 1 and 10 had events and both were dropped: (0, 2] may have lost tick 1.
        LiveEventLog log({.ticks = 5, .bytes = std::size_t{1} << 20U});
        for (std::uint64_t tick = 1; tick <= 15; ++tick) {
            log.record(busy_snapshot(tick, 0, tick == 1 || tick == 10 ? 1 : 0));
        }
        expect(log.after(0, 2).lost_through == 2, "events_after(0, 2) reports its loss up to 2, not 10");
        expect(log.after(0, 15).lost_through == 10, "the whole range reports the newest dropped tick");
    }
    {
        // The exact eviction boundary: an event of tick T is kept through completed tick
        // T + ticks - 1 and dropped at T + ticks.
        LiveEventLog log({.ticks = 5, .bytes = std::size_t{1} << 20U});
        for (std::uint64_t tick = 1; tick <= 14; ++tick) log.record(busy_snapshot(tick, 0, tick == 10 ? 1 : 0));
        expect(log.size() == 1 && !log.after(0, 14).lost_through, "tick 10 is kept at completed tick 14");
        log.record(busy_snapshot(15, 0, 0));
        expect(log.size() == 0 && log.after(0, 15).lost_through == 10, "and dropped at completed tick 15");
    }
}

// Real-time pacing: orders submitted without a tick while the session runs are stamped with the
// next tick; the recorded replay reproduces every live hash headless.
void test_real_time_records_its_replay(const std::filesystem::path& fixtures) {
    const auto parsed = tactical::parse_replay(read_bytes(fixtures / "tactical-motion.eawr-replay"), "tactical-motion");
    if (!parsed) return;
    const auto& setup = parsed.value().setup;
    auto live = LiveSession::start(setup, {}, {}, motion_table(), {},
        {.workers = LiveSession::game_worker_count(), .pacing = LiveSession::Pacing::real_time});
    expect(static_cast<bool>(live), "a real-time session starts");
    if (!live) return;
    auto& session = *live.value();
    std::vector<std::uint64_t> seen;
    {
        const Reader reader(session);
        expect(session.wait_for(3, std::chrono::seconds(10)), "real time reaches tick 3");
        seen.push_back(session.completed_tick());
        session.submit({1, {1}, tactical::MovePayload{at(0, -1500, 0)}, std::nullopt});
        session.submit({2, {2}, tactical::MovePayload{at(-1500, 2500, 0)}, std::nullopt});
        expect(session.wait_for(seen.back() + 10, std::chrono::seconds(10)), "real time runs on");
        seen.push_back(session.completed_tick());
        session.submit({1, {1}, tactical::FacePayload{at(-600, 0, 0)}, std::nullopt});
        // Player 1 does not own unit 2: accepted into the queue, rejected at execution.
        session.submit({1, {2}, tactical::StopPayload{}, std::nullopt});
        // A tick that has already executed is refused at submission.
        session.submit({2, {2}, tactical::StopPayload{}, 1});
        expect(session.wait_for(seen.back() + 10, std::chrono::seconds(10)), "real time runs further");
        expect(reader.ordered(), "the real-time reader sees ordered frames");
    }
    session.stop();
    const auto hashes = session.tick_hashes();
    const auto replay = session.record();
    expect(!session.failure(), "no real-time step failed");
    expect(replay.final_tick_count == hashes.size() && hashes.size() == session.completed_tick(),
           "one hash per completed tick");
    expect(replay.commands.size() == 4, "the four accepted submissions are recorded");
    if (replay.commands.size() == 4) {
        expect(replay.commands[0].key.tick >= seen[0] && replay.commands[1].key.tick >= replay.commands[0].key.tick,
               "orders are stamped with the next tick when the simulation thread takes them");
        expect(replay.commands[2].key.tick >= seen[1], "later orders get later ticks");
    }
    bool late = false;
    bool not_owned = false;
    for (const auto& diagnostic : session.rejected_orders()) {
        late = late || diagnostic.code == tactical::diagnostic_codes::late_command;
        not_owned = not_owned || diagnostic.code == tactical::diagnostic_codes::command_rejected;
    }
    expect(late, "a late order is reported");
    expect(not_owned, "an order for another player's unit is reported at execution");
    expect(headless_hashes(replay) == hashes, "the recorded real-time run reproduces its hashes headless");
    const auto helper = eawr::platform::headless_tick_hashes(replay, {}, {}, motion_table(), {});
    expect(helper && helper.value() == hashes, "headless_tick_hashes gives the same hashes");
}

[[nodiscard]] tactical::PlayerCommand command_at(const std::uint64_t tick, const tactical::PlayerId player,
                                                  const std::uint64_t sequence, const eawr::sim::EntityId unit,
                                                  tactical::CommandPayload payload) {
    tactical::PlayerCommand command;
    command.key = {tick, player, sequence};
    command.units = {unit};
    command.payload = std::move(payload);
    return command;
}

// #316 review 1 and the UI-07 contract: a local player's input comes from the command source
// on the simulation thread with the scheduler's keys, and a debug order preloaded for a later
// tick of the same player neither blocks that input nor collides with its keys.
void test_command_source_and_held_orders(const std::filesystem::path& fixtures) {
    const auto parsed = tactical::parse_replay(read_bytes(fixtures / "tactical-motion.eawr-replay"), "tactical-motion");
    if (!parsed) return;
    const auto main_thread = std::this_thread::get_id();
    std::atomic<std::uint64_t> calls{0};
    std::atomic<bool> off_main{true};
    std::atomic<bool> in_order{true};
    // The scheduler's stream for player 1: tick 5 sequence 0, tick 12 sequence 7.
    const std::vector<tactical::PlayerCommand> input{
        command_at(5, 1, 0, 1, tactical::MovePayload{at(0, -1500, 0)}),
        command_at(12, 1, 7, 1, tactical::FacePayload{at(-600, 0, 0)})};
    LiveSession::Options options{.workers = 2, .pacing = LiveSession::Pacing::driven};
    options.command_source = [&, input](const std::uint64_t next_tick) {
        off_main = off_main && std::this_thread::get_id() != main_thread;
        in_order = in_order && calls.fetch_add(1) == next_tick;
        std::vector<tactical::PlayerCommand> due;
        for (const auto& command : input) {
            if (command.key.tick == next_tick) due.push_back(command);
        }
        return due;
    };
    auto live = LiveSession::start(parsed.value().setup, {}, {}, motion_table(), {}, options);
    expect(static_cast<bool>(live), "a session with a command source starts");
    if (!live) return;
    auto& session = *live.value();
    // Preloaded before any input: player 1 at tick 20, player 2 at tick 3.
    session.submit({1, {1}, tactical::StopPayload{}, 20});
    session.submit({2, {2}, tactical::MovePayload{at(-1500, 2500, 0)}, 3});
    session.advance_to(8);
    expect(session.wait_for(8, std::chrono::seconds(60)), "the session reaches tick 8");
    // An untimed order for the same player between its input commands.
    session.submit({1, {1}, tactical::MovePayload{at(400, -900, 0)}, std::nullopt});
    session.advance_to(30);
    expect(session.wait_for(30, std::chrono::seconds(60)), "the session reaches tick 30");
    session.stop();
    expect(!session.failure(), "no step failed with a command source");
    expect(session.rejected_orders().empty(), "a held later order does not reject the player's earlier input");
    expect(off_main, "the command source runs on the simulation thread");
    expect(in_order && calls == 30, "the command source is called once per tick, right before it runs");
    const auto replay = session.record();
    std::vector<std::tuple<std::uint64_t, tactical::PlayerId, std::uint64_t>> keys;
    for (const auto& command : replay.commands) keys.emplace_back(command.key.tick, command.key.player_id, command.key.sequence);
    const std::vector<std::tuple<std::uint64_t, tactical::PlayerId, std::uint64_t>> expected{
        {3, 2, 0}, {5, 1, 0}, {8, 1, 1}, {12, 1, 7}, {20, 1, 8}};
    expect(keys == expected, "source keys are kept and held orders take the next sequence at their tick");
    expect(headless_hashes(replay) == session.tick_hashes(), "the run with a command source reproduces headless");
}

// #316 review 2: an exception on the simulation thread becomes the session's failure instead
// of terminating the process; waiters wake and stop() joins.
void test_simulation_thread_exception() {
    for (const bool standard : {true, false}) {
        const std::string label = standard ? "std::exception: " : "non-standard exception: ";
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
        LiveSession::Options options{.workers = 2, .pacing = LiveSession::Pacing::driven};
        options.command_source = [standard](const std::uint64_t next_tick) -> std::vector<tactical::PlayerCommand> {
            if (next_tick == 3) {
                if (standard) throw std::runtime_error("injected fault");
                throw 42;
            }
            return {};
        };
        auto live = LiveSession::start(setup, {}, {}, {}, {}, options);
        expect(static_cast<bool>(live), label + "the session starts");
        if (!live) continue;
        auto& session = *live.value();
        session.advance_to(10);
        const auto started = std::chrono::steady_clock::now();
        expect(!session.wait_for(10, std::chrono::seconds(30)), label + "the failed session never reaches tick 10");
        expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(20), label + "waiters wake on the failure");
        const auto failure = session.failure();
        expect(failure && failure->code == tactical::diagnostic_codes::worker_failure, label + "the exception is the session's failure");
        expect(failure && failure->message.find("stopped before tick 3") != std::string::npos, label + "the failure names the tick");
        if (failure && standard) expect(failure->message.find("injected fault") != std::string::npos, label + "the failure names the exception");
        expect(session.completed_tick() == 3, label + "stepping stops before the failing tick");
        session.stop();
        expect(session.tick_hashes().size() == 3, label + "the completed ticks keep their hashes");
    }
}

// #77: a live session bound to victory rules decides the outcome on its own thread, publishes it
// in its snapshots and records a replay whose headless hashes, with the same rules, match.
void test_victory_outcome() {
    tactical::TacticalSetup setup;
    setup.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable}};
    for (const auto& [id, owner] : {std::pair{1U, 1U}, std::pair{2U, 2U}}) {
        tactical::UnitState station;
        station.entity_id = id;
        station.type_id = 40;
        station.owner = owner;
        station.position = at(id == 1 ? -3000 : 3000, 0, 0);
        setup.units.push_back(station);
    }
    tactical::DurabilityTable durability;
    durability.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    durability.profiles = {{40, units(2400), std::nullopt, false, {}}};
    tactical::VictoryRules victory;
    victory.condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    victory.starbase_types = {40};
    victory.contenders = {1, 2};
    victory.humans = {1};
    auto live = LiveSession::start(setup, {}, durability, {}, {},
        {.workers = 2, .pacing = LiveSession::Pacing::driven}, victory);
    expect(static_cast<bool>(live), "a live session with victory rules starts");
    if (!live) return;
    auto& session = *live.value();
    session.submit({1, {2}, tactical::DamagePayload{units(3000)}, 3});
    session.advance_to(6);
    expect(session.wait_for(6, std::chrono::seconds(30)), "the victory session reaches tick 6");
    const auto latest = session.frame().latest;
    expect(latest && latest->outcome() && latest->outcome()->winner == 1U && latest->outcome()->decided_tick == 3
            && latest->outcome()->deciding_unit == 2U,
        "the live snapshot carries the outcome: player 1 wins at tick 3");
    const auto decided = session.snapshot_at(4);
    bool reported = false;
    if (decided) {
        for (const auto& event : decided->events()) reported = reported || event.kind == tactical::EventKind::victory;
    }
    expect(reported, "the deciding tick's snapshot has the victory event");
    session.stop();
    const auto headless = eawr::platform::headless_tick_hashes(session.record(), {}, durability, {}, {}, victory);
    expect(headless && headless.value() == session.tick_hashes(), "the live victory run matches its headless replay");
}

// #459 (docs/behaviour/tactical-time-controls.md TP-01 to TP-03, TMC-05, TMC-06) and #453
// (battle-end.md BEP-02): pausing, changing the target rate and halting change only when ticks
// run. A real-time run that is paused, given orders while paused, fast-forwarded, slowed and
// halted records a replay whose headless hashes equal its own, and a plain driven run of the
// same commands at the same ticks gives the same hashes. Orders given while paused run at the
// tick after the pause.
void test_time_controls_keep_hashes(const std::filesystem::path& fixtures) {
    const auto parsed = tactical::parse_replay(read_bytes(fixtures / "tactical-motion.eawr-replay"), "tactical-motion");
    if (!parsed) return;
    const auto& setup = parsed.value().setup;
    using namespace std::chrono_literals;
    auto live = LiveSession::start(setup, {}, {}, motion_table(), {},
        {.workers = 2, .pacing = LiveSession::Pacing::real_time, .target_rate = 120});
    expect(static_cast<bool>(live), "a time-controlled session starts");
    if (!live) return;
    auto& session = *live.value();
    std::uint64_t paused_at = 0;
    std::uint64_t halted_at = 0;
    {
        const Reader reader(session);
        expect(session.wait_for(5, std::chrono::seconds(10)), "fast forward reaches tick 5");
        session.set_paused(true);
        expect(session.paused(), "the session reports the pause");
        std::this_thread::sleep_for(100ms);
        paused_at = session.completed_tick();
        // Orders given while paused wait for the next tick (TM-10).
        session.submit({1, {1}, tactical::MovePayload{at(0, -1500, 0)}, std::nullopt});
        session.submit({2, {2}, tactical::MovePayload{at(-1500, 2500, 0)}, std::nullopt});
        std::this_thread::sleep_for(300ms);
        expect(session.completed_tick() == paused_at, "no tick runs while paused");
        expect(session.rejected_orders().empty(), "orders given while paused are accepted");
        // The slowest step: at most one tick per 100 ms, whatever the host's speed.
        session.set_target_rate(10);
        session.set_paused(false);
        const auto slow_start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(350ms);
        const auto slow_ticks = session.completed_tick() - paused_at;
        const auto slow_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - slow_start).count();
        expect(slow_ticks <= static_cast<std::uint64_t>(slow_ms / 100 + 1), "rate 10 runs at most one tick per 100 ms");
        session.set_target_rate(120);
        expect(session.wait_for(paused_at + 30, std::chrono::seconds(10)), "fast forward runs on after the pause");
        session.set_paused(true);
        session.set_paused(false);
        session.set_target_rate(45);
        halted_at = session.completed_tick() + 12;
        session.halt_at(halted_at);
        session.halt_at(halted_at + 100); // a later limit never raises it
        expect(session.halt_tick() && *session.halt_tick() == halted_at, "the halt keeps the lower limit");
        expect(session.wait_for(halted_at, std::chrono::seconds(10)), "the session reaches the halt tick");
        expect(!session.wait_for(halted_at + 1, 300ms), "the session never steps past the halt tick");
        expect(reader.ordered(), "the time-controlled reader sees ordered frames");
    }
    session.stop();
    expect(!session.failure(), "no time-controlled step failed");
    const auto hashes = session.tick_hashes();
    const auto replay = session.record();
    expect(session.completed_tick() == halted_at && replay.final_tick_count == halted_at,
           "the run and its replay end at the halt tick");
    expect(replay.commands.size() == 2, "both orders given while paused are recorded");
    for (const auto& command : replay.commands) {
        expect(command.key.tick == paused_at, "an order given while paused runs at the tick after the pause");
    }
    expect(headless_hashes(replay) == hashes, "the time-controlled run reproduces its hashes headless");
    // The same commands at the same ticks, stepped without any pause or rate change.
    auto plain = LiveSession::start(setup, {}, {}, motion_table(), {},
        {.workers = 1, .pacing = LiveSession::Pacing::driven});
    expect(static_cast<bool>(plain), "the plain session starts");
    if (!plain) return;
    for (const auto& command : replay.commands) plain.value()->submit(order_of(command, true));
    plain.value()->advance_to(halted_at);
    expect(plain.value()->wait_for(halted_at, std::chrono::seconds(60)), "the plain run completes");
    plain.value()->stop();
    expect(plain.value()->tick_hashes() == hashes, "pauses and speed changes leave every tick hash unchanged");

    // Driven pacing holds while paused too, and stops at the halt tick.
    auto driven = LiveSession::start(setup, {}, {}, motion_table(), {},
        {.workers = 2, .pacing = LiveSession::Pacing::driven});
    if (!driven) return;
    driven.value()->set_paused(true);
    driven.value()->advance_to(10);
    expect(!driven.value()->wait_for(1, 200ms), "a paused driven session does not step");
    driven.value()->halt_at(6);
    driven.value()->set_paused(false);
    expect(driven.value()->wait_for(6, std::chrono::seconds(30)), "the driven session resumes");
    expect(!driven.value()->wait_for(7, 200ms), "the driven session stops at its halt tick");
    driven.value()->stop();
    expect(driven.value()->tick_hashes().size() == 6, "six ticks ran");
}

void test_start_rejects_worker_counts() {
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    expect(!LiveSession::start(setup, {}, {}, {}, {}, {.workers = 0}), "zero workers are refused");
    expect(!LiveSession::start(setup, {}, {}, {}, {}, {.workers = 257}), "more than 256 workers are refused");
    expect(LiveSession::game_worker_count() >= 1, "the game pool has at least one worker");
    auto idle = LiveSession::start(setup, {}, {}, {}, {}, {.workers = 2, .pacing = LiveSession::Pacing::driven});
    expect(static_cast<bool>(idle), "an empty session starts");
    if (idle) {
        expect(!idle.value()->wait_for(1, std::chrono::milliseconds(50)), "driven pacing waits for advance_to");
        idle.value()->stop();
        idle.value()->stop();
        expect(idle.value()->tick_hashes().empty(), "no tick ran");
    }
}

} // namespace

int main(const int argc, const char* const argv[]) {
    if (argc != 2) {
        std::cerr << "usage: tactical_live_session_tests <fixture directory>\n";
        return 2;
    }
    test_start_rejects_worker_counts();
    test_victory_outcome();
    test_driven_matches_headless(argv[1]);
    test_event_log_outlives_the_history();
    test_event_log_bounds_a_high_event_rate();
    test_event_log_gap_stays_in_the_asked_range();
    test_real_time_records_its_replay(argv[1]);
    test_command_source_and_held_orders(argv[1]);
    test_time_controls_keep_hashes(argv[1]);
    test_simulation_thread_exception();
    if (failures != 0) {
        std::cerr << failures << " live session check(s) failed\n";
        return 1;
    }
    std::cout << "live session contracts passed\n";
    return 0;
}
