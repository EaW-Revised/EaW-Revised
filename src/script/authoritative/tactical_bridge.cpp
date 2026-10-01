#include "eawr/script/authoritative/tactical_bridge.hpp"

#include <algorithm>
#include <optional>
#include <utility>

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

struct ScriptedTacticalSession::Impl {
    tactical::TacticalSession world;
    ScriptScheduler scripts;
    std::map<std::string, CommandTranslator, std::less<>> translators;
    std::shared_ptr<ScriptEngine> engine;
    std::shared_ptr<sim::StateHasher> hasher; // #637: null hashes on the stepping thread
    bool authoritative_hash{true};            // #895: off, the tick's combined hash stays empty
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
        tactical::PlayerCommand player_command;
        player_command.key = {tick, order.value().issuer, next_sequence(order.value().issuer)};
        player_command.units = std::move(order.value().units);
        player_command.payload = std::move(order.value().payload);
        if (auto submitted = world.submit(player_command); !submitted) {
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
    impl_->engine = std::move(engine);
    return core::Result<void>::success();
}

core::Result<ScriptedTick> ScriptedTacticalSession::step(
    const sim::PartitionExecutor& executor, std::span<const tactical::PlayerCommand> player_input) {
    Impl& impl = *impl_;
    if (impl.failure) return core::Result<ScriptedTick>::failure(*impl.failure);
    impl.stepped = true;
    ScriptedTick out;
    const std::uint64_t tick = impl.world.completed_tick();
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

    auto world = impl.world.step(executor);
    if (!world) {
        impl.failure = world.error();
        return core::Result<ScriptedTick>::failure(world.error());
    }
    out.world = std::move(world).value();
    ServiceOptions options;
    if (impl.engine) {
        auto prepared = [&]() -> core::Result<ServiceOptions> {
            try {
                return impl.engine->before_service(impl.world, out.world, impl.scripts);
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
    auto service = impl.scripts.service(executor, options);
    if (!service) {
        impl.failure = service.error();
        return core::Result<ScriptedTick>::failure(service.error());
    }
    out.scripts = std::move(service).value();
    if (impl.engine) {
        auto taken = [&]() -> core::Result<void> {
            try {
                return impl.engine->after_service(out.scripts);
            } catch (...) {
                return core::Result<void>::failure(make_error(codes::session_abort, "the script engine threw an exception"));
            }
        }();
        if (!taken) {
            impl.failure = taken.error();
            return core::Result<ScriptedTick>::failure(taken.error());
        }
    }
    impl.pending = out.scripts.commands;
    if (!impl.authoritative_hash) return core::Result<ScriptedTick>::success(std::move(out));
    auto script_hash = impl.scripts.state_hash();
    if (!script_hash) {
        impl.failure = script_hash.error();
        return core::Result<ScriptedTick>::failure(script_hash.error());
    }
    if (impl.hasher) {
        // #637: the world's hash is still being computed; combine it on the hasher after it.
        out.state_hash = impl.hasher->derive(out.world.state_hash,
            [tick = out.world.completed_tick, script = std::move(script_hash).value()](const std::string& world) {
                return authoritative_state_sha256(tick, world, script);
            });
    } else {
        out.state_sha256 = authoritative_state_sha256(out.world.completed_tick, out.world.state_sha256, script_hash.value());
        out.state_hash = sim::StateHash(out.state_sha256);
    }
    return core::Result<ScriptedTick>::success(std::move(out));
}

void ScriptedTacticalSession::set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept {
    impl_->world.set_state_hasher(hasher);
    impl_->hasher = std::move(hasher);
}

void ScriptedTacticalSession::set_authoritative_hash(const bool every_tick) noexcept {
    impl_->authoritative_hash = every_tick;
}

const tactical::TacticalSession& ScriptedTacticalSession::world() const noexcept { return impl_->world; }

std::uint64_t ScriptedTacticalSession::completed_tick() const noexcept { return impl_->world.completed_tick(); }

core::Result<std::string> ScriptedTacticalSession::script_state_hash() { return impl_->scripts.state_hash(); }

std::size_t ScriptedTacticalSession::pending_script_commands() const noexcept { return impl_->pending.size(); }

tactical::TacticalReplay ScriptedTacticalSession::record() const { return impl_->world.record(); }

} // namespace eawr::script::authoritative
