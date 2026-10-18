#pragma once

#include "eawr/campaign/conflict.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/content.hpp"

namespace eawr::campaign {

// The admission/scene rule owners supply marker choices and the already split force record.
// Reserves keep their IDs without being instantiated; every other force needs one placement.
struct ForcePlacement {
    sim::EntityId force{};
    std::uint32_t marker{};
    bool garrison_enabled{true};
    std::uint32_t creation_order{}; // zero retains persistent-ID order for fixture callers
};
struct ResolvedConflictEntry {
    ConflictRecord conflict;
    sim::tactical::TacticalContent content;
};
struct ConflictEntry {
    ConflictRecord conflict;
    sim::tactical::TacticalContent content;
    skirmish::SkirmishStart start;
    std::vector<sim::tactical::CampaignUpgrade> scene_upgrades; // pre-force ledger inputs, WGT-113
};
// first_free is the galaxy allocator after reserving the conflict and all persistent objects.
// The returned setup advances it past battle-only map objects and squadron craft (ADR-014).
[[nodiscard]] core::Result<ConflictEntry> build_conflict_entry(const ConflictRecord& record,
    const skirmish::Fixture& fixture, const skirmish::StartInputs& inputs,
    std::span<const ForcePlacement> placements, sim::EntityId first_free, std::uint64_t session_seed);

// Also used by the ordinary tactical replay runner: header tag 6 plus the existing map/lobby
// metadata identify a selected-force child. No skirmish purchase/default-force economy is inferred.
[[nodiscard]] core::Result<sim::tactical::TacticalContent> conflict_entry_content(
    const sim::tactical::TacticalSetup& setup, const skirmish::StartInputs& inputs);
[[nodiscard]] core::Result<sim::tactical::TacticalContent> conflict_entry_content(
    const sim::tactical::TacticalSetup& setup, const skirmish::StartInputs& inputs,
    const units::SpawnPolicy& spawn_policy);

} // namespace eawr::campaign
