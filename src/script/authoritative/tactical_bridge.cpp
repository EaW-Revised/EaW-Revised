#include "eawr/core/tick_profile.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>
#include <tuple>

namespace eawr::script::authoritative {
namespace {

namespace tactical = sim::tactical;

core::Diagnostic make_error(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return diagnostic;
}

std::string command_label(const ScriptCommand& command) {
    return "script command " + command.verb + " (instance " + std::to_string(command.issuer) + ", sequence " +
        std::to_string(command.sequence) + ", tick " + std::to_string(command.tick) + ")";
}

// A translator is host code: whatever it throws fails the translation like a
// returned failure, before anything reaches the world. The message is fixed so
// the report is the same on every target.
core::Result<TacticalOrder> translate(const CommandTranslator& translator, const ScriptCommand& command) {
    try {
        return translator(command);
    } catch (...) {
        return core::Result<TacticalOrder>::failure(make_error(codes::command_unroutable, "the translator threw an exception"));
    }
}

} // namespace

core::Result<void> ScriptEngine::initialize_fresh(const tactical::TacticalSession&, ScriptScheduler&) {
    return core::Result<void>::success();
}

core::Result<std::optional<std::vector<std::uint8_t>>> ScriptEngine::save_native(
    const tactical::TacticalSession&, ScriptScheduler&) {
    return core::Result<std::optional<std::vector<std::uint8_t>>>::success({});
}
core::Result<void> ScriptEngine::restore_native(std::span<const std::uint8_t>,
    const tactical::TacticalSession&, ScriptScheduler&) {
    return core::Result<void>::failure(make_error(codes::load_rejected, "engine has no native continuation owner"));
}

struct ScriptedTacticalSession::Impl {
    tactical::TacticalSession world;
    ScriptScheduler scripts;
    std::map<std::string, CommandTranslator, std::less<>> translators;
    std::shared_ptr<ScriptEngine> engine;
    std::optional<std::vector<std::uint8_t>> pending_native;
    std::shared_ptr<sim::StateHasher> hasher; // #637: null hashes on the stepping thread
    std::function<bool(const TacticalOrder&)> order_admission;
    std::function<core::Result<void>()> shared_mode_service;
    bool authoritative_hash{true};            // #895: off, the tick's combined hash stays empty
    std::function<std::uint64_t()> clock;     // #957: measurement only, null without one

    [[nodiscard]] std::uint64_t now() const { return clock ? clock() : 0; }
    // The last service's commands, submitted with the next step.
    std::vector<ScriptCommand> pending;
    // Per issuer: (tick, one past the sequence) of the latest key the world
    // accepted from either path.
    std::map<tactical::PlayerId, std::pair<std::uint64_t, std::uint64_t>> last_keys;
    bool stepped{};
    std::optional<core::Diagnostic> failure;

    Impl(tactical::TacticalSession world_in, ScriptScheduler scripts_in)
        : world(std::move(world_in)), scripts(std::move(scripts_in)) {}

    void accepted(const sim::CommandKey& key) {
        const std::pair next{key.tick, key.sequence + 1U};
        auto [entry, inserted] = last_keys.emplace(key.player_id, next);
        if (!inserted) entry->second = std::max(entry->second, next);
    }

    // The issuer's next sequence at `tick`: after any key it used this tick,
    // and after every key it used before (sequences never repeat).
    std::uint64_t next_sequence(tactical::PlayerId issuer) const {
        const auto found = last_keys.find(issuer);
        return found == last_keys.end() ? 0 : found->second.second;
    }

    RoutedCommand route(ScriptCommand command, std::uint64_t tick) {
        RoutedCommand routed;
        routed.command = std::move(command);
        const auto translator = translators.find(routed.command.verb);
        if (translator == translators.end()) {
            routed.dropped = make_error(codes::command_unroutable, command_label(routed.command) + ": no translator for the verb");
            return routed;
        }
        auto order = translate(translator->second, routed.command);
        if (!order) {
            routed.dropped = make_error(codes::command_unroutable,
                command_label(routed.command) + ": " + order.error().code + " " + order.error().message);
            return routed;
        }
        if (order_admission && !order_admission(order.value())) {
            routed.dropped = make_error(codes::command_unroutable, command_label(routed.command) + ": faction controller refused generated order");
            return routed;
        }
        tactical::PlayerCommand player_command;
        player_command.key = {tick, order.value().issuer, next_sequence(order.value().issuer)};
        player_command.units = std::move(order.value().units);
        player_command.payload = std::move(order.value().payload);
        const auto submitted = order.value().reinforcement_search
            ? world.submit_reinforcement_search(player_command, *order.value().reinforcement_search)
            : world.submit(player_command);
        if (!submitted) {
            routed.dropped = make_error(codes::command_unroutable,
                command_label(routed.command) + ": " + submitted.error().code + " " + submitted.error().message);
            return routed;
        }
        accepted(player_command.key);
        routed.submitted = true;
        routed.key = player_command.key;
        return routed;
    }
};

core::Result<ScriptedTacticalSession> ScriptedTacticalSession::create(tactical::TacticalSession world, ScriptScheduler scripts) {
    if (world.completed_tick() != scripts.completed_tick()) {
        return core::Result<ScriptedTacticalSession>::failure(make_error(codes::invalid_request,
            "world tick " + std::to_string(world.completed_tick()) + " and script tick " +
                std::to_string(scripts.completed_tick()) + " differ"));
    }
    return core::Result<ScriptedTacticalSession>::success(
        ScriptedTacticalSession(std::make_unique<Impl>(std::move(world), std::move(scripts))));
}

ScriptedTacticalSession::ScriptedTacticalSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

core::Result<TacticalScriptSave> ScriptedTacticalSession::save_scripts() {
    if (impl_->failure) return core::Result<TacticalScriptSave>::failure(*impl_->failure);
    if (impl_->pending_native) return core::Result<TacticalScriptSave>::failure(make_error(codes::load_rejected, "native child state needs its owner"));
    auto saved = impl_->scripts.save();
    if (!saved) return core::Result<TacticalScriptSave>::failure(saved.error());
    TacticalScriptSave result{completed_tick(), std::move(saved).value(), impl_->pending, impl_->last_keys, {}};
    if (impl_->engine) {
        auto native = impl_->engine->save_native(impl_->world, impl_->scripts);
        if (!native) return core::Result<TacticalScriptSave>::failure(native.error());
        result.native = std::move(native).value();
    }
    return core::Result<TacticalScriptSave>::success(std::move(result));
}

core::Result<ScriptedTacticalSession> ScriptedTacticalSession::restore(
    tactical::TacticalSession world, ScriptScheduler scripts, const TacticalScriptSave& saved) {
    const auto refuse = [](std::string message) {
        return core::Result<ScriptedTacticalSession>::failure(make_error(codes::load_rejected, std::move(message)));
    };
    if (world.completed_tick() != saved.completed_tick || scripts.uses_galactic_clock())
        return refuse("child world and script save must share a tactical barrier");
    if (auto loaded = scripts.load(saved.scheduler); !loaded) return refuse(loaded.error().message);
    if (scripts.completed_tick() != saved.completed_tick) return refuse("child scheduler clock differs");
    const auto recording = world.record();
    const auto& players = recording.setup.players;
    for (const auto& [player, key] : saved.last_keys) {
        if (key.first >= saved.completed_tick || std::none_of(players.begin(), players.end(),
            [&](const auto& row) { return row.player_id == player && row.commandable(); }))
            return refuse("child routing cursor has an invalid player or tick");
    }
    std::map<tactical::PlayerId, std::pair<std::uint64_t, std::uint64_t>> expected_keys;
    for (const auto& command : recording.commands) {
        auto& key = expected_keys[command.key.player_id];
        key = std::max(key, std::pair(command.key.tick, command.key.sequence + 1U));
    }
    if (expected_keys != saved.last_keys) return refuse("child routing cursors disagree with accepted input");
    for (std::size_t index = 0; index < saved.pending.size(); ++index) {
        const auto& command = saved.pending[index];
        if (command.issuer == 0 || command.verb.empty() || saved.completed_tick == 0
            || command.tick != saved.completed_tick
            || (index != 0 && std::pair(saved.pending[index - 1].issuer, saved.pending[index - 1].sequence)
                >= std::pair(command.issuer, command.sequence)))
            return refuse("child pending script commands have an invalid barrier or order");
    }
    auto restored = create(std::move(world), std::move(scripts));
    if (!restored) return restored;
    restored.value().impl_->pending = saved.pending;
    restored.value().impl_->last_keys = saved.last_keys;
    restored.value().impl_->pending_native = saved.native;
    // Pure translators and the engine are reinstalled by the caller before continuation.
    return restored;
}
ScriptedTacticalSession::ScriptedTacticalSession(ScriptedTacticalSession&&) noexcept = default;
ScriptedTacticalSession& ScriptedTacticalSession::operator=(ScriptedTacticalSession&&) noexcept = default;
ScriptedTacticalSession::~ScriptedTacticalSession() = default;

core::Result<void> ScriptedTacticalSession::register_verb(std::string_view verb, CommandTranslator translator) {
    if (impl_->stepped) {
        return core::Result<void>::failure(make_error(codes::invalid_request, "verbs are registered before the first step"));
    }
    if (verb.empty() || !translator) {
        return core::Result<void>::failure(make_error(codes::invalid_request, "a verb needs a name and a translator"));
    }
    if (!impl_->translators.emplace(std::string(verb), std::move(translator)).second) {
        return core::Result<void>::failure(make_error(codes::invalid_request, "duplicate verb " + std::string(verb)));
    }
    return core::Result<void>::success();
}

core::Result<void> ScriptedTacticalSession::set_engine(std::shared_ptr<ScriptEngine> engine) {
    if (impl_->stepped) {
        return core::Result<void>::failure(make_error(codes::invalid_request, "the engine is set before the first step"));
    }
    if (impl_->pending_native) {
        if (!engine) return core::Result<void>::failure(make_error(codes::load_rejected, "native child state needs its owner"));
        auto restored = engine->restore_native(*impl_->pending_native, impl_->world, impl_->scripts);
        if (!restored) return restored;
        impl_->pending_native.reset();
    } else if (engine && completed_tick() == 0) {
        if (auto initialized = engine->initialize_fresh(impl_->world, impl_->scripts); !initialized) return initialized;
    } else if (engine && completed_tick() != 0) {
        auto state = engine->save_native(impl_->world, impl_->scripts);
        if (!state) return core::Result<void>::failure(state.error());
        if (state.value()) return core::Result<void>::failure(make_error(codes::load_rejected, "stateful child engine is missing its native continuation"));
    }
    auto provider = engine ? engine->object_turn_service(impl_->world, impl_->scripts)
        : core::Result<std::shared_ptr<tactical::ObjectTurnService>>::success({});
    if (!provider) return core::Result<void>::failure(provider.error());
    if (auto installed = impl_->world.set_object_turn_service(std::move(provider).value()); !installed) return installed;
    impl_->engine = std::move(engine);
    return core::Result<void>::success();
}

void ScriptedTacticalSession::set_order_admission(std::function<bool(const TacticalOrder&)> admission) {
    impl_->order_admission = std::move(admission);
}

core::Result<ScriptedTick> ScriptedTacticalSession::step(
    const sim::PartitionExecutor& executor, std::span<const tactical::PlayerCommand> player_input) {
    core::tick_profile::Scope tail("serial.bridge.tail");
    Impl& impl = *impl_;
    if (impl.failure) return core::Result<ScriptedTick>::failure(*impl.failure);
    if (impl.pending_native) return core::Result<ScriptedTick>::failure(make_error(codes::load_rejected, "native child state needs its owner"));
    impl.stepped = true;
    ScriptedTick out;
    const std::uint64_t tick = impl.world.completed_tick();
    {
        core::tick_profile::Scope route("serial.bridge.route");
        for (const tactical::PlayerCommand& command : player_input) {
            if (auto submitted = impl.world.submit(command); !submitted) {
                out.refused_input.push_back(submitted.error());
            } else {
                impl.accepted(command.key);
            }
        }
        std::vector<ScriptCommand> pending = std::move(impl.pending);
        impl.pending.clear();
        out.script_input.reserve(pending.size());
        for (ScriptCommand& command : pending) out.script_input.push_back(impl.route(std::move(command), tick));

    }
    const std::uint64_t world_started = impl.now();
    if (impl.engine) {
        auto scoring = [&]() -> core::Result<void> {
            try { return impl.engine->before_world(executor, impl.world); }
            catch (...) { return core::Result<void>::failure(make_error(codes::session_abort, "scoring preparation threw an exception")); }
        }();
        if (!scoring) {
            impl.failure = scoring.error();
            return core::Result<ScriptedTick>::failure(scoring.error());
        }
    }
    auto world = impl.world.step(executor);
    if (!world) {
        impl.failure = world.error();
        return core::Result<ScriptedTick>::failure(world.error());
    }
    out.world = std::move(world).value();
    if (impl.shared_mode_service) {
        auto published = impl.shared_mode_service();
        if (!published) { impl.failure = published.error(); return core::Result<ScriptedTick>::failure(published.error()); }
    }
    const std::uint64_t engine_started = impl.now();
    out.timing.world_ns = engine_started - world_started;
    ServiceOptions options;
    if (impl.engine) {
        core::tick_profile::Scope preparation("serial.bridge.preparation");
        auto prepared = [&]() -> core::Result<ServiceOptions> {
            try {
                return impl.engine->before_service(executor, impl.world, out.world, impl.scripts);
            } catch (const std::exception& error) {
                return core::Result<ServiceOptions>::failure(
                    make_error(codes::session_abort, "script preparation threw: " + std::string(error.what())));
            } catch (...) {
                return core::Result<ServiceOptions>::failure(
                    make_error(codes::session_abort, "the script engine threw an exception"));
            }
        }();
        if (!prepared) {
            impl.failure = prepared.error();
            return core::Result<ScriptedTick>::failure(prepared.error());
        }
        options = std::move(prepared).value();
    }
    const std::uint64_t service_started = impl.now();
    out.timing.engine_ns = service_started - engine_started;
    auto service = [&] {
        core::tick_profile::Scope lua("serial.bridge.lua");
        return impl.scripts.service(executor, options);
    }();
    if (!service) {
        impl.failure = service.error();
        return core::Result<ScriptedTick>::failure(service.error());
    }
    out.timing.service_ns = impl.now() - service_started;
    out.scripts = std::move(service).value();
    if (impl.engine) {
        core::tick_profile::Scope commands("serial.bridge.commands");
        auto taken = [&]() -> core::Result<void> {
            try {
                return impl.engine->after_service(out.scripts);
            } catch (const std::exception& error) {
                return core::Result<void>::failure(
                    make_error(codes::session_abort, "script command processing threw: " + std::string(error.what())));
            } catch (...) {
                return core::Result<void>::failure(make_error(codes::session_abort, "the script engine threw an exception"));
            }
        }();
        if (!taken) {
            impl.failure = taken.error();
            return core::Result<ScriptedTick>::failure(taken.error());
        }
    }
    // Turn and barrier output share per-instance sequences. The engine merges
    // both services before routing or saving the next tick's commands.
    std::sort(out.scripts.commands.begin(), out.scripts.commands.end(), [](const auto& left, const auto& right) {
        return std::tie(left.issuer, left.sequence) < std::tie(right.issuer, right.sequence);
    });
    impl.pending = out.scripts.commands;
    if (!impl.authoritative_hash) return core::Result<ScriptedTick>::success(std::move(out));
    core::tick_profile::Scope capture("serial.bridge.capture");
    auto script_hash = script_state_hash(executor);
    if (!script_hash) {
        impl.failure = script_hash.error();
        return core::Result<ScriptedTick>::failure(script_hash.error());
    }
    out.script_sha256 = std::move(script_hash).value();
    if (impl.hasher) {
        // #637: the world's hash is still being computed; combine it on the hasher after it.
        out.state_hash = impl.hasher->derive(out.world.state_hash,
            [tick = out.world.completed_tick, script = out.script_sha256](const std::string& world) {
                return authoritative_state_sha256(tick, world, script);
            });
    } else {
        out.state_sha256 = authoritative_state_sha256(out.world.completed_tick, out.world.state_sha256, out.script_sha256);
        out.state_hash = sim::StateHash(out.state_sha256);
    }
    return core::Result<ScriptedTick>::success(std::move(out));
}

void ScriptedTacticalSession::set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept {
    impl_->world.set_state_hasher(hasher);
    impl_->hasher = std::move(hasher);
}

void ScriptedTacticalSession::set_shared_mode_service(std::function<core::Result<void>()> service) { impl_->shared_mode_service = std::move(service); }
void ScriptedTacticalSession::set_authoritative_hash(const bool every_tick) noexcept {
    impl_->authoritative_hash = every_tick;
}

void ScriptedTacticalSession::set_step_clock(std::function<std::uint64_t()> clock) { impl_->clock = std::move(clock); }

diagnostics::MemoryCapacity ScriptedTacticalSession::memory_capacity() const {
    return impl_->scripts.memory_capacity();
}

const tactical::TacticalSession& ScriptedTacticalSession::world() const noexcept { return impl_->world; }

std::uint64_t ScriptedTacticalSession::completed_tick() const noexcept { return impl_->world.completed_tick(); }

core::Result<std::string> ScriptedTacticalSession::script_state_hash() {
    const sim::InlineExecutor executor;
    return script_state_hash(executor);
}

core::Result<std::string> ScriptedTacticalSession::script_state_hash(const sim::PartitionExecutor& executor) {
    if (impl_->failure) return core::Result<std::string>::failure(*impl_->failure);
    if (impl_->pending_native) return core::Result<std::string>::failure(make_error(codes::load_rejected, "native child state needs its owner"));
    auto lua = impl_->scripts.state_hash(executor);
    if (!lua || !impl_->engine) return lua;
    auto native = impl_->engine->save_native(impl_->world, impl_->scripts);
    if (!native) return core::Result<std::string>::failure(native.error());
    if (!native.value()) return lua;
    const auto combined = "eawr-tactical-host-v2\n" + lua.value() + "\n" + core::sha256_hex(*native.value());
    return core::Result<std::string>::success(core::sha256_hex(std::span(
        reinterpret_cast<const std::uint8_t*>(combined.data()), combined.size())));
}

std::size_t ScriptedTacticalSession::pending_script_commands() const noexcept { return impl_->pending.size(); }

tactical::TacticalReplay ScriptedTacticalSession::record() const { return impl_->world.record(); }
void ScriptedTacticalSession::scramble_storage_for_testing() { impl_->world.scramble_storage_for_testing(); }

} // namespace eawr::script::authoritative
