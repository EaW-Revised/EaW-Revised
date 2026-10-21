#pragma once

// The tactical AI's battle mode (#3780): everything the goal system reads by mode from the
// mounted data - goal mode, plan directory, template and freestore tags, required files - in
// one table keyed by the BF-1 battle domain. No object or plan names live here.

#include "eawr/sim/tactical/types.hpp"

#include <span>
#include <string_view>

namespace eawr::script::foc::ai {

struct AiMode {
    std::string_view goal_mode;        // AIGoalType Mode, upper case
    std::string_view plan_directory;   // directory below Data/Scripts/AI/, upper case
    std::string_view script_directory; // the same directory as a script search root
    std::string_view template_tag;     // AIPlayerType Templates/<tag>
    std::string_view freestore_tag;    // AIPlayerType <tag>FreeStoreScript
    std::string_view closure_marker;   // appended to the closure bytes; empty keeps space bytes
    std::span<const std::string_view> required_xml; // logical paths the mode needs besides directory discovery
};

[[nodiscard]] const AiMode& ai_mode(sim::tactical::BattleDomain domain) noexcept;

} // namespace eawr::script::foc::ai
