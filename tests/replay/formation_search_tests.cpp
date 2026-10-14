#include "formation_support.hpp"

#include <array>
#include <atomic>

namespace formation_test_support {

class PhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        calls.emplace_back(std::string{}, count);
        return inline_executor.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        calls.emplace_back(std::string(phase), count);
        return inline_executor.execute(count, partition);
    }
    mutable std::vector<std::pair<std::string, std::size_t>> calls;

private:
    eawr::sim::InlineExecutor inline_executor;
};

// #503: the path searches due together in different layers (AV-01) run in the partitioned
// `plan-searches` phase: the front corvette and Nebulon-B in the order's tick, the second pair
// two frames later (FM-08), and in no other tick. The hashes stay the serial ones (the C-12
// pins in test_group_session and its worker equality).
void test_plan_searches_phase() {
    const auto replay = group_replay();
    const auto reference = run(replay, 1, false);
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
    expect(created && reference, "the plan-searches session replays");
    if (!created || !reference) return;
    auto session = std::move(created).value();
    const PhaseRecorder recorder;
    std::vector<std::uint64_t> searched;
    for (std::uint64_t tick = 0; tick < 60; ++tick) {
        recorder.calls.clear();
        const auto stepped = session.step(recorder);
        expect(stepped && stepped.value().state_sha256 == reference->hashes[tick], "the recorded step hashes like 1 worker");
        for (const auto& [name, partitions] : recorder.calls) {
            if (name != "plan-searches") continue;
            expect(partitions == eawr::sim::tick_partition_count, "plan-searches is partitioned");
            searched.push_back(tick);
        }
    }
    expect(searched == std::vector<std::uint64_t>{30, 32}, "plan-searches runs in the order's tick and the staggered one");
}

// PC-07 (#520): single moves of ships in different layers, in one tick, search side by side in
// the partitioned `plan-searches` phase (one lane per layer), with the serial hashes.
void test_single_lanes() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {3, frigate_type, 1, at(-2100, -2100, -90), yaw(0), {}},
        {7, corvette_type, 1, at(-1800, -900, -20), yaw(0), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        {{30, 1, 0}, {1}, tactical::MovePayload{at(1200, -1500, 0)}},
        {{30, 1, 1}, {7}, tactical::MovePayload{at(1200, -900, 0)}},
        {{30, 1, 2}, {3}, tactical::MovePayload{at(1200, -2100, 0)}},
    };
    const auto reference = run(replay, 1, false);
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
    expect(created && reference, "the single-lanes session replays");
    if (!created || !reference) return;
    auto session = std::move(created).value();
    const PhaseRecorder recorder;
    std::vector<std::uint64_t> searched;
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
        recorder.calls.clear();
        const auto stepped = session.step(recorder);
        expect(stepped && stepped.value().state_sha256 == reference->hashes[tick], "the lanes step hashes like 1 worker");
        for (const auto& [name, partitions] : recorder.calls) {
            if (name == "plan-searches") searched.push_back(tick);
        }
    }
    // The first corvette and the frigate together, then the second corvette after the first's plan.
    expect(searched == std::vector<std::uint64_t>{30}, "single moves of two layers search in plan-searches in their tick");
    const auto path = [&](const eawr::sim::EntityId id) {
        const auto state = session.motion_state(id);
        return state && state->kind == tactical::MotionKind::path;
    };
    expect(path(1) && path(3) && path(7), "every single move planned in its tick");
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const auto again = run(replay, workers, true);
        expect(again && again->hashes == reference->hashes, "the lanes hash alike on scrambled workers");
    }
}

// PC-07, PC-08 (#520): a layer searches in a tick only within its budget. With a budget of 1
// every search passes it: the first of each layer is given up at the budget and the others do
// not start; each runs in slices from where its unit will be search_delay (4) frames later and
// lands then. Until the landing the ships keep their plan (here: at rest); a stop in between
// drops the search. The slice size changes no result, and neither does the worker count.
void test_search_budget() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {2, corvette_type, 1, at(-1800, -1000, -20), yaw(0), {}},
        {3, frigate_type, 1, at(-2100, -2100, -90), yaw(0), {}},
        {7, corvette_type, 1, at(-1800, -500, -20), yaw(0), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        {{30, 1, 0}, {1}, tactical::MovePayload{at(1200, -1500, 0)}},
        {{30, 1, 1}, {2}, tactical::MovePayload{at(1200, -1000, 0)}},
        {{30, 1, 2}, {7}, tactical::MovePayload{at(1200, -500, 0)}},
        {{30, 1, 3}, {3}, tactical::MovePayload{at(1200, -2100, 0)}},
        {{32, 1, 0}, {7}, tactical::StopPayload{}},
    };
    auto table = foc_table();
    table.avoidance->search_budget = 1;
    // The units with a path after each of ticks 30 to 35, and every tick's hash.
    std::vector<std::size_t> in_flight;
    const auto trace = [&](const tactical::MotionTable& with, std::vector<std::string>& hashes) {
        in_flight.clear();
        std::vector<std::vector<eawr::sim::EntityId>> planned;
        auto created = tactical::TacticalSession::from_replay(replay, {}, {}, with);
        expect(static_cast<bool>(created), "the budget session starts");
        if (!created) return planned;
        auto session = std::move(created).value();
        const eawr::sim::InlineExecutor executor;
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the budget session steps");
            if (!stepped) return planned;
            hashes.push_back(stepped.value().state_sha256);
            if (tick < 30 || tick > 35) continue;
            // PC-08: a search lives from its start to its landing or its drop, no longer.
            in_flight.push_back(session.sliced_searches().searches);
            planned.emplace_back();
            for (const eawr::sim::EntityId id : {1, 2, 3, 7}) {
                const auto state = session.motion_state(id);
                if (state && state->kind == tactical::MotionKind::path) planned.back().push_back(id);
            }
        }
        return planned;
    };
    std::vector<std::string> hashes;
    const auto planned = trace(table, hashes);
    const std::vector<std::vector<eawr::sim::EntityId>> expected{{}, {}, {}, {}, {1, 2, 3}, {1, 2, 3}};
    expect(planned == expected, "every search lands 4 frames after its order; the stopped ship's is dropped");
    expect(in_flight == std::vector<std::size_t>{4, 4, 3, 3, 0, 0}, "the searches live until their landing or their drop");
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const auto again = run(replay, workers, workers > 1, table);
        expect(again && again->hashes == hashes, "the sliced searches hash alike on every worker count");
    }
    for (const std::uint64_t slice : {std::uint64_t{1}, std::uint64_t{50}}) {
        auto sliced = table;
        sliced.avoidance->search_slice = slice;
        std::vector<std::string> other;
        static_cast<void>(trace(sliced, other));
        expect(other == hashes, "the slice size changes no hash");
    }
    // The default budget plans all four in the order's tick, as FoC does (PC-02).
    std::vector<std::string> plain;
    const auto base = trace(foc_table(), plain);
    const std::vector<eawr::sim::EntityId> all{1, 2, 3, 7};
    expect(base.size() == 6 && base[0] == all && base[1] == all, "the default budget plans every single move in its tick");
}

// FSC-70/FD-64: exhausted initialization retains B/C and admits a retry at t+61 only.
void test_failed_path_retry() {
    class SearchCounter final : public tactical::PathSearchProbe {
    public:
        std::uint64_t now() noexcept override { return 0; }
        void searched(const tactical::PathSearchStats& stats) noexcept override {
            if (!stats.slot && stats.entity == 1) ++calls;
        }
        std::atomic<std::uint64_t> calls{};
        ~SearchCounter() override { tactical::set_path_search_probe(nullptr); }
    };
    constexpr tactical::TypeId wall_type = 9001;
    constexpr std::array<std::uint8_t, 4> retry_tag{'R', 'T', 'R', 'Y'};
    const auto contains_retry = [&](const tactical::TacticalSession& session,
                                    const std::optional<std::uint64_t> failure = std::nullopt,
                                    std::uint64_t* const captured_failure = nullptr) {
        const auto bytes = session.canonical_state_bytes();
        const auto found = std::search(bytes.begin(), bytes.end(), retry_tag.begin(), retry_tag.end());
        if (found == bytes.end()) return false;
        const auto offset = static_cast<std::size_t>(found - bytes.begin());
        expect(bytes.size() - offset >= 96, "FSC-70: RTRY includes the retained failure history");
        if (bytes.size() - offset < 96) return true;
        const auto read = [&](const std::size_t relative, const std::size_t count) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < count; ++i)
                value |= static_cast<std::uint64_t>(bytes[offset + relative + i]) << (8U * i);
            return value;
        };
        expect(read(4, 4) == 1 && read(8, 8) == 1 && read(16, 8) == 1,
            "FSC-70: versioned RTRY contains the failed unit only");
        expect(read(24, 8) == read(88, 8) + 61,
            "FSC-70: canonical state retains failure frame and strict retry deadline");
        if (captured_failure) *captured_failure = read(88, 8);
        if (failure) expect(read(88, 8) == *failure,
            "FSC-70: engine disable/recovery does not reset the failure frame");
        return true;
    };
    for (const std::uint64_t budget : {std::uint64_t{100000}, std::uint64_t{1}}) {
        auto table = foc_table();
        table.rules.max_waypoints = 6;
        table.avoidance->search_budget = budget;
        table.avoidance->max_expansions = 128;
        // AV-11 permits blocked bounded steps near the start at a penalty. Make the
        // enclosing shell thicker than even the later attempts' first forward step,
        // while leaving the start and both destinations clear.
        table.footprints.push_back({.type_id = wall_type, .layer = tactical::SpaceLayer::static_object,
            .radius = units(800), .obstacle = true});
        tactical::TacticalReplay replay;
        replay.setup = setup({{1, corvette_type, 1, at(0, 0, 0), yaw(0), {}},
            {9, wall_type, 1, at(1000, 0, 0), yaw(0), {}},
            {10, wall_type, 1, at(-1000, 0, 0), yaw(0), {}},
            {11, wall_type, 1, at(0, 1000, 0), yaw(0), {}},
            {12, wall_type, 1, at(0, -1000, 0), yaw(0), {}}});
        std::vector<tactical::TrackedLeaf> cage;
        for (std::size_t i = 1; i < replay.setup.units.size(); ++i) {
            const auto& obstacle = replay.setup.units[i];
            const tactical::Prediction still{obstacle.position, {}};
            const auto leaf = tactical::tracking_leaf(obstacle.entity_id, *table.footprint(wall_type), still, still);
            expect(static_cast<bool>(leaf), "FSC-70: cage obstacles have valid tracking leaves");
            if (leaf) cage.push_back(leaf.value());
        }
        bool exhausted = false;
        tactical::PathSearchStats blocked_stats;
        const auto blocked = tactical::plan_space_move(table, *table.find(corvette_type), *table.footprint(corvette_type),
            tactical::CollisionWorld{90, {}, &cage}, 1, 0, at(0, 0, 0), {}, {}, at(2400, 0, 0), &blocked_stats,
            tactical::PathSearchMode::bounded, false, std::nullopt, &exhausted);
        expect(blocked && exhausted && blocked_stats.tries == table.avoidance->tries,
            "FSC-70: shell exhausts every planner attempt before testing retained state");
        bool control_exhausted = true;
        const auto control = tactical::plan_space_move(table, *table.find(corvette_type), *table.footprint(corvette_type),
            tactical::CollisionWorld{90, {}, nullptr}, 1, 0, at(0, 0, 0), {}, {}, at(2400, 0, 0), nullptr,
            tactical::PathSearchMode::bounded, false, std::nullopt, &control_exhausted);
        expect(control && !control_exhausted && control.value().kind == tactical::MotionKind::path,
            "FSC-70: the same unblocked route succeeds");
        // AV-14 can accept a half-speed later attempt. Leave enough travel time for B
        // after the cooldown; the retry boundary assertions below remain at +60/+61.
        replay.final_tick_count = 2000;
        replay.commands = {{{0, 1, 0}, {1}, tactical::MovePayload{at(0, 0, 0), false, true}},
            {{0, 1, 1}, {1}, tactical::MovePayload{at(2400, 0, 0), false, true}},
            {{0, 1, 2}, {1}, tactical::MovePayload{at(3000, 0, 0), false, true}}};
        std::vector<std::string> reference;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto created = tactical::TacticalSession::from_replay(replay, {}, {}, table);
            expect(static_cast<bool>(created), "FSC-70: blocked route starts");
            if (!created) continue;
            auto session = std::move(created).value();
            SearchCounter counter;
            tactical::set_path_search_probe(&counter);
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::optional<std::uint64_t> failed;
            bool retried = false, reached_c = false;
            std::vector<std::string> hashes;
            for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
                counter.calls = 0;
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "FSC-70: exhaustion never fails the battle step");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto state = session.units().front();
                if (!failed && contains_retry(session)) {
                    failed = tick;
                    expect(budget == 1 ? tick > 1U : tick == 1U,
                        "FSC-70: close A completes without failure; only B exhaustion stamps the clock");
                    expect(state.order.destination == at(2400, 0, 0), "FSC-70: A completed; failed B is current");
                    for (const auto obstacle : {9U, 10U, 11U, 12U})
                        expect(static_cast<bool>(session.stage_remove(obstacle)), "FSC-70: remove obstruction during cooldown");
                }
                if (failed && tick <= *failed + 60) {
                    expect(contains_retry(session) && state.order.destination == at(2400, 0, 0)
                        && state.position == at(0, 0, 0), "FSC-70: B is retained through equality t+60");
                    if (tick > *failed) expect(counter.calls == 0,
                        "FSC-70: no centre-path attempt at t+1, t+2 or t+60");
                }
                if (failed && tick == *failed + 61) {
                    retried = !contains_retry(session);
                    expect(retried && counter.calls > 0 && state.order.destination == at(2400, 0, 0),
                        "FSC-70: t+61 admits B, preserving pending C");
                    if (budget == 1) expect(session.sliced_searches().searches == 1,
                        "FSC-70: budgeted retry starts sliced at t+61");
                    else expect(session.motion_state(1) && session.motion_state(1)->kind == tactical::MotionKind::path,
                        "FSC-70: unbudgeted retry installs B at t+61");
                }
                reached_c |= state.order.destination == at(3000, 0, 0);
                session.scramble_storage_for_testing();
            }
            expect(failed.has_value() && retried, "FSC-70: failed B is retained and retries");
            expect(reached_c, "FSC-70: successful B eventually continues to C");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "FSC-70: retained retries equal on 1/2/4/8 workers");
        }
        // EN-08/09 + UFS-33: a real draining hit and its expiry preserve the failed B wait.
        constexpr tactical::TypeId shooter_type = 9002;
        tactical::DurabilityTable health;
        tactical::DamageRules damage;
        damage.shield_recharge_frames = 90;
        damage.depleted_disable_seconds = units(5);
        damage.depleted_regen_cap = decimal("0.25");
        damage.diminishing = {{units(0), units(1)}, {units(2), units(1)}};
        damage.damage_types = 1;
        damage.armor_types = 1;
        damage.armor_mods = {units(1)};
        damage.energy_recharge_frames = 150;
        damage.energy_to_shield = units(5);
        health.damage = damage;
        health.rules.engines_disabled_speed = decimal("0.4");
        tactical::DurabilityProfile durable;
        durable.type_id = corvette_type;
        durable.max_hull = units(1000);
        durable.max_speed = table.find(corvette_type)->max_speed;
        durable.powered = true;
        durable.max_energy = units(300);
        durable.armor_type = 0;
        health.profiles = {durable};
        tactical::WeaponProfile weapon;
        weapon.range = units(700);
        weapon.min_recharge_hundredths = 100;
        weapon.max_recharge_hundredths = 100;
        weapon.pulse_count = 1;
        weapon.cone_width = units(360);
        weapon.cone_height = units(180);
        weapon.opportunity_when_idle = true;
        weapon.opportunity_when_targeting = true;
        weapon.shot = tactical::ShotProfile{units(15), 0, units(100), units(700), true, false, {}};
        weapon.shot->energy_damage = true;
        weapon.shot->disable_engines_frames = 10;
        tactical::CombatProfile target;
        target.type_id = corvette_type;
        target.category_bits = 1;
        target.collision = tactical::CollisionBox{at(-20, -10, -10), at(20, 10, 10)};
        tactical::CombatProfile shooter;
        shooter.type_id = shooter_type;
        shooter.category_bits = 2;
        shooter.max_attack_distance = units(700);
        shooter.weapons = {weapon};
        shooter.collision = target.collision;
        tactical::CombatTable combat;
        combat.profiles = {target, shooter};
        const std::vector<tactical::SensorProfile> sensors{{shooter_type, units(3000)}};
        auto engine_replay = replay;
        engine_replay.setup.players.push_back({2, 2, 2, tactical::player_flag_commandable});
        reference.clear();
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto created = tactical::TacticalSession::from_replay(engine_replay, sensors, health, table,
                std::nullopt, combat);
            expect(static_cast<bool>(created), "FSC-70: engine cooldown session starts");
            if (!created) continue;
            auto session = std::move(created).value();
            SearchCounter counter;
            tactical::set_path_search_probe(&counter);
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::optional<std::uint64_t> failed, shooter_id, recovery;
            std::uint64_t failure_frame{};
            bool disabled = false, recovered = false, retried = false;
            std::vector<std::string> hashes;
            for (std::uint64_t tick = 0; tick < 1000; ++tick) {
                counter.calls = 0;
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "FSC-70: engine cooldown steps successfully");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                if (!failed && contains_retry(session, std::nullopt, &failure_frame)) {
                    failed = tick;
                    for (const auto obstacle : {9U, 10U, 11U, 12U})
                        expect(static_cast<bool>(session.stage_remove(obstacle)), "FSC-70: clear cage before engine hit");
                    const auto spawned = session.stage_spawn({0, shooter_type, 2, at(-100, 0, 0), yaw(0), {}});
                    expect(static_cast<bool>(spawned), "FSC-70: spawn ion shooter during cooldown");
                    if (spawned) shooter_id = spawned.value();
                }
                const auto durability = session.durability_state(1);
                if (durability && durability->engines_disabled_until && !disabled) {
                    disabled = true;
                    recovery = durability->engines_disabled_until;
                    expect(failed && tick > *failed && tick < *failed + 60 && durability->energy < units(300),
                        "EN-08: positive projectile drain disables engines inside the failure cooldown");
                    if (shooter_id) expect(static_cast<bool>(session.stage_remove(*shooter_id)),
                        "FSC-70: remove shooter after one disabling hit");
                }
                if (recovery && tick == *recovery) {
                    recovered = durability && !durability->engines_disabled_until;
                    expect(recovered && failed && tick < *failed + 60,
                        "EN-09: engines recover inside the failure cooldown");
                }
                if (failed && tick <= *failed + 60) {
                    const auto state = session.units().front();
                    const auto motion = session.motion_state(1);
                    expect(contains_retry(session, failure_frame) && motion && motion->kind == tactical::MotionKind::none
                            && state.position == at(0, 0, 0) && state.order.destination == at(2400, 0, 0),
                        "FSC-70: disabling hit and recovery retain B and its rest state through t+60");
                    if (tick > *failed) expect(counter.calls == 0,
                        "FSC-70: engine transitions admit no search before t+61");
                }
                if (failed && tick == *failed + 61) {
                    const auto state = session.units().front();
                    retried = counter.calls > 0 && !contains_retry(session);
                    expect(retried && state.order.destination == at(2400, 0, 0),
                        "FSC-70: disabled/recovered B retries at the original failure +61");
                    break;
                }
                session.scramble_storage_for_testing();
            }
            expect(failed && disabled && recovered && retried,
                "FSC-70: both engine transitions are exercised before the strict retry");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "FSC-70: engine cooldown retries equal on 1/2/4/8 workers");
        }
        // An ordinary initial Move shares the producer; a second exhausted attempt renews it.
        replay.commands = {{{0, 1, 0}, {1}, tactical::MovePayload{at(2400, 0, 0)}}};
        auto repeated = tactical::TacticalSession::from_replay(replay, {}, {}, table);
        expect(static_cast<bool>(repeated), "FSC-70: repeated failure control starts");
        if (repeated) {
            auto& session = repeated.value();
            SearchCounter counter;
            tactical::set_path_search_probe(&counter);
            const eawr::sim::InlineExecutor executor;
            std::optional<std::uint64_t> first_failure, second_failure;
            bool retry_started = false, renewed_retry = false;
            for (std::uint64_t tick = 0; tick < 1000U; ++tick) {
                counter.calls = 0;
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "FSC-70: repeated exhausted searches step successfully");
                if (!stepped) break;
                if (!first_failure && contains_retry(session)) first_failure = tick;
                if (first_failure && tick == *first_failure + 61U) {
                    retry_started = counter.calls > 0;
                    expect(retry_started, "FSC-70: blocked route attempts again at first failure +61");
                }
                if (first_failure && !second_failure && tick >= *first_failure + 61U && contains_retry(session)) {
                    second_failure = tick;
                    for (const auto obstacle : {9U, 10U, 11U, 12U})
                        expect(static_cast<bool>(session.stage_remove(obstacle)), "FSC-70: clear obstruction after second failure");
                }
                if (second_failure && tick > *second_failure && tick <= *second_failure + 60U)
                    expect(counter.calls == 0 && contains_retry(session), "FSC-70: renewed cooldown remains strict");
                if (second_failure && tick == *second_failure + 61U) {
                    renewed_retry = counter.calls > 0 && !contains_retry(session);
                    expect(renewed_retry, "FSC-70: renewed cooldown retries at +61");
                    break;
                }
            }
            expect(first_failure && second_failure && retry_started && renewed_retry,
                "FSC-70: two actual exhausted searches renew the retained failure clock");
        }
    }
}

// FM-10: a new move, face or stop replaces a waiting member's staggered plan.
void test_wait_replaced() {
    auto replay = group_replay();
    replay.final_tick_count = 60;
    replay.commands.push_back({{31, 1, 1}, {1, 2}, tactical::StopPayload{}});
    const auto stopped = run(replay, 1, false);
    expect(stopped.has_value(), "the replaced group session runs");
    if (!stopped) return;
    const auto& list = stopped->units.back();
    const auto* first = unit_of(list, 1);
    const auto* second = unit_of(list, 2);
    const auto* start_first = unit_of(stopped->units[31], 1);
    const auto* start_second = unit_of(stopped->units[31], 2);
    expect(first && second && start_first && start_second && distance(first->position, start_first->position) < units(1)
            && distance(second->position, start_second->position) < units(1),
        "a stop in the next tick leaves both corvettes where they were (the waiting one never plans)");
    const auto again = run(replay, 4, true);
    expect(again && again->hashes == stopped->hashes, "the replaced group hashes alike on 4 scrambled workers");
}

void test_disabled_engine_group_session() {
    tactical::DurabilityTable durability;
    durability.rules.engines_disabled_speed = Fixed{};
    tactical::HardpointProfile engine;
    engine.role = tactical::HardpointRole::engine;
    engine.destroyable = true;
    engine.max_health = units(100);
    durability.profiles.push_back({corvette_type, units(1000), decimal("3.72"), false, {engine}});
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-500, 0, 0), yaw(0), {}},
        {2, corvette_type, 1, at(0, -100, 0), yaw(0), {}},
        {3, corvette_type, 1, at(-200, 100, 0), yaw(0), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        {{0, 1, 0}, {1}, tactical::DamagePayload{units(100), 0}},
        {{1, 1, 1}, {1, 2, 3}, tactical::MovePayload{at(2000, 0, 0)}},
    };
    std::vector<std::string> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto created = tactical::TacticalSession::from_replay(replay, {}, durability, foc_table());
        expect(static_cast<bool>(created), "the disabled-engine group session starts");
        if (!created) return;
        auto session = std::move(created).value();
        const bool scramble = workers > 1;
        if (scramble) session.scramble_storage_for_testing();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
            const auto stepped = session.step(executor);
            if (!stepped) {
                std::cerr << stepped.error().message << '\n';
                expect(false, "the disabled-engine group finishes every tick");
                return;
            }
            hashes.push_back(stepped.value().state_sha256);
            if (scramble) session.scramble_storage_for_testing();
        }
        if (workers == 1) reference = hashes;
        else {
            expect(hashes == reference,
                "the disabled-engine group hashes alike on " + std::to_string(workers) + " scrambled workers");
        }
        const auto units_list = session.units();
        expect(unit_of(units_list, 1)->position == at(-500, 0, 0), "the engine-disabled ship stays put");
        expect(unit_of(units_list, 2)->position.x > units(0) && unit_of(units_list, 3)->position.x > units(-200),
            "both movable ships travel after the group's order");
        expect(!session.motion_state(1) || session.motion_state(1)->kind == tactical::MotionKind::none,
            "the engine-disabled ship has no motion plan");
    }
}

// --- A block of 16 (#613) -----------------------------------------------------------------------

// The owner's eye-check order in small: a block of four columns by four rows, 400 apart, the
// corvettes in the two northern rows and the Nebulon-Bs in the two southern ones, all facing +X,
// ordered as one group to a point 3,680 units to the south-east.
constexpr std::int64_t block_spacing = 400;
[[nodiscard]] Vec3 block_position(const std::size_t index) {
    const auto column = static_cast<std::int64_t>(index % 4);
    const auto row = static_cast<std::int64_t>(index / 4);
    return at(block_spacing * column - 600, 600 - block_spacing * row, row < 2 ? -20 : -90);
}
[[nodiscard]] tactical::TypeId block_type(const std::size_t index) { return index < 8 ? corvette_type : frigate_type; }
const Vec3 block_target = at(2600, -2600, 0);

[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

// FM-02 to FM-09 on the block, against the rule rather than by hand: each layer maps on its own
// around the target; every ship's slot lies on the ray from the target along its own XY offset
// from its layer's centroid (world space: no turn to the move's heading, no rows); the ship
// nearest the centroid sits one occupation radius out, and each later one at the first 50-unit
// step (max(50, r / 4)) along its ray that clears every other slot of its layer by the two
// radii; the layers plan frigates first, each front first along the centroid's shift to the
// target, two frames apart; each ship plans with its maximum speed times its time over the
// group's time.
void test_block_slots() {
    std::vector<tactical::FormationMember> members;
    for (std::size_t index = 0; index < 16; ++index) {
        members.push_back(member(static_cast<eawr::sim::EntityId>(index + 1), block_type(index), block_position(index)));
    }
    const auto mapped = tactical::map_group_move(members, block_target, tactical::CollisionWorld{90, {}, nullptr}, 31,
        *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 16, "the block maps sixteen slots");
    if (!mapped || mapped.value().size() != 16) return;
    const auto& slots = mapped.value();
    Fixed slowest{};
    for (const auto& ship : members) slowest = std::max(slowest, tactical::time_to_reach(ship, block_target).value());
    for (const auto layer_type : {frigate_type, corvette_type}) {
        double cx = 0;
        double cy = 0;
        for (std::size_t index = 0; index < 16; ++index) {
            if (block_type(index) != layer_type) continue;
            cx += real(members[index].position.x) / 8;
            cy += real(members[index].position.y) / 8;
        }
        const double shift_x = real(block_target.x) - cx;
        const double shift_y = real(block_target.y) - cy;
        std::vector<const tactical::FormationSlot*> layer_slots;
        for (const auto& slot : slots) {
            if (block_type(slot.entity - 1) == layer_type) layer_slots.push_back(&slot);
        }
        expect(layer_slots.size() == 8, "each layer maps its eight ships");
        double previous_ahead = 1e18;
        double nearest = 1e18;
        for (std::size_t order = 0; order < layer_slots.size(); ++order) {
            const auto& slot = *layer_slots[order];
            const auto& ship = members[slot.entity - 1];
            const double ox = real(ship.position.x) - cx;
            const double oy = real(ship.position.y) - cy;
            nearest = std::min(nearest, std::hypot(ox, oy));
            const double sx = real(slot.destination.x) - real(block_target.x);
            const double sy = real(slot.destination.y) - real(block_target.y);
            const std::string name = "ship " + std::to_string(slot.entity);
            // FM-03, FM-05: on its own ray, in world space.
            const double along = (sx * ox + sy * oy) / std::hypot(ox, oy);
            expect(std::abs(sx * oy - sy * ox) / std::hypot(ox, oy) < 0.01 && along > 0,
                name + "'s slot lies on the ray along its offset from its layer's centroid");
            expect(slot.destination.z == block_target.z, name + "'s slot keeps the target's height");
            // FM-05: slots of a layer clear each other by the two radii; the step before a slot
            // on its ray (if any) would not.
            for (const auto* other : layer_slots) {
                if (other == &slot) continue;
                const auto& other_ship = members[other->entity - 1];
                const double gap = std::hypot(real(slot.destination.x) - real(other->destination.x),
                    real(slot.destination.y) - real(other->destination.y));
                expect(gap >= real(ship.occupation_radius) + real(other_ship.occupation_radius) - 0.01,
                    name + " and ship " + std::to_string(other->entity) + " sit two radii apart");
            }
            const double step = std::max(50.0, real(ship.occupation_radius) / 4);
            const double steps = along / step;
            const bool first_mapped = std::abs(along - real(ship.occupation_radius)) < 0.01;
            expect(first_mapped || std::abs(steps - std::round(steps)) < 0.001,
                name + "'s slot is one radius out or a whole number of steps along its ray");
            if (!first_mapped && along > step / 2) {
                const double back_x = real(block_target.x) + sx * (along - step) / along;
                const double back_y = real(block_target.y) + sy * (along - step) / along;
                bool blocked = false;
                for (const auto* other : layer_slots) {
                    if (other == &slot) continue;
                    const double gap = std::hypot(back_x - real(other->destination.x), back_y - real(other->destination.y));
                    blocked = blocked || gap < real(ship.occupation_radius) + real(members[other->entity - 1].occupation_radius);
                }
                expect(blocked, name + " takes the first free step on its ray");
            }
            // FM-07, FM-08: front first along the shift, two frames apart.
            const double ahead = (ox * shift_x + oy * shift_y) / std::hypot(shift_x, shift_y);
            expect(ahead <= previous_ahead + 0.001, name + " plans after every ship ahead of it");
            previous_ahead = ahead;
            expect(slot.delay == 2 * order, name + " plans two frames after the ship before it");
            // FM-09: the group arrives together.
            const auto time = tactical::time_to_reach(ship, block_target).value();
            const auto expected = time < slowest ? math::multiply(ship.max_speed, math::divide(time, slowest).value()).value()
                                                 : ship.max_speed;
            expect(near(slot.max_speed, expected, decimal("0.0001")), name + " plans with its speed matched to the group");
        }
        for (const auto* slot : layer_slots) {
            const auto& ship = members[slot->entity - 1];
            const double along = std::hypot(real(slot->destination.x) - real(block_target.x),
                real(slot->destination.y) - real(block_target.y));
            if (std::abs(along - real(ship.occupation_radius)) < 0.01) {
                expect(std::abs(std::hypot(real(ship.position.x) - cx, real(ship.position.y) - cy) - nearest) < 0.001,
                    "the ship one radius out is one of those nearest the centroid");
            }
        }
    }
    expect(block_type(slots.front().entity - 1) == frigate_type && block_type(slots[8].entity - 1) == corvette_type,
        "the frigate layer plans before the corvettes (FM-02)");
}

// The block's approach in a session (FM-10, AV-19, PC-04, PC-08, PC-09): every ship flies to the
// end of its plan, none falls back behind where it started, measured along the order's direction,
// and none moves away from its slot on the way. With the default budget the searches plan in their frames as in FoC
// (PC-02); with a budget of 1 each runs sliced and lands 4 frames later (PC-08), which must not
// change the route a later ship of the layer takes around one whose search has not landed yet.
// Hash-identical on 1, 2, 4 and 8 workers.
void test_block_approach() {
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> ships;
    std::vector<eawr::sim::EntityId> group;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto id = static_cast<eawr::sim::EntityId>(index + 1);
        ships.push_back({id, block_type(index), 1, block_position(index), yaw(0), {}});
        group.push_back(id);
    }
    replay.setup = setup(ships);
    replay.final_tick_count = 1900;
    replay.commands = {{{30, 1, 0}, group, tactical::MovePayload{block_target}}};
    std::vector<tactical::FormationMember> members;
    for (std::size_t index = 0; index < 16; ++index) {
        members.push_back(member(static_cast<eawr::sim::EntityId>(index + 1), block_type(index), block_position(index)));
    }
    const auto mapped = tactical::map_group_move(members, block_target, tactical::CollisionWorld{90, {}, nullptr}, 31,
        *foc_table().avoidance);
    expect(mapped.has_value(), "the block maps");
    if (!mapped) return;
    const double dx = real(block_target.x) / std::hypot(real(block_target.x), real(block_target.y));
    const double dy = real(block_target.y) / std::hypot(real(block_target.x), real(block_target.y));
    for (const std::uint64_t budget : {std::uint64_t{1000}, std::uint64_t{1}}) {
        auto table = foc_table();
        table.avoidance->search_budget = budget;
        const std::string config = budget == 1 ? " (every search sliced)" : "";
        const auto reference = run(replay, 1, false, table);
        expect(reference.has_value(), "the block session runs" + config);
        if (!reference) continue;
        for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            const auto other = run(replay, workers, true, table);
            expect(other && other->hashes == reference->hashes,
                "the block hashes alike on " + std::to_string(workers) + " scrambled workers" + config);
        }
        for (const auto& slot : mapped.value()) {
            const auto& start = members[slot.entity - 1].position;
            const double start_gap = std::hypot(real(slot.destination.x) - real(start.x), real(slot.destination.y) - real(start.y));
            double worst_back = 0;
            double worst_away = 0;
            for (std::size_t tick = 30; tick < reference->units.size(); ++tick) {
                const auto* ship = unit_of(reference->units[tick], slot.entity);
                if (ship == nullptr) break;
                const double px = real(ship->position.x) - real(start.x);
                const double py = real(ship->position.y) - real(start.y);
                worst_back = std::min(worst_back, px * dx + py * dy);
                const double gap = std::hypot(real(slot.destination.x) - real(ship->position.x),
                    real(slot.destination.y) - real(ship->position.y));
                worst_away = std::max(worst_away, gap - start_gap);
            }
            const std::string name = "ship " + std::to_string(slot.entity);
            expect(worst_back > -25, name + " never falls back along the order (worst " + std::to_string(worst_back) + ")" + config);
            expect(worst_away < 25, name + " never moves away from its slot (worst +" + std::to_string(worst_away) + ")" + config);
            // AV-19: the ship's own search moves its slot to the nearest point no ship of its layer
            // occupies or will cross, so the ships behind stop short of slots the front ships'
            // routes pass through, and a later try of its search may finish on a step short of
            // that point (AV-14). The ship stops where its path ends.
            const auto& plan = reference->motion_at_100[slot.entity - 1];
            const bool heads = plan && plan->kind == tactical::MotionKind::path && !plan->nodes.empty();
            expect(heads, name + " plans a path" + config);
            const auto* last = unit_of(reference->units.back(), slot.entity);
            if (!heads || last == nullptr) continue;
            const auto& end = plan->nodes.back().position;
            const double end_gap = std::hypot(real(end.x) - real(last->position.x), real(end.y) - real(last->position.y));
            expect(end_gap < 1, name + " stops where its path ends (off by " + std::to_string(end_gap) + ")" + config);
        }
    }
}

// PC-09 (#613): two Nebulon-Bs in a line along the move, the rear one 400 behind the front one.
// The rear one has further to go, so it plans faster (FM-09), closes on the front one and passes
// it 63 units aside. With every search sliced (a budget of 1) the front ship's search starts in
// the order's frame, ends in the next and lands 4 frames after the order; the rear ship searches
// 2 frames after the order (FM-08), before that landing. It reads the front ship's published plan
// and passes it as it does when every search plans in its frame (FoC's timing, PC-02), instead
// of steering around it as if it would stay where it stands.
void test_published_plan() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, frigate_type, 1, at(0, 0, -90), yaw(0), {}},
        {2, frigate_type, 1, at(-400, 0, -90), yaw(0), {}},
    });
    replay.final_tick_count = 400;
    replay.commands = {{{30, 1, 0}, {1, 2}, tactical::MovePayload{at(3000, 0, 0)}}};
    const auto sideways = [&](const std::uint64_t budget) {
        auto table = foc_table();
        table.avoidance->search_budget = budget;
        const auto result = run(replay, 1, false, table);
        expect(result.has_value(), "the line session runs");
        double worst = 0;
        if (!result) return worst;
        for (const auto& list : result->units) {
            if (const auto* rear = unit_of(list, 2)) worst = std::max(worst, std::abs(real(rear->position.y)));
        }
        if (budget == 1) {
            for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
                const auto again = run(replay, workers, true, table);
                expect(again && again->hashes == result->hashes, "the sliced line hashes alike on scrambled workers");
            }
        }
        return worst;
    };
    const double exact = sideways(1'000'000);
    const double sliced = sideways(1);
    expect(exact > 50 && exact < 75, "with FoC's timing the rear ship passes the front one about 63 aside ("
            + std::to_string(exact) + ")");
    expect(std::abs(sliced - exact) < 2, "sliced, the rear ship reads the front one's published plan and passes it alike ("
            + std::to_string(sliced) + " against " + std::to_string(exact) + ")");
}


} // namespace formation_test_support
