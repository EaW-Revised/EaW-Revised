#pragma once

// The FoC tactical AI of the M2 skirmish (#79): the AI host's inputs from the start, its
// faction flags and the unit tables (docs/behaviour/foc-tactical-ai.md "#79 host").

#include "eawr/core/result.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/vfs/vfs.hpp"

#include <map>
#include <memory>
#include <string>

namespace eawr::skirmish {

// Every player of the start; the lobby players that are not human run the AI (SK-10, SK-40).
// Perception inputs are the GC context of SK-41 to SK-45; `seed` is the setup's.
[[nodiscard]] script::foc::AiSetup ai_setup(
    const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables);

// WSS-35, AI-11: the AI player type a lobby AI (or a demo map's named player) runs; empty for a human, a faction
// without Basic_AI, or a non-AI player.
[[nodiscard]] std::string ai_player_type(const StartInputs& inputs, const StartPlayer& player);

// WTA-46 (#735): the mounted players' difficulty table (the AI player types' Difficulty_Adjustments and
// difficultyadjustments.xml); null when the data carries none.
[[nodiscard]] std::shared_ptr<const script::foc::AiDifficultyTable> load_difficulty_table(const vfs::Vfs& files);

// WTA-46, WTA-47 (#735): the difficulty adjustment of one start player: its AI player type's entry for the slot's
// level. A human, a player without an AI type and inputs without a table get the neutral adjustment.
[[nodiscard]] script::foc::AiDifficultyAdjustment player_difficulty(const StartInputs& inputs, const StartPlayer& player);

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
