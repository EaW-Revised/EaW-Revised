#include "eawr/platform/live_ai.hpp"
#include "eawr/battle/start.hpp"

#include "eawr/checkpoint/skirmish.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/land_start.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::platform {

namespace {

// The AI closure of the sessions one LiveAi wraps; set when a session starts or loads, read by its saves.
class SharedClosure final {
public:
    void set(std::string value) {
        const std::lock_guard lock(mutex_);
        value_ = std::move(value);
    }
    [[nodiscard]] std::string get() const {
        const std::lock_guard lock(mutex_);
        return value_;
    }
private:
    mutable std::mutex mutex_;
    std::string value_;
};

core::Diagnostic save_error(std::string message) {
    core::Diagnostic error;
    error.code = std::string(checkpoint::diagnostic_codes::invalid_save);
    error.message = std::move(message);
    return error;
}

// The part both battle domains share: the goal system's XML, the frozen script inventory and the session wrap.
core::Result<LiveAi> mount(script::foc::AiSetup setup, const vfs::Vfs& files, std::shared_ptr<const BattleScoring> scoring,
    std::shared_ptr<script::foc::AiJournal> journal, const bool trace_proposals) {
    setup.journal = std::move(journal);
    setup.trace_proposals = trace_proposals;
    // #449: the goal system runs the retail space plans beside the freestore.
    if (auto enabled = skirmish::enable_goal_system(files, setup); !enabled) return core::Result<LiveAi>::failure(enabled.error());
    LiveAi out;
    auto modules = skirmish::ai_modules(files, setup, false, &out.load_notes);
    if (!modules) return core::Result<LiveAi>::failure(modules.error());
    for (const auto& player : setup.players) {
        if (player.ai) out.players.push_back(player.player);
    }
    using Scripted = script::authoritative::ScriptedTacticalSession;
    // Each initial/rebuild/restored session owns a fresh scorer; staging cannot mutate
    // the live provider or the viewer's separate results instance.
    using Host = std::function<core::Result<script::foc::PreparedAiSession>()>;
    const auto host = std::make_shared<const Host>([setup = std::move(setup), modules = std::move(modules).value(),
        scoring = std::move(scoring)]() -> core::Result<script::foc::PreparedAiSession> {
        auto owned = setup;
        if (scoring) {
            auto fresh = scoring->fresh();
            if (!fresh) return core::Result<script::foc::PreparedAiSession>::failure(fresh.error());
            owned.scoring = std::move(fresh).value();
        }
        return script::foc::prepare_session(owned, modules);
    });
    // The AI's frozen script closure, the lua_save identity of its saves; the same for every session.
    auto closure = std::make_shared<SharedClosure>();
    auto scripts = std::make_shared<platform::LiveScripts>();
    scripts->wrap = [host, closure](sim::tactical::TacticalSession world) -> core::Result<Scripted> {
        auto prepared = (*host)();
        if (!prepared) return core::Result<Scripted>::failure(prepared.error());
        closure->set(prepared.value().closure_sha256);
        auto session = Scripted::create(std::move(world), std::move(prepared.value().scripts));
        if (!session) return session;
        if (auto bound = prepared.value().bind(session.value()); !bound) return core::Result<Scripted>::failure(bound.error());
        return session;
    };
    scripts->save = [closure](Scripted& session) -> core::Result<std::vector<std::uint8_t>> {
        const auto identity = closure->get();
        if (identity.empty()) return core::Result<std::vector<std::uint8_t>>::failure(save_error("the battle's AI has not started"));
        const auto battle = session.world().record().setup;
        const auto build = checkpoint::skirmish_identities(battle.content_identity, checkpoint::script_closure_digest(identity));
        return checkpoint::save_skirmish_checkpoint(session, checkpoint::default_seats(battle), build);
    };
    scripts->restore = [host, closure](std::span<const std::uint8_t> bytes, const sim::tactical::TacticalSetup& battle,
        const LiveWorldFactory& world, const sim::PartitionExecutor& executor) -> core::Result<Scripted> {
        auto prepared = (*host)();
        if (!prepared) return core::Result<Scripted>::failure(prepared.error());
        closure->set(prepared.value().closure_sha256);
        const auto build = checkpoint::skirmish_identities(battle.content_identity,
            checkpoint::script_closure_digest(prepared.value().closure_sha256));
        // The load asks for its host once, after the world; hand it the host prepared above.
        auto once = std::make_shared<std::optional<script::foc::PreparedAiSession>>(std::move(prepared).value());
        const checkpoint::SkirmishHostFactory host_factory = [once](const sim::tactical::TacticalReplay&)
            -> core::Result<checkpoint::SkirmishHost> {
            if (!*once) return core::Result<checkpoint::SkirmishHost>::failure(save_error("the AI host was already used"));
            auto used = std::move(**once);
            once->reset();
            return core::Result<checkpoint::SkirmishHost>::success(
                checkpoint::SkirmishHost{std::move(used.scripts), std::move(used.bind)});
        };
        auto loaded = checkpoint::load_skirmish_checkpoint(bytes, build, world, host_factory, executor);
        if (!loaded) return core::Result<Scripted>::failure(loaded.error());
        if (loaded.value().session.record().setup != battle)
            return core::Result<Scripted>::failure(save_error("the save holds another battle than the prepared one"));
        return core::Result<Scripted>::success(std::move(loaded.value().session));
    };
    out.scripts = std::move(scripts);
    return core::Result<LiveAi>::success(std::move(out));
}

} // namespace

core::Result<LiveAi> live_ai(const battle::BattleStart& start, const skirmish::StartInputs& inputs, const units::UnitTables& tables,
    const vfs::Vfs& files, std::shared_ptr<const BattleScoring> scoring,
    std::shared_ptr<script::foc::AiJournal> journal, const bool trace_proposals) {
    return mount(skirmish::ai_setup(start, inputs, tables), files, std::move(scoring), std::move(journal), trace_proposals);
}

core::Result<void> validate_live_ai_save(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
    const units::UnitTables& tables, const vfs::Vfs& files, const std::span<const std::uint8_t> bytes) {
    auto container = checkpoint::parse_checkpoint(bytes);
    if (!container) return core::Result<void>::failure(container.error());
    auto setup = skirmish::ai_setup(start, inputs, tables);
    if (auto enabled = skirmish::enable_goal_system(files, setup); !enabled) return core::Result<void>::failure(enabled.error());
    auto modules = skirmish::ai_modules(files, setup, false);
    if (!modules) return core::Result<void>::failure(modules.error());
    auto prepared = script::foc::prepare_session(setup, modules.value());
    if (!prepared) return core::Result<void>::failure(prepared.error());
    if (container.value().header.identities.lua_save != checkpoint::script_closure_digest(prepared.value().closure_sha256))
        return core::Result<void>::failure(save_error("the save was made with different AI scripts"));
    return core::Result<void>::success();
}

// #3207 G1-b: the land twin. Same host, the land mode: ai_setup carries the domain, so the goal mode, template,
// plan directory, freestore tag and difficulty fields all follow from the mounted land data.
core::Result<LiveAi> live_land_ai(const skirmish::LandStart& start, const skirmish::LandStartInputs& inputs,
    const units::UnitTables& tables, const vfs::Vfs& files, std::shared_ptr<const BattleScoring> scoring,
    std::shared_ptr<script::foc::AiJournal> journal, const bool trace_proposals) {
    return mount(skirmish::land_ai_setup(start, inputs, tables), files, std::move(scoring), std::move(journal), trace_proposals);
}

} // namespace eawr::platform
