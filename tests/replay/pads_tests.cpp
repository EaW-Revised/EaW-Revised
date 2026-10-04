#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>

namespace { std::atomic<std::size_t> allocations{}; }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(const std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
namespace t = eawr::sim::tactical;
namespace m = eawr::sim::math;
using m::Fixed;
constexpr auto one = Fixed::scale;
int failures{};
Fixed q(const std::int64_t n) { return Fixed::from_raw(n * one); }
m::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) { return {q(x), q(y), q(z)}; }
void expect(const bool value, const std::string_view message) {
    if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
t::TacticalSetup setup() {
    t::TacticalSetup value;
    value.seed = 995;
    value.players = {{1, 0, 100, 1}, {2, 1, 200, 1}, {3, 0, 300, 1}, {4, 2, 400, 0}};
    value.units = {{1, 10, 1, at(0), m::identity_quat(), {}},
        {2, 10, 2, at(500), m::identity_quat(), {}}, {3, 20, 4, at(10), m::identity_quat(), {}}};
    return value;
}
t::EconomyRules rules() {
    t::EconomyRules value;
    value.players = {{1, q(1000), 0}, {2, q(1000), 0}, {3, q(1000), 0}};
    for (const auto faction : {100ULL, 300ULL}) {
        t::StationMenu menu;
        menu.station = 20;
        menu.faction = faction;
        menu.options = {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}};
        menu.station_producer = false;
        value.menus.push_back(std::move(menu));
    }
    value.pads.neutral = 4;
    value.pads.capture = {{20, q(100), q(1), {100, 200, 300, 400}, false, true, true}};
    value.pads.construction = {{30, 40, q(100), 1, 1}};
    value.pads.influence = {{20, false}, {30, false}, {40, false}};
    return value;
}
t::DurabilityTable durability() {
    t::DurabilityTable value;
    for (const t::TypeId type : {10U, 20U, 30U, 40U}) {
        t::DurabilityProfile profile;
        profile.type_id = type;
        profile.max_hull = q(type == 40 ? 600 : 300);
        value.profiles.push_back(profile);
    }
    return value;
}
t::TacticalSession session(t::TacticalSetup start = setup(), t::EconomyRules economy = rules(),
    const t::VictoryRules& victory = {}) {
    auto made = t::TacticalSession::create(start, {}, durability(), {}, {}, {}, victory, {}, economy);
    expect(static_cast<bool>(made), "valid synthetic pad content");
    return std::move(made).value();
}
void steps(t::TacticalSession& value, const std::uint64_t until, const eawr::sim::PartitionExecutor& executor) {
    while (value.completed_tick() < until) {
        const auto result = value.step(executor);
        expect(static_cast<bool>(result), "partitioned step succeeds");
        if (!result) { std::cerr << result.error().message << '\n'; break; }
    }
}
class AllocationExecutor final : public eawr::sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        for (std::size_t slot = 0; slot < count; ++slot) run(slot);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        const auto before = allocations.load();
        auto result = execute(count, run);
        if (name == "capture-prepare" || name == "capture-service" || name == "unit-systems" || name == "income-sources" || name == "income-modifiers") {
            expect(allocations.load() == before, "warmed capture preparation, queries and systems allocate nothing");
        }
        return result;
    }
};
class FailingSystemsExecutor final : public eawr::sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        for (std::size_t slot = 0; slot < count; ++slot) run(slot);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        if (name == "unit-systems" || name == "income-sources") return eawr::core::Result<void>::failure(eawr::core::Diagnostic{});
        return execute(count, run);
    }
};
void test_capture() {
    auto content = rules();
    const auto& profile = content.pads.capture.front();
    auto start = setup();
    t::PadState state{4};
    std::vector<t::CaptureCandidate> candidates{{{1, 1, at(100)}, 10}, {{2, 2, at(101)}, 10}};
    auto owner = t::service_capture(profile, state, 4, 4, start.players, candidates, content.pads, 3, at(0));
    expect(owner == 4 && state.target == 1 && state.progress.raw() > 0, "WBP-04 inclusive raw radius, no scale");
    expect(t::pad_construction_allowed(profile, {}, 4, start.players, candidates, content.pads, 3, at(0)),
        "WBP-07 exact boundary does not block construction");
    candidates[0].body.position = at(99);
    expect(!t::pad_construction_allowed(profile, {}, 4, start.players, candidates, content.pads, 3, at(0)),
        "WBP-07 hostile strictly inside blocks");
    const auto progress = state.progress;
    candidates[0].body.owner = 2;
    owner = t::service_capture(profile, state, 4, 4, start.players, candidates, content.pads, 3, at(0));
    expect(owner == 4 && state.target == 2 && state.progress > progress, "WHZ-44 target change retains progress");
    candidates.push_back({{5, 1, at(0)}, 10});
    const auto contested = state.progress;
    owner = t::service_capture(profile, state, 4, 4, start.players, candidates, content.pads, 3, at(0));
    expect(owner == 4 && state.progress < contested, "WHZ-42 contested neutral point rolls progress back");
    auto domain = content.pads;
    domain.influence.push_back({50, true, true, false});
    candidates = {{{1, 1, at(50)}, 10}, {{5, 4, at(0)}, 50}};
    state = {4};
    owner = t::service_capture(profile, state, 4, 4, start.players, candidates, domain, 3, at(0));
    expect(owner == 4 && state.target == 1 && state.progress.raw() > 0,
        "WBP-50: a noncollidable neutral field does not contest capture despite default influence");
    expect(t::pad_construction_allowed(profile, {}, 1, start.players, candidates, domain, 3, at(0)),
        "WBP-50: a noncollidable neutral field does not block the build palette");
    domain.influence.back().collidable = true;
    state = {4};
    owner = t::service_capture(profile, state, 4, 4, start.players, candidates, domain, 3, at(0));
    expect(owner == 4 && state.progress.raw() == 0,
        "WBP-50: an admitted neutral contender still contests capture");
    expect(!t::pad_construction_allowed(profile, {}, 1, start.players, candidates, domain, 3, at(0)),
        "WBP-50: an admitted nonallied contender still blocks construction");
    candidates = {{{1, 1, at(100)}, 10}, {{2, 2, at(101)}, 10}, {{5, 1, at(0)}, 10}};
    state = {1, q(1)};
    owner = t::service_capture(profile, state, 1, 4, start.players, {}, content.pads, 3, at(0));
    expect(owner == 4 && state.progress.raw() == 0, "WHZ-43 empty nonsticky point neutralizes");
    auto sticky = profile;
    sticky.ownership_sticks = true;
    state = {1};
    expect(t::service_capture(sticky, state, 1, 4, start.players, {}, content.pads, 3, at(0)) == 1,
        "WHZ-43 sticky owner survives absence");
    state.under_construction = 9;
    expect(t::service_capture(profile, state, 1, 4, start.players, candidates, content.pads, 3, at(0)) == 1,
        "WBP-05 child locks pad ownership");
    auto value = session();
    eawr::sim::InlineExecutor executor;
    steps(value, 3, executor);
    expect(value.pads().at(3).progress.raw() == 0, "WHZ-40 no service before four logical frames");
    steps(value, 4, executor);
    expect(value.pads().at(3).progress.raw() > 0, "WHZ-40 first four-frame service");
    steps(value, 32, executor);
    expect(value.units().back().owner == 1, "WHZ-46 frame service captures a neutral pad");
    auto sparse = setup();
    for (eawr::sim::EntityId id = 4; id < 204; ++id) sparse.units.push_back({id, 10, 2,
        at(static_cast<std::int64_t>(id) * 1000), m::identity_quat(), {}});
    auto budget = session(sparse);
    steps(budget, 3, executor);
    const auto due = budget.step(executor);
    expect(due && due.value().capture_candidates <= 3 && due.value().capture_index_bodies == sparse.units.size()
        && due.value().capture_prepare_bodies == sparse.units.size() && due.value().capture_serial_bodies == 0,
        "WBP-04 count inspected broadphase bodies and partitioned fallback index preparation");
    t::CombatTable combat;
    t::CombatProfile passive;
    passive.type_id = 10;
    combat.profiles.push_back(passive);
    auto shared = t::TacticalSession::create(sparse, {}, durability(), {}, {}, combat, {}, {}, rules()).value();
    steps(shared, 3, executor);
    const auto reused = shared.step(executor);
    expect(reused && reused.value().capture_candidates <= 3 && reused.value().capture_index_bodies == 0
        && reused.value().capture_prepare_bodies == sparse.units.size() && reused.value().capture_serial_bodies == 0,
        "WBP-04 reuse the existing moved-world index without rebuilding any body");
    auto instant_rules = rules();
    instant_rules.pads.capture.front().transition_seconds = {};
    auto dying_pad = session(setup(), instant_rules);
    expect(dying_pad.submit({{3, 1, 0}, {3}, t::DamagePayload{q(300), t::attack_hull}}).has_value(),
        "dead-pad capture boundary damage delivers");
    steps(dying_pad, 3, executor);
    const auto killed = dying_pad.step(executor);
    expect(killed && !dying_pad.pads().contains(3)
        && std::none_of(killed.value().snapshot->events().begin(), killed.value().snapshot->events().end(),
            [](const auto& event) { return event.kind == t::EventKind::pad_captured; }),
        "U-BP-1 live-pad policy: lethal damage prevents same-frame capture");
}
void test_ship_class_capture() {
    // WBP-04/50/51: capture eligibility does not depend on the ship layer.
    for (const auto layer : {t::SpaceLayer::corvette, t::SpaceLayer::frigate, t::SpaceLayer::capital}) {
        auto start = setup();
        start.units.front().position = at(70, 0, 80); // Exactly 100 units from the pad in 3D.
        auto economy = rules();
        economy.pads.influence.insert(economy.pads.influence.begin(), {10, true, true, true});
        t::MotionTable motion;
        motion.footprints = {{10, layer, q(20), q(20), q(20), false}};
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto made = t::TacticalSession::create(start, {}, durability(), motion, {}, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "WBP-04: capture session admits every ship layer");
            if (!made) continue;
            auto value = std::move(made).value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            for (unsigned tick = 0; tick < 32; ++tick) {
                const auto result = value.step(executor);
                expect(static_cast<bool>(result), "WBP-04: class capture frame succeeds");
                if (!result) break;
                if (workers == 1) reference.push_back(result.value().state_sha256);
                else expect(tick < reference.size() && result.value().state_sha256 == reference[tick],
                    "WBP-04: corvette/frigate/capital capture hashes agree on 1/2/4/8 workers");
            }
            const auto units = value.units();
            const auto pad = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 3; });
            expect(pad != units.end() && pad->owner == 1,
                "WBP-04/50: an admitted corvette, frigate or capital captures at the inclusive 3D boundary");
        }
    }
}

void test_capture_allocations() {
    auto content = rules();
    content.pads.capture.front().transition_seconds = q(10000);
    auto enabled = session(setup(), content);
    content.pads = {};
    auto disabled = session(setup(), content);
    eawr::sim::InlineExecutor warm;
    steps(enabled, 16, warm);
    steps(disabled, 16, warm);
    const auto measure = [](t::TacticalSession& value) {
        const auto before = allocations.load();
        AllocationExecutor executor;
        for (int frame = 0; frame < 8; ++frame) expect(value.step(executor).has_value(), "warmed allocation step");
        return allocations.load() - before;
    };
    const auto without = measure(disabled);
    const auto with = measure(enabled);
    std::cout << "warmed eight-tick allocations: disabled=" << without << " enabled=" << with << '\n';
    expect(with <= without + 8, "capture adds only one published pad-view allocation per tick");

    auto start = setup();
    start.units.back().owner = 1;
    auto locked = session(start);
    expect(locked.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "locked-pad allocation fixture");
    steps(locked, 16, warm);
    for (int frame = 0; frame < 8; ++frame) {
        const auto tick = locked.step(AllocationExecutor{});
        expect(tick && tick.value().capture_candidates == 0 && tick.value().capture_index_bodies == 0
            && tick.value().capture_prepare_bodies == 0 && tick.value().capture_serial_bodies == 0,
            "WBP-05 locked pads perform no capture preparation, indexing or queries");
    }
}
void test_legacy_queue() {
    eawr::sim::InlineExecutor executor;
    for (const auto occupied : {0U, 1U, 2U}) {
        auto start = setup();
        start.units.back().owner = 1;
        auto value = session(start);
        if (occupied != 0) {
            expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "legal occupancy fixture");
            steps(value, occupied == 1 ? 1 : 32, executor);
        }
        const auto credits = value.ledgers().front().credits;
        const auto tick = value.completed_tick();
        expect(value.submit({{tick, 1, 1}, {3}, t::BuyPayload{30}}).has_value(), "legacy opcode 9 delivers to pad");
        expect(value.submit({{tick, 1, 2}, {}, t::CancelPayload{0, 0}}).has_value(), "cancel illegal pad buy delivers");
        const auto result = value.step(executor);
        expect(result && std::any_of(result.value().snapshot->events().begin(), result.value().snapshot->events().end(),
            [](const auto& event) { return event.sequence == 1 && event.reason == t::RejectReason::cannot_produce; }),
            "legacy station queue rejects empty, under-construction and occupied pads");
        steps(value, tick + 35, executor);
        const auto& ledger = value.ledgers().front();
        expect(ledger.credits == credits && ledger.queues[0].empty() && ledger.completed.empty() && ledger.pool.empty(),
            "illegal legacy pad buys never debit, refund or leave phantom ledger builds");
        const auto bytes = t::write_replay(value.record());
        expect(bytes.has_value(), "legacy pad buy replay writes");
        if (!bytes) continue;
        const auto replay = t::parse_replay(bytes.value());
        expect(replay.has_value(), "legacy pad buy replay round trip");
        if (replay) {
            auto rerun = t::TacticalSession::from_replay(replay.value(), {}, durability(), {}, {}, {}, {}, {}, rules()).value();
            steps(rerun, value.completed_tick(), executor);
            expect(rerun.state_sha256() == value.state_sha256(), "crafted opcode 9 replay uses the same rejection route");
        }
    }
    for (const bool producer : {false, true}) {
        auto content = rules();
        const t::TypeId type = producer ? 30 : 40;
        t::StationMenu menu;
        menu.station = 10;
        menu.faction = 100;
        menu.options = {{type, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 0, true}};
        menu.station_producer = producer;
        content.menus = {std::move(menu)};
        auto value = session(setup(), content);
        expect(value.submit({{0, 1, 0}, {1}, t::BuyPayload{type}}).has_value(), "legacy non-pad producer fixture");
        steps(value, 35, executor);
        expect(value.ledgers().front().credits == q(1000) && value.ledgers().front().pool.empty(),
            "station queues reject UC types even on station menus, and reject non-station producers");
    }
}
void test_captured_producer() {
    // WBP-33/34: capture points can be ordinary producers; construction pads cannot.
    auto content = rules();
    content.pads.capture.front().build_pad = false;
    content.pads.capture.front().ownership_sticks = true;
    for (auto& player : content.players) player.population_cap = 2;
    for (auto& menu : content.menus) {
        menu.station_producer = true;
        menu.options = {{10, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 2, true}};
    }
    t::TacticalReplay replay{setup(), 260, {
        {{0, 1, 0}, {3}, t::BuyPayload{10}}, // neutral ownership
        {{40, 1, 1}, {3}, t::PadBuildPayload{30}}, // ordinary producer is not a UC pad
        {{40, 1, 2}, {3}, t::BuyPayload{10}},
        {{40, 2, 0}, {3}, t::BuyPayload{10}}, // hostile ownership
        {{40, 3, 0}, {3}, t::BuyPayload{10}}, // ally uses the dock owner's faction menu
        {{75, 1, 3}, {}, t::ReinforcePayload{10, at(200, 200)}},
        {{75, 3, 1}, {}, t::ReinforcePayload{10, at(300, 200)}}}};
    const auto encoded = t::write_replay(replay);
    const auto parsed = encoded ? t::parse_replay(encoded.value())
        : eawr::core::Result<t::TacticalReplay>::failure(encoded.error());
    expect(parsed && parsed.value() == replay, "captured-producer commands round trip");
    if (!parsed) return;
    std::vector<std::string> hashes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto made = t::TacticalSession::from_replay(parsed.value(), {}, durability(), {}, {}, {}, {}, {}, content);
        expect(made.has_value(), "captured-producer content binds");
        if (!made) return;
        auto value = std::move(made).value();
        value.scramble_storage_for_testing();
        while (value.completed_tick() < replay.final_tick_count) {
            const auto stepped = value.step(executor);
            expect(stepped.has_value(), "captured-producer frame succeeds");
            if (!stepped) return;
            const auto tick = value.completed_tick();
            if (workers == 1) hashes.push_back(value.state_sha256());
            else expect(value.state_sha256() == hashes[tick - 1], "merchant per-frame 1/2/4/8 equality");
            if (tick == 1 || tick == 41) {
                const auto events = stepped.value().snapshot->events();
                const auto refused = std::count_if(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == t::EventKind::order_rejected && event.reason == t::RejectReason::cannot_produce;
                });
                expect(refused == (tick == 1 ? 1 : 2), "neutral, enemy and UC-route purchases refuse without payment");
            }
            if (tick == 41) {
                expect(value.units().back().owner == 1, "dock captures before the purchase");
                expect(value.ledgers()[0].credits == q(900) && value.ledgers()[2].credits == q(900),
                    "owner and allied buyer each pay once");
                expect(value.ledgers()[0].queues[0].size() == 1 && value.ledgers()[2].queues[0].size() == 1,
                    "merchant buys enter ordinary units queues");
                expect(value.pads().at(3).under_construction == 0 && value.pads().at(3).constructed == 0,
                    "merchant buy leaves capture point free of pad children");
            }
            if (tick == 75) {
                expect(value.ledgers()[0].pool == std::vector<t::TypeId>{10}
                    && value.ledgers()[2].pool == std::vector<t::TypeId>{10}, "merchant completes into each buyer's pool");
            }
        }
        const auto snapshot = value.snapshot();
        expect(snapshot->economy()[0].population == 2 && snapshot->economy()[2].population == 2,
            "merchant reinforcements count normal population");
        expect(value.ledgers()[0].pool.empty() && value.ledgers()[2].pool.empty(), "delivery consumes bought pool entries");
        const auto units = value.units();
        expect(std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.type_id == 10 && ((unit.owner == 1 && unit.position == at(200, 200))
                || (unit.owner == 3 && unit.position == at(300, 200)));
        }) == 2, "merchant units land at the buyer's chosen points");
    }
}
void test_construction() {
    eawr::sim::InlineExecutor executor;
    auto start = setup();
    start.units.back().owner = 1;
    start.units.back().rotation = {Fixed{}, Fixed{}, q(1), Fixed{}};
    auto attachment_rules = rules();
    attachment_rules.pads.capture.front().attachment = at(2, 3, 4);
    auto value = session(start, attachment_rules);
    expect(value.submit({{0, 3, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "WBP-09 ally may build");
    steps(value, 1, executor);
    expect(value.pads().at(3).under_construction == 4 && value.units().back().owner == 3,
        "WBP-05/13 immediate occupancy, builder differs from pad owner");
    expect(value.durability_state(4)->hull == t::initial_construction_hull, "WBP-14 0.01 absolute hull on creation");
    expect(value.units().back().position == at(8, -3, 4)
        && value.units().back().rotation == start.units.back().rotation,
        "WBP-13 UC uses the transformed attachment and pad facing");
    expect(value.ledgers()[2].credits == q(900) && value.ledgers()[0].credits == q(1000), "WBP-10 debit builder once");
    expect(value.ledgers()[2].queues[0].empty() && value.ledgers()[2].pool.empty(), "WBP-12 no queue or population");
    steps(value, 11, executor);
    expect(value.durability_state(4)->hull.raw() == t::initial_construction_hull.raw() + q(100).raw(),
        "WBP-15/16 fixed healing per frame");
    const auto deadline = value.construction().at(4).finish_frame;
    expect(value.submit({{11, 3, 1}, {4}, t::DamagePayload{q(50), t::attack_hull}}).has_value(), "scripted construction damage");
    steps(value, 12, executor);
    expect(value.construction().at(4).finish_frame == deadline, "WBP-16 damage does not move deadline");
    steps(value, 31, executor);
    expect(!value.durability_state(4) && value.pads().at(3).constructed == 5 && value.construction().empty(),
        "WBP-18/19 replace UC and clear link");
    expect(value.durability_state(5)->hull < q(600) && value.durability_state(5)->hull > q(499),
        "WBP-19 replacement preserves damaged health fraction across maxima");
    auto completed_at = at(8, -3, 4);
    completed_at.z = Fixed::from_raw(completed_at.z.raw() + one / 10);
    expect(value.units().back().position == completed_at
        && value.units().back().rotation == start.units.back().rotation,
        "WBP-18 completion retains the attachment, raises Z and uses pad facing");
    auto dying = session(start);
    expect(dying.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "death case build");
    expect(dying.submit({{1, 1, 1}, {4}, t::DamagePayload{q(1), t::attack_hull}}).has_value(), "death case damage");
    steps(dying, 32, executor);
    expect(dying.construction().empty() && dying.pads().at(3).constructed == 0
        && dying.pads().at(3).under_construction == 0 && dying.ledgers()[0].credits == q(900),
        "WBP-16 killed UC detaches, never completes or refunds");

    auto racing = session(start);
    expect(racing.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value()
        && racing.submit({{0, 3, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "two allied requests in one frame");
    steps(racing, 1, executor);
    expect(racing.construction().size() == 1 && racing.ledgers()[0].credits == q(900)
        && racing.ledgers()[2].credits == q(1000), "WBP-09/13 execution rechecks links before second debit");
    auto killed_before_execution = session(start);
    expect(killed_before_execution.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value()
        && killed_before_execution.submit({{0, 1, 1}, {3}, t::DamagePayload{q(300), t::attack_hull}}).has_value(),
        "pad death between human request and execution delivers");
    steps(killed_before_execution, 1, executor);
    expect(killed_before_execution.construction().empty() && killed_before_execution.ledgers().front().credits == q(1000),
        "WBP-09/10 live-pad execution check rejects a dead parent before debit");

    for (const bool ai_builder : {false, true}) {
        auto gated_rules = rules();
        gated_rules.disabled_types = {30};
        gated_rules.players.front().ai = ai_builder;
        auto gated = session(start, gated_rules);
        expect(!gated.build_allowed(1, 3, 30), "RG-04: AI pad discovery rejects a gated type in an available menu");
        expect(gated.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(),
            "gated pad request with stale available menu delivers");
        const auto rejected = gated.step(executor);
        expect(rejected && std::any_of(rejected.value().snapshot->events().begin(),
            rejected.value().snapshot->events().end(), [](const auto& event) {
                return event.reason == t::RejectReason::cannot_produce;
            }) && gated.construction().empty() && gated.pads().at(3).under_construction == 0
            && gated.ledgers().front().credits == q(1000),
            "indexed roster gate rejects human and AI pad construction before debit or creation");
    }

    auto poor_rules = rules();
    poor_rules.players.front().credits = q(99);
    auto poor = session(start, poor_rules);
    expect(poor.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "unaffordable request delivers");
    steps(poor, 1, executor);
    expect(poor.construction().empty() && poor.ledgers().front().credits == q(99), "WBP-09 rejects unaffordable request");

    auto occupied_start = start;
    occupied_start.units[1].position = occupied_start.units.back().position;
    auto entered = session(occupied_start);
    expect(!entered.pad_build_allowed(1, 3).value(), "WBP-07 hostile presence prevents opening a new menu");
    expect(entered.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "already-open menu request delivers");
    steps(entered, 1, executor);
    expect(entered.pads().at(3).under_construction != 0,
        "WBP-08/10 request and execution do not repeat menu proximity");

    auto missing_child_health = durability();
    std::erase_if(missing_child_health.profiles, [](const auto& profile) { return profile.type_id == 30; });
    auto failed = t::TacticalSession::create(start, {}, missing_child_health, {}, {}, {}, {}, {}, rules());
    expect(failed && failed.value().submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(),
        "post-debit creation failure delivers");
    if (failed) {
        steps(failed.value(), 1, executor);
        expect(failed.value().construction().empty() && failed.value().ledgers().front().credits == q(900),
            "WBP-10 failure after debit has no refund");
    }

    auto missing_final_health = durability();
    std::erase_if(missing_final_health.profiles, [](const auto& profile) { return profile.type_id == 40; });
    auto incomplete = t::TacticalSession::create(start, {}, missing_final_health, {}, {}, {}, {}, {}, rules());
    expect(incomplete && incomplete.value().submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(),
        "completion creation failure delivers");
    if (incomplete) {
        steps(incomplete.value(), 31, executor);
        expect(incomplete.value().construction().empty() && !incomplete.value().durability_state(4)
            && incomplete.value().pads().at(3).under_construction == 0
            && incomplete.value().pads().at(3).constructed == 0 && incomplete.value().ledgers().front().credits == q(900),
            "WBP-18 completion failure clears occupancy and UC without a refund");
    }

    auto ai_rules = rules();
    ai_rules.players.front().ai = true;
    ai_rules.pads.construction.front().ai_seconds = 2;
    auto ai = session(start, ai_rules);
    expect(ai.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "AI direct construction delivers");
    steps(ai, 1, executor);
    expect(ai.construction().at(4).finish_frame == 60 && ai.ledgers().front().credits == q(1000),
        "WBP-13/15 AI duration is distinct and this path does not debit plan budget");
    steps(ai, 60, executor);
    expect(ai.pads().at(3).constructed == 0, "AI does not complete before its deadline");
    steps(ai, 61, executor);
    expect(ai.pads().at(3).constructed != 0, "AI completes on its fixed deadline");

    auto lethal_finish = session(start);
    expect(lethal_finish.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value()
        && lethal_finish.submit({{30, 1, 1}, {4}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(),
        "deadline lethal damage delivers");
    steps(lethal_finish, 31, executor);
    expect(lethal_finish.pads().at(3).constructed == 0 && lethal_finish.construction().empty(),
        "U-BP-1 explicit policy: lethal damage wins on the deadline");

    auto retry = session(start);
    auto successful = session(start);
    const t::PlayerCommand request{{0, 1, 0}, {3}, t::PadBuildPayload{30}};
    expect(retry.submit(request).has_value() && successful.submit(request).has_value(), "transaction retry fixture");
    const auto before = retry.state_sha256();
    expect(!retry.step(FailingSystemsExecutor{}) && retry.state_sha256() == before
        && retry.pads().at(3).under_construction == 0 && retry.construction().empty(),
        "failed systems phase leaves committed pad links and construction unchanged");
    steps(retry, 32, executor);
    steps(successful, 32, executor);
    expect(retry.state_sha256() == successful.state_sha256(), "retained staging restores failed command writes before retry");
}
void test_sale() {
    auto content = rules();
    content.pad_sales = {{40, Fixed::from_raw(one / 2)}};
    content.pads.capture.front().destroy_when_child_dies = true;
    content.pads.respawn = {{20, 45 * 30}};
    content.menus.front().options.front().price = q(875);
    content.pads.construction.front().price = q(875);
    auto start = setup(); start.units.back().owner = 1;
    std::vector<std::string> hashes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto value = session(start, content);
        expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "sale fixture build");
        steps(value, 31, pool);
        expect(value.pad_sale_allowed(1, 5) && !value.pad_sale_allowed(3, 5)
            && !value.pad_sale_allowed(1, 5, true), "WBP-30 exact owner, allied refusal and single-step guard");
        expect(!value.pad_sale_allowed(1, 3), "WBP-30 empty pad has no sale behavior");
        expect(value.submit({{31, 3, 0}, {5}, t::PadSellPayload{}}).has_value(), "allied sale command records");
        steps(value, 32, pool);
        expect(value.pads().at(3).constructed == 5 && value.ledgers()[0].credits == q(125),
            "WBP-30 allied sale execution cannot remove or refund child");
        expect(value.submit({{32, 1, 1}, {5}, t::DamagePayload{q(200), t::attack_hull}}).has_value()
            && value.submit({{32, 1, 2}, {5}, t::PadSellPayload{}}).has_value(), "damaged sale records");
        steps(value, 33, pool);
        expect(!value.durability_state(5) && value.durability_state(3)
            && value.pads().at(3).constructed == 0 && value.pads().at(3).cooldown_until == 0
            && value.units().back().owner == 1 && value.ledgers()[0].credits == q(563),
            "WBP-31/32 damaged 875-cost satellite refunds 438 and preserves live owned pad without cooldown");
        const auto hash = value.state_sha256();
        if (workers == 1) hashes.push_back(hash); else expect(hash == hashes.front(), "sale hash equals 1/2/4/8 workers");
        const auto record = value.record();
        const auto bytes = t::write_replay(record);
        const auto decoded = bytes ? t::parse_replay(bytes.value()) : eawr::core::Result<t::TacticalReplay>::failure(bytes.error());
        expect(decoded && decoded.value() == record, "WBP-30 opcode 15 sale replay round trip");
        if (decoded) {
            auto replayed = t::TacticalSession::from_replay(decoded.value(), {}, durability(), {}, {}, {}, {}, {}, content).value();
            replayed.scramble_storage_for_testing();
            steps(replayed, value.completed_tick(), pool);
            expect(replayed.canonical_state_bytes() == value.canonical_state_bytes(),
                "WBP-30 sale replay executes identically with scrambled storage");
        }
        expect(value.submit({{33, 1, 3}, {5}, t::PadSellPayload{}}).has_value(), "repeat sale records");
        steps(value, 34, pool);
        expect(value.ledgers()[0].credits == q(563) && value.pads().contains(3),
            "WBP-31 repeat sale cannot refund twice");
        auto neutralize = start;
        neutralize.units.front().position = at(1000);
        auto vacant = session(neutralize, content);
        expect(vacant.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "vacant build records");
        steps(vacant, 31, pool);
        expect(vacant.submit({{32, 1, 1}, {5}, t::PadSellPayload{}}).has_value(), "vacant sale records");
        steps(vacant, 64, pool);
        expect(vacant.pads().contains(3) && vacant.units().back().owner == 4,
            "WBP-32 empty sold pad uses later ordinary neutralization");
    }
    t::PadState locked; locked.contents_locked = true;
    expect(!t::pad_sale_permission(1, 1, true, &locked), "WBP-30 parent contents lock denies sale");
    {
        auto value = session(start, content);
        eawr::sim::InlineExecutor executor;
        expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "rollback sale builds");
        steps(value, 1, executor);
        expect(!value.pad_sale_allowed(1, 4), "WBP-30 UC has no sale action");
        steps(value, 31, executor);
        expect(value.submit({{32, 1, 1}, {5}, t::PadSellPayload{}}).has_value(), "rollback sale queues");
        steps(value, 32, executor);
        const auto before = value.state_sha256();
        expect(!value.step(FailingSystemsExecutor{}) && value.state_sha256() == before,
            "WBP-32 failed tick rolls back sale refund and parent detach");
        steps(value, 33, executor);
        expect(value.ledgers()[0].credits == q(563) && value.pads().at(3).constructed == 0,
            "WBP-32 sale retry commits once");
    }
    {
        auto value = session(start, content);
        eawr::sim::InlineExecutor executor;
        expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "lethal sale builds");
        steps(value, 31, executor);
        expect(value.submit({{32, 1, 1}, {5}, t::DamagePayload{q(1000), t::attack_hull}}).has_value()
            && value.submit({{32, 1, 2}, {5}, t::PadSellPayload{}}).has_value(), "death before sale records");
        steps(value, 33, executor);
        expect(value.pads().empty() && value.ledgers()[0].credits == q(125),
            "WBP-30/31 lethal removal before sale refuses refund and retains the death route");
    }
    for (const auto price : {700, 750, 1000, 1200, 875}) {
        auto alternate = content;
        alternate.menus.front().options.front().price = q(price);
        alternate.pads.construction.front().price = q(price);
        alternate.players.front().credits = q(2000);
        auto value = session(start, alternate);
        eawr::sim::InlineExecutor executor;
        expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "alternative sale builds");
        steps(value, 31, executor);
        expect(value.submit({{32, 1, 1}, {5}, t::PadSellPayload{}}).has_value(), "full-health sale records");
        steps(value, 33, executor);
        expect(value.ledgers().front().credits == q(2000 - price + (price + 1) / 2),
            "WBP-31 full-health faction alternatives use round-half generic cost");
    }
    {
        auto value = session(start, content);
        eawr::sim::InlineExecutor executor;
        expect(value.submit({{0, 3, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "allied builder sale fixture");
        steps(value, 31, executor);
        expect(!value.pad_sale_allowed(1, 5) && value.pad_sale_allowed(3, 5),
            "WBP-30 child builder owns the sale permission independently of parent owner");
        expect(value.submit({{31, 3, 1}, {5}, t::PadSellPayload{}}).has_value()
            && value.submit({{31, 3, 2}, {5}, t::PadSellPayload{}}).has_value(), "same-frame duplicate sales record");
        steps(value, 32, executor);
        expect(value.ledgers()[0].credits == q(1000) && value.ledgers()[2].credits == q(175)
            && value.pads().at(3).constructed == 0 && value.units().back().owner == 1,
            "WBP-31 owning faction's 100-cost menu refunds 50 once, independent of the 875 pad debit");
    }
    for (const bool missing_menu : {false, true}) {
        auto alternate = content;
        alternate.pad_sales.front().percentage = Fixed::from_raw(-one / 2);
        auto value = session(start, alternate);
        eawr::sim::InlineExecutor executor;
        if (missing_menu) {
            auto detached = start;
            detached.units.back().type_id = 40;
            auto no_menu = session(detached, content);
            expect(no_menu.submit({{0, 1, 0}, {3}, t::PadSellPayload{}}).has_value(), "detached sale records");
            steps(no_menu, 1, executor);
            expect(!no_menu.durability_state(3) && no_menu.ledgers()[0].credits == q(1000),
                "WBP-31 missing parent menu still removes the child with zero refund");
        } else {
            expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "negative percentage builds");
            steps(value, 31, executor);
            expect(value.submit({{31, 1, 1}, {5}, t::PadSellPayload{}}).has_value(), "negative percentage sale records");
            steps(value, 32, executor);
            expect(!value.durability_state(5) && value.ledgers()[0].credits == q(125),
                "WBP-31 nonpositive refund removes child without debiting its owner");
        }
    }
}

void test_sale_victory() {
    auto content = rules();
    content.pad_sales = {{40, Fixed::from_raw(one / 2)}};
    auto start = setup();
    start.units.erase(start.units.begin()); // Seller has only the pad and its future child.
    start.units.back().owner = 1;
    t::VictoryRules victory;
    victory.condition = t::VictoryCondition::all_enemy_units_destroyed;
    victory.contenders = {1, 2, 3}; victory.humans = {1, 2, 3};
    victory.controlled_players = {1, 2, 3}; victory.installed_players = {1, 2, 3};
    victory.relevant_types = {10, 40};
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto value = session(start, content, victory);
        expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "last-unit sale fixture builds");
        steps(value, 31, pool);
        expect(!value.outcome() && value.pads().at(3).constructed == 5, "admitting the relevant child decides nothing");
        expect(value.submit({{31, 3, 0}, {5}, t::PadSellPayload{}}).has_value(), "allied last-unit sale records");
        steps(value, 32, pool);
        expect(!value.outcome() && value.pads().at(3).constructed == 5, "rejected sale preserves victory count");
        expect(value.submit({{32, 1, 1}, {5}, t::PadSellPayload{}}).has_value(), "last relevant structure sale records");
        steps(value, 33, pool);
        expect(value.outcome() && value.outcome()->winner == 2U && value.outcome()->deciding_unit == 5
            && value.outcome()->decided_tick == 32 && value.pads().contains(3),
            "WBP-32/WBF-31/35 selling the last relevant structure evaluates elimination in its frame");
        if (workers == 1) reference = value.state_sha256();
        else expect(value.state_sha256() == reference, "last-structure sale victory hashes agree on 1/2/4/8 workers");
    }
    victory.condition = t::VictoryCondition::enemy_starbase_destroyed;
    victory.starbase_types = {10};
    auto value = session(start, content, victory);
    eawr::sim::InlineExecutor executor;
    expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "starbase-only sale fixture builds");
    steps(value, 31, executor);
    expect(value.submit({{32, 1, 1}, {5}, t::PadSellPayload{}}).has_value(), "starbase-only sale records");
    steps(value, 33, executor);
    expect(!value.outcome(), "WBF-35 selling a non-starbase cannot decide the starbase-only condition");
}

void test_mine_income() {
    eawr::sim::InlineExecutor executor;
    for (const auto amount : {200, 205}) {
        auto content = rules();
        const auto rate = m::divide(q(amount), q(300)).value();
        content.income = {{40, rate, {}}};
        auto start = setup(); start.units.back().owner = 1;
        auto value = session(start, content);
        expect(value.submit({{0, 3, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "mine builds for allied builder");
        steps(value, 30, executor);
        expect(value.ledgers()[0].credits == q(1000) && value.ledgers()[2].credits == q(900),
            "WBP-22 empty pad and UC earn nothing");
        steps(value, 31, executor);
        expect(value.ledgers()[0].credits.raw() == q(1000).raw() + rate.raw()
            && value.ledgers()[2].credits.raw() == q(900).raw() + rate.raw()
            && value.ledgers()[1].credits == q(1000),
            "WBP-22/23 completion-frame payment gives each ally the full rate, no enemy income");
        steps(value, 330, executor);
        expect(value.ledgers()[0].credits.raw() == q(1000).raw() + 300 * rate.raw()
            && std::abs(300 * rate.raw() - q(amount).raw()) <= 150,
            "WBP-22 E/R 200 and U 205 per ten seconds, bounded fixed-point rounding");
        for (int frame = 0; frame < 8; ++frame) expect(value.step(AllocationExecutor{}).has_value(), "warmed income step");
        const auto before = value.ledgers()[0].credits;
        const auto mine = value.pads().at(3).constructed;
        expect(value.submit({{value.completed_tick(), 3, 1}, {mine}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(),
            "mine lethal damage delivers");
        steps(value, value.completed_tick() + 10, executor);
        expect(value.ledgers()[0].credits == before && !value.durability_state(mine),
            "WBP-26 lethal damage removes the stream before that frame's income");
    }
    // A synthetic capture source exercises live ownership through the existing owner-change path;
    // stock completed mines themselves have no occupied-pad recapture route (WBP-26).
    auto content = rules();
    content.income = {{40, q(1), {}}};
    auto source_profile = content.pads.capture.front(); source_profile.type = 40; source_profile.build_pad = false;
    content.pads.capture.push_back(source_profile);
    auto start = setup(); start.units.back().type_id = 40; start.units.back().owner = 1; start.units.back().position = at(500);
    auto value = session(start, content);
    steps(value, 64, executor);
    expect(value.units().back().owner == 2, "synthetic source ownership changes through partitioned capture");
    const auto old = value.ledgers()[0].credits; const auto live = value.ledgers()[1].credits;
    steps(value, 65, executor);
    expect(value.ledgers()[0].credits == old && value.ledgers()[1].credits == m::add(live, q(1)).value(),
        "WBP-26 live ownership moves the stream away from cached placement ownership");
}
Fixed fraction(const char* value) { return Fixed::from_decimal(value).value(); }
t::EconomyRules modifier_rules() {
    auto content = rules();
    content.pads = {};
    content.menus.clear();
    t::IncomeProfile stream{40, m::divide(q(200), q(300)).value(), {}};
    stream.base_value = q(200); stream.interval_seconds = q(10);
    content.income = {stream};
    stream.source = 50; content.income.push_back(stream);
    t::IncomeModifier low{40, 1, fraction("0.2"), {}, {}, true, false};
    auto high = low; high.percentage = fraction("0.4");
    content.upgrades = {{90, false, false, 0, {}, {low}}, {91, false, false, 90, {}, {high}},
        {92, false, false, 91, {}, {}}};
    for (const auto faction : {100ULL, 300ULL}) {
        t::StationMenu menu;
        menu.station = 40; menu.faction = faction;
        for (const auto type : {90ULL, 91ULL, 92ULL}) {
            t::BuildOption option;
            option.type = type; option.kind = t::BuildKind::upgrade; option.queue = t::BuildQueue::upgrades;
            option.price = q(type == 91 ? 500 : 250);
            option.build_frames = 2; option.ai_build_frames = 2; option.available = true;
            option.requirements.current_allies = 1;
            if (type == 91) option.requirements.prerequisites = {90};
            menu.options.push_back(std::move(option));
        }
        content.menus.push_back(std::move(menu));
    }
    return content;
}
t::TacticalSetup modifier_setup() {
    auto start = setup();
    start.units = {{10, 40, 1, at(0), m::identity_quat(), {}},
        {11, 40, 3, at(300), m::identity_quat(), {}},
        {12, 40, 2, at(700), m::identity_quat(), {}},
        {13, 50, 3, at(1000), m::identity_quat(), {}}};
    return start;
}
void test_modifier_reduction() {
    auto content = modifier_rules();
    auto other = content.upgrades.front().income_modifiers.front();
    other.stacking_category = 2;
    content.upgrades.back().income_modifiers = {other};
    auto categories = t::initial_income_categories(content, 1);
    t::reduce_income_modifier(content.upgrades.front().income_modifiers.front(), categories);
    t::reduce_income_modifier(content.upgrades[1].income_modifiers.front(), categories);
    expect(t::modified_income_per_frame(q(200), q(10), categories).value()
        == m::divide(q(280), q(300)).value(), "WBP-44 same-category maximum is 40 percent, not 60");
    t::reduce_income_modifier(other, categories);
    expect(std::abs(t::modified_income_per_frame(q(200), q(10), categories).value().raw()
        - m::divide(q(320), q(300)).value().raw()) <= 1, "WBP-44 different category winners add");
    for (auto& category : categories) category.winners = {};
    other.stacking_category = 1; other.percentage = fraction("-0.4");
    t::reduce_income_modifier(other, categories);
    other.percentage = fraction("-0.2"); t::reduce_income_modifier(other, categories);
    other.percentage = {}; t::reduce_income_modifier(other, categories);
    expect(std::abs(t::modified_income_per_frame(q(200), q(10), categories).value().raw()
        - m::divide(q(160), q(300)).value().raw()) <= 1, "WBP-44 signed negative maximum survives neutral input");
    for (auto& category : categories) category.winners = {};
    other.percentage = {}; other.additive = q(10); other.interval_percentage = fraction("-0.99");
    t::reduce_income_modifier(other, categories);
    expect(t::modified_income_per_frame(q(200), q(10), categories).value() == q(7),
        "WBP-44 additive value and one-second interval floor are independent lists");
    for (const auto& [base, percent, rate] : {std::tuple{205, "0.15", "0.785833333"},
                                           std::tuple{205, "0.45", "0.990833333"}}) {
        for (auto& category : categories) category.winners = {};
        other.percentage = fraction(percent); other.additive = {}; other.interval_percentage = {};
        t::reduce_income_modifier(other, categories);
        expect(std::abs(t::modified_income_per_frame(q(base), q(10), categories).value().raw()
            - fraction(rate).raw()) <= 2, "WBP-44 Underworld rates are 23.575 and 29.725 credits per second");
    }
    for (auto& category : categories) category.winners = {};
    other.percentage = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    t::reduce_income_modifier(other, categories);
    expect(!t::modified_income_per_frame(q(200), q(10), categories), "modifier overflow is a checked failure");
}
void test_modifier_service() {
    eawr::sim::InlineExecutor executor;
    auto value = session(modifier_setup(), modifier_rules());
    expect(value.submit({{0, 1, 0}, {10}, t::BuyPayload{91}}).has_value(), "L2 request without prerequisite delivers");
    steps(value, 1, executor);
    expect(value.ledgers()[0].completed.empty() && value.ledgers()[0].queues[1].empty(), "WBP-24 L2 prerequisite refuses before debit");
    expect(value.submit({{1, 1, 1}, {10}, t::BuyPayload{90}}).has_value(), "mine queues L1 through ordinary upgrade route");
    steps(value, 4, executor);
    expect(value.ledgers()[0].completed.size() == 1, "WBP-24 queue completion creates a modifier object");
    if (value.ledgers()[0].completed.empty()) return;
    auto state = value.ledgers()[0].completed.front().income_modifiers.front();
    expect(state.initialized && state.attached == std::vector<eawr::sim::EntityId>{10, 11},
        "WBP-25 activation immediately discovers allied matching source types, excluding enemy and other stream");
    expect(state.next_scan_frame >= 3 && state.next_scan_frame <= 153, "WBP-45 bounded randomized first deadline");
    const auto before = value.ledgers()[0].credits;
    const auto ally_before = value.ledgers()[2].credits;
    const auto enemy_before = value.ledgers()[1].credits;
    steps(value, 5, executor);
    const auto base = m::divide(q(200), q(300)).value();
    const auto boosted = m::divide(q(240), q(300)).value();
    expect(value.ledgers()[0].credits.raw() - before.raw() == 2 * boosted.raw() + base.raw()
        && value.ledgers()[2].credits.raw() - ally_before.raw() == 2 * boosted.raw() + base.raw()
        && value.ledgers()[1].credits.raw() - enemy_before.raw() == base.raw(),
        "WBP-23/44 full upgraded amount reaches each ally; enemy only receives its base stream");
    const auto first_due = std::max(value.completed_tick(), state.next_scan_frame);
    steps(value, first_due + 1, executor);
    const auto next = value.ledgers()[0].completed.front().income_modifiers.front().next_scan_frame;
    expect(next == first_due + 150, "WBP-45 subsequent service is actual scan frame plus 150");
    const auto new_mine = value.stage_spawn({0, 40, 3, at(1500), m::identity_quat(), {}});
    expect(new_mine.has_value(), "new allied mine appears after the scan");
    if (!new_mine) return;
    const auto waiting_before = value.ledgers()[2].credits;
    steps(value, next, executor);
    expect(value.ledgers()[2].credits.raw() - waiting_before.raw()
        == static_cast<std::int64_t>(next - first_due - 1) * (2 * boosted.raw() + 2 * base.raw()),
        "WBP-45 newly created source receives base income until discovery, without repayment");
    state = value.ledgers()[0].completed.front().income_modifiers.front();
    expect(!std::binary_search(state.attached.begin(), state.attached.end(), new_mine.value()), "new mine absent before due scan");
    const auto scan_before = value.ledgers()[2].credits;
    steps(value, next + 1, executor);
    state = value.ledgers()[0].completed.front().income_modifiers.front();
    expect(state.attached.size() == 3 && state.next_scan_frame == next + 150
        && value.ledgers()[2].credits.raw() - scan_before.raw() == 3 * boosted.raw() + base.raw(),
        "WBP-45 due scan discovers once and boosts only the current and later frames");
    for (int frame = 0; frame < 6; ++frame) expect(value.step(AllocationExecutor{}).has_value(), "warmed modifier reduction allocation contract");
    expect(value.submit({{value.completed_tick(), 3, 0}, {11}, t::BuyPayload{90}}).has_value(), "allied duplicate L1 request delivers");
    steps(value, value.completed_tick() + 1, executor);
    expect(value.ledgers()[2].queues[1].empty(), "WBP-24 current allied build limit prevents duplicate L1");
    expect(value.submit({{value.completed_tick(), 1, 2}, {10}, t::BuyPayload{91}}).has_value(), "L2 with prerequisite delivers");
    steps(value, value.completed_tick() + 3, executor);
    expect(value.ledgers()[0].completed.size() == 1 && value.ledgers()[0].completed.front().type == 91,
        "WBP-24 previous-level replacement removes only allied L1 object");
    const auto high_before = value.ledgers()[2].credits;
    steps(value, value.completed_tick() + 1, executor);
    const auto high = m::divide(q(280), q(300)).value();
    expect(value.ledgers()[2].credits.raw() - high_before.raw() == 3 * high.raw() + base.raw(), "WBP-44 L2 gives 28/sec per mine");
    expect(value.submit({{value.completed_tick(), 1, 3}, {10}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "mine source lethal damage delivers");
    const auto death_before = value.ledgers()[2].credits;
    steps(value, value.completed_tick() + 1, executor);
    expect(value.ledgers()[2].credits.raw() - death_before.raw() == 2 * high.raw() + base.raw(),
        "WBP-26 dead stream disappears before current-frame income; U-BP-6 does not infer host modifier deletion");
    state = value.ledgers()[0].completed.front().income_modifiers.front();
    expect(state.attached.size() == 2 && !std::binary_search(state.attached.begin(), state.attached.end(), 10),
        "WBP-46 dead source attachment is removed without disturbing surviving streams");
}
void test_modifier_determinism() {
    std::vector<std::string> hashes;
    std::optional<t::TacticalReplay> recording;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto value = recording
            ? t::TacticalSession::from_replay(*recording, {}, durability(), {}, {}, {}, {}, {}, modifier_rules()).value()
            : session(modifier_setup(), modifier_rules());
        value.scramble_storage_for_testing();
        if (!recording) expect(value.submit({{0, 1, 0}, {10}, t::BuyPayload{90}}).has_value()
            && value.submit({{10, 1, 1}, {10}, t::BuyPayload{91}}).has_value(), "modifier deterministic commands deliver");
        while (value.completed_tick() < 400) {
            const auto stepped = value.step(executor);
            expect(stepped.has_value(), "partitioned modifier frame succeeds");
            if (!stepped) break;
            const auto slot = static_cast<std::size_t>(value.completed_tick() - 1);
            if (workers == 1) hashes.push_back(value.state_sha256());
            else expect(hashes[slot] == value.state_sha256(), "WBP-45 randomized discovery and IMOD are identical on 1/2/4/8 workers");
        }
        if (workers == 1) {
            const auto encoded = t::write_replay(value.record());
            expect(encoded.has_value(), "mine upgrade queue replay writes");
            if (!encoded) return;
            const auto decoded = t::parse_replay(encoded.value());
            expect(decoded && decoded.value() == value.record(), "mine upgrade ordinary buy commands round trip");
            if (!decoded) return;
            recording = decoded.value();
        }
    }
}
void test_modifier_removal() {
    eawr::sim::InlineExecutor executor;
    auto content = modifier_rules();
    content.upgrades[1].removes_previous = 0; // Coexisting contributors exercise source-specific withdrawal.
    auto value = session(modifier_setup(), content);
    expect(value.submit({{0, 1, 0}, {10}, t::BuyPayload{90}}).has_value()
        && value.submit({{4, 1, 1}, {10}, t::BuyPayload{91}}).has_value(), "coexisting L1/L2 requests deliver");
    steps(value, 8, executor);
    expect(value.ledgers()[0].completed.size() == 2, "WBP-44 explicit previous-level opt-out retains L1");
    const auto high_before = value.ledgers()[2].credits;
    steps(value, 9, executor);
    const auto base = m::divide(q(200), q(300)).value();
    const auto low = m::divide(q(240), q(300)).value();
    const auto high = m::divide(q(280), q(300)).value();
    expect(value.ledgers()[2].credits.raw() - high_before.raw() == 2 * high.raw() + base.raw(),
        "WBP-44 coexisting same-category L1/L2 choose L2");
    expect(value.submit({{9, 1, 2}, {10}, t::BuyPayload{92}}).has_value(), "L2 removal request delivers");
    steps(value, 13, executor);
    const auto fallback_before = value.ledgers()[2].credits;
    steps(value, 14, executor);
    expect(value.ledgers()[2].credits.raw() - fallback_before.raw() == 2 * low.raw() + base.raw(),
        "WBP-46 removing L2 withdraws only its contribution and exposes surviving L1");
    steps(value, 620, executor);
    const auto retained_before = value.ledgers()[2].credits;
    steps(value, 621, executor);
    const auto& attached = value.ledgers()[0].completed.front().income_modifiers.front().attached;
    expect(value.ledgers()[2].credits.raw() - retained_before.raw() == 2 * low.raw() + base.raw()
        && attached == std::vector<eawr::sim::EntityId>{10, 11},
        "WBP-45/46 repeated scans neither duplicate contributions nor expire them at five seconds");

    content = modifier_rules();
    content.upgrades.front().income_modifiers.front().all_allies = false;
    auto exact = session(modifier_setup(), content);
    expect(exact.submit({{0, 1, 0}, {10}, t::BuyPayload{90}}).has_value(), "exact-owner modifier delivers");
    steps(exact, 4, executor);
    expect(exact.ledgers()[0].completed.front().income_modifiers.front().attached
        == std::vector<eawr::sim::EntityId>{10}, "WBP-25 false allied flag discovers only the exact owner's matching source");
    const auto exact_before = exact.ledgers()[2].credits;
    steps(exact, 5, executor);
    expect(exact.ledgers()[2].credits.raw() - exact_before.raw() == low.raw() + 2 * base.raw(),
        "WBP-23 an exact-owner source modifier still pays each allied recipient in full");

    content = modifier_rules();
    content.upgrades.front().income_modifiers.front().reverse = true;
    auto reversed = session(modifier_setup(), content);
    expect(reversed.submit({{0, 1, 0}, {10}, t::BuyPayload{90}}).has_value(), "reverse modifier delivers");
    steps(reversed, 4, executor);
    const auto& reverse_state = reversed.ledgers()[0].completed.front().income_modifiers.front();
    expect(reverse_state.initialized && reverse_state.attached.empty() && reverse_state.next_scan_frame == 0,
        "WBP-45 reverse activation removes contributions and schedules no periodic service");
    const auto reverse_before = reversed.ledgers()[2].credits;
    steps(reversed, 5, executor);
    expect(reversed.ledgers()[2].credits.raw() - reverse_before.raw() == 3 * base.raw(), "reverse activation leaves base income");
    content = modifier_rules();
    // L2 removal of reverse L1 exchanges termination's remove role for apply.
    // L2's own neutral modifier does not obscure the reverse termination contribution.
    content.upgrades.front().income_modifiers.front().reverse = true;
    content.upgrades[1].income_modifiers.front().percentage = {};
    auto termination = session(modifier_setup(), content);
    expect(termination.submit({{0, 1, 0}, {10}, t::BuyPayload{90}}).has_value()
        && termination.submit({{4, 1, 1}, {10}, t::BuyPayload{91}}).has_value(), "reverse termination commands deliver");
    steps(termination, 8, executor);
    const auto& residuals = termination.ledgers()[0].income_residuals;
    expect(residuals.size() == 1 && residuals.front().income_modifiers.front().attached
        == std::vector<eawr::sim::EntityId>{10, 11}
        && residuals.front().income_modifiers.front().next_scan_frame == 0,
        "WBP-45 reverse termination applies existing eligible streams once without a periodic service");
    const auto terminated_before = termination.ledgers()[2].credits;
    steps(termination, 9, executor);
    expect(termination.ledgers()[2].credits.raw() - terminated_before.raw() == 2 * low.raw() + base.raw(),
        "WBP-45 reverse termination exchanges application/removal roles");
}
void test_respawn() {
    eawr::sim::InlineExecutor executor;
    for (const auto seconds : {38U, 45U, 80U}) {
        for (const bool completed : {false, true}) {
            auto content = rules();
            content.pads.capture.front().destroy_when_child_dies = true;
            content.pads.respawn = {{20, seconds * 30}};
            auto start = setup(); start.units.back().owner = 1;
            start.units.back().rotation = {Fixed{}, Fixed{}, q(1), Fixed{}};
            auto value = session(start, content);
            expect(value.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "respawn construction fixture");
            steps(value, completed ? 31 : 1, executor);
            const auto dead = completed ? value.pads().at(3).constructed : value.pads().at(3).under_construction;
            const auto death = value.completed_tick(); const auto due = death + seconds * 30;
            expect(value.submit({{death, 1, 1}, {dead}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(),
                "child death delivers");
            steps(value, death + 1, executor);
            expect(value.pads().empty() && !value.durability_state(3) && !value.durability_state(dead)
                && value.ledgers()[0].credits == q(900), "WBP-27 UC/final death destroys stock pad without refund");
            steps(value, due, executor);
            expect(value.pads().empty(), "WBP-29 replacement absent before the due logical frame");
            steps(value, due + 1, executor);
            expect(value.pads().size() == 1, "WBP-29 deferred creation occurs at frame equal to deadline");
            const auto id = value.pads().begin()->first;
            const auto units = value.units();
            const auto pad = std::find_if(units.begin(), units.end(), [id](const auto& unit) { return unit.entity_id == id; });
            expect(pad != units.end() && id != 3 && pad->type_id == 20 && pad->owner == 4
                && pad->position == start.units.back().position && pad->rotation == start.units.back().rotation
                && value.pads().begin()->second.under_construction == 0 && value.pads().begin()->second.constructed == 0
                && value.pads().begin()->second.progress.raw() == 0,
                "WBP-29 neutral typed replacement keeps position/facing and no old child/capture state");
        }
    }
    auto content = rules(); content.pads.capture.front().destroy_when_child_dies = true;
    auto start = setup(); start.units.back().owner = 1;
    auto no_respawn = session(start, content);
    expect(no_respawn.submit({{0, 1, 0}, {3}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "no-delay death");
    steps(no_respawn, 2500, executor);
    expect(no_respawn.pads().empty(), "WHZ-52 no authored positive delay schedules no replacement");
    content.pads.respawn = {{10, 2}, {20, 2}};
    const auto neutral = t::respawn_after_death(start.units.back(), content.pads);
    const auto ordinary = t::respawn_after_death(start.units.front(), content.pads);
    expect(neutral && neutral->owner == 4 && ordinary && ordinary->owner == 1,
        "WHZ-52 capture replacements neutralize, ordinary replacements retain the live owner");
    expect(!t::respawn_after_death(start.units.back(), content.pads, true), "WHZ-52 death clones never schedule");
    auto direct = session(start, content);
    expect(direct.submit({{0, 1, 0}, {3}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "direct capture-point death");
    steps(direct, 2, executor); expect(direct.pads().empty(), "direct death waits its rounded deadline");
    steps(direct, 3, executor); expect(direct.pads().size() == 1, "direct capture-point death respawns");
    auto cooldown = rules(); cooldown.pads.capture.front().rebuild_frames = 15;
    auto surviving = session(start, cooldown);
    expect(surviving.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value()
        && surviving.submit({{1, 1, 1}, {4}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "surviving pad cooldown fixture");
    steps(surviving, 2, executor);
    expect(surviving.units().back().owner == 4 && surviving.pads().at(3).cooldown_until == 16
        && t::pad_cooldown_progress(surviving.pads().at(3), 1).raw() == 0
        && t::pad_cooldown_progress(surviving.pads().at(3), 16) == q(1)
        && t::pad_cooldown_progress({}, 1) == q(1), "WBP-28 neutral surviving pad, authored cooldown and clamped progress");
}
class FailingIncomeExecutor final : public eawr::sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        for (std::size_t slot = 0; slot < count; ++slot) run(slot);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        if (name == "income-sources") return eawr::core::Result<void>::failure(eawr::core::Diagnostic{});
        return execute(count, run);
    }
};
void test_bulk_child_death_work() {
    constexpr std::uint32_t count = 128;
    for (const bool completed : {false, true}) {
        auto start = setup();
        start.units.clear();
        for (std::uint32_t index = 0; index < count; ++index) {
            start.units.push_back({index + 1U, 20, 1, at(static_cast<std::int64_t>(index) * 250),
                m::identity_quat(), {}});
        }
        auto content = rules();
        content.players.front().credits = q(100000);
        content.pads.capture.front().destroy_when_child_dies = true;
        content.pads.respawn = {{20, 5}};
        std::string hash;
        std::uint64_t work = 0;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            auto value = session(start, content);
            value.scramble_storage_for_testing();
            for (std::uint32_t index = 0; index < count; ++index) {
                expect(value.submit({{0, 1, index}, {index + 1U}, t::PadBuildPayload{30}}).has_value(),
                    "bulk pad construction command accepts");
            }
            steps(value, completed ? 31 : 1, executor);
            std::vector<eawr::sim::EntityId> children;
            for (const auto& [id, pad] : value.pads()) {
                static_cast<void>(id);
                children.push_back(completed ? pad.constructed : pad.under_construction);
            }
            expect(children.size() == count, "bulk fixture holds every requested child");
            expect(value.submit({{value.completed_tick(), 1, count}, children,
                t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "bulk child damage accepts");
            expect(value.step(executor).has_value(), "bulk child death frame succeeds");
            const auto measured = value.tick_work().pad_death_membership_work;
            expect(value.pads().empty() && value.units().empty(), "bulk child death destroys all stock parents");
            // 128 queries with at most log2(128)+2 comparisons each. This deterministic
            // budget excludes sort work and rejects a scan of 128 removals for each pad.
            expect(measured >= count && measured <= count * 10U,
                "WBP-27 bulk parent membership stays within logarithmic work per pad");
            if (workers == 1) { hash = value.state_sha256(); work = measured; }
            else expect(value.state_sha256() == hash && measured == work,
                "bulk deaths and measured commit work match 1/2/4/8 workers");
        }
    }
}
void test_respawn_rollback() {
    auto content = rules(); content.income = {{40, q(1), {}}}; content.pads.respawn = {{20, 2}};
    auto start = setup(); start.units.push_back({4, 40, 2, at(800), m::identity_quat(), {}});
    auto retry = session(start, content); auto baseline = session(start, content);
    const t::PlayerCommand death{{0, 1, 0}, {3}, t::DamagePayload{q(1000), t::attack_hull}};
    expect(retry.submit(death).has_value() && baseline.submit(death).has_value(), "respawn rollback death fixture");
    const auto before = retry.state_sha256();
    expect(!retry.step(FailingIncomeExecutor{}) && retry.state_sha256() == before && retry.pads().contains(3),
        "failed income phase rolls back deferred scheduling and parent death");
    eawr::sim::InlineExecutor executor;
    steps(retry, 5, executor); steps(baseline, 5, executor);
    expect(retry.state_sha256() == baseline.state_sha256() && retry.pads().size() == 1,
        "respawn retry creates exactly one replacement with the baseline stable ID");
}
void test_modifier_rollback() {
    eawr::sim::InlineExecutor executor;
    auto retry = session(modifier_setup(), modifier_rules());
    auto baseline = session(modifier_setup(), modifier_rules());
    const t::PlayerCommand buy{{0, 1, 0}, {10}, t::BuyPayload{90}};
    expect(retry.submit(buy).has_value() && baseline.submit(buy).has_value(), "modifier rollback requests deliver");
    steps(retry, 4, executor); steps(baseline, 4, executor);
    const auto due = std::max(retry.completed_tick(), retry.ledgers()[0].completed.front().income_modifiers.front().next_scan_frame);
    steps(retry, due, executor); steps(baseline, due, executor);
    const auto spawned = retry.stage_spawn({0, 40, 3, at(1500), m::identity_quat(), {}});
    const auto baseline_spawned = baseline.stage_spawn({0, 40, 3, at(1500), m::identity_quat(), {}});
    expect(spawned && baseline_spawned && spawned.value() == baseline_spawned.value(), "rollback discovers the same new source ID");
    const auto before = retry.state_sha256();
    expect(!retry.step(FailingIncomeExecutor{}) && retry.state_sha256() == before,
        "failed income payment rolls back the preceding due scan's attachments and deadline");
    steps(retry, due + 2, executor); steps(baseline, due + 2, executor);
    expect(retry.state_sha256() == baseline.state_sha256(), "modifier retry matches an uninterrupted scan and payment");

    auto overflow = session(modifier_setup(), modifier_rules());
    expect(overflow.submit({{0, 1, 0}, {10}, t::BuyPayload{90}}).has_value(), "overflow fixture activates its modifier");
    steps(overflow, 4, executor);
    const auto grant = Fixed::from_raw(std::numeric_limits<std::int64_t>::max()
        - overflow.ledgers()[0].credits.raw() - q(1).raw());
    expect(overflow.submit({{4, 1, 1}, {}, t::CreditGrantPayload{grant}}).has_value(), "overflow fixture records a checked positive credit grant");
    const auto overflow_before = overflow.state_sha256();
    expect(!overflow.step(executor) && overflow.state_sha256() == overflow_before && overflow.completed_tick() == 4,
        "ledger addition overflow fails atomically instead of wrapping credits");
}
void test_determinism() {
    auto start = setup();
    start.units.back().owner = 1;
    auto content = rules();
    content.income = {{40, q(1), {}}};
    content.pads.capture.front().destroy_when_child_dies = true;
    content.pads.respawn = {{20, 7}};
    auto baseline = session(start, content);
    expect(baseline.submit({{0, 1, 0}, {3}, t::PadBuildPayload{30}}).has_value(), "record build command");
    expect(baseline.submit({{35, 1, 1}, {5}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "record mine and pad death");
    eawr::sim::InlineExecutor inline_executor;
    std::vector<std::string> hashes;
    while (baseline.completed_tick() < 55) {
        expect(baseline.step(inline_executor).has_value(), "baseline logical frame succeeds");
        hashes.push_back(baseline.state_sha256());
    }
    const auto bytes = t::write_replay(baseline.record());
    expect(static_cast<bool>(bytes), "pad replay writes");
    const auto replay = t::parse_replay(bytes.value());
    expect(static_cast<bool>(replay) && replay.value() == baseline.record(), "opcode 13 round trip");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto replayed = t::TacticalSession::from_replay(replay.value(), {}, durability(), {}, {}, {}, {}, {}, content).value();
        replayed.scramble_storage_for_testing();
        while (replayed.completed_tick() < 55) {
            expect(replayed.step(executor).has_value(), "worker logical frame succeeds");
            expect(replayed.state_sha256() == hashes[replayed.completed_tick() - 1], "per-frame 1/2/4/8 worker equality");
        }
        expect(replayed.canonical_state_bytes() == baseline.canonical_state_bytes(), "1/2/4/8 worker and storage equality");
    }
    auto no_pads = rules();
    no_pads.pads = {};
    const auto ordinary = session(start, no_pads).canonical_state_bytes();
    const std::array<std::uint8_t, 4> tag{'P', 'A', 'D', 'S'};
    expect(std::search(ordinary.begin(), ordinary.end(), tag.begin(), tag.end()) == ordinary.end(),
        "empty pads omit PADS state extension");
}
}
int main() {
    test_capture();
    test_ship_class_capture();
    test_capture_allocations();
    test_legacy_queue();
    test_captured_producer();
    test_construction();
    test_mine_income();
    test_modifier_reduction();
    test_modifier_service();
    test_modifier_determinism();
    test_modifier_removal();
    test_modifier_rollback();
    test_sale();
    test_sale_victory();
    test_respawn();
    test_bulk_child_death_work();
    test_respawn_rollback();
    test_determinism();
    return failures == 0 ? 0 : 1;
}
