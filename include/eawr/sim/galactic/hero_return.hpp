#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <functional>

namespace eawr::sim::galactic {
struct HeroCompanion {
    EntityId id{};
    bool dead{}, deletion_pending{};
};
enum class HeroParent : std::uint8_t { none, team, transport, nested_squadron };
struct HeroDefeat {
    EntityId id{};
    TypeId type{}, respawn_type{};
    PlayerId owner{}, killer{}, parent_killer{};
    PlanetId planet{}, current_system{};
    bool named{}, multiplayer_tactical{}, bypass_companions{}, respawn_whole_team{};
    TypeId structure{};
    std::uint32_t owned_structures{}, owned_instances{};
    HeroParent parent{HeroParent::none};
    std::vector<HeroCompanion> companions;
    math::Fixed seconds{}, default_seconds{};
    // WGCH-95: ability removal may also destroy an immediate transport and its
    // real carried identities; this list never schedules companion returns.
    std::vector<EntityId> removed_objects;
};
struct HeroReturnRecord {
    EntityId defeated{};
    TypeId type{}, structure{};
    PlayerId owner{}, killer{};
    PlanetId planet{};
    std::uint64_t deadline{};
    std::uint64_t start_frame{}; // WGUI-89: the actual scheduling frame, retained across save/restore.
    friend bool operator==(const HeroReturnRecord&, const HeroReturnRecord&) = default;
};
// Immutable presentation facts; no defeated identity, killer or backing-structure internals.
struct HeroReturnView {
    TypeId type{};
    PlayerId owner{};
    PlanetId planet{};
    std::uint64_t start_frame{}, deadline{};
    friend bool operator==(const HeroReturnView&, const HeroReturnView&) = default;
};
[[nodiscard]] std::vector<HeroReturnView> local_hero_returns(
    std::span<const HeroReturnRecord> records, PlayerId local);
struct HeroSchedule {
    bool scheduled{};
    std::vector<EntityId> destroyed_companions;
};
// WGM-78..80: records preserve insertion order. Named duplicates do not reschedule.
[[nodiscard]] core::Result<HeroSchedule> schedule_hero_return(std::vector<HeroReturnRecord>& records,
    const HeroDefeat& defeat, std::uint64_t galactic_frame, std::span<const Planet> planets);
// GFS-10/GFC-25/GFSC-17: due service belongs to the admitted scheduled-creation tail,
// after current planet control and before fiscal reads. A failed placement retains its deadline.
using HeroPlacement = std::function<core::Result<bool>(const HeroReturnRecord&, PlanetId)>;
// WGM-110: live census for the recorded owner and selected hero/structure types.
// The provider must be rebound after restore, like the production placement provider.
struct HeroStructureState {
    std::uint64_t owned_structures{}, owned_instances{};
    std::vector<PlanetId> planets; // currently owned sites with an owned backing structure
};
using HeroStructureResolver = std::function<core::Result<HeroStructureState>(const HeroReturnRecord&)>;
[[nodiscard]] core::Result<void> service_hero_returns(std::vector<HeroReturnRecord>& records,
    std::uint64_t frame, bool gameplay_paused, std::span<const Planet> planets,
    const HeroPlacement& placement, const HeroStructureResolver& structures = {});
[[nodiscard]] std::vector<std::uint8_t> encode_hero_returns(std::span<const HeroReturnRecord> records);
[[nodiscard]] core::Result<std::vector<HeroReturnRecord>> decode_hero_returns(std::span<const std::uint8_t> bytes);
} // namespace eawr::sim::galactic
