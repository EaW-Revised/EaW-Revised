#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <vector>

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
namespace math = eawr::sim::math;
namespace tactical = eawr::sim::tactical;
int failures{};
void expect(bool ok, std::string_view message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
constexpr math::Fixed whole(std::int64_t n) { return math::Fixed::from_raw(n * math::Fixed::scale); }

class AllocationExecutor final : public eawr::sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& run) const override {
        for (std::size_t p = 0; p < count; ++p) run(p);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> execute_phase(std::string_view name, std::size_t count,
        const std::function<void(std::size_t)>& run) const override {
        const auto before = allocations.load();
        auto result = execute(count, run);
        const auto made = allocations.load() - before;
        if (name == "movement" || name == "targeting" || name == "unit-systems" || name == "collection-boxes") {
            expect(made == 0, "warmed movement, no-fire combat, collection and systems have no transient allocations");
        }
        return result;
    }
};

void matrix_budget() {
    tactical::TacticalSetup setup;
    setup.seed = 894;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    tactical::CombatTable combat;
    tactical::CombatProfile profile;
    profile.type_id = 1;
    profile.collision = tactical::CollisionBox{{whole(-10), whole(-7), whole(-3)}, {whole(10), whole(7), whole(3)}};
    combat.profiles = {profile};
    for (std::uint64_t id = 1; id <= 32; ++id) {
        tactical::UnitState unit;
        unit.entity_id = id;
        unit.type_id = 1;
        unit.owner = id % 2 == 0 ? 1 : 2;
        unit.position = {whole(static_cast<std::int64_t>(id) * 25), whole(3), whole(-2)};
        unit.rotation = math::normalize(math::Quat{math::Fixed::from_raw(static_cast<std::int64_t>(id) * 12345),
            math::Fixed::from_raw(345678), math::Fixed::from_raw(-456789), whole(1)}).value();
        setup.units.push_back(unit);
    }
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, combat);
        expect(static_cast<bool>(created), "budget fixture creates");
        if (!created) return;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::size_t tick = 0; tick < 4; ++tick) {
            const auto stepped = session.step(pool);
            expect(static_cast<bool>(stepped), "budget tick succeeds");
            if (!stepped) return;
            expect(session.tick_work().level_matrix_builds == setup.units.size(), "exactly one placement build per unit per tick");
            expect(session.tick_work().banked_matrix_builds == 0, "unbanked snapshots reuse the level matrix");
            for (const auto& instance : session.snapshot()->instances()) {
                const auto& source = setup.units[static_cast<std::size_t>(instance.entity_id - 1)];
                expect(instance.fixed_transform == math::to_matrix(source.rotation, source.position).value(),
                    "shared transform is bit-identical to the independent builder");
            }
            const auto hash = session.state_sha256();
            if (workers == 1) hashes.push_back(hash);
            else expect(hash == hashes[tick], "cached results agree at 1/2/4/8 workers");
        }
        if (workers == 1) expect(static_cast<bool>(session.step(AllocationExecutor{})), "warmed phase allocations are bounded");
    }
}

void locomotor_budget() {
    tactical::CraftProfile profile;
    profile.type_id = 1;
    profile.max_speed = whole(5);
    profile.min_speed = whole(1);
    profile.rate_of_turn = whole(6);
    profile.lift = whole(6);
    profile.thrust = math::Fixed::from_raw(math::Fixed::scale / 5);
    profile.roll_rate = whole(6);
    profile.bank_angle = whole(70);
    profile.strafe_distance = whole(200);
    tactical::CraftView self{1, {}, {}, &profile};
    tactical::CraftFrame frame;
    frame.self = &self;
    frame.leader = &self;
    frame.moving = true;
    frame.hold = {whole(1000), whole(100), {}};
    math::TrigCache cache;
    frame.trig_cache = &cache;
    const auto reference = tactical::step_craft(frame);
    expect(static_cast<bool>(reference), "locomotor warmup succeeds");
    const auto builds = cache.builds();
    const auto before = allocations.load();
    for (std::size_t repeat = 0; repeat < 64; ++repeat) {
        const auto step = tactical::step_craft(frame);
        if (!step || !reference || step.value().position != reference.value().position
            || step.value().rotation != reference.value().rotation || step.value().state != reference.value().state) ++failures;
    }
    const auto made = allocations.load() - before;
    expect(made == 0, "warmed moving locomotor allocates nothing");
    expect(cache.builds() == builds, "unchanged locomotor headings need no additional CORDIC builds");
    frame.trig_cache = nullptr;
    const auto uncached = tactical::step_craft(frame);
    expect(uncached && reference && uncached.value().state == reference.value().state
        && uncached.value().rotation == reference.value().rotation, "persistent and local trig caches agree");
    for (std::int64_t raw = -math::Fixed::scale; raw < math::Fixed::scale; raw += 11213) {
        const auto angle = math::Fixed::from_raw(raw);
        const auto value = cache.sample(angle);
        expect(value.sine == math::sin_turn(angle) && value.cosine == math::cos_turn(angle), "cached trig stays exact through eviction and wrap");
    }
}
}

int main() {
    matrix_budget();
    locomotor_budget();
    return failures == 0 ? 0 : 1;
}
