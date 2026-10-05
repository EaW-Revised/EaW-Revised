#include "eawr/sim/tactical/pads.hpp"

#include <algorithm>

namespace eawr::sim::tactical {
namespace {
template <typename T>
const T* find_type(const std::vector<T>& entries, const TypeId type) noexcept {
    const auto found = std::lower_bound(entries.begin(), entries.end(), type,
        [](const T& entry, const TypeId key) { return entry.type < key; });
    return found != entries.end() && found->type == type ? &*found : nullptr;
}
const Player* player_of(const std::span<const Player> players, const PlayerId id) {
    const auto found = std::lower_bound(players.begin(), players.end(), id,
        [](const Player& entry, const PlayerId key) { return entry.player_id < key; });
    return found != players.end() && found->player_id == id ? &*found : nullptr;
}
bool allied(const std::span<const Player> players, const PlayerId a, const PlayerId b) {
    const auto* left = player_of(players, a);
    const auto* right = player_of(players, b);
    return left != nullptr && right != nullptr && left->team_id == right->team_id;
}
math::Fixed step_of(const math::Fixed seconds) {
    // WHZ-40/45: service cadence 4, logical FPS 30. Zero duration advances immediately.
    if (seconds.raw() <= 0) return math::Fixed::from_raw(math::Fixed::scale);
    // Round positive progress upward by at most one Q24 unit: authored 8/10-second captures
    // reach their expected service instead of slipping four frames on accumulated truncation.
    constexpr auto numerator = std::int64_t{4} * math::Fixed::scale * math::Fixed::scale;
    const auto denominator = seconds.raw() * 30;
    return math::Fixed::from_raw(numerator / denominator + (numerator % denominator != 0 ? 1 : 0));
}
bool influences(const PadRules& rules, const TypeId type) {
    const auto* entry = rules.candidate(type);
    return entry == nullptr || (entry->collidable && entry->influences);
}
}

const CaptureProfile* PadRules::point(const TypeId type) const noexcept { return find_type(capture, type); }
const ConstructionProfile* PadRules::child(const TypeId type) const noexcept { return find_type(construction, type); }
const CaptureInfluence* PadRules::candidate(const TypeId type) const noexcept { return find_type(influence, type); }
const PadRules::RespawnProfile* PadRules::replacement(const TypeId type) const noexcept { return find_type(respawn, type); }

std::optional<RespawnState> respawn_after_death(const UnitState& unit, const PadRules& rules,
    const bool death_clone) noexcept {
    const auto* profile = rules.replacement(unit.type_id);
    if (death_clone || profile == nullptr) return std::nullopt;
    return RespawnState{unit.type_id, rules.point(unit.type_id) != nullptr ? rules.neutral : unit.owner,
        unit.position, unit.rotation};
}

math::Fixed pad_cooldown_progress(const PadState& state, const std::uint64_t frame) noexcept {
    if (state.cooldown_until <= state.cooldown_start || frame >= state.cooldown_until) {
        return math::Fixed::from_raw(math::Fixed::scale);
    }
    if (frame <= state.cooldown_start) return {};
    return math::Fixed::from_raw(static_cast<std::int64_t>((frame - state.cooldown_start)
        * math::Fixed::scale / (state.cooldown_until - state.cooldown_start)));
}

PlayerId service_capture(const CaptureProfile& profile, PadState& state, const PlayerId owner,
    const PlayerId neutral, const std::span<const Player> players,
    const std::span<const CaptureCandidate> candidates, const PadRules& rules,
    const EntityId point, const math::Vec3& position, const math::Fixed adjustment) {
    if (state.under_construction != invalid_entity_id || state.constructed != invalid_entity_id) return owner;
    PlayerId wanted = neutral;
    bool present = false;
    for (const auto& candidate : candidates) {
        const auto& body = candidate.body;
        const auto* player = player_of(players, body.owner);
        if (!candidate.eligible || body.entity_id == point || !influences(rules, candidate.type)
            || player == nullptr
            || !std::binary_search(profile.affiliation.begin(), profile.affiliation.end(), player->faction_id)
            || !within_range(position, body.position, profile.radius, RangeMetric::spatial)) continue;
        if (!present) {
            wanted = owner == neutral ? body.owner : owner;
            present = true;
        }
        if (!allied(players, wanted, body.owner)) {
            wanted = neutral;
            break;
        }
    }
    if (!present && profile.ownership_sticks) wanted = owner;
    if (wanted == owner) {
        state.progress = math::Fixed::from_raw(std::max<std::int64_t>(0,
            state.progress.raw() - step_of(profile.transition_seconds).raw()));
        if (state.progress.raw() == 0) state.target = owner;
        return owner;
    }
    state.target = wanted;
    const auto modifier = math::Fixed::from_raw(math::Fixed::scale + std::max<std::int64_t>(0, adjustment.raw()));
    const auto seconds = math::multiply(profile.transition_seconds, modifier).value();
    state.progress = math::Fixed::from_raw(std::min(math::Fixed::scale,
        state.progress.raw() + step_of(seconds).raw()));
    if (state.progress.raw() < math::Fixed::scale) return owner;
    state.progress = {};
    return wanted;
}

bool pad_construction_allowed(const CaptureProfile& profile, const PadState& state,
    const PlayerId builder, const std::span<const Player> players,
    const std::span<const CaptureCandidate> candidates, const PadRules& rules,
    const EntityId point, const math::Vec3& position) {
    if (state.under_construction != invalid_entity_id || state.constructed != invalid_entity_id) return false;
    for (const auto& candidate : candidates) {
        const auto* entry = rules.candidate(candidate.type);
        if (!candidate.eligible || candidate.body.entity_id == point || !influences(rules, candidate.type)
            || (entry != nullptr && !entry->blocks_construction) || allied(players, builder, candidate.body.owner)) continue;
        if (strictly_within_range(position, candidate.body.position, profile.radius, RangeMetric::spatial)) return false;
    }
    return true;
}

ConstructionState begin_construction(const EntityId parent, const PlayerId builder,
    const std::uint64_t frame, const std::uint32_t seconds, const math::Fixed maximum) {
    const auto frames = std::uint64_t{seconds} * 30;
    return {parent, builder, frame + frames,
        math::Fixed::from_raw(frames == 0 ? 0 : maximum.raw() / static_cast<std::int64_t>(frames)), frame};
}

math::Fixed service_construction(const math::Fixed hull, const math::Fixed maximum,
    const ConstructionState& state) noexcept {
    return math::Fixed::from_raw(std::min(maximum.raw(), hull.raw() + state.increment.raw()));
}
} // namespace eawr::sim::tactical
