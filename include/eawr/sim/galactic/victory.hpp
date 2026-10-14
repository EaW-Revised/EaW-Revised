#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/world.hpp"

#include <optional>
#include <string>
#include <vector>

namespace eawr::sim::galactic {
inline constexpr std::uint32_t victory_state_encoding_version = 2;

struct VictoryAuthoring {
    std::vector<VictoryKind> human, ai;
    VictoryParameters human_parameters, ai_parameters;
};

[[nodiscard]] VictoryState install_victory(const VictoryAuthoring&, const VictoryContext&, PlayerId starting);
[[nodiscard]] core::Result<bool> victory_predicate(const GalacticVictoryCondition&, const VictoryContext&);
[[nodiscard]] bool register_victory(VictoryState&, VictoryKind, PlayerId, std::uint32_t delay,
    std::uint64_t frame);
[[nodiscard]] core::Result<bool> test_victory(VictoryState&, VictoryTest, const VictoryContext&,
    const PartitionExecutor&, std::uint64_t frame);
[[nodiscard]] core::Result<bool> release_victory_wait(VictoryState&, bool evaluate,
    const VictoryContext&, const PartitionExecutor&, std::uint64_t frame);
void service_victory(VictoryState&, std::uint64_t frame, bool retry_active);
[[nodiscard]] bool pending_victory(const VictoryState&, GalacticVictoryCondition& output);
// WGS-28/29: a trusted return producer; reconciliation of losses stays with its owner.
struct VictorySuperweaponReturn {
    PlayerId weapon_owner{}, winner{}, loser{};
    bool weapon_alive{}, weapon_deletion_pending{}, killer_survivor{};
};
void victory_superweapon_return(VictoryState&, const VictorySuperweaponReturn&,
    const VictoryContext&, std::uint64_t frame);
// WGS-79: the strategic/tactical action owner supplies current-controller landed contents.
[[nodiscard]] bool victory_leader_planet_destruction(VictoryState&, PlayerId actor, PlayerId controller,
    std::span<const std::string> landed_units, bool tactical, std::uint64_t frame);
// WGS-82: immutable final result projection; dialogs/movies/exit remain presentation.
enum class CampaignResultRoute { score, multiplayer_quit };
struct CampaignResult {
    GalacticVictoryCondition condition;
    bool local_won{}; // score uses exact winner, WGS-82 (distinct from allied all-planets)
    CampaignResultRoute route{CampaignResultRoute::score};
};
[[nodiscard]] std::optional<CampaignResult> campaign_result(const VictoryState&, PlayerId local,
    bool dialog_active, bool movie_playing);
[[nodiscard]] core::Result<void> validate_victory(const VictoryState&);
namespace detail {
void encode_victory(std::vector<std::uint8_t>&, const VictoryState&);
[[nodiscard]] std::optional<VictoryState> decode_victory(std::span<const std::uint8_t>);
}
} // namespace eawr::sim::galactic
