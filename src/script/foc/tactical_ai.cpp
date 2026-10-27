#include "eawr/core/tick_profile.hpp"
// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "ai_mode.hpp"
#include "ai_script_evaluators.hpp"
#include "tactical_ai_internal.hpp"
#include "mode_time.hpp"
#include "tactical_ai_world.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <iomanip>
#include <iterator>
#include <tuple>
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

class FreestoreEngine final : public authoritative::ScriptEngine, public tactical::ObjectTurnService,
    public std::enable_shared_from_this<FreestoreEngine> {
public:
    FreestoreEngine(HostPtr host, std::shared_ptr<ai::Engine> goals, std::string closure,
        std::vector<std::string> notes)
        : host_(std::move(host)), goals_(std::move(goals)), closure_(std::move(closure)), startup_notes_(std::move(notes)) {}

    bool supports_battle_ai(const tactical::PlayerId player) const noexcept override {
        const auto found = host_->players.find(player);
        return goals_ && found != host_->players.end() && found->second->ai && !found->second->player_type.empty();
    }

    core::Result<std::shared_ptr<tactical::ObjectTurnService>> object_turn_service(
        const tactical::TacticalSession& world, ScriptScheduler& scripts) override {
        scripts_ = &scripts;
        host_->world = &world;
        host_->snapshot = world.snapshot();
        host_->view = build_view(world, *host_->snapshot);
        const sim::InlineExecutor executor;
        // A root chunk may create threads before Base_Definitions/Object are
        // delivered. Fresh attachments still initialize at the first barrier.
        if (world.completed_tick() != 0) {
            if (auto prepared = prepare_turn(executor); !prepared)
                return core::Result<std::shared_ptr<tactical::ObjectTurnService>>::failure(prepared.error());
        }
        return core::Result<std::shared_ptr<tactical::ObjectTurnService>>::success(shared_from_this());
    }

    bool due(const std::uint64_t tick) override {
        return tick == turn_tick_ && !due_.empty();
    }

    core::Result<std::vector<tactical::ObjectTurnUse>> run(
        const sim::PartitionExecutor& executor, const tactical::ObjectTurnView& turn) override {
        using Result = core::Result<std::vector<tactical::ObjectTurnUse>>;
        host_->view = build_turn_view(executor, turn, *host_->view);
        struct TurnGuard {
            Host& host;
            explicit TurnGuard(Host& value) : host(value) {
                host.rejected_turn_read.store(false, std::memory_order_relaxed);
                host.in_object_turn = true;
            }
            ~TurnGuard() { host.in_object_turn = false; }
        } guard(*host_);
        std::vector<std::uint64_t> instances;
        std::uint64_t sequence = 0;
        for (const auto& pump : due_) {
            const auto* object = host_->view->find(pump.instance - first_object_instance);
            if (!object || (object->hull && object->hull->raw() <= 0)) continue;
            ScriptEvent event;
            event.key = {scripts_->completed_tick() + 1, producer_foc_engine, pump.instance, sequence++};
            event.target = pump.instance;
            event.kind = ScriptEvent::Kind::pump;
            if (auto queued = scripts_->submit_event(event); !queued) return Result::failure(queued.error());
            event.key.sequence = sequence++;
            event.kind = ScriptEvent::Kind::assign;
            event.name = "LastService";
            auto random = object_phase_random(host_->setup.seed, turn.tick, pump.instance);
            event.parameter = number(pump.first ? LuaNumber(0) - random.next_unit() * pump.rate : mode_seconds(turn.tick));
            if (auto queued = scripts_->submit_event(std::move(event)); !queued) return Result::failure(queued.error());
            instances.push_back(pump.instance);
        }
        auto serviced = scripts_->service_turn(executor, instances);
        if (!serviced) return Result::failure(serviced.error());
        if (host_->rejected_turn_read.load(std::memory_order_relaxed))
            return Result::failure(make_error(authoritative::codes::invalid_request,
                "object turn attempted a completed-world dynamic read"));
        turn_report_ = std::move(serviced).value();
        object_pumps_ = static_cast<std::uint32_t>(instances.size());
        turn_rows_ = static_cast<std::uint32_t>(turn.rows.size());
        turn_ran_ = true;
        std::vector<tactical::ObjectTurnUse> uses;
        std::vector<authoritative::ScriptCommand> next_tick;
        std::uint64_t use_sequence = 0;
        for (auto& command : turn_report_.commands) {
            if (command.verb != verb_ability) { next_tick.push_back(std::move(command)); continue; }
            auto translated = translate_order(command);
            if (!translated) return Result::failure(translated.error());
            const auto* payload = std::get_if<tactical::AbilityPayload>(&translated.value().payload);
            if (!payload) return Result::failure(make_error(authoritative::codes::command_unroutable,
                "object turn ability did not translate to an ability payload"));
            for (const auto id : translated.value().units) {
                const auto* source = host_->view->find(id);
                if (!source) continue;
                // AB-15: preserve the container use so the seam can gate every
                // current holder before applying any of them.
                if (ability_view(*host_, *source, payload->ability).has)
                    uses.push_back({id, use_sequence++, payload->ability, payload->action,
                        payload->target, payload->position});
            }
        }
        turn_report_.commands = std::move(next_tick);
        return Result::success(std::move(uses));
    }

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
            object_diagnostics_, object_count_, object_pumps_, false);
        if (!prepared) return prepared;
        if (!object_diagnostics_.empty()) return core::Result<void>::failure(
            make_error(object_diagnostics_.front().code, object_diagnostics_.front().message));
        return core::Result<void>::success();
    }

    core::Result<std::optional<std::vector<std::uint8_t>>> save_native(
        const tactical::TacticalSession& world, ScriptScheduler& scripts) override {
        using Result = core::Result<std::optional<std::vector<std::uint8_t>>>;
        const bool delayed = std::any_of(host_->setup.player_ai_start_ticks.begin(), host_->setup.player_ai_start_ticks.end(),
            [](const auto& entry) { return entry.second != 0; });
        // GFS-A24: a standalone goals owner also retains its streams and decisions across saves.
        // Admission fixtures also retain freestore-only continuation before/after their boundary.
        // SOS-46: even an otherwise stateless owner retains a nonempty omission
        // closure, so hashing and checkpoint admission distinguish its policy.
        if (!host_->setup.scoring && !host_->suspension_used && !delayed && !goals_
            && host_->omitted_object_scripts.empty()) return Result::success(std::nullopt);
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
        host_->script_service_active.store(false, std::memory_order_relaxed);
        report.commands.insert(report.commands.end(), std::make_move_iterator(turn_report_.commands.begin()),
            std::make_move_iterator(turn_report_.commands.end()));
        report.diagnostics.insert(report.diagnostics.end(), std::make_move_iterator(turn_report_.diagnostics.begin()),
            std::make_move_iterator(turn_report_.diagnostics.end()));
        report.loads.insert(report.loads.end(), turn_report_.loads.begin(), turn_report_.loads.end());
        report.removed_instances.insert(report.removed_instances.end(), turn_report_.removed_instances.begin(), turn_report_.removed_instances.end());
        turn_report_ = {};
        std::sort(report.commands.begin(), report.commands.end(), [](const auto& left, const auto& right) {
            return std::tie(left.issuer, left.sequence) < std::tie(right.issuer, right.sequence);
        });
        std::sort(report.loads.begin(), report.loads.end(), [](const auto& left, const auto& right) { return left.instance < right.instance; });
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
        if (!startup_notes_.empty()) report.diagnostics.push_back({std::string(authoritative::codes::script_warning),
            "AI discovery diagnostic entries=" + std::to_string(startup_notes_.size()), report.tick, 0, -1});
        for (const auto& note : startup_notes_) report.diagnostics.push_back({
            std::string(authoritative::codes::script_warning), note, report.tick, 0, -1});
        startup_notes_.clear();
        report.diagnostics.insert(report.diagnostics.end(), object_diagnostics_.begin(), object_diagnostics_.end());
        object_diagnostics_.clear();
        if (goals_) {
            if (auto serviced = goals_->after_service(report); !serviced) return serviced;
        }
        if (host_->setup.journal && !host_->setup.journal->costs.empty()) {
            auto& cost = host_->setup.journal->costs.back();
            cost.object_instances = object_count_;
            cost.object_pumps = object_pumps_;
            cost.object_turn_due_ticks = turn_ran_ ? 1U : 0U;
            cost.object_turn_rows = turn_rows_;
        }
        object_pumps_ = turn_rows_ = 0;
        turn_ran_ = false;
        return prepare_turn(*executor_);
    }

    core::Result<authoritative::ServiceOptions> before_service(
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        return before_service(inline_executor_, world, tick, scripts);
    }

    core::Result<authoritative::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        using OptionsResult = core::Result<authoritative::ServiceOptions>;
        host_->script_service_active.store(false, std::memory_order_relaxed);
        executor_ = &executor;
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
            std::uint32_t unused{};
            if (auto serviced = service_objects(executor, host_, scripts, sequence, object_diagnostics_, object_count_, unused, false); !serviced) return OptionsResult::failure(serviced.error());
        }
        std::set<tactical::PlayerId> tested; // freestores already tested this frame
        for (const AiPlayer& player : host_->setup.players) {
            // Prepare dormant freestores at the first barrier. Their threads
            // remain host paced; freestore_tests gates every gameplay pump.
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
                if (auto serviced = freestore_tests(player.player, instance, next, world, scripts, sequence,
                        [&scripts](ScriptEvent event) { return scripts.submit_event(std::move(event)); },
                        [this](PlanEvent row) { if (host_->setup.journal) host_->setup.journal->append_plan(std::move(row)); });
                    !serviced)
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
            const ai::Engine::ExecutionHook hook = [&](tactical::PlayerId id, ai::Engine::ExecutionEffects& effects) -> core::Result<void> {
                const std::uint64_t instance = freestore_instance(id);
                if (scripts.completed_tick() == 0 || tested.contains(id)
                    || !std::binary_search(instances.begin(), instances.end(), instance)) return core::Result<void>::success();
                return freestore_tests(id, instance, next, world, scripts, effects.sequence, effects.submit, effects.journal);
            };
            if (auto serviced = goals_->before_service(*host_->ai_world, scripts, sequence, &executor, hook); !serviced) {
                return OptionsResult::failure(serviced.error());
            }
        }
        authoritative::ServiceOptions options;
        options.host_paced = [](std::uint64_t instance) { return instance >= 1000U; };
        options.defer_instance = [initialized = initialized_](std::uint64_t instance) {
            return std::binary_search(initialized.begin(), initialized.end(), instance);
        };
        // The scheduler services its instances next, partitioned: serial-only Lua stays out until after_service.
        host_->script_service_active.store(true, std::memory_order_relaxed);
        return OptionsResult::success(std::move(options));
    }

private:
    struct Pump { std::uint64_t instance{}; bool first{}; LuaNumber rate{}; };
    core::Result<void> prepare_turn(const sim::PartitionExecutor& executor) {
        turn_tick_ = host_->view->tick;
        const auto attachments = scripts_->object_scripts();
        const auto active = scripts_->instances();
        struct Admission { std::optional<Pump> pump; bool initialized{}; core::Diagnostic error; };
        std::vector<Admission> rows(attachments.size());
        const auto prepared = executor.execute_phase("object-script-admission", sim::tick_partition_count,
            [&](const std::size_t partition) {
                const auto range = sim::partition_range(partition, rows.size());
                const auto now = mode_seconds(turn_tick_);
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto id = attachments[index].instance;
                    auto& row = rows[index];
                    if (!std::binary_search(active.begin(), active.end(), id)) continue;
                    auto threads = scripts_->thread_slots(id);
                    if (!threads) { row.error = threads.error(); continue; }
                    if (threads.value().empty()) continue; // fresh definitions still run at the barrier
                    row.initialized = true;
                    auto rate = scripts_->read_global(id, "ServiceRate");
                    auto last = scripts_->read_global(id, "LastService");
                    if (!rate || !last) { row.error = !rate ? rate.error() : last.error(); continue; }
                    const auto* period = number_of(rate.value());
                    const auto* previous = number_of(last.value());
                    if (period && (!previous || *period < now - *previous)) row.pump = Pump{id, previous == nullptr, *period};
                }
            });
        if (!prepared) return prepared;
        due_.clear(); initialized_.clear();
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            if (!row.error.code.empty()) return core::Result<void>::failure(row.error);
            if (row.initialized) initialized_.push_back(attachments[index].instance);
            if (row.pump) due_.push_back(*row.pump);
        }
        return core::Result<void>::success();
    }
    sim::InlineExecutor inline_executor_;
    ScriptScheduler* scripts_{};
    const sim::PartitionExecutor* executor_{};
    std::uint64_t turn_tick_{};
    std::vector<Pump> due_;
    std::vector<std::uint64_t> initialized_;
    authoritative::ServiceReport turn_report_;
    std::uint32_t turn_rows_{};
    bool turn_ran_{};

    // FH-11/FH-12, WTA-02: the freestore's ServiceRate and UnitServiceRate tests for one player.
    // The events go to `submit` and the journal rows to `journal`: the sequential pass passes the scheduler and
    // the journal, the engine's player turn stages both for its ordered commit.
    using SubmitEvent = std::function<core::Result<void>(ScriptEvent)>;
    using JournalRow = std::function<void(PlanEvent)>;
    core::Result<void> freestore_tests(tactical::PlayerId player, std::uint64_t instance, std::uint64_t next,
        const tactical::TacticalSession& world, ScriptScheduler& scripts, std::uint64_t& sequence,
        const SubmitEvent& submit, const JournalRow& journal) const {
        if (!host_->ai_admitted(player)) return core::Result<void>::success();
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
                const bool eligible = unit_service(unit, player);
                if (host_->setup.journal && unit.owner == player && !unit.craft) {
                    const auto* type = host_->type(unit.type);
                    journal(PlanEvent{world.completed_tick(), player, {}, {},
                        "OBJECT_" + std::to_string(unit.id), "freestore_unit",
                        "eligible=" + std::to_string(eligible) + " can_move=" + std::to_string(unit.can_move)
                        + " type=" + (type ? type->name : std::string{"unknown"})});
                }
                if (!eligible) continue;
                ScriptEvent call = event(ScriptEvent::Kind::call, "On_Unit_Service");
                call.arguments.push_back(handle(handle_game_object, unit.id));
                events.push_back(std::move(call));
            }
            assign("LastUnitService", number(now));
        }
        for (ScriptEvent& out : events) {
            if (auto submitted = submit(std::move(out)); !submitted) return submitted;
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
    std::vector<std::string> startup_notes_; // observer output only; never saved or hashed
    std::uint32_t object_count_{};
    std::uint32_t object_pumps_{};
};

} // namespace

core::Result<void> tactical_ai_detail::service_objects(const sim::PartitionExecutor& executor, const HostPtr& host_, ScriptScheduler& scripts, std::uint64_t& sequence, std::vector<authoritative::ScriptDiagnostic>& object_diagnostics_, std::uint32_t& object_count_, std::uint32_t& object_pumps_, const bool schedule) {
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
                auto module = "Data/Scripts/GameObject/" + name + ".lua";
                if (host_->omitted_object_scripts.contains(module)) continue;
                proposed[index] = authoritative::ObjectScript{{handle_game_object, unit.id}, unit.id,
                    first_object_instance + unit.id, std::move(module)};
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
    if (!schedule) {
        object_count_ = static_cast<std::uint32_t>(std::count_if(attachments.begin(), attachments.end(),
            [&](const auto& object) { return std::binary_search(active.begin(), active.end(), object.instance); }));
        object_pumps_ = 0;
        return core::Result<void>::success();
    }
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
    auto load_notes = object_script_load_notes(setup, modules);
    for (const auto& note : load_notes) host->omitted_object_scripts.insert(*note.logical_path);
    host->ai_world = std::make_shared<ai::TacticalWorld>(*host);
    for (const AiType& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    for (const AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    if (auto loaded = load_contrast(*host, modules); !loaded) return SessionResult::failure(loaded.error());
    std::shared_ptr<ai::Engine> goals;
    std::vector<std::string> notes;
    for (const auto& note : load_notes) notes.push_back(core::format_diagnostic(note));
    std::set<std::string> selected_goals;
    if (setup.bounds && !setup.xml.empty()) {
        auto data = ai::load_ai_data(setup.xml, converters(host));
        if (!data) return SessionResult::failure(data.error());
        // AI-16: broken authored references must never silently disable an AI.
        for (const auto& player : setup.players) {
            if (!player.ai || player.player_type.empty()) continue;
            const auto selected = data.value().players.find(upper(player.player_type));
            if (selected == data.value().players.end()) return SessionResult::failure(make_error(
                authoritative::codes::invalid_request, "AI player type is missing: " + player.player_type));
            for (const auto& name : ai::mode_templates(selected->second, setup.domain)) {
                if (!data.value().templates.contains(name)) return SessionResult::failure(make_error(
                    authoritative::codes::invalid_request, "AI player " + player.player_type + " references missing template " + name));
            }
            for (const auto& name : selected->second.function_sets) {
                const auto functions = data.value().function_sets.find(name);
                if (functions == data.value().function_sets.end()) return SessionResult::failure(make_error(
                    authoritative::codes::invalid_request, "AI player " + player.player_type + " references missing function set " + name));
                for (const auto& function : functions->second) {
                    const auto goal = data.value().goals.find(function.goal);
                    if (goal == data.value().goals.end()) {
                        notes.push_back("AI function set " + name + " skips missing goal " + function.goal);
                    } else if (goal->second.mode == ai::ai_mode(setup.domain).goal_mode) {
                        selected_goals.insert(function.goal);
                        if (!data.value().equations.find(function.function))
                            return SessionResult::failure(make_error(authoritative::codes::invalid_request,
                                "AI goal " + function.goal + " references missing equation " + function.function));
                    }
                }
            }
        }
        auto plans = load_plans(host, modules, notes, &selected_goals);
        if (!plans) return SessionResult::failure(plans.error());
        if (setup.journal) for (const auto& note : notes)
            setup.journal->append_plan({0, 0, {}, {}, {}, "definition_diagnostic", note});
        goals = std::make_shared<ai::Engine>(host, std::move(data).value(), std::move(plans).value());
        host->engine = goals.get();
        // #1702: Script_* perception runs the corpus's own evaluator modules (WGA-453).
        auto evaluators = ScriptEvaluators::create(host, modules);
        if (!evaluators) return SessionResult::failure(evaluators.error());
        goals->attach_script_evaluators(std::move(evaluators).value());
    }

    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return SessionResult::failure(added.error());
    }
    authoritative::SessionConfig config;
    config.seed = setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/", std::string(ai::ai_mode(setup.domain).script_directory)};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return SessionResult::failure(scripts.error());
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (!errors.empty()) return SessionResult::failure(errors.front());
    for (const AiPlayer& player : host->setup.players) {
        if (!player.ai) continue;
        if (auto created = scripts.value().create_instance(freestore_instance(player.player), freestore_module(setup, player)); !created) {
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
    // #3780: a non-space mode names itself in the closure; space writes today's bytes.
    if (const auto marker = ai::ai_mode(setup.domain).closure_marker; !marker.empty()) append(marker);
    for (const auto& [path, bytes] : modules) { append(path); append(bytes); }
    append("xml");
    for (const auto& [path, bytes] : setup.xml) { append(path); append(bytes); }
    // SOS-46 / #4589: bind the deterministic omission set only when nonempty,
    // preserving stock closure bytes and separating sessions with different skips.
    if (!load_notes.empty()) {
        append("omitted-object-scripts-v1");
        const auto roots = object_script_modules(setup);
        for (const auto& note : load_notes) {
            append(*note.logical_path);
            const auto& types = roots.at(*note.logical_path);
            append(std::to_string(types.size()));
            for (const auto& type : types) append(type);
        }
    }
    const auto closure = core::sha256_hex(closure_bytes);
    auto bind = [host, goals, closure, notes = std::move(notes)](authoritative::ScriptedTacticalSession& session) -> core::Result<void> {
        // SOS-46: native stand-ins are implementations of the omitted attachment,
        // not independent abilities. Reapply admission when rebuilding a checkpoint.
        if (!host->omitted_object_scripts.empty()) {
            std::vector<tactical::TypeId> omitted_types;
            for (const auto& type : host->setup.content.types)
                if (host->object_script_omitted(type)) omitted_types.push_back(type.type_id);
            std::sort(omitted_types.begin(), omitted_types.end());
            if (auto admitted = session.omit_object_script_decisions(omitted_types); !admitted) return admitted;
        }
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
        return session.set_engine(std::make_shared<FreestoreEngine>(host, goals, identity, notes));
    };
    return SessionResult::success({std::move(scripts).value(), std::move(bind), std::move(load_notes)});
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
