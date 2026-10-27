// Generic Script_* perception evaluators (#1702, see ai_script_evaluators.hpp).

#include "ai_script_evaluators.hpp"

#include "ai_engine.hpp"
#include "ai_mode.hpp"
#include "tactical_ai_internal.hpp"

#include "eawr/sim/world.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace eawr::script::foc::tactical_ai_detail {

namespace {

using authoritative::ScriptEvent;
using detail::Host;

constexpr std::string_view evaluator_directory = "DATA/SCRIPTS/EVALUATORS/";
constexpr std::string_view adapter_prefix = "EAWR/FOC_EVALUATOR/";
constexpr std::string_view parameter_prefix = "PARAMETER_SCRIPT_";

// Host glue, not a retail script: calls the evaluator under pcall so a failing Evaluate still
// reaches the cleanup, and leaves the outcome in globals the host reads. The arguments ride in a
// table (Lua 5.0 has no vararg expressions).
constexpr char adapter_tail[] = R"LUA(
function EAWR_Run_Evaluator()
    EAWR_Value = nil
    EAWR_Error = nil
    EAWR_Cleanup_Error = nil
    if Evaluate == nil then
        EAWR_Error = "the module defines no Evaluate"
    else
        local ok, value = pcall(Evaluate, unpack(EAWR_Arguments))
        if ok then EAWR_Value = value else EAWR_Error = tostring(value) end
    end
    if Evaluator_Clean_Up ~= nil then
        local ok, message = pcall(Evaluator_Clean_Up)
        if not ok then EAWR_Cleanup_Error = tostring(message) end
    end
end
)LUA";

std::string stem_of(std::string_view path) {
    const auto slash = path.rfind('/');
    std::string stem(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
    const auto dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    return stem;
}

// Memo key part: a type tag, then the exact binary64 bits of a number or the length-prefixed text, so
// number 1 and string "1" (or "a|b" and "a","b") can never share a key.
std::string key_of(const ai::ParameterValue& value) {
    if (const auto* number = std::get_if<ai::Real>(&value)) return "n" + std::to_string(number->repr) + ";";
    const auto& text = std::get<std::string>(value);
    return "s" + std::to_string(text.size()) + ":" + text + ";";
}

} // namespace

struct ScriptEvaluators::Impl {
    struct Module {
        std::string path;       // logical path of the evaluator module as mounted
        std::string adapter;    // the generated root module
        std::uint64_t instance{};
        bool created{};
        bool faulted{};
        std::string fault;
    };
    std::shared_ptr<Host> host;
    std::map<std::string, Module> catalogue; // upper-case stem -> module
    std::optional<authoritative::ScriptScheduler> scheduler;
    std::uint64_t last_time_tick{};
    std::uint64_t sequence{};
    std::atomic<bool> busy{};
    // One AI turn's calls with equal inputs agree: the engine reads one completed view per tick.
    std::shared_ptr<const detail::WorldView> memo_view;
    std::map<std::string, Outcome> memo;
    Stats stats;

    core::Result<void> ensure_instance(Module& module) {
        if (module.created) return core::Result<void>::success();
        auto created = scheduler->create_instance(module.instance, module.adapter);
        if (!created) return created;
        module.created = true;
        ++stats.loaded;
        return core::Result<void>::success();
    }
};

ScriptEvaluators::ScriptEvaluators(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ScriptEvaluators::~ScriptEvaluators() = default;

core::Result<std::shared_ptr<ScriptEvaluators>> ScriptEvaluators::create(
    const std::shared_ptr<Host>& host, const std::map<std::string, std::string>& modules) {
    using Result = core::Result<std::shared_ptr<ScriptEvaluators>>;
    auto impl = std::make_unique<Impl>();
    impl->host = host;
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return Result::failure(added.error());
        const auto normal = authoritative::ModuleManifest::normalise(path);
        if (!normal || !normal->starts_with(evaluator_directory) || !normal->ends_with(".LUA")) continue;
        const std::string_view rest = std::string_view(*normal).substr(evaluator_directory.size());
        if (rest.find('/') != std::string_view::npos) continue; // only the directory's own modules
        const std::string name = stem_of(*normal);
        if (name.empty() || impl->catalogue.contains(name)) continue;
        Impl::Module module;
        module.path = path;
        impl->catalogue.emplace(name, std::move(module));
    }
    std::uint64_t rank = 0;
    for (auto& [name, module] : impl->catalogue) {
        module.instance = ++rank;
        std::string root = module.path;
        if (const auto dot = root.rfind('.'); dot != std::string::npos) root.resize(dot);
        module.adapter = std::string(adapter_prefix) + name + ".LUA";
        const std::string text = "require(\"" + root + "\")\n" + adapter_tail;
        if (auto added = manifest.add(module.adapter, text); !added) return Result::failure(added.error());
    }
    if (impl->catalogue.empty()) return Result::success(std::shared_ptr<ScriptEvaluators>(new ScriptEvaluators(std::move(impl))));
    authoritative::SessionConfig config;
    config.seed = host->setup.seed;
    config.tick_duration = authoritative::TickDuration{sim::tactical::tick_numerator, sim::tactical::tick_denominator};
    config.query_clock = true; // synchronous calls: script time follows the world, not the call count
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/",
        std::string(ai::ai_mode(host->setup.domain).script_directory)};
    auto scheduler = authoritative::ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scheduler) return Result::failure(scheduler.error());
    std::vector<core::Diagnostic> errors;
    register_globals(scheduler.value(), host, errors);
    register_methods(scheduler.value(), host, errors);
    if (!errors.empty()) return Result::failure(errors.front());
    impl->scheduler.emplace(std::move(scheduler).value());
    return Result::success(std::shared_ptr<ScriptEvaluators>(new ScriptEvaluators(std::move(impl))));
}

bool ScriptEvaluators::known(std::string_view upper_name) const {
    return impl_->catalogue.find(std::string(upper_name)) != impl_->catalogue.end();
}

std::vector<std::string> ScriptEvaluators::names() const {
    std::vector<std::string> out;
    for (const auto& entry : impl_->catalogue) out.push_back(entry.first);
    return out;
}

const ScriptEvaluators::Stats& ScriptEvaluators::stats() const noexcept { return impl_->stats; }

ScriptEvaluators::Outcome ScriptEvaluators::evaluate(std::string_view upper_name, sim::tactical::PlayerId player,
    const ai::Target* target, const std::vector<ai::Binding>& bindings) {
    Impl& state = *impl_;
    ++state.stats.calls;
    const auto refuse = [&](std::string note) {
        ++state.stats.failures;
        return Outcome{std::nullopt, std::move(note)};
    };
    const auto found = state.catalogue.find(std::string(upper_name));
    if (found == state.catalogue.end()) return refuse("no evaluator module " + std::string(upper_name));
    Impl::Module& module = found->second;
    if (module.faulted) return refuse("evaluator unavailable: " + module.fault);
    const Host& host = *state.host;
    if (!host.view) return refuse("evaluator needs a completed world view");
    if (host.script_service_active.load(std::memory_order_relaxed) || host.in_object_turn)
        return refuse("evaluators run in the serial AI turn, not inside the partitioned script service");
    struct Busy {
        std::atomic<bool>& flag;
        bool entered;
        explicit Busy(std::atomic<bool>& value) : flag(value), entered(!value.exchange(true)) {}
        ~Busy() { if (entered) flag = false; }
    } busy(state.busy);
    if (!busy.entered) return refuse("evaluator call is already running");

    // The arguments are the script parameters in authored order (string and number alike).
    std::vector<authoritative::Value> arguments;
    std::string signature(upper_name);
    signature += '|' + std::to_string(player) + '|' + std::to_string(target ? target->id : 0);
    for (const ai::Binding& binding : bindings) {
        if (!binding.parameter.starts_with(parameter_prefix)) continue;
        if (const auto* number = std::get_if<ai::Real>(&binding.value)) arguments.push_back(detail::number(*number));
        else arguments.push_back(authoritative::Value::text(std::get<std::string>(binding.value)));
        signature += key_of(binding.value);
    }
    if (state.memo_view != host.view) {
        state.memo_view = host.view;
        state.memo.clear();
    }
    if (const auto memo = state.memo.find(signature); memo != state.memo.end()) {
        ++state.stats.memo_hits;
        return memo->second;
    }

    const auto settle = [&](Outcome outcome) {
        if (!outcome.value) ++state.stats.failures;
        state.memo.emplace(signature, outcome);
        return outcome;
    };
    const auto fault = [&](std::string message) {
        module.faulted = true;
        module.fault = message;
        return settle(Outcome{std::nullopt, "evaluator unavailable: " + std::move(message)});
    };
    if (auto created = state.ensure_instance(module); !created) return fault(created.error().message);

    const std::uint64_t tick = state.scheduler->completed_tick() + 1;
    const auto event = [&](ScriptEvent::Kind kind, std::string name, std::optional<authoritative::Value> parameter) {
        ScriptEvent out;
        out.key = authoritative::EventKey{tick, authoritative::first_simulation_producer, module.instance, state.sequence++};
        out.target = module.instance;
        out.kind = kind;
        out.name = std::move(name);
        out.parameter = std::move(parameter);
        return out;
    };
    std::optional<authoritative::Value> target_value;
    if (target != nullptr) target_value = detail::handle(ai::handle_ai_target, target->id);
    for (auto queued : {
             event(ScriptEvent::Kind::assign, "PlayerObject", detail::handle(handle_player, player)),
             event(ScriptEvent::Kind::assign, "Target", std::move(target_value)),
             event(ScriptEvent::Kind::assign, "EAWR_Arguments", authoritative::Value{std::move(arguments)}),
             event(ScriptEvent::Kind::call, "EAWR_Run_Evaluator", std::nullopt)}) {
        if (auto submitted = state.scheduler->submit_event(std::move(queued)); !submitted)
            return fault(submitted.error().message);
    }
    authoritative::ServiceOptions options;
    options.time_tick = std::max(state.last_time_tick, host.view->tick);
    options.host_paced = [](std::uint64_t) { return true; }; // no thread pumping: a call, not a plan
    const sim::InlineExecutor executor;
    auto serviced = state.scheduler->service(executor, options);
    if (!serviced) return fault(serviced.error().message);
    state.last_time_tick = *options.time_tick;
    for (const auto& load : serviced.value().loads) state.stats.instructions += static_cast<std::uint64_t>(load.instructions);
    std::string note;
    for (const auto& diagnostic : serviced.value().diagnostics) note += diagnostic.code + ": " + diagnostic.message + "; ";
    if (std::find(serviced.value().removed_instances.begin(), serviced.value().removed_instances.end(), module.instance)
        != serviced.value().removed_instances.end()) {
        module.created = false;
        return fault(note.empty() ? "the evaluator instance stopped" : note);
    }
    // An evaluator reads the world; a command it issued is dropped and fails the call.
    if (!serviced.value().commands.empty())
        return settle(Outcome{std::nullopt, "evaluator issued command " + serviced.value().commands.front().verb});

    const auto value = state.scheduler->read_global(module.instance, "EAWR_Value");
    if (!value) return settle(Outcome{std::nullopt, value.error().message});
    if (value.value()) if (const auto* number = std::get_if<numeric::LuaNumber>(&value.value()->data))
        return settle(Outcome{ai::to_single(*number), std::move(note)});
    // Not a number: an error (message kept) or a result of another type. Never zero.
    const auto error = state.scheduler->read_global(module.instance, "EAWR_Error");
    if (error && error.value()) if (const auto* message = std::get_if<std::string>(&error.value()->data))
        return settle(Outcome{std::nullopt, "evaluator error: " + *message});
    return settle(Outcome{std::nullopt, "evaluator result is not a number"});
}

} // namespace eawr::script::foc::tactical_ai_detail
