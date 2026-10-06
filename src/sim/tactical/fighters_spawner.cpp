#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "fighters_algorithms.hpp"

namespace eawr::sim::tactical {
using namespace fighters_detail;

SpawnerState initial_spawner(const std::uint64_t seed, const std::uint64_t frame, const EntityId unit) {
    CombatRandom draw(seed, frame, unit, service_slot);
    SpawnerState state;
    state.next_service_frame = frame + draw.uniform(0, static_cast<std::uint32_t>(service_interval - 1));
    return state;
}

std::optional<SpawnDecision> service_spawner(const SpawnerProfile& profile, SpawnerState& state, const std::uint64_t seed,
    const std::uint64_t frame, const EntityId unit, const std::vector<bool>& bay_intact, const bool suspended) {
    if (frame != state.next_service_frame) return std::nullopt;
    state.next_service_frame = frame + service_interval;
    if (!state.ready) {
        // FL-02: the first service builds the entries and may launch at once.
        state.ready = true;
        state.entries.clear();
        for (const auto& entry : profile.entries) {
            const auto remaining = entry.reserve < 0 ? unlimited_reserve : entry.reserve + entry.starting;
            state.entries.push_back(SpawnEntryState{0, remaining});
        }
        state.next_spawn_frame = frame;
    }
    // FL-12: arrival suspension gates launch, without pausing the hangar's service cadence.
    if (suspended || state.next_spawn_frame > frame) return std::nullopt;
    // FL-03: a space spawner needs a standing fighter bay.
    std::vector<std::uint32_t> bays;
    for (std::uint32_t index = 0; index < profile.bays.size(); ++index) {
        if (index < bay_intact.size() && bay_intact[index]) bays.push_back(index);
    }
    if (bays.empty() || state.entries.empty()) return std::nullopt;
    const auto eligible = [&](const std::size_t index) {
        const auto& entry = state.entries[index];
        return entry.alive < profile.entries[index].starting
            && (entry.remaining > 0 || entry.remaining == unlimited_reserve);
    };
    if (std::none_of(state.entries.begin(), state.entries.end(),
            [&](const SpawnEntryState& entry) { return eligible(static_cast<std::size_t>(&entry - state.entries.data())); })) {
        return std::nullopt;
    }
    // FL-04, FL-05: a random start, then the first eligible entry in cyclic order.
    const auto count = static_cast<std::uint32_t>(state.entries.size());
    CombatRandom entry_draw(seed, frame, unit, entry_slot);
    const auto start = entry_draw.uniform(0, count - 1);
    std::optional<SpawnDecision> decision;
    for (std::uint32_t step = 0; step < count; ++step) {
        const auto index = (start + step) % count;
        if (!eligible(index)) continue;
        auto& entry = state.entries[index];
        if (entry.remaining > 0) --entry.remaining;
        ++entry.alive;
        CombatRandom bay_draw(seed, frame, unit, bay_slot);
        decision = SpawnDecision{index, bays[bay_draw.uniform(0, static_cast<std::uint32_t>(bays.size() - 1))]};
        break;
    }
    state.next_spawn_frame = frame + profile.delay_frames;
    return decision;
}

std::optional<std::uint32_t> free_garrison_bay(const SpawnerProfile& profile, const std::uint64_t seed,
    const std::uint64_t frame, const EntityId unit, const std::vector<bool>& bay_intact) {
    std::vector<std::uint32_t> bays;
    for (std::uint32_t index = 0; index < profile.bays.size(); ++index)
        if (index < bay_intact.size() && bay_intact[index]) bays.push_back(index);
    if (bays.empty()) return std::nullopt;
    CombatRandom draw(seed, frame, unit, bay_slot);
    return bays[draw.uniform(0, static_cast<std::uint32_t>(bays.size() - 1))];
}

void squadron_lost(const SpawnerProfile& profile, SpawnerState& state, const std::uint32_t entry, const std::uint64_t frame) {
    // FL-08: the entries up to the lost one were all full: the next launch waits a full delay.
    bool full = true;
    for (std::uint32_t index = 0; index < state.entries.size() && index < profile.entries.size(); ++index) {
        auto& current = state.entries[index];
        if (current.alive < profile.entries[index].starting) full = false;
        if (index == entry) {
            if (current.alive > 0) --current.alive;
            break;
        }
    }
    if (full) state.next_spawn_frame = frame + profile.delay_frames;
}

bool squadron_move_arrived(const math::Vec3& origin, const math::Vec3& destination, const math::Vec3& leader) noexcept {
    // The sign of (leader - destination) . (destination - origin) in the plane, from the sums of
    // the positive and the negative products (raw differences fit 64 bits for accepted positions).
    math::detail::UInt192 positive{};
    math::detail::UInt192 negative{};
    const auto add_product = [&](const Fixed a0, const Fixed a1, const Fixed b0, const Fixed b1) {
        const auto a = static_cast<std::int64_t>(static_cast<std::uint64_t>(a0.raw()) - static_cast<std::uint64_t>(a1.raw()));
        const auto b = static_cast<std::int64_t>(static_cast<std::uint64_t>(b0.raw()) - static_cast<std::uint64_t>(b1.raw()));
        if (a == 0 || b == 0) return;
        const auto magnitude = [](const std::int64_t value) {
            return value < 0 ? std::uint64_t{} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        };
        auto& sum = (a < 0) == (b < 0) ? positive : negative;
        static_cast<void>(math::detail::add_magnitude(sum, math::detail::multiply_u64(magnitude(a), magnitude(b))));
    };
    add_product(leader.x, destination.x, destination.x, origin.x);
    add_product(leader.y, destination.y, destination.y, origin.y);
    return math::detail::compare(positive, negative) >= 0;
}


} // namespace eawr::sim::tactical
