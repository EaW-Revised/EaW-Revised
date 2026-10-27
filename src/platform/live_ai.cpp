#include "eawr/platform/live_ai.hpp"
#include "eawr/battle/start.hpp"

#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include "eawr/skirmish/ai.hpp"

namespace eawr::platform {

core::Result<LiveAi> live_ai(const battle::BattleStart& start, const skirmish::StartInputs& inputs, const units::UnitTables& tables,
    const vfs::Vfs& files, std::shared_ptr<const BattleScoring> scoring,
    std::shared_ptr<script::foc::AiJournal> journal, const bool trace_proposals) {
    auto setup = skirmish::ai_setup(start, inputs, tables);
    setup.journal = std::move(journal);
    setup.trace_proposals = trace_proposals;
    // #449: the goal system runs the retail space plans beside the freestore.
    if (auto enabled = skirmish::enable_goal_system(files, setup); !enabled) return core::Result<LiveAi>::failure(enabled.error());
    auto modules = skirmish::ai_modules(files, setup);
    if (!modules) return core::Result<LiveAi>::failure(modules.error());
    LiveAi out;
    for (const auto& player : setup.players) {
        if (player.ai) out.players.push_back(player.player);
    }
    auto scripts = std::make_shared<platform::LiveScripts>();
    scripts->wrap = [setup = std::move(setup), modules = std::move(modules).value(), scoring = std::move(scoring)](sim::tactical::TacticalSession world) {
        auto owned = setup;
        // Each initial/rebuild session owns a fresh scorer; staging cannot mutate
        // the live provider or the viewer's separate results instance.
        if (scoring) {
            auto fresh = scoring->fresh();
            if (!fresh) return core::Result<script::authoritative::ScriptedTacticalSession>::failure(fresh.error());
            owned.scoring = std::move(fresh).value();
        }
        return script::foc::create_session(std::move(world), owned, modules);
    };
    out.scripts = std::move(scripts);
    return core::Result<LiveAi>::success(std::move(out));
}

} // namespace eawr::platform
