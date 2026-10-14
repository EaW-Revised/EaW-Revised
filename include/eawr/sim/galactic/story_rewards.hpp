#pragma once

#include "eawr/sim/galactic/story_adapters.hpp"

namespace eawr::sim { class PartitionExecutor; }
namespace eawr::sim::galactic { class GalacticSession; }
namespace eawr::sim::galactic::story {
// Owning live reward adapter; the active-mode owner calls it only after the
// plot's Lua trigger callback (WST-20/WGS-173). Other reward families keep their
// provider seams. Notice DTOs remain safe for presentation to include.
[[nodiscard]] RewardResult apply(GalacticSession&, const Reward&, const PartitionExecutor&);
} // namespace eawr::sim::galactic::story
