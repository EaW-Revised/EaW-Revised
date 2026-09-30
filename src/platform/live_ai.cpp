#include "eawr/platform/live_ai.hpp"

#include "eawr/platform/live_scripts.hpp"
#include "eawr/skirmish/ai.hpp"

namespace eawr::platform {

core::Result<LiveAi> live_ai(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs, const units::UnitTables& tables,
    const vfs::Vfs& files) {
    auto setup = skirmish::ai_setup(start, inputs, tables);
    // #449: the goal system runs the retail space plans beside the freestore.
    if (auto enabled = skirmish::enable_goal_system(files, setup); !enabled) return core::Result<LiveAi>::failure(enabled.error());
    auto modules = skirmish::ai_modules(files, setup);
    if (!modules) return core::Result<LiveAi>::failure(modules.error());
    LiveAi out;
    for (const auto& player : setup.players) {
        if (player.ai) out.players.push_back(player.player);
    }
    auto scripts = std::make_shared<platform::LiveScripts>();
    scripts->wrap = [setup = std::move(setup), modules = std::move(modules).value()](sim::tactical::TacticalSession world) {
        return script::foc::create_session(std::move(world), setup, modules);
    };
    out.scripts = std::move(scripts);
    return core::Result<LiveAi>::success(std::move(out));
}

} // namespace eawr::platform
