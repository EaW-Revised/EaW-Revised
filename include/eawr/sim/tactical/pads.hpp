#pragma once

#include "eawr/sim/tactical/space.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::sim::tactical {

// WBP-01/02/04: content, separate from placement and from producer queues.
struct CaptureProfile {
    TypeId type{};
    math::Fixed radius{};
    math::Fixed transition_seconds{};
    std::vector<FactionId> affiliation;
    bool ownership_sticks{};
    bool community_property{};
    bool build_pad{};
    math::Vec3 attachment{}; // scaled bind position, or zero for pad-transform fallback
    bool destroy_when_child_dies{};
    std::uint32_t rebuild_frames{};
    friend bool operator==(const CaptureProfile&, const CaptureProfile&) = default;
};

struct ConstructionProfile {
    TypeId type{};
    TypeId constructed{};
    math::Fixed price{};
    std::uint32_t seconds{}; // WBP-15: whole seconds, no queue multiplier
    std::uint32_t ai_seconds{};
    bool child_persists{true};
    friend bool operator==(const ConstructionProfile&, const ConstructionProfile&) = default;
};

struct CaptureInfluence {
    TypeId type{};
    bool influences{true}; // WBP-02: absent XML flag is true, including fighters
    bool blocks_construction{true}; // WBP-07: collidable, excluding shield/dummy objects
    bool collidable{true}; // WBP-50: member of the live projectile-collidable query domain
    friend bool operator==(const CaptureInfluence&, const CaptureInfluence&) = default;
};

struct PadRules {
    PlayerId neutral{}; // declared neutral player, never an undeclared owner zero
    std::vector<CaptureProfile> capture;
    std::vector<ConstructionProfile> construction;
    std::vector<CaptureInfluence> influence;
    // WHZ-52: only ordinary live objects enter this typed deferred-creation schedule.
    struct RespawnProfile {
        TypeId type{};
        std::uint32_t frames{};
        friend bool operator==(const RespawnProfile&, const RespawnProfile&) = default;
    };
    std::vector<RespawnProfile> respawn;
    [[nodiscard]] const CaptureProfile* point(TypeId type) const noexcept;
    [[nodiscard]] const ConstructionProfile* child(TypeId type) const noexcept;
    [[nodiscard]] const CaptureInfluence* candidate(TypeId type) const noexcept;
    [[nodiscard]] const RespawnProfile* replacement(TypeId type) const noexcept;
    friend bool operator==(const PadRules&, const PadRules&) = default;
};

// WHZ-44/45 and WBP-05: target changes retain progress; child and pad owners differ.
struct PadState {
    PlayerId target{};
    math::Fixed progress{};
    EntityId under_construction{};
    EntityId constructed{};
    std::uint64_t cooldown_until{};
    std::uint64_t cooldown_start{};
    bool contents_locked{}; // WBP-30: interface to container operations
    friend constexpr bool operator==(const PadState&, const PadState&) noexcept = default;
};

struct ConstructionState {
    EntityId parent{};
    PlayerId builder{};
    std::uint64_t finish_frame{};
    math::Fixed increment{};
    std::uint64_t start_frame{};
    friend constexpr bool operator==(const ConstructionState&, const ConstructionState&) noexcept = default;
};

struct PadView {
    EntityId entity{};
    PadState state;
    // WBP-17/37: presentation timing; canonical snapshot bytes remain the PadState encoding.
    std::optional<ConstructionState> construction{};
    friend constexpr bool operator==(const PadView&, const PadView&) noexcept = default;
};

struct CaptureCandidate {
    SpaceBody body;
    TypeId type{};
    bool eligible{true}; // caller excludes dead, deleting and limbo objects
};

// WHZ-40..46: one due service, candidates in authoritative object-query order.
// `adjustment` is the sum of applicable positive battlefield capture-time adjustments.
[[nodiscard]] PlayerId service_capture(const CaptureProfile& profile, PadState& state,
    PlayerId owner, PlayerId neutral, std::span<const Player> players,
    std::span<const CaptureCandidate> candidates, const PadRules& rules,
    EntityId point, const math::Vec3& position, math::Fixed adjustment = {});

// WBP-07 uses a strict boundary, unlike inclusive capture. Called when opening a menu.
[[nodiscard]] bool pad_construction_allowed(const CaptureProfile& profile, const PadState& state,
    PlayerId builder, std::span<const Player> players, std::span<const CaptureCandidate> candidates,
    const PadRules& rules, EntityId point, const math::Vec3& position);

// WBP-14..16: start at 0.01 absolute hull and heal once per logical frame.
[[nodiscard]] ConstructionState begin_construction(EntityId parent, PlayerId builder,
    std::uint64_t frame, std::uint32_t seconds, math::Fixed maximum);
[[nodiscard]] math::Fixed service_construction(math::Fixed hull, math::Fixed maximum,
    const ConstructionState& state) noexcept;
inline constexpr math::Fixed initial_construction_hull = math::Fixed::from_raw(math::Fixed::scale / 100);

struct RespawnState {
    TypeId type{};
    PlayerId owner{};
    math::Vec3 position{};
    math::Quat rotation{};
    friend bool operator==(const RespawnState&, const RespawnState&) = default;
};
// WHZ-52: death clones never enter the ordinary destruction schedule.
[[nodiscard]] std::optional<RespawnState> respawn_after_death(const UnitState& unit,
    const PadRules& rules, bool death_clone = false) noexcept;
[[nodiscard]] math::Fixed pad_cooldown_progress(const PadState& state, std::uint64_t frame) noexcept;

// WBP-30: a request checks exact ownership and single-step mode; execution also
// checks the current parent lock. A missing parent permits sale with no refund.
[[nodiscard]] constexpr bool pad_sale_permission(PlayerId issuer, PlayerId owner,
    bool has_sale_behavior, const PadState* parent = nullptr, bool single_step = false) noexcept {
    return issuer == owner && has_sale_behavior && !single_step
        && (parent == nullptr || !parent->contents_locked);
}

} // namespace eawr::sim::tactical
