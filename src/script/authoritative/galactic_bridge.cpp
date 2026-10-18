#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/core/gc_profile.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "galactic_api_inventory.hpp"

#include <algorithm>
#include <map>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace eawr::script::authoritative {
namespace {

namespace gc = sim::galactic;
namespace b64 = numeric::binary64;

core::Diagnostic error(std::string_view code, std::string message) {
    core::Diagnostic out;
    out.code = std::string(code);
    out.message = std::move(message);
    return out;
}

std::string hash(std::string_view text) {
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// Length-framed textual fields, including explicit value type names. The GC
// bridge hash includes its outbound queue and routing cursors, not only Lua.
void field(std::string& out, std::string_view text) {
    out += std::to_string(text.size()) + ":";
    out += text;
}
void value(std::string& out, const Value& item) {
    std::visit([&](const auto& part) {
        using T = std::decay_t<decltype(part)>;
        if constexpr (std::is_same_v<T, std::monostate>) field(out, "nil");
        else if constexpr (std::is_same_v<T, bool>) field(out, part ? "true" : "false");
        else if constexpr (std::is_same_v<T, numeric::LuaNumber>) {
            field(out, "number"); field(out, std::to_string(part.repr));
        } else if constexpr (std::is_same_v<T, std::string>) {
            field(out, "string"); field(out, part);
        } else if constexpr (std::is_same_v<T, Handle>) {
            field(out, "handle"); field(out, std::to_string(part.kind)); field(out, std::to_string(part.id));
        } else {
            field(out, "list"); field(out, std::to_string(part.size()));
            for (const auto& child : part) value(out, child);
        }
    }, item.data);
}

bool unique_instances(const ScriptScheduler& ai, const ScriptScheduler& story) {
    const auto a = ai.instances();
    const auto s = story.instances();
    for (const auto id : s) if (std::binary_search(a.begin(), a.end(), id)) return false;
    return true;
}

Binding missing(std::string name) {
    return [name = std::move(name)](BindingContext&, const ValueList&) {
        return core::Result<ValueList>::failure(error(codes::missing_api, "galactic Lua API " + name + " is unbound"));
    };
}

} // namespace

struct ScriptedGalacticSession::Impl {
    gc::GalacticSession world;
    ScriptScheduler ai;
    ScriptScheduler story;
    std::shared_ptr<GalacticScriptEngine> ai_engine;
    std::shared_ptr<GalacticScriptEngine> story_engine;
    std::shared_ptr<sim::StateHasher> hasher;
    std::map<std::string, GalacticCommandTranslator, std::less<>> translators;
    std::map<gc::SeatId, std::uint64_t> next_sequence;
    std::set<gc::SeatId> exhausted_seats;
    std::vector<ScriptCommand> pending;
    std::optional<core::Diagnostic> failure;
    bool stepped{};

    Impl(gc::GalacticSession world_in, ScriptScheduler ai_in, ScriptScheduler story_in)
        : world(std::move(world_in)), ai(std::move(ai_in)), story(std::move(story_in)) {
        ai.set_managed_handle_roots([this] {
            ValueList roots;
            for (const auto& command : pending) roots.insert(roots.end(), command.arguments.begin(), command.arguments.end());
            return roots;
        });
    }

    RoutedGalacticCommand route(ScriptCommand command) {
        RoutedGalacticCommand out;
        out.command = std::move(command);
        const auto found = translators.find(out.command.verb);
        auto refuse = [&](std::string message) {
            out.dropped = error(codes::command_unroutable, "galactic script command " + out.command.verb +
                " (instance " + std::to_string(out.command.issuer) + "): " + message);
        };
        if (found == translators.end()) {
            refuse("no translator");
            return out;
        }
        auto translated = [&]() -> core::Result<GalacticOrder> {
            try { return found->second(out.command); }
            catch (...) { return core::Result<GalacticOrder>::failure(error(codes::command_unroutable, "translator threw")); }
        }();
        if (!translated) { refuse(translated.error().code + " " + translated.error().message); return out; }
        const auto& order = translated.value();
        const auto& seats = world.setup().seats;
        const auto seat = std::find_if(seats.begin(), seats.end(), [&](const gc::Seat& entry) { return entry.seat_id == order.seat; });
        if (seat == seats.end() || seat->kind == gc::SeatKind::human) {
            refuse("script orders require an AI or script seat");
            return out;
        }
        // Input and both script hosts share the seat's routing cursor.
        if (exhausted_seats.contains(order.seat)) { refuse("seat sequence is exhausted"); return out; }
        const auto sequence = next_sequence.contains(order.seat) ? next_sequence.at(order.seat) : 0;
        gc::GameCommand input{{world.completed_tick(), order.seat, sequence}, order.payload};
        auto admitted = world.submit(input);
        if (!admitted) { refuse(admitted.error().code + " " + admitted.error().message); return out; }
        accepted(input.key);
        out.key = input.key;
        out.accepted = input;
        return out;
    }

    void accepted(const gc::CommandOrderKey& key) {
        if (key.sequence == std::numeric_limits<std::uint64_t>::max()) {
            exhausted_seats.insert(key.seat);
            next_sequence[key.seat] = key.sequence;
        } else {
            next_sequence[key.seat] = std::max(next_sequence[key.seat], key.sequence + 1);
        }
    }

    core::Result<ServiceReport> service(const sim::PartitionExecutor& executor, const gc::GalacticTick& tick,
        ScriptScheduler& scripts, const std::shared_ptr<GalacticScriptEngine>& engine, const bool is_story) {
        if (auto clock = scripts.set_galactic_frame(world.clock().galactic_frame); !clock)
            return core::Result<ServiceReport>::failure(clock.error());
        ServiceOptions options;
        if (engine) {
            auto prepared = engine->before_service(executor, world, tick, scripts);
            if (!prepared) return core::Result<ServiceReport>::failure(prepared.error());
            options = std::move(prepared).value();
        }
        // The mode clock is the just-stepped galactic frame, while scheduler
        // ticks remain service/RNG/event keys. Story therefore sees frozen
        // time at successive paused command barriers (ADR-013, WST-12).
        options.time_tick = world.clock().galactic_frame;
        if (is_story) {
            const auto ids = scripts.instances();
            const auto now = time_at_tick(world.clock().galactic_frame, scripts.tick_duration());
            for (const auto id : ids) {
                auto rate = scripts.read_global(id, "ServiceRate");
                auto last = scripts.read_global(id, "LastService");
                if (!rate) return core::Result<ServiceReport>::failure(rate.error());
                if (!last) return core::Result<ServiceReport>::failure(last.error());
                const auto* r = rate.value() ? std::get_if<numeric::LuaNumber>(&rate.value()->data) : nullptr;
                const auto* l = last.value() ? std::get_if<numeric::LuaNumber>(&last.value()->data) : nullptr;
                // WST-54: strict elapsed > rate when both globals are numbers;
                // a missing/nonnumeric global removes the throttle.
                if (r && l && b64::less_equal(b64::subtract(now.repr, l->repr), r->repr)) continue;
                ScriptEvent pump;
                pump.key = {scripts.completed_tick() + 1, galactic_story_producer, id, 0};
                pump.target = id;
                pump.kind = ScriptEvent::Kind::pump;
                if (auto posted = scripts.submit_event(std::move(pump)); !posted)
                    return core::Result<ServiceReport>::failure(posted.error());
                ScriptEvent stamp;
                stamp.key = {scripts.completed_tick() + 1, galactic_story_producer, id, 1};
                stamp.target = id;
                stamp.kind = ScriptEvent::Kind::assign;
                stamp.name = "LastService";
                stamp.parameter = Value::number(now);
                if (auto posted = scripts.submit_event(std::move(stamp)); !posted)
                    return core::Result<ServiceReport>::failure(posted.error());
            }
            // Host pacing changes thread pumping only; engine events/callbacks
            // still reach throttled and suspended attached plots (WST-16).
            options.host_paced = [](std::uint64_t) { return true; };
        }
        auto report = scripts.service(executor, options);
        if (!report) return report;
        if (engine) {
            if (auto taken = engine->after_service(report.value()); !taken)
                return core::Result<ServiceReport>::failure(taken.error());
        }
        if (is_story) {
            for (const auto id : scripts.instances()) {
                auto slots = scripts.thread_slots(id);
                if (!slots) return core::Result<ServiceReport>::failure(slots.error());
                if (std::any_of(slots.value().begin(), slots.value().end(), [](bool live) { return live; })) continue;
                if (auto removed = scripts.remove_instance(id); !removed)
                    return core::Result<ServiceReport>::failure(removed.error());
                report.value().removed_instances.push_back(id);
            }
            std::sort(report.value().removed_instances.begin(), report.value().removed_instances.end());
        }
        return report;
    }

    core::Result<std::string> script_hash(bool sync_ai = false, const sim::PartitionExecutor& executor = sim::InlineExecutor{}) {
        auto a = sync_ai ? ai.sync_hash(executor, ai_engine && ai_engine->verifies_sync_hash()) : ai.state_hash();
        auto s = story.state_hash();
        if (!a) return core::Result<std::string>::failure(a.error());
        if (!s) return core::Result<std::string>::failure(s.error());
        std::string bytes;
        field(bytes, sync_ai ? "eawr-script-galactic-sync-state-v1" : "eawr-script-galactic-state-v1");
        field(bytes, "ai"); field(bytes, a.value());
        field(bytes, "story"); field(bytes, s.value());
        field(bytes, std::to_string(next_sequence.size()));
        for (const auto& [seat, sequence] : next_sequence) {
            field(bytes, std::to_string(seat.value)); field(bytes, std::to_string(sequence));
        }
        field(bytes, std::to_string(exhausted_seats.size()));
        for (const auto seat : exhausted_seats) field(bytes, std::to_string(seat.value));
        field(bytes, std::to_string(pending.size()));
        for (const auto& command : pending) {
            field(bytes, std::to_string(command.tick)); field(bytes, std::to_string(command.issuer));
            field(bytes, std::to_string(command.sequence)); field(bytes, command.verb);
            value(bytes, Value{command.arguments});
        }
        return core::Result<std::string>::success(hash(bytes));
    }
};

core::Result<ScriptedGalacticSession> ScriptedGalacticSession::create(
    gc::GalacticSession world, ScriptScheduler ai, ScriptScheduler story) {
    if (world.completed_tick() != 0 || world.pending_command_count() != 0 || ai.completed_tick() != 0 || story.completed_tick() != 0 ||
        !ai.uses_galactic_clock() || !story.uses_galactic_clock() || !unique_instances(ai, story)) {
        return core::Result<ScriptedGalacticSession>::failure(error(codes::invalid_request,
            "fresh GC-profile schedulers with disjoint AI/story instance IDs and a fresh galaxy are required"));
    }
    return core::Result<ScriptedGalacticSession>::success(
        ScriptedGalacticSession(std::make_unique<Impl>(std::move(world), std::move(ai), std::move(story))));
}

ScriptedGalacticSession::ScriptedGalacticSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScriptedGalacticSession::ScriptedGalacticSession(ScriptedGalacticSession&&) noexcept = default;
ScriptedGalacticSession& ScriptedGalacticSession::operator=(ScriptedGalacticSession&&) noexcept = default;
ScriptedGalacticSession::~ScriptedGalacticSession() = default;

core::Result<ScriptedGalacticSession> ScriptedGalacticSession::restore(
    gc::GalacticSession world, ScriptScheduler ai, ScriptScheduler story, const GalacticScriptSave& saved) {
    return restore_preserving(world, std::move(ai), std::move(story), saved);
}
core::Result<ScriptedGalacticSession> ScriptedGalacticSession::restore_preserving(
    gc::GalacticSession& world, ScriptScheduler ai, ScriptScheduler story, const GalacticScriptSave& saved,
    const bool conflict_paused) {
    auto refuse = [](std::string message) {
        return core::Result<ScriptedGalacticSession>::failure(error(codes::load_rejected, std::move(message)));
    };
    if (!ai.uses_galactic_clock() || !story.uses_galactic_clock() || world.completed_tick() != saved.completed_tick ||
        world.clock().galactic_frame != saved.galactic_frame || world.state_sha256() != saved.world_sha256)
        return refuse("the galaxy and script save must belong to the same GC clock barrier");
    if (auto loaded = ai.load(saved.ai); !loaded) return refuse(loaded.error().message);
    if (auto loaded = story.load(saved.story); !loaded) return refuse(loaded.error().message);
    // Tactical child return reconciles the galaxy to the shared session frontier
    // without servicing these frozen schedulers (GameSession::step / reconcile_battle).
    // Story service ticks may therefore lag that frontier, but never lead it.
    // GFS-15/GFS-33: the campaign's outer mode tail advances the parent timer after both hosts serviced
    // the tick (it runs for conflict services or a victory monitor; campaign setup always installs the
    // monitor), so a barrier's schedulers hold the frame they last served, one tail advance behind the
    // galaxy's. A galaxy without a monitor has no such tail here and keeps the strict frame equality
    // (a tail installed only for pending hero returns leaves the timer with the galaxy step).
    const std::uint64_t tail_lag = world.setup().victory ? 1 : 0;
    const auto served_frame = [&](const std::uint64_t frame) {
        return frame == saved.galactic_frame || (tail_lag != 0 && frame + tail_lag == saved.galactic_frame);
    };
    // A conflict admitted inside a hosted tick pauses the galaxy after that tick's AI service and before the outer
    // frame timer, so while the pause is pending the AI host has served at least one frame more than the galaxy has
    // counted, all stamped with the barrier frame. A paused tick services no AI unless the engine admits one (the
    // retained manager's auto-resolve planning), so the lead is not capped at one; it is capped by one service per
    // elapsed tick. The pause resumes with a timer advance that closes the first frame of the lead.
    const bool paused_service_lead = conflict_paused && world.clock().pause_depth != 0 &&
        ai.galactic_frame() == saved.galactic_frame && ai.completed_tick() > saved.galactic_frame &&
        ai.completed_tick() <= saved.completed_tick;
    if ((ai.completed_tick() != saved.galactic_frame && !paused_service_lead) ||
        story.completed_tick() > saved.completed_tick || !served_frame(ai.galactic_frame()) ||
        !served_frame(story.galactic_frame()) || !unique_instances(ai, story))
        return refuse("saved AI/story service clocks or instance identities do not match the barrier");
    for (const auto& [id, sequence] : saved.next_sequence) {
        static_cast<void>(sequence);
        const auto& seats = world.setup().seats;
        if (std::none_of(seats.begin(), seats.end(), [&](const gc::Seat& seat) { return seat.seat_id == id; }))
            return refuse("a saved routing cursor names an unknown seat");
    }
    for (const auto id : saved.exhausted_seats) {
        if (!saved.next_sequence.contains(id) || saved.next_sequence.at(id) != std::numeric_limits<std::uint64_t>::max())
            return refuse("a saved exhausted seat has no matching maximum sequence");
    }
    if (!std::is_sorted(saved.pending.begin(), saved.pending.end(), [](const ScriptCommand& a, const ScriptCommand& b) {
            return std::pair(a.issuer, a.sequence) < std::pair(b.issuer, b.sequence);
        })) return refuse("saved script orders are not in canonical commit order");
    for (std::size_t index = 0; index < saved.pending.size(); ++index) {
        const auto& command = saved.pending[index];
        if (command.issuer == 0 || command.verb.empty() || (index != 0 &&
            command.issuer == saved.pending[index - 1].issuer && command.sequence == saved.pending[index - 1].sequence))
            return refuse("saved script orders have an empty verb, invalid issuer or duplicate key");
    }
    auto shell = gc::GalacticSession::create(world.setup());
    if (!shell) return refuse(shell.error().message);
    auto impl = std::make_unique<Impl>(std::move(shell).value(), std::move(ai), std::move(story));
    impl->pending = saved.pending;
    impl->next_sequence = saved.next_sequence;
    impl->exhausted_seats = saved.exhausted_seats;
    auto digest = impl->script_hash();
    if (!digest || digest.value() != saved.scripts_sha256) return refuse("saved script barrier digest differs");
    impl->world = std::move(world);
    return core::Result<ScriptedGalacticSession>::success(ScriptedGalacticSession(std::move(impl)));
}

core::Result<void> ScriptedGalacticSession::register_verb(std::string_view verb, GalacticCommandTranslator translator) {
    if (impl_->stepped || verb.empty() || !translator || !impl_->translators.emplace(std::string(verb), std::move(translator)).second)
        return core::Result<void>::failure(error(codes::invalid_request, "register each named translator once before stepping"));
    return core::Result<void>::success();
}
core::Result<void> ScriptedGalacticSession::set_ai_engine(std::shared_ptr<GalacticScriptEngine> engine) {
    if (impl_->stepped) return core::Result<void>::failure(error(codes::invalid_request, "set the engine before stepping"));
    impl_->ai_engine = std::move(engine);
    return core::Result<void>::success();
}
core::Result<void> ScriptedGalacticSession::set_story_engine(std::shared_ptr<GalacticScriptEngine> engine) {
    if (impl_->stepped) return core::Result<void>::failure(error(codes::invalid_request, "set the engine before stepping"));
    impl_->story_engine = std::move(engine);
    return core::Result<void>::success();
}

core::Result<ScriptedGalacticTick> ScriptedGalacticSession::step(
    const sim::PartitionExecutor& executor, std::span<const gc::GameCommand> input, bool local_conflict_pending) {
    Impl& impl = *impl_;
    if (impl.failure) return core::Result<ScriptedGalacticTick>::failure(*impl.failure);
    impl.stepped = true;
    auto abort = [&](core::Diagnostic diagnostic) {
        impl.failure = std::move(diagnostic);
        return core::Result<ScriptedGalacticTick>::failure(*impl.failure);
    };
    try {
        ScriptedGalacticTick out;
        for (const auto& command : input) {
            if (auto admitted = impl.world.submit(command); !admitted) out.refused_input.push_back(admitted.error());
            else impl.accepted(command.key);
        }
        auto pending = std::move(impl.pending);
        impl.pending.clear();
        for (auto& command : pending) out.script_input.push_back(impl.route(std::move(command)));
        auto stepped = gc_profile::timed(gc_profile::Phase::tick_galaxy_step, [&] { return impl.world.step(executor, local_conflict_pending); });
        if (!stepped) return abort(stepped.error());
        out.world = std::move(stepped).value();
        if (impl.ai_engine ? impl.ai_engine->admits_service(out.world) : out.world.work.serviced) {
            auto serviced = gc_profile::timed_ai([&] { return impl.service(executor, out.world, impl.ai, impl.ai_engine, false); });
            if (!serviced) return abort(serviced.error());
            out.ai = std::move(serviced).value();
            impl.pending = out.ai->commands;
        }
        auto serviced = gc_profile::timed(gc_profile::Phase::tick_story_service, [&] { return impl.service(executor, out.world, impl.story, impl.story_engine, true); });
        if (!serviced) return abort(serviced.error());
        out.story = std::move(serviced).value();
        if (!unique_instances(impl.ai, impl.story))
            return abort(error(codes::invalid_request, "AI and story instance IDs must be disjoint"));
        impl.pending.insert(impl.pending.end(), out.story.commands.begin(), out.story.commands.end());
        std::sort(impl.pending.begin(), impl.pending.end(), [](const ScriptCommand& a, const ScriptCommand& b) {
            return std::pair(a.issuer, a.sequence) < std::pair(b.issuer, b.sequence);
        });
        gc_profile::Scope profile_scope(gc_profile::Phase::scripted_hash_wall);
        auto scripts = impl.script_hash(impl.ai_engine && impl.ai_engine->uses_sync_hash(), executor);
        if (!scripts) return abort(scripts.error());
        const auto derive = [tick = completed_tick(), script = std::move(scripts).value()](const std::string& world_hash) {
            return authoritative_state_sha256(tick, world_hash, script);
        };
        // Native admissions may dirty account/production/force sections after
        // the world step. Reuse clean sections, but include those final writes.
        const auto world_hash = impl.world.cached_state_hash();
        out.state_hash = impl.hasher ? impl.hasher->derive(world_hash, derive)
                                    : sim::StateHash(derive(world_hash.get()));
        return core::Result<ScriptedGalacticTick>::success(std::move(out));
    } catch (...) {
        return abort(error(codes::session_abort, "the galactic script host threw an exception"));
    }
}

void ScriptedGalacticSession::set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept {
    impl_->world.set_state_hasher(hasher);
    impl_->hasher = std::move(hasher);
}
gc::GalacticSession& ScriptedGalacticSession::parent_world() noexcept { return impl_->world; }
const gc::GalacticSession& ScriptedGalacticSession::world() const noexcept { return impl_->world; }
ScriptScheduler& ScriptedGalacticSession::ai_scheduler() noexcept { return impl_->ai; }
std::uint64_t ScriptedGalacticSession::completed_tick() const noexcept { return impl_->world.completed_tick(); }
std::size_t ScriptedGalacticSession::pending_script_commands() const noexcept { return impl_->pending.size(); }
gc::GalacticReplay ScriptedGalacticSession::record() const { return impl_->world.record(); }
core::Result<std::string> ScriptedGalacticSession::script_state_hash() { return impl_->script_hash(); }
core::Result<std::string> ScriptedGalacticSession::script_sync_hash() { return impl_->script_hash(true); }
bool ScriptedGalacticSession::uses_sync_hash() const noexcept { return impl_->ai_engine && impl_->ai_engine->uses_sync_hash(); }
core::Result<std::string> ScriptedGalacticSession::save_ai() { return impl_->ai.save(); }
core::Result<std::string> ScriptedGalacticSession::save_story() { return impl_->story.save(); }
core::Result<GalacticScriptSave> ScriptedGalacticSession::save_scripts() {
    if (impl_->failure) return core::Result<GalacticScriptSave>::failure(*impl_->failure);
    auto a = impl_->ai.save();
    if (!a) return core::Result<GalacticScriptSave>::failure(a.error());
    auto s = impl_->story.save();
    if (!s) return core::Result<GalacticScriptSave>::failure(s.error());
    auto digest = impl_->script_hash();
    if (!digest) return core::Result<GalacticScriptSave>::failure(digest.error());
    return core::Result<GalacticScriptSave>::success({completed_tick(), impl_->world.clock().galactic_frame,
        std::move(a).value(), std::move(s).value(), impl_->pending, impl_->next_sequence, impl_->exhausted_seats,
        impl_->world.state_sha256(), std::move(digest).value()});
}

core::Result<void> register_galactic_orbit_recheck_method(ScriptScheduler& scripts, std::uint32_t kind) {
    return scripts.register_method(kind, "Force_Test_Space_Conflict", [kind](BindingContext& context, const ValueList& args) {
        if (args.empty()) return core::Result<ValueList>::failure(error(codes::invalid_request, "orbit recheck needs its receiver"));
        const auto* target = std::get_if<Handle>(&args[0].data);
        if (!target || target->kind != kind || !target->id)
            return core::Result<ValueList>::failure(error(codes::invalid_request, "orbit recheck needs an object handle"));
        context.issue_command("galactic.orbit_recheck", {args[0]});
        return core::Result<ValueList>::success({});
    });
}
core::Result<void> ScriptedGalacticSession::bind_orbit_recheck(gc::SeatId seat) {
    const auto& seats = impl_->world.setup().seats;
    const auto selected = std::find_if(seats.begin(), seats.end(), [seat](const auto& row) { return row.seat_id == seat; });
    if (selected == seats.end() || selected->kind != gc::SeatKind::script)
        return core::Result<void>::failure(error(codes::invalid_request, "orbit recheck requires a script seat"));
    return register_verb("galactic.orbit_recheck", [seat](const ScriptCommand& command) {
        const auto* target = command.arguments.size() == 1 ? std::get_if<Handle>(&command.arguments[0].data) : nullptr;
        if (!target || !target->id)
            return core::Result<GalacticOrder>::failure(error(codes::invalid_request, "orbit recheck command needs its target"));
        return core::Result<GalacticOrder>::success({seat, gc::RecheckOrbit{target->id}});
    });
}

core::Result<void> register_missing_galactic_api(ScriptScheduler& scripts, std::span<const std::string_view> implemented) {
    for (const auto name : detail::missing_galactic_globals) {
        if (std::find(implemented.begin(), implemented.end(), name) != implemented.end()) continue;
        if (auto registered = scripts.register_binding(name, missing(std::string(name))); !registered) return registered;
    }
    return core::Result<void>::success();
}
core::Result<void> register_missing_galactic_method(ScriptScheduler& scripts, std::uint32_t kind, std::string_view method) {
    return scripts.register_method(kind, method, missing(std::string(method)));
}

} // namespace eawr::script::authoritative
