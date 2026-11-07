#pragma once

// Scripts beside a live session's world (#79): the factory that wraps the tick-zero world in a
// scripted session (script commands become next-tick replay input). Kept out of
// live_session.hpp so presentation, which includes that header, never names a session type.

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace eawr::platform {

// Builds the tick-zero world of a saved battle's log with the live session's content tables.
using LiveWorldFactory =
    std::function<core::Result<sim::tactical::TacticalSession>(const sim::tactical::TacticalReplay&)>;

struct LiveScripts {
    std::function<core::Result<script::authoritative::ScriptedTacticalSession>(sim::tactical::TacticalSession)> wrap;
    // Skirmish battle saves (docs/checkpoint-format.md "Skirmish battle saves"). `save` writes the
    // running scripted battle between two ticks; `restore` loads one into a scripted session at the
    // saved tick, for the battle `setup` describes (a save of another battle is refused). Empty: the
    // battle cannot be saved or loaded.
    std::function<core::Result<std::vector<std::uint8_t>>(script::authoritative::ScriptedTacticalSession&)> save;
    std::function<core::Result<script::authoritative::ScriptedTacticalSession>(std::span<const std::uint8_t>,
        const sim::tactical::TacticalSetup&, const LiveWorldFactory&, const sim::PartitionExecutor&)> restore;
};

} // namespace eawr::platform
