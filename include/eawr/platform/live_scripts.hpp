#pragma once

// Scripts beside a live session's world (#79): the factory that wraps the tick-zero world in a
// scripted session (script commands become next-tick replay input). Kept out of
// live_session.hpp so presentation, which includes that header, never names a session type.

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <functional>

namespace eawr::platform {

struct LiveScripts {
    std::function<core::Result<script::authoritative::ScriptedTacticalSession>(sim::tactical::TacticalSession)> wrap;
};

} // namespace eawr::platform
