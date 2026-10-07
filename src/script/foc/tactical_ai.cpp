// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "tactical_ai_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <set>
#include <utility>

namespace eawr::script::foc {
using namespace tactical_ai_detail;
namespace {

namespace tactical = sim::tactical;
namespace math = sim::math;
using authoritative::Binding;
using authoritative::BindingContext;
using authoritative::Handle;
using authoritative::ScriptEvent;
using authoritative::ScriptScheduler;
using authoritative::Value;
using authoritative::ValueList;
using numeric::LuaNumber;

using namespace detail;

using HostPtr = std::shared_ptr<Host>;

// ---- Engine side (FH-10 to FH-13) -------------------------------------------------------

// FH-11: the frame count times the single-precision inverse FPS, then
// widened to the Lua double.
LuaNumber mode_seconds(std::uint64_t frame) {
    const float inverse_fps = 1.0F / static_cast<float>(tactical::logical_frames_per_second);
    const float seconds = static_cast<float>(frame) * inverse_fps;
    return lua_double(static_cast<double>(seconds));
}

const LuaNumber* number_of(const std::optional<Value>& value) {
    return value ? std::get_if<LuaNumber>(&value->data) : nullptr;
}

class FreestoreEngine final : public authoritative::ScriptEngine {
public:
    FreestoreEngine(HostPtr host, std::shared_ptr<ai::Engine> goals) : host_(std::move(host)), goals_(std::move(goals)) {}

    core::Result<void> after_service(authoritative::ServiceReport& report) override {
        if (!goals_) return core::Result<void>::success();
        return goals_->after_service(report);
    }

    core::Result<authoritative::ServiceOptions> before_service(
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        const sim::InlineExecutor executor;
        return before_service(executor, world, tick, scripts);
    }

    core::Result<authoritative::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        using OptionsResult = core::Result<authoritative::ServiceOptions>;
        host_->view = build_view(world, *tick.snapshot, &executor);
        host_->world = &world;
        host_->snapshot = tick.snapshot;
        const std::uint64_t next = scripts.completed_tick() + 1;
        const auto instances = scripts.instances();
        std::uint64_t sequence = 0;
        for (const AiPlayer& player : host_->setup.players) {
            if (!player.ai) continue;
            const std::uint64_t instance = freestore_instance(player.player);
            if (!std::binary_search(instances.begin(), instances.end(), instance)) continue;
            std::vector<ScriptEvent> events;
            const auto event = [&](ScriptEvent::Kind kind, std::string name) {
                ScriptEvent out;
                out.key = authoritative::EventKey{next, producer_foc_engine, instance, sequence++};
                out.target = instance;
                out.kind = kind;
                out.name = std::move(name);
                return out;
            };
            const auto assign = [&](std::string name, Value value) {
                ScriptEvent out = event(ScriptEvent::Kind::assign, std::move(name));
                out.parameter = std::move(value);
                events.push_back(std::move(out));
            };
            if (scripts.completed_tick() == 0) {
                // FH-10, on a fresh load, before the first service.
                events.push_back(event(ScriptEvent::Kind::call, "Base_Definitions"));
                events.push_back(event(ScriptEvent::Kind::start_thread, "main"));
                assign("PlayerObject", handle(handle_player, player.player));
                // SCH-03: the staggered schedule starts the player at position k of the AI players
                // k frames into the service periods, so the players' freestore services (their
                // pass over every free unit above all) fall on different ticks.
                std::uint64_t phase = 0;
                if (host_->setup.schedule.mode == AiSchedule::Mode::staggered) {
                    for (const AiPlayer& other : host_->setup.players) {
                        if (other.ai && other.player < player.player) ++phase;
                    }
                }
                assign("LastService", phase == 0 ? number(LuaNumber(0)) : number(mode_seconds(phase)));
                assign("LastUnitService", phase == 0 ? number(LuaNumber(0)) : number(mode_seconds(phase)));
            } else {
                // FH-11.
                const LuaNumber now = mode_seconds(world.completed_tick());
                auto read = [&](std::string_view name) { return scripts.read_global(instance, name); };
                auto rate = read("ServiceRate");
                auto last = read("LastService");
                auto unit_rate = read("UnitServiceRate");
                auto last_unit = read("LastUnitService");
                if (!rate || !last || !unit_rate || !last_unit) return OptionsResult::failure(make_error(
                    authoritative::codes::session_abort, "cannot read the freestore service globals"));
                const auto due = [&](const std::optional<Value>& period, const std::optional<Value>& previous) {
                    const LuaNumber* seconds = number_of(period);
                    if (seconds == nullptr) return false;
                    const LuaNumber* since = number_of(previous);
                    return since == nullptr || *seconds < now - *since;
                };
                if (due(rate.value(), last.value())) {
                    events.push_back(event(ScriptEvent::Kind::pump, {}));
                    assign("LastService", number(now));
                }
                if (due(unit_rate.value(), last_unit.value())) {
                    // FH-12: every freestore unit in entity order (retail: hash order).
                    for (const ViewUnit& unit : host_->view->units) {
                        if (!in_freestore(unit, player.player)) continue;
                        ScriptEvent call = event(ScriptEvent::Kind::call, "On_Unit_Service");
                        call.arguments.push_back(handle(handle_game_object, unit.id));
                        events.push_back(std::move(call));
                    }
                    assign("LastUnitService", number(now));
                }
            }
            for (ScriptEvent& out : events) {
                if (auto submitted = scripts.submit_event(std::move(out)); !submitted) return OptionsResult::failure(submitted.error());
            }
        }
        if (goals_) {
            if (auto serviced = goals_->before_service(world, *tick.snapshot, scripts, sequence, &executor); !serviced) {
                return OptionsResult::failure(serviced.error());
            }
        }
        authoritative::ServiceOptions options;
        options.host_paced = [](std::uint64_t instance) { return instance >= 1000U; };
        return OptionsResult::success(std::move(options));
    }

private:
    // FH-13: the AI player's own objects that move (BEHAVIOR_LOCO), except
    // squadron members.
    [[nodiscard]] bool in_freestore(const ViewUnit& unit, tactical::PlayerId player) const {
        if (goals_) return goals_->in_freestore(unit, player);
        if (unit.owner != player || unit.craft) return false;
        const AiType* type = host_->type(unit.type);
        return type != nullptr && type->locomotor && !type->star_base;
    }

    HostPtr host_;
    std::shared_ptr<ai::Engine> goals_;
};

} // namespace

core::Result<authoritative::ScriptedTacticalSession> create_session(
    tactical::TacticalSession world, const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    using SessionResult = core::Result<authoritative::ScriptedTacticalSession>;
    auto host = std::make_shared<Host>();
    host->setup = setup;
    for (const AiType& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    for (const AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    if (auto loaded = load_contrast(*host, modules); !loaded) return SessionResult::failure(loaded.error());
    std::shared_ptr<ai::Engine> goals;
    if (setup.bounds && !setup.xml.empty()) {
        auto data = ai::load_ai_data(setup.xml, converters(host));
        if (!data) return SessionResult::failure(data.error());
        std::vector<std::string> notes;
        auto plans = load_plans(host, modules, notes);
        if (!plans) return SessionResult::failure(plans.error());
        goals = std::make_shared<ai::Engine>(host, std::move(data).value(), std::move(plans).value());
        host->engine = goals.get();
    }

    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return SessionResult::failure(added.error());
    }
    authoritative::SessionConfig config;
    config.seed = setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/"};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return SessionResult::failure(scripts.error());
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (!errors.empty()) return SessionResult::failure(errors.front());
    for (const AiPlayer& player : host->setup.players) {
        if (!player.ai) continue;
        if (auto created = scripts.value().create_instance(freestore_instance(player.player), setup.freestore_module); !created) {
            return SessionResult::failure(created.error());
        }
    }
    auto session = authoritative::ScriptedTacticalSession::create(std::move(world), std::move(scripts).value());
    if (!session) return session;
    for (const std::string_view verb : {verb_move, verb_attack, verb_attack_move, verb_guard, verb_ability, verb_buy, verb_pad_build, verb_reinforce, verb_credit_grant, verb_reservation_debit, verb_reveal_all}) {
        if (auto added = session.value().register_verb(verb, translate_order); !added) return SessionResult::failure(added.error());
    }
    if (auto set = session.value().set_engine(std::make_shared<FreestoreEngine>(host, goals)); !set) return SessionResult::failure(set.error());
    return session;
}

std::vector<std::string> unsupported_plan_calls() {
    return {
        "recharge/expiration ability plan events remain pending (environmental cancel/ready signals are supported)",
        "exploration sweep (Explore_Area): the TaskForce moves to the area's centre",
        "star base Fire_Special_Weapon answers nil (FH-27); missing: GameObject.Fire_Special_Weapon on other objects, Garrison, Leave_Garrison, Get_Parent_Object, "
        "Get_Combat_Rating, Get_All_Projectile_Types",
        "GameRandom and FindTarget draws use the plan instance's seeded stream, not the game's shared one",
    };
}

} // namespace eawr::script::foc
