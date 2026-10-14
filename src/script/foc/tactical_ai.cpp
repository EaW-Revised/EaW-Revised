#include "eawr/core/tick_profile.hpp"
// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "tactical_ai_internal.hpp"
#include "mode_time.hpp"
#include "tactical_ai_world.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <iomanip>
#include <sstream>
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

const LuaNumber* number_of(const std::optional<Value>& value) {
    return value ? std::get_if<LuaNumber>(&value->data) : nullptr;
}

class FreestoreEngine final : public authoritative::ScriptEngine {
public:
    FreestoreEngine(HostPtr host, std::shared_ptr<ai::Engine> goals, std::string closure)
        : host_(std::move(host)), goals_(std::move(goals)), closure_(std::move(closure)) {}

    core::Result<void> before_world(const sim::PartitionExecutor&,
        const tactical::TacticalSession& world) override {
        if (!host_->setup.scoring) return core::Result<void>::success();
        auto scoring = host_->setup.scoring->visit(*world.snapshot());
        if (!scoring) return core::Result<void>::failure(scoring.error());
        if (scoring.value()) { host_->ai_suspended = *scoring.value(); host_->suspension_used = true; }
        return core::Result<void>::success();
    }

    core::Result<void> initialize_fresh(const tactical::TacticalSession& world, ScriptScheduler& scripts) override {
        host_->view = build_view(world, *world.snapshot());
        host_->world = &world;
        host_->snapshot = world.snapshot();
        std::uint64_t sequence = 0;
        const sim::InlineExecutor executor;
        auto prepared = service_objects(executor, host_, scripts, sequence,
            object_diagnostics_, object_count_, object_pumps_);
        if (!prepared) return prepared;
        if (!object_diagnostics_.empty()) return core::Result<void>::failure(
            make_error(object_diagnostics_.front().code, object_diagnostics_.front().message));
        return core::Result<void>::success();
    }

    core::Result<std::optional<std::vector<std::uint8_t>>> save_native(
        const tactical::TacticalSession& world, ScriptScheduler& scripts) override {
        using Result = core::Result<std::optional<std::vector<std::uint8_t>>>;
        if (!host_->setup.scoring && !host_->suspension_used
            && (!host_->setup.perception.campaign_game || !goals_)) return Result::success(std::nullopt);
        static_cast<void>(world);
        const auto& content = closure_;
        std::string scoring, native;
        if (host_->setup.scoring) {
            auto saved = host_->setup.scoring->save();
            if (!saved) return Result::failure(saved.error());
            scoring = std::move(saved).value();
        }
        if (goals_) {
            auto state = goals_->save_tactical_native(content, scripts);
            if (!state) return Result::failure(state.error());
            native.assign(state.value().begin(), state.value().end());
        }
        std::ostringstream out;
        out << "tactical-host-v3 " << content << ' ' << host_->ai_suspended << ' '
            << host_->suspension_used << ' ' << bool(host_->setup.scoring) << ' ' << bool(goals_)
            << ' ' << staggered_clock_.paused_frames << ' ' << bool(staggered_clock_.held_since)
            << ' ' << staggered_clock_.held_since.value_or(0)
            << ' ' << std::quoted(scoring) << ' ' << std::quoted(native);
        const auto bytes = out.str();
        return Result::success(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    }
    core::Result<void> restore_native(std::span<const std::uint8_t> bytes,
        const tactical::TacticalSession& world, ScriptScheduler& scripts) override {
        const auto refuse = [] { return core::Result<void>::failure(make_error(
            authoritative::codes::load_rejected, "tactical host continuation differs")); };
        const auto& content = closure_;
        std::istringstream in{std::string(bytes.begin(), bytes.end())};
        std::string tag, identity, scoring, native;
        unsigned suspended{}, used{}, has_scoring{}, has_goals{};
        unsigned held{};
        std::uint64_t paused_frames{}, held_since{};
        if (!(in >> tag >> identity >> suspended >> used >> has_scoring >> has_goals
                >> paused_frames >> held >> held_since
                >> std::quoted(scoring) >> std::quoted(native)) || !(in >> std::ws).eof()
            || tag != "tactical-host-v3" || identity != content || suspended > 1 || used > 1
            || held > 1 || paused_frames > world.completed_tick()
            || (held ? held_since > world.completed_tick() || paused_frames > held_since : held_since != 0)
            || (host_->setup.schedule.mode != AiSchedule::Mode::staggered && (paused_frames != 0 || held != 0))
            || has_scoring != unsigned(bool(host_->setup.scoring)) || has_goals != unsigned(bool(goals_))
            || (!has_scoring && !scoring.empty()) || (!has_goals && !native.empty())) return refuse();
        host_->world = &world; host_->snapshot = world.snapshot();
        host_->view = build_view(world, *host_->snapshot);
        if (goals_) {
            const std::vector<std::uint8_t> state(native.begin(), native.end());
            if (auto restored = goals_->restore_tactical_native(state, content, scripts); !restored) return restored;
        }
        if (host_->setup.scoring) {
            if (auto restored = host_->setup.scoring->load(scoring); !restored) return restored;
        }
        host_->ai_suspended = suspended != 0; host_->suspension_used = used != 0;
        staggered_clock_.paused_frames = paused_frames;
        staggered_clock_.held_since = held ? std::optional<std::uint64_t>(held_since) : std::nullopt;
        return core::Result<void>::success();
    }

    core::Result<void> after_service(authoritative::ServiceReport& report) override {
        // FD-72: late Lua commands take effect only after this frame's AI turn.
        std::vector<authoritative::ScriptCommand> kept;
        for (auto& command : report.commands) {
            if (command.verb != "tactical-suspend-ai") { kept.push_back(std::move(command)); continue; }
            const auto* value = command.arguments.size() == 1 ? std::get_if<LuaNumber>(&command.arguments[0].data) : nullptr;
            if (value == nullptr) return core::Result<void>::failure(make_error(
                authoritative::codes::session_abort, "invalid suspension command"));
            host_->ai_suspended = *value > LuaNumber(0);
            host_->suspension_used = true;
        }
        report.commands = std::move(kept);
        report.diagnostics.insert(report.diagnostics.end(), object_diagnostics_.begin(), object_diagnostics_.end());
        object_diagnostics_.clear();
        if (goals_) {
            if (auto serviced = goals_->after_service(report); !serviced) return serviced;
        }
        if (host_->setup.journal && !host_->setup.journal->costs.empty()) {
            auto& cost = host_->setup.journal->costs.back();
            cost.object_instances = object_count_;
            cost.object_pumps = object_pumps_;
        }
        return core::Result<void>::success();
    }

    core::Result<authoritative::ServiceOptions> before_service(
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        const sim::InlineExecutor executor;
        return before_service(executor, world, tick, scripts);
    }

    core::Result<authoritative::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        using OptionsResult = core::Result<authoritative::ServiceOptions>;
        host_->view = [&] {
            core::tick_profile::Scope scope("serial.ai.view");
            return build_view(world, *tick.snapshot, &executor);
        }();
        host_->world = &world;
        host_->snapshot = tick.snapshot;
        const std::uint64_t next = scripts.completed_tick() + 1;
        const auto instances = scripts.instances();
        std::uint64_t sequence = 0;
        staggered_resume_shift_ = LuaNumber(0);
        if (host_->setup.schedule.mode == AiSchedule::Mode::staggered) {
            if (!host_->ai_suspended && staggered_clock_.held_since)
                staggered_resume_shift_ = mode_seconds(world.completed_tick()) - mode_seconds(*staggered_clock_.held_since);
            staggered_clock_.visit(world.completed_tick(), host_->ai_suspended);
        }
        {
            core::tick_profile::Scope scope("serial.ai.objects");
            if (auto serviced = service_objects(executor, host_, scripts, sequence, object_diagnostics_, object_count_, object_pumps_); !serviced) return OptionsResult::failure(serviced.error());
        }
        std::set<tactical::PlayerId> tested; // freestores already tested this frame
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
            }
            // SCH-03 / FD-73: translate the existing authored stamps on release,
            // including freestores whose execution phase is still pending.
            // Keep globals in mode time so script reads share GetCurrentTime's clock.
            if (staggered_resume_shift_ != LuaNumber(0)) {
                for (const auto name : {"LastService", "LastUnitService"}) {
                    auto previous = scripts.read_global(instance, name);
                    if (!previous) return OptionsResult::failure(previous.error());
                    if (const auto* stamp = number_of(previous.value()))
                        assign(name, number(*stamp + staggered_resume_shift_));
                }
            }
            if (scripts.completed_tick() != 0 && !host_->ai_suspended && (!goals_ || !goals_->runs_player(player.player))) {
                // FH-11, WTA-02: a player the goal system does not run (yet) is tested every frame.
                tested.insert(player.player);
                if (auto serviced = freestore_tests(player.player, instance, next, world, scripts, sequence); !serviced)
                    return OptionsResult::failure(serviced.error());
            }
            for (ScriptEvent& out : events) {
                if (auto submitted = scripts.submit_event(std::move(out)); !submitted) return OptionsResult::failure(submitted.error());
            }
        }
        if (goals_) {
            core::tick_profile::Scope scope("serial.ai.manager");
            // WTA-01/02: the execution service runs the freestore's tests after the player's goals and
            // planning, so this frame's reservations already hold, and before its build tasks.
            const ai::Engine::ExecutionHook hook = [&](tactical::PlayerId id, std::uint64_t& live_sequence) -> core::Result<void> {
                const std::uint64_t instance = freestore_instance(id);
                if (scripts.completed_tick() == 0 || tested.contains(id)
                    || !std::binary_search(instances.begin(), instances.end(), instance)) return core::Result<void>::success();
                return freestore_tests(id, instance, next, world, scripts, live_sequence);
            };
            if (auto serviced = goals_->before_service(*host_->ai_world, scripts, sequence, &executor, hook); !serviced) {
                return OptionsResult::failure(serviced.error());
            }
        }
        authoritative::ServiceOptions options;
        options.host_paced = [](std::uint64_t instance) { return instance >= 1000U; };
        return OptionsResult::success(std::move(options));
    }

private:
    // FH-11/FH-12, WTA-02: the freestore's ServiceRate and UnitServiceRate tests for one player.
    core::Result<void> freestore_tests(tactical::PlayerId player, std::uint64_t instance, std::uint64_t next,
        const tactical::TacticalSession& world, ScriptScheduler& scripts, std::uint64_t& sequence) const {
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
        const LuaNumber now = mode_seconds(world.completed_tick());
        auto read = [&](std::string_view name) { return scripts.read_global(instance, name); };
        auto rate = read("ServiceRate");
        auto last = read("LastService");
        auto unit_rate = read("UnitServiceRate");
        auto last_unit = read("LastUnitService");
        if (!rate || !last || !unit_rate || !last_unit) return core::Result<void>::failure(make_error(
            authoritative::codes::session_abort, "cannot read the freestore service globals"));
        const auto due = [&](const std::optional<Value>& period, const std::optional<Value>& previous) {
            const LuaNumber* seconds = number_of(period);
            if (seconds == nullptr) return false;
            const LuaNumber* since = number_of(previous);
            // Release assignments are queued for the later Lua pass; this
            // same-frame test must already see the translated numeric stamp.
            return since == nullptr || *seconds < now - (staggered_resume_shift_ == LuaNumber(0)
                ? *since : *since + staggered_resume_shift_);
        };
        if (due(rate.value(), last.value())) {
            events.push_back(event(ScriptEvent::Kind::pump, {}));
            assign("LastService", number(now));
        }
        if (due(unit_rate.value(), last_unit.value())) {
            // FH-12: every freestore unit in entity order (retail: hash order) that no
            // plan has reserved (FH-11, AI-08).
            for (const ViewUnit& unit : host_->view->units) {
                if (!unit_service(unit, player)) continue;
                ScriptEvent call = event(ScriptEvent::Kind::call, "On_Unit_Service");
                call.arguments.push_back(handle(handle_game_object, unit.id));
                events.push_back(std::move(call));
            }
            assign("LastUnitService", number(now));
        }
        for (ScriptEvent& out : events) {
            if (auto submitted = scripts.submit_event(std::move(out)); !submitted) return submitted;
        }
        return core::Result<void>::success();
    }

    // FH-13: the AI player's own objects that move (BEHAVIOR_LOCO), except
    // squadron members; with the goal system, not the units a plan holds or reserves (FH-11).
    [[nodiscard]] bool unit_service(const ViewUnit& unit, tactical::PlayerId player) const {
        // FH-13/WSW-47: source attachment eligibility does not give a planetary
        // source the ordinary moving-unit service callback.
        if (unit.planetary_weapon) return false;
        if (goals_) return goals_->unit_service(unit, player);
        if (unit.owner != player || unit.craft) return false;
        const AiType* type = host_->type(unit.type);
        return type != nullptr && type->locomotor && !type->star_base;
    }

    HostPtr host_;
    std::shared_ptr<ai::Engine> goals_;
    std::string closure_;
    ai::StaggeredServiceClock staggered_clock_;
    LuaNumber staggered_resume_shift_{}; // derived each turn, never continuation state
    std::vector<authoritative::ScriptDiagnostic> object_diagnostics_;
    std::uint32_t object_count_{};
    std::uint32_t object_pumps_{};
};

} // namespace

core::Result<void> tactical_ai_detail::service_objects(const sim::PartitionExecutor& executor, const HostPtr& host_, ScriptScheduler& scripts, std::uint64_t& sequence, std::vector<authoritative::ScriptDiagnostic>& object_diagnostics_, std::uint32_t& object_count_, std::uint32_t& object_pumps_) {
    // SOS-40/42: immutable view, disjoint proposals, ordered structural commit.
    std::vector<std::optional<authoritative::ObjectScript>> proposed(host_->view->units.size());
    const auto prepared = executor.execute_phase("object-script-attachments", sim::tick_partition_count,
        [&](const std::size_t partition) {
            const auto range = sim::partition_range(partition, proposed.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = host_->view->units[index];
                const auto* type = host_->type(unit.type);
                if (type == nullptr || type->decoration) continue;
                const auto name = upper(type->lua_script);
                if (!admitted_object_script(name)) continue;
                // SOS-44: native consumers retain their sourced same-turn
                // service until the world exposes an object-script barrier.
                // Ownership comes from the existing consumer profiles, not
                // a second module-name list in the script host.
                if (type->point_defence_script || type->corrupt_systems_script) continue;
                if (host_->world) {
                    const auto* native = host_->world->abilities().find(unit.type);
                    if (native && (native->defend_script || native->self_destruct_script || native->mc30_script)) continue;
                }
                // Tactical identities are monotonically allocated and never reused.
                // Their allocation identity is also their generation (SOS-44).
                proposed[index] = authoritative::ObjectScript{{handle_game_object, unit.id}, unit.id,
                    first_object_instance + unit.id, "Data/Scripts/GameObject/" + name + ".lua"};
            }
        });
    if (!prepared) return prepared;
    std::vector<authoritative::ObjectScript> objects;
    for (auto& proposal : proposed) if (proposal) objects.push_back(std::move(*proposal));
    auto reconciled = scripts.reconcile_objects(std::move(objects));
    if (!reconciled) return core::Result<void>::failure(reconciled.error());
    auto diagnostics = std::move(reconciled).value();
    object_diagnostics_.insert(object_diagnostics_.end(), diagnostics.begin(), diagnostics.end());
    const auto attachments = scripts.object_scripts();
    const auto active = scripts.instances();
    struct Due { bool active{}; bool pump{}; bool first{}; LuaNumber rate{}; core::Diagnostic error; };
    std::vector<Due> due(attachments.size());
    const auto admitted = executor.execute_phase("object-script-admission", sim::tick_partition_count,
        [&](const std::size_t partition) {
            const auto range = sim::partition_range(partition, due.size());
            const auto now = mode_seconds(host_->view->tick);
            for (auto index = range.begin; index < range.end; ++index) {
                const auto id = attachments[index].instance;
                auto& slot = due[index];
                if (!std::binary_search(active.begin(), active.end(), id)) continue;
                slot.active = true;
                // SOS-31/33, FC-31: a deletion-pending object is never pumped. The
                // world erases an object in the tick of its lethal damage (FS-07),
                // so the completed view holds no deletion-pending object.
                if (host_->view->find(attachments[index].object.id) == nullptr) continue;
                auto threads = scripts.thread_slots(id);
                if (!threads) { slot.error = threads.error(); continue; }
                if (threads.value().empty()) continue; // queued fresh definitions/main/Object
                auto rate = scripts.read_global(id, "ServiceRate");
                auto last = scripts.read_global(id, "LastService");
                if (!rate || !last) { slot.error = !rate ? rate.error() : last.error(); continue; }
                const auto* period = number_of(rate.value());
                if (period == nullptr) continue;
                const auto* previous = number_of(last.value());
                slot.first = previous == nullptr;
                slot.rate = *period;
                slot.pump = previous == nullptr || *period < now - *previous;
            }
        });
    if (!admitted) return admitted;
    object_count_ = object_pumps_ = 0;
    for (std::size_t index = 0; index < due.size(); ++index) {
        const auto& slot = due[index];
        if (!slot.error.code.empty()) return core::Result<void>::failure(slot.error);
        if (slot.active) ++object_count_;
        if (!slot.pump) continue;
        ++object_pumps_;
        const auto id = attachments[index].instance;
        ScriptEvent event;
        event.key = {scripts.completed_tick() + 1, producer_foc_engine, id, sequence++};
        event.target = id;
        event.kind = ScriptEvent::Kind::pump;
        if (auto queued = scripts.submit_event(event); !queued) return queued;
        event.key.sequence = sequence++;
        event.kind = ScriptEvent::Kind::assign;
        event.name = "LastService";
        // FC-57: first continuing pump retains a negative random phase;
        // later pumps stamp mode time. The instance-stream RNG is the
        // existing host seam, distinct from the original shared stream.
        authoritative::RandomStream random(host_->setup.seed, host_->view->tick, id);
        event.parameter = number(slot.first ? LuaNumber(0) - random.next_unit() * slot.rate : mode_seconds(host_->view->tick));
        if (auto queued = scripts.submit_event(std::move(event)); !queued) return queued;
    }
    return core::Result<void>::success();
}

numeric::LuaNumber scoring_seconds(const std::uint64_t frame) noexcept {
    return ai::to_single(ai::to_single(numeric::LuaNumber(frame))
        / numeric::LuaNumber(tactical::logical_frames_per_second));
}

core::Result<PreparedAiSession> prepare_session(
    const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    using SessionResult = core::Result<PreparedAiSession>;
    auto host = std::make_shared<Host>();
    host->setup = setup;
    host->ai_world = std::make_shared<ai::TacticalWorld>(*host);
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
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/"};
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
    // WGS-66: freeze the exact selected Lua/XML closure, with length framing.
    std::vector<std::uint8_t> closure_bytes;
    const auto append = [&](std::string_view value) {
        const auto size = static_cast<std::uint64_t>(value.size());
        for (unsigned i = 0; i < 8; ++i) closure_bytes.push_back(static_cast<std::uint8_t>(size >> (i * 8)));
        closure_bytes.insert(closure_bytes.end(), value.begin(), value.end());
    };
    append("eawr-tactical-closure-v1");
    auto configuration = native_configuration(setup);
    if (!configuration) return SessionResult::failure(configuration.error());
    append(configuration.value());
    append(setup.freestore_module);
    for (const auto& [path, bytes] : modules) { append(path); append(bytes); }
    append("xml");
    for (const auto& [path, bytes] : setup.xml) { append(path); append(bytes); }
    const auto closure = core::sha256_hex(closure_bytes);
    auto bind = [host, goals, closure](authoritative::ScriptedTacticalSession& session) -> core::Result<void> {
        for (const std::string_view verb : {verb_move, verb_attack, verb_attack_move, verb_guard, verb_ability, verb_special_fire, verb_buy, verb_pad_build, verb_reinforce, verb_credit_grant, verb_reservation_debit, verb_reveal_all}) {
            if (auto added = session.register_verb(verb, translate_order); !added) return added;
        }
        if (host->setup.perception.campaign_game && host->setup.campaign_can_retreat && host->setup.campaign_retreat_order) {
            auto translate = [host](const authoritative::ScriptCommand& command) -> core::Result<authoritative::TacticalOrder> {
                using OrderResult = core::Result<authoritative::TacticalOrder>;
                const auto* issuer = command.arguments.size() == 1 ? std::get_if<LuaNumber>(&command.arguments[0].data) : nullptr;
                const auto id = issuer ? numeric::to_exact_integer(*issuer) : core::Result<std::int64_t>::failure(
                    make_error(authoritative::codes::command_unroutable, "campaign retreat expects a player ID"));
                if (!id || id.value() <= 0 || id.value() > std::numeric_limits<tactical::PlayerId>::max())
                    return OrderResult::failure(make_error(authoritative::codes::command_unroutable, "invalid campaign retreat player ID"));
                return host->setup.campaign_retreat_order(static_cast<tactical::PlayerId>(id.value()));
            };
            if (auto added = session.register_verb(verb_campaign_retreat, std::move(translate)); !added) return added;
        }
        // The session owns immutable content; bind its identity once rather than
        // copying the ever-growing replay at every native hash request.
        const auto world_content = session.record().setup.content_identity;
        const auto combined = closure + core::sha256_hex(world_content);
        const auto identity = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(combined.data()), combined.size()));
        return session.set_engine(std::make_shared<FreestoreEngine>(host, goals, identity));
    };
    return SessionResult::success({std::move(scripts).value(), std::move(bind)});
}

core::Result<authoritative::ScriptedTacticalSession> create_session(
    tactical::TacticalSession world, const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    using SessionResult = core::Result<authoritative::ScriptedTacticalSession>;
    auto prepared = prepare_session(setup, modules);
    if (!prepared) return SessionResult::failure(prepared.error());
    auto session = authoritative::ScriptedTacticalSession::create(std::move(world), std::move(prepared.value().scripts));
    if (!session) return session;
    if (auto bound = prepared.value().bind(session.value()); !bound) return SessionResult::failure(bound.error());
    return session;
}

std::vector<std::string> unsupported_plan_calls() {
    return {
        "recharge/expiration ability plan events remain pending (environmental cancel/ready signals are supported)",
        "exploration sweep (Explore_Area): the TaskForce moves to the area's centre",
        "missing: Garrison, Leave_Garrison, GameObject.Get_Combat_Rating",
        "GameRandom and FindTarget draws use the plan instance's seeded stream, not the game's shared one",
    };
}

} // namespace eawr::script::foc
