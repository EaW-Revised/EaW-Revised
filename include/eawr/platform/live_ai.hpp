#pragma once

// The FoC tactical AI of a live M2 battle (#79) as LiveSession::Options::scripts. Opaque: the
// view that starts the session names no session or script type (UI-07).

#include "eawr/core/result.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <memory>
#include <vector>

namespace eawr::platform {

struct LiveAi {
    std::shared_ptr<const LiveScripts> scripts;
    std::vector<sim::tactical::PlayerId> players; // the players the AI commands
};

// The retail freestore of each non-human lobby player (skirmish::ai_setup), its Lua files read
// from the mounted FoC view (skirmish::ai_modules).
[[nodiscard]] core::Result<LiveAi> live_ai(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
    const units::UnitTables& tables, const vfs::Vfs& files);

} // namespace eawr::platform
