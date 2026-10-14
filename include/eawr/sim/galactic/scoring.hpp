#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

// GHUD-S61 / WGP-98..103: the campaign scoring ledger. Raw counters only: the authored scoring
// module (Game_Scoring_Script_Name) turns them into efficiencies and a rank title at the
// campaign end, so a mod that replaces the module keeps its own scoring. Every key is
// ordered, every count is an integer, and every producer is an ordered commit (ADR-009), so
// the section is equal for 1, 2, 4 and 8 workers.
namespace eawr::sim::galactic {

struct CampaignScoreLedger {
    // WGP-99: one completed galactic production, attributed to the planet's CURRENT owner.
    struct BuildKey {
        PlayerId owner{};
        PlanetId planet{};
        TypeId type{};
        friend constexpr auto operator<=>(const BuildKey&, const BuildKey&) noexcept = default;
    };
    // WGP-98: one galactic death. `killer` zero is the missing-id sentinel (the neutral
    // player is credited at the scoring boundary, WGP-104).
    struct DeathKey {
        PlayerId owner{};
        PlayerId killer{};
        TypeId type{};
        friend constexpr auto operator<=>(const DeathKey&, const DeathKey&) noexcept = default;
    };
    // WGP-101: one planet faction change; the new owner acquires and the old owner loses.
    struct ChangeKey {
        PlanetId planet{};
        PlayerId old_owner{}, new_owner{};
        friend constexpr auto operator<=>(const ChangeKey&, const ChangeKey&) noexcept = default;
    };

    std::map<BuildKey, std::uint64_t> builds;
    std::map<DeathKey, std::uint64_t> deaths;
    std::map<ChangeKey, std::uint64_t> changes;
    std::set<PlayerId> quits; // WGP-102: the quit table; its title score is forced to zero

    [[nodiscard]] bool empty() const noexcept {
        return builds.empty() && deaths.empty() && changes.empty() && quits.empty();
    }
    friend bool operator==(const CampaignScoreLedger&, const CampaignScoreLedger&) = default;
};

// A final blow supplied by a battle-return owner: persistent identity -> killing player.
struct BattleKiller {
    EntityId id{};
    PlayerId killer{};
    friend constexpr bool operator==(const BattleKiller&, const BattleKiller&) noexcept = default;
};

namespace detail {
void encode_score_ledger(std::vector<std::uint8_t>&, const CampaignScoreLedger&);
[[nodiscard]] std::optional<CampaignScoreLedger> decode_score_ledger(std::span<const std::uint8_t>);
} // namespace detail

} // namespace eawr::sim::galactic
