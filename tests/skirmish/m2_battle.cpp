#include "m2_battle.hpp"

#include "eawr/skirmish/ai.hpp"

#include <algorithm>
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
    // SAE-01: these fixed-force soak/schedule fixtures omit the skirmish economy.
    // The buying regression supplies real economy rules in foc_plan_tests.
    ai.perception.campaign_game = true;
    if (auto goals = skirmish::enable_goal_system(loaded.filesystem, ai); !goals) {
        return failed("the goal system's XML", goals.error());
    }
    for (std::uint32_t added = 0; added < options.extra_ai_players; ++added) {
        const auto last = std::find_if(ai.players.rbegin(), ai.players.rend(), [](const script::foc::AiPlayer& player) {
            return player.ai && !player.player_type.empty();
        });
        if (last == ai.players.rend()) return std::string("the setup has no AI player to copy");
        script::foc::AiPlayer extra = *last;
        extra.player = static_cast<tactical::PlayerId>(extra.player + 1);
        for (const auto& player : ai.players) extra.player = std::max(extra.player, static_cast<tactical::PlayerId>(player.player + 1));
        ai.players.push_back(extra);
    }
    if (options.schedule) ai.schedule = *options.schedule;
    if (options.journal) ai.journal = options.journal;
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
