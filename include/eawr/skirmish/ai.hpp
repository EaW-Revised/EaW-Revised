#pragma once

// The FoC tactical AI of the M2 skirmish (#79): the AI host's inputs from the start, its
// faction flags and the unit tables (docs/behaviour/foc-tactical-ai.md "#79 host").

#include "eawr/core/result.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/vfs/vfs.hpp"

#include <map>
#include <string>

namespace eawr::skirmish {

// Every player of the start; the lobby players that are not human run the AI (SK-10, SK-40).
// Perception inputs are the GC context of SK-41 to SK-45; `seed` is the setup's.
[[nodiscard]] script::foc::AiSetup ai_setup(
    const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables);

// AI-16: freeze the mounted AI XML directories, constants and converter enum.
// Without XML or map bounds, only the explicitly configured freestore runs.
[[nodiscard]] core::Result<void> enable_goal_system(const vfs::Vfs& files, script::foc::AiSetup& setup);

// ML-34: freeze the bounded mounted logical script inventory, keeping winners
// and source/compiled candidates. With `plans`, require the plan library roots.
[[nodiscard]] core::Result<std::map<std::string, std::string>> ai_modules(
    const vfs::Vfs& files, const script::foc::AiSetup& setup, bool plans = false);

// Validate required session roots and retain the previously frozen inventory.
[[nodiscard]] core::Result<std::map<std::string, std::string>> select_ai_modules(
    const std::map<std::string, std::string>& cached, const script::foc::AiSetup& setup, bool plans = false);

} // namespace eawr::skirmish
