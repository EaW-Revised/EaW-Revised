#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace {
thread_local bool count_allocations = false;
std::atomic<std::size_t> selection_allocations{0};
} // namespace

void* operator new(const std::size_t size) {
    if (count_allocations) selection_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
// The replacement new uses malloc; GCC otherwise diagnoses its matching free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
namespace tactical = eawr::sim::tactical;
using eawr::sim::EntityId;
using eawr::sim::math::Fixed;

Fixed units(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

// Independent old scan, in the input's container/roster order (FD-06).
std::vector<EntityId> scan(const tactical::CraftView& self, const tactical::CombatCell cell,
    const tactical::TeamId team, const std::vector<tactical::ChaseCandidate>& rows, std::uint64_t& tests) {
    std::vector<EntityId> result;
    for (const auto& row : rows) {
        if (row.joined_cell != cell || row.team == team || row.craft == nullptr) continue;
        Fixed yaw;
        Fixed pitch;
        ++tests;
        const auto follows = tactical::in_follow_cone(self, row.craft->position, yaw, pitch);
        if (!follows) throw follows.error().message;
        if (follows.value()) result.push_back(row.craft->id);
    }
    return result;
}

bool differential(const std::size_t workers, const std::uint64_t seed) {
    constexpr std::size_t count = 32;
    std::vector<tactical::CraftProfile> profiles(count);
    std::vector<tactical::CraftView> views(count);
    std::vector<tactical::ChaseCandidate> rows;
    // Deliberately different entity and roster orders, repeated teams, negative cells.
    for (std::size_t i = 0; i < count; ++i) {
        profiles[i].attack_distance = units(i % 7 == 0 ? 16000 : i % 5 == 0 ? 0 : 500);
        views[i] = {static_cast<EntityId>(100 + i), {}, {}, &profiles[i]};
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = (i * 13) % count;
        rows.push_back({{static_cast<std::int32_t>(i / 16) - 1, -1}, static_cast<tactical::TeamId>(i % 3), &views[slot]});
    }
    std::map<EntityId, tactical::CraftState> old_states;
    std::map<EntityId, tactical::CraftState> new_states;
    std::uint64_t rng = seed;
    const auto draw = [&rng]() {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::int64_t>((rng >> 32) % 4001) - 2000;
    };
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::uint64_t old_tests = 0;
    std::uint64_t new_tests = 0;
    std::uint64_t paired = 0;
    std::array<tactical::ChaseQueryScratch, eawr::sim::tick_partition_count> scratch;
    std::vector<std::vector<EntityId>> new_candidates(count);
    for (std::uint64_t tick = 1; tick <= 640; ++tick) {
        for (auto& view : views) {
            // Include exact bucket boundaries and nonzero altitude/pitch.
            view.position = {units(draw()), units(draw()), units(draw() / 8)};
            view.state.yaw = units(draw() % 180);
            view.state.pitch = units(draw() % 90);
            for (auto* states : {&old_states, &new_states}) {
                auto& state = (*states)[view.id];
                if (tick >= state.chase_until) {
                    state.chase = 0;
                    state.chase_until = 0;
                }
            }
            view.state.chase_until = new_states.at(view.id).chase_until;
        }
        // Joined-cell changes, dead rows and returned rows exercise index rebuilds.
        rows[3].craft = tick % 17 == 0 ? nullptr : &views[(3 * 13) % count];
        rows[4].joined_cell.x = tick % 31 == 0 ? 2 : -1;
        const tactical::ChaseCandidateIndex index(rows);
        std::vector<std::vector<EntityId>> old_candidates(count);
        std::vector<std::uint64_t> work(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.craft == &views[i]; });
            if (row == rows.end() || old_states[views[i].id].chase_until != 0) continue;
            old_candidates[i] = scan(views[i], row->joined_cell, row->team, rows, old_tests);
        }
        const auto listed = executor.execute(eawr::sim::tick_partition_count, [&](const std::size_t partition) {
            const auto range = eawr::sim::partition_range(partition, count);
            // Warm this snapshot, then count only the repeated selection loop:
            // index/staging construction and executor scheduling are outside it.
            for (const bool measured : {false, true}) {
                count_allocations = measured;
                for (auto i = range.begin; i < range.end; ++i) {
                    new_candidates[i].clear();
                    const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.craft == &views[i]; });
                    if (row == rows.end() || new_states.at(views[i].id).chase_until != 0) continue;
                    for (const auto* candidate : index.query(views[i], row->joined_cell, row->team, scratch[partition])) {
                        Fixed yaw;
                        Fixed pitch;
                        if (measured) ++work[i];
                        const auto follows = tactical::in_follow_cone(views[i], candidate->position, yaw, pitch);
                        if (!follows) {
                            count_allocations = false;
                            throw follows.error().message;
                        }
                        if (follows.value()) new_candidates[i].push_back(candidate->id);
                    }
                }
                count_allocations = false;
            }
        });
        // The old scan lists busy craft too, but the commit can never take them.
        // Compare the ordered available lists, then run the unchanged old commit
        // against its complete lists to prove every tick's selected targets agree.
        auto old_available = old_candidates;
        for (auto& candidates : old_available) {
            std::erase_if(candidates, [&](const EntityId id) { return old_states.at(id).chase_until != 0; });
        }
        if (!listed || old_available != new_candidates) return false;
        for (const auto tests : work) new_tests += tests;
        for (const auto& [candidates, states] : {
                 std::pair{&old_candidates, &old_states}, std::pair{&new_candidates, &new_states}}) {
            for (std::size_t i = 0; i < count; ++i) {
                auto& self = (*states)[views[i].id];
                if (self.chase_until != 0) continue;
                for (const auto candidate : (*candidates)[i]) {
                    auto& chased = (*states)[candidate];
                    if (chased.chase_until != 0) continue;
                    self.chase = candidate;
                    self.chase_until = tick + tactical::chase_frames;
                    chased.chase = 0;
                    chased.chase_until = self.chase_until;
                    ++paired;
                    break;
                }
            }
        }
        for (const auto& [id, state] : old_states) {
            const auto& next = new_states.at(id);
            if (state.chase != next.chase || state.chase_until != next.chase_until) return false;
        }
    }
    std::cout << "seed " << seed << ", workers " << workers << ": cone tests " << old_tests << " -> " << new_tests << '\n';
    return paired > 0 && old_tests > 0 && new_tests * 2 < old_tests;
}

// Inclusive range endpoints on both sides of zero and at a tile boundary.
bool boundaries() {
    tactical::CraftProfile profile;
    profile.attack_distance = units(400);
    tactical::CraftView self{1, {units(-400), units(-400), {}}, {}, &profile};
    std::vector<tactical::CraftView> views{
        {9, {units(0), units(-400), {}}, {}, &profile},
        {3, {Fixed::from_raw(1), units(-400), {}}, {}, &profile},
        {7, {units(-800), units(-400), {}}, {}, &profile},
        {2, {units(-400), units(0), {}}, {}, &profile}};
    std::vector<tactical::ChaseCandidate> rows;
    for (const auto& view : views) rows.push_back({{-2, -1}, 2, &view});
    const tactical::ChaseCandidateIndex index(rows);
    tactical::ChaseQueryScratch scratch;
    const auto found = index.query(self, {-2, -1}, 1, scratch);
    const bool endpoints = found.size() == 3 && found[0]->id == 9 && found[1]->id == 7 && found[2]->id == 2
        && index.query(self, {-2, -1}, 2, scratch).empty() && index.query(self, {0, 0}, 1, scratch).empty();
    // Enough occupied tiles to take the neighbour-bucket branch instead of fallback.
    for (std::int64_t x = -8; x <= 8; ++x) {
        for (std::int64_t y = -8; y <= 8; ++y) {
            views.push_back({static_cast<EntityId>(100 + (x + 8) * 17 + y + 8),
                {units(x * 400), units(y * 400), {}}, {}, &profile});
        }
    }
    rows.clear();
    for (const auto& view : views) rows.push_back({{-2, -1}, 2, &view});
    const tactical::ChaseCandidateIndex neighbours(rows);
    std::uint64_t tests = 0;
    const auto expected = scan(self, {-2, -1}, 1, rows, tests);
    std::vector<EntityId> actual;
    for (const auto* view : neighbours.query(self, {-2, -1}, 1, scratch)) {
        Fixed yaw;
        Fixed pitch;
        const auto follows = tactical::in_follow_cone(self, view->position, yaw, pitch);
        if (!follows) return false;
        if (follows.value()) actual.push_back(view->id);
    }
    return endpoints && actual == expected;
}
} // namespace

int main() {
    if (!boundaries()) return 1;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        for (const std::uint64_t seed : {893U, 457U, 974U}) {
            if (!differential(workers, seed)) return 1;
        }
    }
    const auto allocations = selection_allocations.load(std::memory_order_relaxed);
    std::cout << "warmed selection-loop allocations: " << allocations << '\n';
    if (allocations != 0) return 1;
    std::cout << "chase candidate differential and work budgets passed\n";
    return 0;
}
