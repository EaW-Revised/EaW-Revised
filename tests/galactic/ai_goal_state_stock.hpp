#pragma once
#include "ai_engine.hpp"
#include "eawr/vfs/vfs.hpp"
// Production-provider continuation proof shared with the stock catalog test.
void installed_goal_state(const eawr::vfs::Vfs&, eawr::script::foc::ai::AiData,
    std::vector<eawr::script::foc::ai::PlanDef>, std::string content_identity);
void goal_state_contracts();
int galactic_goal_state_failures();
