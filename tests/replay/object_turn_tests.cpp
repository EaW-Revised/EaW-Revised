// #3644 A1 (#3701): the object-turn world seam. A synthetic C++ provider stands in for the object-script
// host: it answers due(tick), reads the staged rows and returns ability uses that the world applies in the
// same tick, in (unit, sequence) order, before tracking, late impacts and the commands. The cases cover the
// acceptance of the slice: same-turn DEFEND, cluster bombs and speed replans equal the native/command
// results, ordering and refusals, 1/2/4/8 and reverse partitions, record() with a world-only replay,
// a checkpoint at a barrier restored on another worker count, the re-entrancy guard and the phase name.

#include "../support/check.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/content.hpp"
#include "eawr/sim/tactical/object_turn.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using Kind = tactical::AbilityKind;
using Action = tactical::AbilityAction;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId corvette_type = 11;
constexpr tactical::TypeId frigate_type = 12;
constexpr tactical::TypeId plain_type = 13;
constexpr eawr::sim::EntityId corvette = 1;
constexpr eawr::sim::EntityId frigate = 2;
constexpr eawr::sim::EntityId bomber = 3;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// Runs the partitions last-first on one thread, with a worker count of its own.
class ReverseInline final : public eawr::sim::PartitionExecutor {
public:
    explicit ReverseInline(const std::size_t workers) : workers_(workers) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return workers_; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t index = count; index != 0; --index) partition(index - 1);
        return eawr::core::Result<void>::success();
    }

private:
    std::size_t workers_;
};

// Records the named phases of a tick, in order.
class PhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 2; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
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

// --- Content ----------------------------------------------------------------------------------

[[nodiscard]] tactical::AbilityProfile defend() {
    tactical::AbilityProfile profile;
    profile.kind = Kind::defend;
    profile.expiration_frames = 450;
    profile.recharge_frames = 1800;
    profile.modifiers.weapon_delay = units(1);
    profile.modifiers.shield_regen = units(1);
    profile.modifiers.shield_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen = units(3);
    profile.modifiers.speed = decimal("0.8");
    profile.supports_autofire = true;
    return profile;
}

[[nodiscard]] tactical::AbilityProfile turbo() {
    tactical::AbilityProfile profile;
    profile.kind = Kind::turbo;
    profile.expiration_frames = 600;
    profile.recharge_frames = 1500;
    profile.modifiers.weapon_delay = units(3);
    profile.modifiers.shield_regen = Fixed{};
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.speed = units(2);
    return profile;
}

[[nodiscard]] tactical::AbilityProfile cluster_bomb() {
    tactical::AbilityProfile ability;
    ability.kind = Kind::cluster_bomb;
    ability.recharge_frames = 1500;
    ability.supports_autofire = true;
    ability.spawned = tactical::SpawnedAbilityProfile{};
    auto& spawn = *ability.spawned;
    spawn.type = 99;
    spawn.damage_type = 0;
    spawn.shield_damage = false;
    spawn.ring_count = 12;
    spawn.countdown_frames = 45;
    spawn.speed = units(12);
    spawn.max_travel = units(1000);
    spawn.blast.damage = units(150);
    spawn.blast.radius = units(150);
    return ability;
}

struct Fixture {
    tactical::TacticalSetup setup;
    tactical::DurabilityTable durability;
    tactical::MotionTable motion;
    tactical::CombatTable combat;
    tactical::AbilityTable abilities;
    std::vector<tactical::PlayerCommand> commands;

    // `native_defend` gives the frigate the retired DEFEND stand-in instead of leaving DEFEND to the provider.
    explicit Fixture(const bool native_defend = false) {
        setup.seed = 3644;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
        setup.units = {
            {corvette, corvette_type, 1, at(-1800, -1500), math::identity_quat(), {}},
            {frigate, frigate_type, 1, at(0, 2000), math::identity_quat(), {}},
            {bomber, plain_type, 1, at(0, 0), math::identity_quat(), {}},
        };
        durability.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
        tactical::DamageRules damage;
        damage.shield_recharge_frames = 90;
        damage.depleted_disable_seconds = units(5);
        damage.depleted_regen_cap = decimal("0.25");
        damage.damage_types = 1;
        damage.armor_types = 1;
        damage.armor_mods = {units(1)};
        damage.energy_recharge_frames = 150;
        damage.energy_to_shield = units(5);
        durability.damage = damage;
        for (const auto type : {corvette_type, frigate_type, plain_type}) {
            tactical::DurabilityProfile profile;
            profile.type_id = type;
            profile.max_hull = units(3600);
            profile.max_shields = units(700);
            profile.shield_refresh = units(50);
            profile.armor_type = 0;
            profile.shield_armor_type = 0;
            profile.damage_tracking = true;
            durability.profiles.push_back(profile);
        }
        const tactical::MotionProfile fast{corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"),
            decimal("1.5"), units(2), decimal("0.24"), units(15)};
        const tactical::MotionProfile slow{frigate_type, decimal("2.64"), decimal("0.048"), decimal("0.048"),
            decimal("0.84"), units(3), decimal("0.24"), units(5)};
        motion = {{units(15), units(300)}, {fast, slow}, std::nullopt, {}, {}};
        tactical::CombatProfile collision;
        collision.type_id = plain_type;
        collision.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
        combat.profiles = {collision};
        abilities.profiles = {tactical::UnitAbilityProfile{corvette_type, {turbo()}, false},
            tactical::UnitAbilityProfile{frigate_type, {defend()}, native_defend},
            tactical::UnitAbilityProfile{plain_type, {cluster_bomb()}, false}};
        commands = {{{0, 1, 0}, {corvette}, tactical::MovePayload{at(3000, -1500)}},
            {{500, 2, 0}, {frigate}, tactical::DamagePayload{units(400)}}};
    }

    [[nodiscard]] eawr::core::Result<tactical::TacticalSession> create() const {
        auto created = tactical::TacticalSession::create(
            setup, {}, durability, motion, std::nullopt, combat, {}, abilities);
        if (!created) return created;
        for (const auto& command : commands) {
            if (auto submitted = created.value().submit(command); !submitted)
                return eawr::core::Result<tactical::TacticalSession>::failure(submitted.error());
        }
        return created;
    }

    [[nodiscard]] eawr::core::Result<tactical::TacticalSession> from_replay(const tactical::TacticalReplay& replay) const {
        return tactical::TacticalSession::from_replay(replay, {}, durability, motion, std::nullopt, combat, {}, abilities);
    }

    // The TacticalContent overload, which binds a replay without validate_replay.
    [[nodiscard]] eawr::core::Result<tactical::TacticalSession> from_replay_content(
        const tactical::TacticalReplay& replay) const {
        tactical::TacticalContent content;
        content.durability = durability;
        content.motion = motion;
        content.combat = combat;
        content.abilities = abilities;
        return tactical::TacticalSession::from_replay(replay, content);
    }
};

// The synthetic provider: due on the ticks of its script, returning the scripted uses in the order given.
class ScriptedTurn final : public tactical::ObjectTurnService {
public:
    using Uses = std::vector<tactical::ObjectTurnUse>;
    std::map<std::uint64_t, Uses> script;
    std::map<std::uint64_t, std::vector<tactical::ObjectTurnRow>> seen;
    std::uint64_t due_calls{}, runs{};
    std::function<void(std::uint64_t)> probe;

    [[nodiscard]] bool due(const std::uint64_t tick) override {
        ++due_calls;
        return script.contains(tick);
    }
    [[nodiscard]] eawr::core::Result<Uses> run(
        const eawr::sim::PartitionExecutor&, const tactical::ObjectTurnView& view) override {
        ++runs;
        seen[view.tick].assign(view.rows.begin(), view.rows.end());
        if (probe) probe(view.tick);
        return eawr::core::Result<Uses>::success(script.at(view.tick));
    }
};

[[nodiscard]] tactical::ObjectTurnUse use(const eawr::sim::EntityId unit, const std::uint64_t sequence, const Kind kind,
    const Action action = Action::activate) {
    tactical::ObjectTurnUse result;
    result.unit = unit;
    result.sequence = sequence;
    result.ability = kind;
    result.action = action;
    return result;
}

// The scenario's provider script: rows only at 509, then TURBO at 30, bombs at 40, DEFEND at 510.
[[nodiscard]] std::map<std::uint64_t, ScriptedTurn::Uses> main_script() {
    return {{30, {use(corvette, 1, Kind::turbo)}}, {40, {use(bomber, 1, Kind::cluster_bomb)}}, {509, {}},
        {510, {use(frigate, 1, Kind::defend)}}};
}

struct Trace {
    std::vector<std::string> hashes;
    std::vector<std::vector<tactical::ObjectTurnOutcome>> outcomes;
    std::vector<math::Vec3> corvette;
    std::vector<Fixed> shield;
    std::vector<std::size_t> projectiles;
    std::vector<eawr::sim::EntityId> first_projectile_ids;
};

// Steps `session` until it has completed `until` ticks.
bool drive(tactical::TacticalSession& session, const eawr::sim::PartitionExecutor& executor,
    const std::uint64_t until, Trace& trace) {
    while (session.completed_tick() < until) {
        auto stepped = session.step(executor);
        if (!stepped) {
            std::cerr << "step failed: " << stepped.error().message << '\n';
            return false;
        }
        trace.hashes.push_back(stepped.value().state_sha256);
        trace.outcomes.push_back(stepped.value().object_turn);
        for (const auto& unit : session.units()) {
            if (unit.entity_id == corvette) trace.corvette.push_back(unit.position);
        }
        const auto health = session.durability_state(frigate);
        trace.shield.push_back(health ? health->shields : Fixed{});
        trace.projectiles.push_back(session.projectiles().size());
    }
    return true;
}

[[nodiscard]] std::unique_ptr<tactical::TacticalSession> make(const Fixture& fixture, const std::shared_ptr<ScriptedTurn>& turn) {
    auto created = fixture.create();
    expect(static_cast<bool>(created), "the fixture session is created");
    if (!created) { std::cerr << created.error().message << '\n'; return nullptr; }
    auto session = std::make_unique<tactical::TacticalSession>(std::move(created).value());
    if (turn) expect(static_cast<bool>(session->set_object_turn_service(turn)), "the provider installs");
    return session;
}

// --- Cases ------------------------------------------------------------------------------------

constexpr std::uint64_t ticks = 560;

void test_defend_same_turn() {
    // ABC-04 / FD-15: the provider activates DEFEND at 510, the tick the 400/s window closes; the shield gains at
    // 511, 520 and 529, and the result equals the retired native stand-in activating in the same tick.
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = main_script();
    auto session = make(fixture, turn);
    if (!session) return;
    Trace trace;
    if (!drive(*session, eawr::sim::InlineExecutor{}, ticks, trace)) return;

    expect(turn->runs == 4, "the rows are built and the provider runs only on its four due ticks");
    expect(turn->due_calls == ticks, "due() answers once per tick");
    const auto row = [&](const std::uint64_t tick, const eawr::sim::EntityId unit) -> const tactical::ObjectTurnRow* {
        const auto found = turn->seen.find(tick);
        if (found == turn->seen.end()) return nullptr;
        const auto match = std::find_if(found->second.begin(), found->second.end(),
            [&](const tactical::ObjectTurnRow& entry) { return entry.state.entity_id == unit; });
        return match == found->second.end() ? nullptr : &*match;
    };
    const auto* early = row(509, frigate);
    const auto* closed = row(510, frigate);
    expect(early != nullptr && closed != nullptr, "the provider saw the frigate's row on both due ticks");
    if (early != nullptr && closed != nullptr) {
        expect(early->has_durability && early->shields < units(700) && early->damage_rate == Fixed{},
            "at 509 the hit has reduced the shield and the rate window is still open");
        expect(closed->damage_rate == units(400), "at 510 the row carries the rate of the window that closed this tick");
        expect(closed->slots.size() == 1 && !closed->slots[0].active, "the row shows DEFEND ready and off");
    }
    const auto rows = turn->seen.find(510);
    expect(rows != turn->seen.end() && rows->second.size() == 3, "one row per moving unit, in ascending unit ID");
    if (rows != turn->seen.end() && rows->second.size() == 3)
        expect(rows->second[0].state.entity_id == corvette && rows->second[1].state.entity_id == frigate
            && rows->second[2].state.entity_id == bomber, "the rows are ascending by unit ID");

    std::vector<std::uint64_t> gains;
    for (std::size_t index = 510; index < 559; ++index)
        if (trace.shield[index] > trace.shield[index - 1]) gains.push_back(index);
    expect(gains.size() >= 3 && gains[0] == 511 && gains[1] == 520 && gains[2] == 529,
        "DEFEND activated at 510 recharges the shield at 511, 520 and 529");
    const auto state = session->ability_state(frigate);
    expect(state && state->slots[0].active && state->slots[0].started_tick == 510 && state->slots[0].expires_tick == 960,
        "DEFEND runs from 510 until 960");
    expect(trace.outcomes[510].size() == 1 && trace.outcomes[510][0].unit == frigate
            && trace.outcomes[510][0].reason == tactical::RejectReason::none,
        "the tick reports the applied use");
    expect(trace.outcomes[509].empty() && trace.outcomes[511].empty(), "a due turn without uses reports nothing");

    // The same battle with the native stand-in and no provider: hash for hash.
    const Fixture native(true);
    auto control = make(native, nullptr);
    if (!control) return;
    Trace expected;
    if (!drive(*control, eawr::sim::InlineExecutor{}, ticks, expected)) return;
    // The control has neither the TURBO nor the bomb use, so compare the frigate alone.
    expect(trace.shield == expected.shield, "the seam's DEFEND leaves the frigate exactly as the native stand-in does");
    const auto native_state = control->ability_state(frigate);
    expect(native_state && native_state->slots[0] == state->slots[0], "and in the same ability state");
}

void test_cluster_bomb_same_turn() {
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = {{20, {use(bomber, 1, Kind::cluster_bomb)}}};
    auto session = make(fixture, turn);
    if (!session) return;
    Trace trace;
    if (!drive(*session, eawr::sim::InlineExecutor{}, 21, trace)) return;
    const auto projectiles = session->projectiles();
    expect(projectiles.size() == 12, "one cluster-bomb use gives twelve projectiles in the turn's own tick");
    for (std::size_t index = 0; index < projectiles.size(); ++index) {
        expect(projectiles[index].id == index + 1 && projectiles[index].position == at(0, 0),
            "the bombs get ascending IDs and start at the ship");
        expect(projectiles[index].yaw == units(static_cast<std::int64_t>(index) * 30), "twelve 30-degree headings");
    }
    const auto state = session->ability_state(bomber);
    expect(state && !state->slots[0].active && state->slots[0].started_tick == 20 && state->slots[0].ready_tick == 1520,
        "the bombs start the full recharge in the same tick");
    expect(trace.projectiles[20] == 12 && trace.projectiles[19] == 0, "the bombs exist after tick 20's step, not before");
}

void test_speed_replan_same_turn() {
    // A speed change inside the turn replans the move this tick: the corvette's path equals the one a TURBO
    // command of the same tick produces (the commands also replan before moving the next frame).
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = {{30, {use(corvette, 1, Kind::turbo)}}};
    auto seam = make(fixture, turn);
    Fixture commanded;
    commanded.commands.push_back({{30, 1, 1}, {corvette}, tactical::AbilityPayload{Kind::turbo, Action::activate}});
    auto control = make(commanded, nullptr);
    if (!seam || !control) return;
    Trace seam_trace, control_trace;
    if (!drive(*seam, eawr::sim::InlineExecutor{}, 90, seam_trace)) return;
    if (!drive(*control, eawr::sim::InlineExecutor{}, 90, control_trace)) return;
    expect(seam_trace.corvette == control_trace.corvette, "the turn's TURBO moves the corvette like the same-tick command");
    expect(seam_trace.corvette[89] != fixture.setup.units[0].position, "and the corvette really moved");
    const auto fast = seam->ability_state(corvette);
    expect(fast && fast->slots[0].active && fast->slots[0].started_tick == 30 && fast->slots[0].expires_tick == 630,
        "TURBO runs from tick 30");
    // A plain run without the use is slower, so the replan was not a no-op.
    auto plain = make(fixture, nullptr);
    if (!plain) return;
    Trace plain_trace;
    if (!drive(*plain, eawr::sim::InlineExecutor{}, 90, plain_trace)) return;
    expect(plain_trace.corvette[89] != seam_trace.corvette[89], "without the use the corvette ends elsewhere");
}

void test_order_and_refusals() {
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    // Returned out of order and with refusals: the world sorts by (unit, sequence).
    turn->script = {{20, {use(bomber, 2, Kind::cluster_bomb), use(frigate, 7, Kind::defend), use(99, 1, Kind::defend),
        use(bomber, 1, Kind::cluster_bomb), use(corvette, 1, Kind::hunt), use(corvette, 2, Kind::turbo, Action::autofire_on)}}};
    auto session = make(fixture, turn);
    if (!session) return;
    Trace trace;
    if (!drive(*session, eawr::sim::InlineExecutor{}, 21, trace)) return;
    const auto& outcomes = trace.outcomes[20];
    using R = tactical::RejectReason;
    expect(outcomes.size() == 6, "every use is reported");
    if (outcomes.size() == 6) {
        const auto matches = [&](const std::size_t index, const eawr::sim::EntityId unit, const std::uint64_t sequence, const R reason) {
            return outcomes[index].unit == unit && outcomes[index].sequence == sequence && outcomes[index].reason == reason;
        };
        expect(matches(0, corvette, 1, R::ability_unavailable), "(1,1) HUNT: the unit lacks it, refused");
        expect(matches(1, corvette, 2, R::ability_unavailable), "(1,2) TURBO autofire: not supported by the profile, refused");
        expect(matches(2, frigate, 7, R::none), "(2,7) DEFEND applied");
        expect(matches(3, bomber, 1, R::none), "(3,1) the first bomb use applied");
        expect(matches(4, bomber, 2, R::ability_unavailable), "(3,2) the second use sees the recharge and is refused");
        expect(matches(5, 99, 1, R::unit_not_live), "(99,1) no such unit");
    }
    expect(session->projectiles().size() == 12, "only the first bomb use created projectiles");
    // The record holds all six uses, applied or refused, in key order.
    const auto record = session->record();
    expect(record.object_turns.size() == 6, "record() carries every use");
    expect(std::is_sorted(record.object_turns.begin(), record.object_turns.end(), [](const auto& a, const auto& b) {
        return std::tie(a.tick, a.use.unit, a.use.sequence) < std::tie(b.tick, b.use.unit, b.use.sequence);
    }), "in (tick, unit, sequence) order");

    // A repeated key is a provider error: the step fails and changes nothing.
    auto repeated = std::make_shared<ScriptedTurn>();
    repeated->script = {{5, {use(frigate, 1, Kind::defend), use(frigate, 1, Kind::defend, Action::deactivate)}}};
    auto failing = make(fixture, repeated);
    if (!failing) return;
    Trace before;
    if (!drive(*failing, eawr::sim::InlineExecutor{}, 5, before)) return;
    const auto hash = failing->state_sha256();
    const auto failed = failing->step(eawr::sim::InlineExecutor{});
    expect(!failed, "a repeated (unit, sequence) fails the step");
    expect(failing->completed_tick() == 5 && failing->state_sha256() == hash && failing->record().object_turns.empty(),
        "a failed step leaves the session and its record untouched");
    repeated->script.clear();
    expect(static_cast<bool>(failing->step(eawr::sim::InlineExecutor{})), "the session steps on once the provider is fixed");

    auto invalid = std::make_shared<ScriptedTurn>();
    invalid->script = {{3, {use(frigate, 1, Kind::none)}}};
    auto bad = make(fixture, invalid);
    if (!bad) return;
    Trace unused;
    expect(!drive(*bad, eawr::sim::InlineExecutor{}, 4, unused), "a use naming no ability fails the step");
}

void test_no_provider_is_unchanged() {
    // Nothing installed, a provider that is never due and a removed provider all step the same bytes.
    const Fixture fixture;
    auto bare = make(fixture, nullptr);
    auto idle_turn = std::make_shared<ScriptedTurn>();
    auto idle = make(fixture, idle_turn);
    auto removed_turn = std::make_shared<ScriptedTurn>();
    removed_turn->script = main_script();
    auto removed = make(fixture, removed_turn);
    if (!bare || !idle || !removed) return;
    expect(static_cast<bool>(removed->set_object_turn_service(nullptr)), "the provider is removed again");
    Trace a, b, c;
    if (!drive(*bare, eawr::sim::InlineExecutor{}, 200, a) || !drive(*idle, eawr::sim::InlineExecutor{}, 200, b)
        || !drive(*removed, eawr::sim::InlineExecutor{}, 200, c)) return;
    expect(a.hashes == b.hashes && a.hashes == c.hashes, "no use, no due turn and no provider give the same hashes");
    expect(idle_turn->runs == 0 && removed_turn->due_calls == 0, "no rows are built and nothing is called");
    const auto written = tactical::write_replay(bare->record());
    const auto idle_written = tactical::write_replay(idle->record());
    expect(written && idle_written && written.value() == idle_written.value() && bare->record().object_turns.empty(),
        "without uses the replay bytes carry no object-turn extension");
}

void test_workers_and_replay() {
    const Fixture fixture;
    const auto run = [&](const eawr::sim::PartitionExecutor& executor, Trace& trace,
        std::optional<tactical::TacticalReplay>& replay) {
        auto turn = std::make_shared<ScriptedTurn>();
        turn->script = main_script();
        auto session = make(fixture, turn);
        if (!session || !drive(*session, executor, ticks, trace)) return false;
        replay = session->record();
        return true;
    };
    Trace reference;
    std::optional<tactical::TacticalReplay> recorded;
    if (!run(eawr::sim::InlineExecutor{}, reference, recorded) || !recorded) return;
    expect(recorded->object_turns.size() == 3, "the live run recorded its three applied uses");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Trace trace;
        std::optional<tactical::TacticalReplay> replay;
        expect(run(eawr::platform::ThreadWorkerAdapter(workers), trace, replay) && trace.hashes == reference.hashes
                && trace.outcomes == reference.outcomes && replay && *replay == *recorded,
            "hashes, outcomes and record equal with " + std::to_string(workers) + " workers");
        Trace reversed;
        std::optional<tactical::TacticalReplay> reversed_replay;
        expect(run(ReverseInline(workers), reversed, reversed_replay) && reversed.hashes == reference.hashes
                && reversed_replay && *reversed_replay == *recorded,
            "reverse-order partitions equal with " + std::to_string(workers) + " workers");
    }

    // The record round-trips and a session with no provider replays it byte for byte.
    const auto bytes = tactical::write_replay(*recorded);
    expect(static_cast<bool>(bytes), "the record encodes");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == *recorded, "the object-turn extension round-trips");
    const auto rewritten = parsed ? tactical::write_replay(parsed.value()) : bytes;
    expect(rewritten && rewritten.value() == bytes.value(), "and re-encodes to the same bytes");
    if (!parsed) return;
    auto replayed = fixture.from_replay(parsed.value());
    expect(static_cast<bool>(replayed), "the world-only replay binds without a provider");
    if (!replayed) return;
    Trace replay_trace;
    expect(drive(replayed.value(), eawr::platform::ThreadWorkerAdapter(4), ticks, replay_trace),
        "the world-only replay steps");
    expect(replay_trace.hashes == reference.hashes, "the world-only replay reproduces every state hash");
    expect(replay_trace.outcomes == reference.outcomes, "and every outcome");
    const auto replay_bytes = tactical::write_replay(replayed.value().record());
    expect(replay_bytes && replay_bytes.value() == bytes.value(), "and its own record() is byte-equal to the live one");

    // Damaged records: unordered, out of range, a reserved bit.
    auto unordered = *recorded;
    std::swap(unordered.object_turns[0], unordered.object_turns[1]);
    const auto unordered_bytes = tactical::write_replay(unordered);
    expect(!unordered_bytes || !tactical::parse_replay(unordered_bytes.value()), "out-of-order uses are refused by the encoder or the parser");
    auto late = *recorded;
    late.object_turns.back().tick = late.final_tick_count;
    expect(!fixture.from_replay(late), "a use at the final tick is refused");
    expect(!fixture.from_replay_content(late), "the content overload refuses a use at the final tick as well");
    expect(static_cast<bool>(fixture.from_replay_content(*recorded)), "and binds the unmodified record");
    constexpr std::size_t replay_record = tactical::replay_object_turn_record_size;
    auto flagged = bytes.value();
    flagged[flagged.size() - replay_record + 26] = 0x80; // the flags word of the final record: a reserved bit
    expect(!tactical::parse_replay(flagged), "a reserved flag bit is refused");
    auto cut = bytes.value();
    cut.resize(cut.size() - 1);
    expect(!tactical::parse_replay(cut), "a truncated object-turn record is refused");
}

void test_checkpoint_restore() {
    const Fixture fixture;
    constexpr std::uint64_t barrier = 45;
    const auto script = main_script();
    // The uninterrupted run, on two workers.
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = script;
    auto whole = make(fixture, turn);
    if (!whole) return;
    Trace expected;
    if (!drive(*whole, eawr::platform::ThreadWorkerAdapter(2), ticks, expected)) return;
    // The same run to the barrier (after the use at 30 and the bombs at 40), saved there.
    auto first_turn = std::make_shared<ScriptedTurn>();
    first_turn->script = script;
    auto first = make(fixture, first_turn);
    if (!first) return;
    Trace head;
    if (!drive(*first, eawr::platform::ThreadWorkerAdapter(2), barrier, head)) return;
    const auto saved = first->save_checkpoint_state();
    expect(static_cast<bool>(saved), "the session saves at the barrier");
    if (!saved) return;
    auto restored = fixture.from_replay(first->record());
    expect(static_cast<bool>(restored), "a fresh session binds the record to the barrier");
    if (!restored) return;
    // The owner's command log also holds the commands of the ticks to come.
    for (const auto& command : first->pending_commands())
        expect(static_cast<bool>(restored.value().submit(command)), "a future command is submitted again");
    expect(static_cast<bool>(restored.value().load_checkpoint_state(saved.value())), "and loads the checkpoint");
    auto second_turn = std::make_shared<ScriptedTurn>();
    second_turn->script = script;
    expect(static_cast<bool>(restored.value().set_object_turn_service(second_turn)), "the provider is installed again");
    expect(restored.value().record() == first->record(), "the restored record holds the uses before the barrier");
    Trace tail;
    // Another worker count for the rest of the run.
    expect(drive(restored.value(), eawr::platform::ThreadWorkerAdapter(8), ticks, tail), "the restored session steps on");
    expect(std::equal(tail.hashes.begin(), tail.hashes.end(), expected.hashes.begin() + barrier),
        "restored on 8 workers it continues with the uninterrupted run's hashes");
    expect(tail.hashes.size() == ticks - barrier, "and covers the remaining ticks");
    const auto whole_bytes = tactical::write_replay(whole->record());
    const auto restored_bytes = tactical::write_replay(restored.value().record());
    expect(whole_bytes && restored_bytes && whole_bytes.value() == restored_bytes.value(),
        "and its final record is byte-equal");
    expect(second_turn->runs == 2, "the restored provider ran for the uses after the barrier only");
}

void test_reentrancy_guard() {
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = {{10, {}}};
    auto session = make(fixture, turn);
    if (!session) return;
    int refused = 0;
    int calls = 0;
    turn->probe = [&](const std::uint64_t) {
        ++calls;
        const auto count = [&](const bool failed) { refused += failed ? 1 : 0; };
        count(!session->step(eawr::sim::InlineExecutor{}));
        count(!session->submit({{50, 1, 5}, {corvette}, tactical::StopPayload{}}));
        count(!session->set_object_turn_service(nullptr));
        count(!session->submit_object_turn({50, use(frigate, 1, Kind::defend)}));
        count(!session->stage_remove(corvette));
        count(!session->stage_spawn(fixture.setup.units[0]));
        count(!session->request_projectile_explosion(1));
        count(!session->load_checkpoint_state({}));
    };
    Trace trace;
    expect(drive(*session, eawr::sim::InlineExecutor{}, 12, trace), "the outer step succeeds");
    expect(calls == 1 && refused == 8, "every mutator refuses while the provider runs");
    expect(session->pending_command_count() == 1 && session->record().object_turns.empty(),
        "and nothing was submitted or recorded from inside the call");
    expect(static_cast<bool>(session->submit_object_turn({50, use(frigate, 1, Kind::defend)})),
        "the guard is released after the step");
}

void test_seam_precedes_commands() {
    // The seam runs ahead of the tick's commands: a provider activation and a same-tick deactivate command end
    // like an activate command followed by the deactivate command, not like the deactivate refused first.
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = {{30, {use(corvette, 1, Kind::turbo)}}};
    Fixture seam_fixture;
    seam_fixture.commands.push_back({{30, 1, 1}, {corvette}, tactical::AbilityPayload{Kind::turbo, Action::deactivate}});
    Fixture control_fixture;
    control_fixture.commands.push_back({{30, 1, 1}, {corvette}, tactical::AbilityPayload{Kind::turbo, Action::activate}});
    control_fixture.commands.push_back({{30, 1, 2}, {corvette}, tactical::AbilityPayload{Kind::turbo, Action::deactivate}});
    auto seam = make(seam_fixture, turn);
    auto control = make(control_fixture, nullptr);
    auto activated = make(fixture, turn);
    if (!seam || !control || !activated) return;
    Trace seam_trace, control_trace, activated_trace;
    if (!drive(*seam, eawr::sim::InlineExecutor{}, 60, seam_trace)) return;
    if (!drive(*control, eawr::sim::InlineExecutor{}, 60, control_trace)) return;
    if (!drive(*activated, eawr::sim::InlineExecutor{}, 60, activated_trace)) return;
    const auto ended = seam->ability_state(corvette);
    expect(ended && !ended->slots[0].active, "the command deactivated what the seam activated in the same tick");
    expect(seam_trace.hashes == control_trace.hashes && seam_trace.corvette == control_trace.corvette,
        "so the seam ran before the commands");
    expect(seam_trace.hashes != activated_trace.hashes, "and the deactivation really changed the run");
}

void migrated_effect_pin(const Fixture& fixture, tactical::ObjectTurnUse request, const std::string_view rule) {
    auto turn=std::make_shared<ScriptedTurn>();turn->script={{1,{request}}};
    auto current=make(fixture,turn);
    auto delayed_fixture=fixture;
    const auto source=std::find_if(fixture.setup.units.begin(),fixture.setup.units.end(),
        [&](const auto& unit) { return unit.entity_id==request.unit; });
    tactical::AbilityPayload payload{request.ability,request.action,request.target};payload.position=request.position;
    delayed_fixture.commands.push_back({{2,source->owner,0},{request.unit},payload});
    auto delayed=make(delayed_fixture,nullptr);
    if (!current || !delayed) return;
    const eawr::sim::InlineExecutor executor;
    expect(bool(current->step(executor)) && bool(delayed->step(executor)),"SOS effect pin reaches first decision");
    expect(bool(current->step(executor)) && bool(delayed->step(executor)),"SOS effect pin reaches turn seam");
    expect(current->ability_state(request.unit)->slots.front().started_tick==1
        && delayed->ability_state(request.unit)->slots.front().started_tick==0,"same-turn effect precedes the former routed command");
    expect(bool(delayed->step(executor)) && delayed->ability_state(request.unit)->slots.front().started_tick==2,
        "former next-tick route starts the same ability at tick 2");
    std::cout<<"SOS-EFFECT,"<<rule<<",2,1,routing-latency\n";
}

void test_migrated_spawns_and_replenishment() {
    for (const auto kind : {Kind::harmonic_bomb, Kind::weaken_enemy}) {
        Fixture fixture; fixture.commands.clear();
        fixture.abilities.profiles.back().abilities.front().kind = kind;
        fixture.abilities.profiles.back().abilities.front().spawned->ring_count = 0;
        auto request = use(bomber,0,kind);
        if (kind == Kind::weaken_enemy) request.position = at(50,20);
        migrated_effect_pin(fixture,request,kind==Kind::harmonic_bomb?"SOS-16":"SOS-17");
        auto turn = std::make_shared<ScriptedTurn>(); turn->script = {{1,{request}}};
        auto session = make(fixture,turn);
        if (!session) continue;
        expect(bool(session->step(eawr::sim::InlineExecutor{})), "SOS-16/17: spawn fixture reaches decision tick");
        auto tick = session->step(eawr::sim::InlineExecutor{});
        expect(tick && tick.value().object_turn.front().reason == tactical::RejectReason::none && session->projectiles().size() == 1,
            "SOS-16/17: bomb or weaken projectile exists in the decision tick");
        expect(session->ability_state(bomber)->slots.front().started_tick == 1,
            "SOS-16/17: recharge is stamped by the object turn");
    }
    Fixture fixture; fixture.commands.clear();
    fixture.motion.profiles.clear();
    tactical::CraftProfile craft;
    craft.type_id = corvette_type; craft.max_speed = units(5); craft.min_speed = units(1);
    craft.rate_of_turn = units(6); craft.lift = units(6); craft.thrust = decimal("0.2");
    craft.roll_rate = units(6); craft.bank_angle = units(70); craft.strafe_distance = units(200);
    fixture.motion.squadrons.craft = {craft};
    fixture.motion.squadrons.squadrons = {{20,{corvette_type,corvette_type},{at(0,0),at(10,0)},units(1000),units(200),units(300),units(20)}};
    tactical::AbilityProfile replenish;
    replenish.kind = Kind::replenish_wingmen; replenish.replenish_team = 20; replenish.recharge_frames = 30;
    fixture.abilities.profiles.front().abilities = {replenish};
    migrated_effect_pin(fixture,use(corvette,0,Kind::replenish_wingmen),"SOS-19");
    auto turn = std::make_shared<ScriptedTurn>(); turn->script = {{1,{use(corvette,0,Kind::replenish_wingmen)}}};
    auto session = make(fixture,turn);
    if (!session) return;
    expect(bool(session->step(eawr::sim::InlineExecutor{})), "SOS-19: leader reaches replenishment turn");
    const auto before = session->snapshot()->instances().size();
    const auto tick = session->step(eawr::sim::InlineExecutor{});
    expect(tick && tick.value().object_turn.front().reason == tactical::RejectReason::none
        && session->snapshot()->instances().size() == before + 2,
        "SOS-19: leader replenishes escort and real container in the same turn");
    expect(session->ability_state(corvette)->slots.front().started_tick == 1,
        "SOS-19: leader recharge starts in the decision tick");
    expect(bool(session->step(eawr::sim::InlineExecutor{})), "SOS-19: sparse births survive the next indexed tick");
}

void test_widened_slot_modes() {
    for (const auto kind : {Kind::invulnerability,Kind::missile_shield}) {
        Fixture fixture;fixture.commands.clear();
        auto ability=turbo();ability.kind=kind;fixture.abilities.profiles.front().abilities={ability};
        migrated_effect_pin(fixture,use(corvette,0,kind),kind==Kind::invulnerability?"SOS-15":"SOS-11");
    }
    for (const auto kind : {Kind::hunt, Kind::lure, Kind::stealth, Kind::interdict, Kind::missile_shield}) {
        Fixture fixture;
        fixture.commands.clear();
        auto ability = turbo(); ability.kind = kind; ability.supports_autofire = true;
        if (kind == Kind::lure) {
            ability.lure = tactical::LureProfile{};
            ability.lure->radius = units(100);
            ability.lure->target_types = {corvette_type};
        }
        if (kind == Kind::stealth) {
            ability.gui_activated_ability_name = "turn-stealth";
            tactical::SpecialAbilityProfile nested;
            nested.name = "turn-stealth"; nested.kind = tactical::SpecialAbilityKind::stealth;
            nested.style = tactical::SpecialActivationStyle::user_input;
            fixture.abilities.profiles.front().special = {nested};
        }
        fixture.abilities.profiles.front().abilities = {ability};
        auto turn = std::make_shared<ScriptedTurn>();
        turn->script = {{0,{use(corvette,0,kind)}}, {1,{use(corvette,0,kind)}},
            {2,{use(corvette,0,kind,Action::autofire_on)}}, {3,{use(corvette,0,kind,Action::deactivate)}}};
        auto session = make(fixture, turn);
        if (!session) continue;
        const auto first = session->step(eawr::sim::InlineExecutor{});
        expect(first && first.value().object_turn.size() == 1 && first.value().object_turn.front().reason == tactical::RejectReason::none,
            "widened slot mode admits activation at the turn seam");
        const auto active = session->ability_state(corvette);
        expect(active && active->slots.front().active && active->slots.front().started_tick == 0,
            "widened slot mode starts in the decision tick");
        auto repeated=session->step(eawr::sim::InlineExecutor{});
        expect(repeated && repeated.value().object_turn.front().reason==tactical::RejectReason::ability_unavailable,
            "active widened mode refuses a repeated activation");
        expect(bool(session->step(eawr::sim::InlineExecutor{})) && session->ability_state(corvette)->slots.front().autofire,
            "widened mode admits autofire switching");
        expect(bool(session->step(eawr::sim::InlineExecutor{})), "widened mode reaches deactivation");
        expect(session->ability_state(corvette) && !session->ability_state(corvette)->slots.front().active,
            "widened slot mode deactivates at the same seam");
    }
    for (const auto kind : {Kind::energy_weapon, Kind::tractor_beam, Kind::leech_shields}) {
        Fixture fixture;
        fixture.commands.clear();
        fixture.setup.units[0].position = at(0,0);
        fixture.setup.units[1].position = at(100,0);
        fixture.setup.units[1].owner = 2;
        for (const auto type : {corvette_type, frigate_type}) {
            tactical::CombatProfile combat;
            combat.type_id = type; combat.category_bits = 1;
            combat.collision = tactical::CollisionBox{at(-10,-10,-10),at(10,10,10)};
            fixture.combat.profiles.push_back(combat);
        }
        std::sort(fixture.combat.profiles.begin(),fixture.combat.profiles.end(),
            [](const auto& left,const auto& right) { return left.type_id < right.type_id; });
        auto ability = turbo(); ability.kind = kind; ability.gui_activated_ability_name = "turn-beam";
        ability.recharge_frames = 1; ability.expiration_frames = 90; ability.supports_autofire = true;
        auto& profile = fixture.abilities.profiles.front();
        tactical::SpecialAbilityProfile nested;
        nested.name = "turn-beam";
        nested.kind = kind == Kind::energy_weapon ? tactical::SpecialAbilityKind::energy_weapon
            : kind == Kind::tractor_beam ? tactical::SpecialAbilityKind::tractor_beam : tactical::SpecialAbilityKind::leech_shields;
        nested.style = tactical::SpecialActivationStyle::user_input;
        nested.beam_max_range = units(1000); nested.filter.applicable_categories = 1; nested.service_interval = 10;
        if (kind == Kind::leech_shields) {
            ability.recharge_at_activation = true;
            nested.leech_duration_frames = 90; nested.leech_damage_bonus = units(1);
        }
        profile.abilities = {ability}; profile.special = {nested};
        auto turn = std::make_shared<ScriptedTurn>();
        auto request = use(corvette,0,kind); request.target = frigate;
        auto missing = request; missing.target = 9999;
        turn->script = {{1,{request}}, {2,{use(corvette,0,kind,Action::autofire_on)}},
            {3,{use(corvette,0,kind,Action::deactivate)}}, {4,{missing}}};
        auto session = make(fixture,turn);
        if (!session) continue;
        const eawr::sim::InlineExecutor executor;
        expect(bool(session->step(executor)),"beam reaches decision tick");
        auto tick = session->step(executor);
        expect(tick && tick.value().object_turn.front().reason == tactical::RejectReason::none
            && session->ability_state(corvette)->slots.front().active
            && session->ability_state(corvette)->slots.front().target == frigate
            && session->ability_state(corvette)->slots.front().started_tick == 1,
            "widened beam locks a live hostile target in its decision tick");
        tick = session->step(executor);
        expect(tick && tick.value().object_turn.front().reason == tactical::RejectReason::none
            && session->ability_state(corvette)->slots.front().autofire,"beam supports autofire switching");
        tick = session->step(executor);
        expect(tick && tick.value().object_turn.front().reason == tactical::RejectReason::none
            && !session->ability_state(corvette)->slots.front().active,"beam releases its target at the turn seam");
        tick = session->step(executor);
        expect(tick && tick.value().object_turn.front().reason == tactical::RejectReason::target_not_live,
            "widened beam refuses a missing target after recharge");
    }
}

void test_ordinary_squadron_fanout() {
    Fixture fixture; fixture.commands.clear();
    fixture.motion.profiles.erase(fixture.motion.profiles.begin());
    for (const auto type : {corvette_type,plain_type}) {
        tactical::CraftProfile craft;
        craft.type_id=type;craft.max_speed=units(5);craft.min_speed=units(1);
        craft.rate_of_turn=units(6);craft.lift=units(6);craft.thrust=decimal("0.2");
        craft.roll_rate=units(6);craft.bank_angle=units(70);craft.strafe_distance=units(200);
        fixture.motion.squadrons.craft.push_back(craft);
    }
    fixture.motion.squadrons.squadrons={{20,{corvette_type,plain_type},{at(0,0),at(10,0)},units(1000),units(200),units(300),units(20)}};
    fixture.setup.units.push_back({4,20,1});fixture.setup.squadrons={{4,{corvette,bomber}}};
    auto ability=turbo();ability.supports_autofire=true;
    fixture.abilities.profiles.front().abilities={ability};fixture.abilities.profiles.back().abilities={ability};
    for (const bool conflict : {false,true}) {
        auto turn=std::make_shared<ScriptedTurn>();
        if (conflict) turn->script[0]={use(corvette,0,Kind::turbo)};
        turn->script[1]={use(4,0,Kind::turbo)};
        turn->script[2]={use(4,0,Kind::turbo,Action::autofire_on)};
        turn->script[3]={use(4,0,Kind::turbo,Action::deactivate)};
        auto session=make(fixture,turn);if (!session) return;
        const eawr::sim::InlineExecutor executor;
        expect(bool(session->step(executor)),"fanout reaches decision tick");
        auto tick=session->step(executor);
        expect(tick && tick.value().object_turn.front().reason==(conflict?tactical::RejectReason::ability_unavailable:tactical::RejectReason::none),
            "container activation gates every current holder atomically");
        expect(session->ability_state(corvette)->slots.front().active
            && (session->ability_state(bomber)->slots.front().active==!conflict),
            "container activation reaches both holders and a refusal leaves the second unchanged");
        expect(bool(session->step(executor)) && session->ability_state(corvette)->slots.front().autofire
            && session->ability_state(bomber)->slots.front().autofire,"container autofire reaches both holders");
        expect(bool(session->step(executor)) && !session->ability_state(corvette)->slots.front().active
            && !session->ability_state(bomber)->slots.front().active,"container deactivation reaches both holders");
        if (!conflict) {
            auto command=fixture.create();
            expect(command && command.value().submit({{1,1,0},{4},tactical::AbilityPayload{Kind::turbo}}),"command fanout control creates");
            if (!command) return;
            expect(bool(command.value().step(executor)) && bool(command.value().step(executor)),"command fanout control reaches activation");
            expect(command.value().ability_state(corvette)->slots.front().active && command.value().ability_state(bomber)->slots.front().active,
                "turn and command paths activate the same real holders");
        }
    }
}

void test_phase_name() {
    // `object-turn-rows` runs partitioned, only on a due tick, after the ability service and before the unit systems.
    const Fixture fixture;
    auto turn = std::make_shared<ScriptedTurn>();
    turn->script = {{3, {}}};
    auto session = make(fixture, turn);
    if (!session) return;
    for (std::uint64_t tick = 0; tick < 5; ++tick) {
        const PhaseRecorder recorder;
        expect(static_cast<bool>(session->step(recorder)), "the recorded tick steps");
        std::vector<std::string> names;
        for (const auto& [name, partitions] : recorder.calls) {
            names.push_back(name);
            if (name == "object-turn-rows")
                expect(partitions == eawr::sim::tick_partition_count, "the rows phase uses the fixed partition count");
        }
        const auto rows = std::find(names.begin(), names.end(), "object-turn-rows");
        expect((rows != names.end()) == (tick == 3), "the rows phase runs on the due tick only");
        if (rows != names.end()) {
            const auto abilities = std::find(names.begin(), names.end(), "abilities");
            const auto systems = std::find(names.begin(), names.end(), "unit-systems");
            expect(abilities != names.end() && systems != names.end() && abilities < rows && rows < systems,
                "after the ability phase and before the unit systems");
        }
    }
}

void test_live_recorded_playback() {
    namespace auth = eawr::script::authoritative;
    using eawr::platform::LiveSession;
    Fixture fixture;
    fixture.commands.push_back({{8, 1, 1}, {corvette}, tactical::StopPayload{}});
    constexpr std::uint64_t end = 16;
    tactical::TacticalContent content;
    content.durability = fixture.durability;
    content.motion = fixture.motion;
    content.combat = fixture.combat;
    content.abilities = fixture.abilities;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto scripts = std::make_shared<eawr::platform::LiveScripts>();
        scripts->wrap = [](tactical::TacticalSession world) {
            auto turn = std::make_shared<ScriptedTurn>();
            turn->script = {{3, {use(corvette, 0, Kind::turbo)}}};
            auto installed = world.set_object_turn_service(turn);
            if (!installed) return eawr::core::Result<auth::ScriptedTacticalSession>::failure(installed.error());
            auth::SessionConfig config; config.seed = 42; config.tick_duration = {1, 30};
            auto scheduler = auth::ScriptScheduler::create(config, {});
            if (!scheduler) return eawr::core::Result<auth::ScriptedTacticalSession>::failure(scheduler.error());
            return auth::ScriptedTacticalSession::create(std::move(world), std::move(scheduler).value());
        };
        const auto source = [](std::vector<tactical::PlayerCommand> commands) {
            return [commands = std::move(commands)](std::uint64_t tick) {
                std::vector<tactical::PlayerCommand> due;
                for (const auto& command : commands) if (command.key.tick == tick) due.push_back(command);
                return due;
            };
        };
        LiveSession::Options options;
        options.workers = workers; options.pacing = LiveSession::Pacing::driven;
        options.scripts = scripts; options.command_source = source(fixture.commands);
        auto live = LiveSession::start(fixture.setup, content, options);
        expect(static_cast<bool>(live), "live object-script session starts");
        if (!live) continue;
        live.value()->advance_to(end);
        expect(live.value()->wait_for(end, std::chrono::seconds(60)), "live object-script session completes");
        live.value()->stop();
        auto hashes = live.value()->tick_hashes();
        auto bytes = tactical::write_replay(live.value()->record());
        expect(!live.value()->failure(), "live object-script session has no failure");
        expect(static_cast<bool>(bytes), "live session records serialized replay");
        if (!bytes) continue;
        auto parsed = tactical::parse_replay(bytes.value());
        expect(static_cast<bool>(parsed), "the live recording reloads");
        if (!parsed) continue;
        auto full = eawr::platform::headless_tick_hashes(parsed.value(), content);
        expect(full && full.value() == hashes, "full headless replay equals live every tick");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "live digests are equal on 1/2/4/8 workers");
        // Negative control: the old viewer fed only the player-command stream.
        // This must differ, so the fixture detects an omitted object-script use.
        options.scripts = {};
        options.command_source = source(parsed.value().commands);
        auto viewer = LiveSession::start(parsed.value().setup, content, options);
        expect(static_cast<bool>(viewer), "command-only control starts");
        if (!viewer) continue;
        viewer.value()->advance_to(end);
        expect(viewer.value()->wait_for(end, std::chrono::seconds(60)), "command-only control completes");
        viewer.value()->stop();
        auto viewer_hashes = viewer.value()->tick_hashes();
        expect(hashes.size() == end && viewer_hashes.size() == end, "every completed tick is captured");
        const auto mismatch = std::mismatch(hashes.begin(), hashes.end(), viewer_hashes.begin(), viewer_hashes.end());
        const auto first = static_cast<std::size_t>(mismatch.first - hashes.begin()) + 1U;
        expect(first == 4, "dropping object turns first differs at completed tick 4 (object-turn use at tick 3)");
        expect(parsed.value().object_turns.size() == 1 && parsed.value().commands.size() == 2,
            "the live recording includes the ability and player commands before and after it");
        options.command_source = {};
        auto playback = LiveSession::start(parsed.value(), content, options);
        expect(static_cast<bool>(playback), "the shared replay initializer starts playback");
        if (!playback) continue;
        playback.value()->advance_to(end);
        expect(playback.value()->wait_for(end, std::chrono::seconds(60)), "playback completes");
        playback.value()->stop();
        expect(!playback.value()->failure() && playback.value()->rejected_orders().empty(),
            "playback accepts both recorded streams without a producer");
        expect(playback.value()->tick_hashes() == hashes, "serialized playback equals live at every tick");
        const auto rewritten = tactical::write_replay(playback.value()->record());
        expect(rewritten && rewritten.value() == bytes.value(), "playback records both streams byte for byte");
        options.command_source = [](const std::uint64_t tick) {
            if (tick == 1) return std::vector<tactical::PlayerCommand>{
                {{1, 1, 0}, {corvette}, tactical::StopPayload{}}};
            return std::vector<tactical::PlayerCommand>{};
        };
        auto interactive = LiveSession::start(parsed.value(), content, options);
        expect(static_cast<bool>(interactive), "interactive playback starts");
        if (!interactive) continue;
        expect(static_cast<bool>(interactive.value()->submit(
            {1, {corvette}, tactical::FacePayload{at(0, 1000)}, 2})), "a timed live order queues before the recording's last command");
        interactive.value()->advance_to(end);
        expect(interactive.value()->wait_for(end, std::chrono::seconds(60)), "interactive playback completes");
        interactive.value()->stop();
        const auto edited = interactive.value()->record();
        const auto edited_hashes = interactive.value()->tick_hashes();
        expect(!interactive.value()->failure() && interactive.value()->rejected_orders().empty(),
            "UI-source and held live orders are accepted before future recorded command keys");
        expect(edited.commands.size() == 4 && edited.object_turns == parsed.value().object_turns,
            "interactive playback records both new inputs and retains the original object turn");
        expect(edited_hashes != hashes, "the interactive inputs actually change the run");
        auto expected = eawr::platform::headless_tick_hashes(edited, content);
        expect(expected && expected.value() == edited_hashes, "the edited playback re-records every tick faithfully");
    }
}

} // namespace

int main() {
    test_live_recorded_playback();
    test_defend_same_turn();
    test_cluster_bomb_same_turn();
    test_speed_replan_same_turn();
    test_order_and_refusals();
    test_no_provider_is_unchanged();
    test_workers_and_replay();
    test_checkpoint_restore();
    test_reentrancy_guard();
    test_seam_precedes_commands();
    test_phase_name();
    test_widened_slot_modes();
    test_ordinary_squadron_fanout();
    test_migrated_spawns_and_replenishment();
    if (failures != 0) {
        std::cerr << failures << " object-turn contract failure(s)\n";
        return 1;
    }
    std::cout << "object-turn contracts passed\n";
    return 0;
}
