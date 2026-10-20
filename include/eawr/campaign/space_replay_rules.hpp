#pragma once

#include "eawr/core/result.hpp"

namespace eawr::sim::tactical { struct TacticalSetup; struct TacticalContent; }
namespace eawr::skirmish { struct StartInputs; }

namespace eawr::campaign {

// Rebind the recorded campaign economy and victory policy using value-only
// replay/content inputs; presentation does not need a simulation owner.
[[nodiscard]] core::Result<void> bind_space_rules(const sim::tactical::TacticalSetup& setup,
    const skirmish::StartInputs& inputs, sim::tactical::TacticalContent& content);

} // namespace eawr::campaign
