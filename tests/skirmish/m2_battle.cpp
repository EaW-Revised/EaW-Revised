#include "m2_battle.hpp"

#include "eawr/skirmish/ai.hpp"

#include <utility>

namespace eawr::soak {

namespace tactical = sim::tactical;
namespace foc = script::foc;

std::variant<std::unique_ptr<Battle>, std::string> build_battle(const Loaded& loaded, const BattleOptions& options) {
    const auto failed = [](const std::string& step, const core::Diagnostic& diagnostic) -> std::string {
        return step + ": " + diagnostic.code + ' ' + diagnostic.message;
    };
    auto fixture = skirmish::m2_fixture();
    fixture.seed = options.seed;
    for (auto& slot : fixture.slots) slot.human = false; // the AI on both sides
    auto inputs = skirmish::read_start_inputs(fixture, loaded.filesystem, loaded.catalog, loaded.tables);
    if (!inputs) return failed("FoC start inputs", inputs.error());
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return failed("FoC start", start.error());
    if (options.anonymous_content) start.value().setup.content_identity = {};
    auto content = skirmish::session_content(loaded.tables);
    if (!content) return failed("FoC session content", content.error());
    auto fog = skirmish::fog_rules(inputs.value());
    if (!fog) return failed("the M2 fog grid", fog.error());
    content.value().fog = fog.value();
    auto ai = skirmish::ai_setup(start.value(), inputs.value(), loaded.tables);
    if (auto goals = skirmish::enable_goal_system(loaded.filesystem, ai); !goals) {
        return failed("the goal system's XML", goals.error());
    }
    auto modules = skirmish::ai_modules(loaded.filesystem, ai);
    if (!modules) return failed("the AI's Lua files", modules.error());

    auto battle = std::make_unique<Battle>(Battle{std::move(content).value(), std::move(ai), std::move(modules).value(), std::nullopt});
    const auto& setup = battle->content;
    auto world = tactical::TacticalSession::create(start.value().setup, setup.sensors, setup.durability, setup.motion,
        setup.fog, setup.combat, skirmish::victory_rules(start.value(), loaded.tables), setup.abilities);
    if (!world) return failed("the world", world.error());
    auto session = foc::create_session(std::move(world).value(), battle->ai, battle->modules);
    if (!session) return failed("the AI session", session.error());
    battle->session.emplace(std::move(session).value());
    return battle;
}

} // namespace eawr::soak
