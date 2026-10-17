#pragma once

// Synthetic two-player galactic AI owner shared by the S9-early contracts (per-player streams and
// identity ranges, partition-order tripwire). Player IDs, faction names and the player type name are
// parameters, so nothing here depends on FoC's authored names.
#include "ai_engine.hpp"
#include "galactic_ai.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_world.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace s9_fixture {

namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace lifecycle = foc::galactic_ai;

template<class T> T must(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
inline void must(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

inline std::string identity() { return eawr::core::sha256_hex(std::span<const std::uint8_t>{}); }

struct Shape {
    std::uint32_t first = 1, second = 2;       // player IDs, ascending
    std::string type = "Fixture";               // AI player type name
    std::string first_faction = "Empire", second_faction = "Rebel";
    std::uint32_t seed = 42;
};

inline ai::AiData data(const Shape& shape) {
    ai::AiData result;
    result.equations = must(ai::EquationSet::parse({{"fixture.xml", "<Equations><Positive>1</Positive><Zero>0</Zero></Equations>"}}, {}));
    ai::GoalType goal; goal.name = "Goal"; goal.mode = "GALACTIC"; goal.category = "OFFENSIVE"; goal.application = {"FRIENDLY"};
    goal.tracking_duration = ai::real(2); goal.activation_tracking_duration = ai::real(1);
    result.goals["GOAL"] = goal;
    result.function_sets["FUNCTIONS"] = {{"GOAL", "Positive"}};
    ai::PlayerType type; type.name = shape.type; type.function_sets = {"FUNCTIONS"}; type.difficulty["NORMAL"] = "NORMAL";
    result.players[ai::upper_case(shape.type)] = type; result.difficulties["NORMAL"] = {};
    return result;
}
inline std::vector<ai::PlanDef> plans() {
    ai::PlanDef first; first.name = "FIRST"; first.module = "first.lua"; first.goals = {"GOAL"};
    first.taskforces.push_back({"Main", {{1, 1, 0, false, {100}}}, false, true, {}, 1});
    auto second = first; second.name = "SECOND"; second.module = "second.lua";
    return {first, second};
}
inline ai::GalacticPerceptionSetup setup(const Shape& shape) {
    ai::GalacticPerceptionSetup result; result.seed = shape.seed;
    result.players = {{shape.first, 1, shape.first_faction, "Normal", false, true, false, 10},
        {shape.second, 2, shape.second_faction, "Normal", false, true, false, 20}};
    result.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}, {20, "BETA", 2000, {}, {}, {}, {}, {}}};
    foc::AiType type; type.type_id = 100; type.name = "UNIT"; result.types.push_back(type);
    return result;
}
inline std::shared_ptr<const gc::GalacticSnapshot> snapshot(const Shape& shape) {
    gc::Fleet fleet; fleet.id = 50; fleet.owner = shape.first; fleet.orbit = 10; fleet.members = {{51, 100}};
    gc::Fleet other; other.id = 60; other.owner = shape.second; other.orbit = 20; other.members = {{61, 100}};
    return std::make_shared<gc::GalacticSnapshot>(0, gc::GalacticClock{},
        std::vector<gc::GalacticPlayer>{{shape.first, 1, 0}, {shape.second, 2, 0}},
        std::vector<gc::Planet>{{10, {}, shape.first}, {20, {}, shape.second}}, std::vector<gc::Route>{},
        std::vector<gc::Fleet>{fleet, other}, std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt, nullptr);
}
inline std::vector<lifecycle::PlayerSelection> selections(const Shape& shape) {
    return {{shape.first, shape.type, "NORMAL", {}, {}}, {shape.second, shape.type, "NORMAL", {}, {}}};
}

struct Owner {
    Shape shape;
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    auth::ScriptScheduler scripts = must(auth::ScriptScheduler::create({}, {}));
    explicit Owner(Shape selected_shape = {})
        : shape(std::move(selected_shape)), world(std::make_shared<ai::GalacticWorld>(setup(shape))),
          engine(std::make_shared<ai::Engine>(host, data(shape), plans())) {
        const auto perception = setup(shape);
        const auto selected = selections(shape);
        host->setup.seed = perception.seed; host->ai_world = world; host->engine = engine.get();
        host->setup.content.types = perception.types;
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(ai::upper_case(type.name), &type);
        }
        for (const auto& player : perception.players) {
            const auto found = std::find_if(selected.begin(), selected.end(), [&](const auto& row) { return row.player == player.player; });
            host->setup.players.push_back({player.player, player.faction, player.neutral, found != selected.end(),
                found != selected.end() ? found->player_type : "", player.human});
        }
        for (const auto& row : host->setup.players) host->players.emplace(row.player, &row);
    }
    void begin(const eawr::sim::PartitionExecutor& executor) {
        const auto selected = selections(shape);
        const auto frozen = snapshot(shape);
        must(world->bind(frozen, {}, executor, 0, 1000));
        std::uint64_t sequence{};
        must(engine->begin_galactic_barrier(*world, {0, frozen->clock().galactic_frame}, scripts, sequence, selected, &executor));
    }
    void policy(std::uint32_t player) {
        lifecycle::TemplatePolicy rules; rules.goal_types["GOAL"] = {true, 1};
        rules.plan_names["FIRST"] = rules.plan_names["SECOND"] = {true, 1};
        must(engine->apply_galactic_policy(player, rules));
    }
    // One proposed goal of the player on a planet, drawing from the player's own synchronized stream.
    [[nodiscard]] std::optional<ai::GalacticGoalIdentity> allocate(std::uint32_t player, eawr::sim::EntityId planet) {
        return must(engine->allocate_galactic_goal(player, 0, engine->target_of_object(planet)->id));
    }
};

} // namespace s9_fixture
