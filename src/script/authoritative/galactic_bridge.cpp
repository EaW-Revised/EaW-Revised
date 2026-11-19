#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/core/gc_profile.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "galactic_api_inventory.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

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

// The per-tick form: the AI side comes from the sim-side copy of its ids (see Impl::ai_ids), not from the AI scheduler.
bool unique_instances(const std::vector<std::uint64_t>& ai_ids, const ScriptScheduler& other) {
    const auto s = other.instances();
    for (const auto id : s) if (std::binary_search(ai_ids.begin(), ai_ids.end(), id)) return false;
    return true;
}

Binding missing(std::string name) {
    return [name = std::move(name)](BindingContext&, const ValueList&) {
        return core::Result<ValueList>::failure(error(codes::missing_api, "galactic Lua API " + name + " is unbound"));
    };
}

} // namespace

core::Result<void> register_galactic_first_object(ScriptScheduler& scripts,
    const std::vector<std::pair<std::string, gc::TypeId>>& types,
    const std::vector<std::pair<std::string, gc::PlanetId>>& planets, GalacticSnapshotReader reader) {
    const auto normalize = [](std::string name) {
        for (char& c : name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
        return name;
    };
    std::map<std::string, gc::TypeId> names;
    for (const auto& [name, id] : types) names.try_emplace(normalize(name), id);
    std::map<std::string, gc::PlanetId> locations;
    for (const auto& [name, id] : planets) {
        const auto [it, inserted] = locations.try_emplace(normalize(name), id);
        if (!inserted) it->second = std::min(it->second, id);
    }
    return scripts.register_binding("Find_First_Object", [names = std::move(names), locations = std::move(locations),
        reader = std::move(reader), normalize](BindingContext& context, const ValueList& arguments) {
        using Result = core::Result<ValueList>;
        if (arguments.empty() || !std::holds_alternative<std::string>(arguments.front().data)) {
            context.report(codes::invalid_request, "Find_First_Object requires a string first argument");
            context.request_exit();
            return Result::success({});
        }
        const auto name = normalize(std::get<std::string>(arguments.front().data));
        const auto snapshot = reader ? reader() : nullptr;
        sim::EntityId selected{};
        const auto consider = [&](sim::EntityId id) { if (id && (!selected || id < selected)) selected = id; };
        if (const auto planet = locations.find(name); planet != locations.end()) {
            if (!snapshot || std::any_of(snapshot->planets().begin(), snapshot->planets().end(),
                [&](const auto& row) { return row.id == planet->second; })) consider(planet->second);
        }
        if (snapshot) if (const auto type = names.find(name); type != names.end()) {
            // Conservative ordering: lowest live identity; retail duplicate ordering remains an open gap.
            for (const auto& object : snapshot->actor_records())
                if (!object.dead && !object.delete_pending && object.current_type == type->second) consider(object.id);
        }
        if (!selected) return Result::success({});
        return Result::success({Value{Handle{foc::handle_game_object, selected}}});
    });
}

struct ScriptedGalacticSession::Impl {
    gc::GalacticSession world;
    ScriptScheduler ai;
    ScriptScheduler story;
    std::optional<ScriptScheduler> objects;
    std::shared_ptr<GalacticScriptEngine> ai_engine;
    std::shared_ptr<GalacticScriptEngine> story_engine;
    std::shared_ptr<GalacticObjectScriptEngine> object_engine;
    std::shared_ptr<sim::StateHasher> hasher;
    std::map<std::string, GalacticCommandTranslator, std::less<>> translators;
    std::map<gc::SeatId, std::uint64_t> next_sequence;
    std::set<gc::SeatId> exhausted_seats;
    std::vector<ScriptCommand> pending;
    std::optional<core::Diagnostic> failure;
    std::function<void(ServiceReport&)> global_value_committer;
    std::function<core::Result<void>(ServiceReport&)> story_command_committer;
    bool stepped{};
    // Async AI step 2c-3 (R3): the sim-side copy of the AI scheduler's instance ids for the per-tick disjointness checks. The AI scheduler
    // owns its instance set; it changes only inside the AI service (window body), at load and when a host creates or removes an instance,
    // all of which move `instance_revision`. The copy is read again from the scheduler only when the revision moved; every other tick
    // reads one scalar and no id list. `ai_instance_list_reads` counts those re-reads.
    std::vector<std::uint64_t> ai_ids_copy;
    std::uint64_t ai_ids_revision{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t ai_instance_list_reads{};
    const std::vector<std::uint64_t>& ai_ids() {
        if (const auto revision = ai.instance_revision(); revision != ai_ids_revision) {
            ai_ids_copy = ai.instances(); ai_ids_revision = revision; ++ai_instance_list_reads;
        }
        return ai_ids_copy;
    }

    Impl(gc::GalacticSession world_in, ScriptScheduler ai_in, ScriptScheduler story_in,
        std::optional<ScriptScheduler> objects_in)
        : world(std::move(world_in)), ai(std::move(ai_in)), story(std::move(story_in)), objects(std::move(objects_in)) {
        world.defer_history_service(true);
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
        const auto run = [&] { return service_once(executor, tick, scripts, engine, is_story); };
        auto result = engine ? engine->service_phase(executor, world, tick, scripts, run) : run();
        if (result) world.add_runtime_load_notes(result.value().load_notes);
        return result;
    }

    core::Result<ServiceReport> service_once(const sim::PartitionExecutor& executor, const gc::GalacticTick& tick,
        ScriptScheduler& scripts, const std::shared_ptr<GalacticScriptEngine>& engine, const bool is_story) {
        const auto frame = world.clock().galactic_frame + (engine && !is_story ? engine->frame_offset() : 0);
        if (auto clock = scripts.set_galactic_frame(frame); !clock)
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
        options.time_tick = frame;
        if (is_story) {
            const auto ids = scripts.instances();
            // WST-54: the due test reads the frame synchronizer's elapsed seconds (debug build: its per-frame advance has no
            // pause test), not the mode clock. So a paused galaxy keeps pumping sub-plot scripts' main threads while their
            // threads sleeping on mode time (GetCurrentTime, below) stand still: the gap the pause dance of the filter scripts
            // works around. Not recorded in a paused retail game yet (GFSC-14). One service per step; the scheduler's own
            // tick is the frame. Differences from the mode clock, all deterministic and saved: in a campaign the frame timer
            // advances after the scripts have served the step, so every LastService stamp is one tick larger than before this
            // change (the pump schedule is the same while the galaxy runs, the story script state hash is not); and every
            // paused step that still serves the story widens the gap, so the pump phase differs after a game's first pause.
            const auto now = time_at_tick(scripts.completed_tick() + 1, scripts.tick_duration());
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
        if (is_story && global_value_committer) global_value_committer(report.value());
        if (is_story && story_command_committer) {
            if (auto committed = story_command_committer(report.value()); !committed)
                return core::Result<ServiceReport>::failure(committed.error());
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
        const auto published = sync_ai && ai_engine ? ai_engine->published_ai_sync_hash() : std::nullopt;
        auto a = published ? core::Result<std::string>::success(*published)
            : sync_ai ? ai.sync_hash(executor, ai_engine && ai_engine->verifies_sync_hash()) : ai.state_hash();
        auto s = story.state_hash();
        if (!a) return core::Result<std::string>::failure(a.error());
        if (!s) return core::Result<std::string>::failure(s.error());
        std::string bytes;
        field(bytes, sync_ai ? "eawr-script-galactic-sync-state-v2" : "eawr-script-galactic-state-v1");
        field(bytes, "ai"); field(bytes, a.value());
        field(bytes, "story"); field(bytes, s.value());
        if (objects) {
            auto object_hash = objects->state_hash();
            if (!object_hash) return core::Result<std::string>::failure(object_hash.error());
            field(bytes, "objects"); field(bytes, object_hash.value());
        }
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
    gc::GalacticSession world, ScriptScheduler ai, ScriptScheduler story, std::optional<ScriptScheduler> objects) {
    if (world.completed_tick() != 0 || world.pending_command_count() != 0 || ai.completed_tick() != 0 || story.completed_tick() != 0 ||
        !ai.uses_galactic_clock() || !story.uses_galactic_clock() || !unique_instances(ai, story) ||
        (objects && (objects->completed_tick() != 0 || !objects->uses_galactic_clock()
            || !unique_instances(ai, *objects) || !unique_instances(story, *objects)))) {
        return core::Result<ScriptedGalacticSession>::failure(error(codes::invalid_request,
            "fresh GC-profile schedulers with disjoint AI/story instance IDs and a fresh galaxy are required"));
    }
    return core::Result<ScriptedGalacticSession>::success(
        ScriptedGalacticSession(std::make_unique<Impl>(std::move(world), std::move(ai), std::move(story), std::move(objects))));
}

ScriptedGalacticSession::ScriptedGalacticSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScriptedGalacticSession::ScriptedGalacticSession(ScriptedGalacticSession&&) noexcept = default;
ScriptedGalacticSession& ScriptedGalacticSession::operator=(ScriptedGalacticSession&&) noexcept = default;
ScriptedGalacticSession::~ScriptedGalacticSession() = default;

core::Result<ScriptedGalacticSession> ScriptedGalacticSession::restore(
    gc::GalacticSession world, ScriptScheduler ai, ScriptScheduler story, const GalacticScriptSave& saved,
    std::optional<ScriptScheduler> objects) {
    return restore_preserving(world, std::move(ai), std::move(story), saved, false, std::move(objects));
}
core::Result<ScriptedGalacticSession> ScriptedGalacticSession::restore_preserving(
    gc::GalacticSession& world, ScriptScheduler ai, ScriptScheduler story, const GalacticScriptSave& saved,
    const bool timer_held, std::optional<ScriptScheduler> objects) {
    auto refuse = [](std::string message) {
        return core::Result<ScriptedGalacticSession>::failure(error(codes::load_rejected, std::move(message)));
    };
    if (!ai.uses_galactic_clock() || !story.uses_galactic_clock() || world.completed_tick() != saved.completed_tick ||
        world.clock().galactic_frame != saved.galactic_frame || world.state_sha256() != saved.world_sha256)
        return refuse("the galaxy and script save must belong to the same GC clock barrier");
    if (auto loaded = ai.load(saved.ai); !loaded) return refuse(loaded.error().message);
    if (auto loaded = story.load(saved.story); !loaded) return refuse(loaded.error().message);
    if (objects.has_value() != saved.objects.has_value()) return refuse("object script host presence differs");
    if (objects) {
        if (!objects->uses_galactic_clock()) return refuse("object scripts require the galactic clock");
        if (auto loaded = objects->load(*saved.objects); !loaded) return refuse(loaded.error().message);
        if (objects->completed_tick() > saved.completed_tick || objects->galactic_frame() > saved.galactic_frame ||
            !unique_instances(ai, *objects) || !unique_instances(story, *objects))
            return refuse("object script clocks or identities differ from the barrier");
    }
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
    // elapsed tick. The pause resumes with a timer advance that closes the first frame of the lead. The outer timer
    // tail is skipped for the tick that selects a tactical child (GFS-15), so a victory-monitored galaxy keeps that
    // lead through the battle, the return and the battle-summary hold until the timer resumes: the owner reports
    // every such held state through `timer_held`, not only the pending-conflict pause.
    const bool paused_service_lead = timer_held && ai.galactic_frame() == saved.galactic_frame && ai.completed_tick() > saved.galactic_frame &&
        ai.completed_tick() <= saved.completed_tick;
    const bool ai_tick_ok = (ai.completed_tick() >= saved.galactic_frame && ai.completed_tick() - saved.galactic_frame < galactic_ai_batch_window)
        || paused_service_lead;
    const bool story_tick_ok = story.completed_tick() <= saved.completed_tick;
    // The AI serves whole batches on a virtual frame that leads the galaxy's (or, while the galaxy is paused, stays ahead of it).
    const bool ai_frame_ok = served_frame(ai.galactic_frame())
        || (ai.galactic_frame() + tail_lag >= saved.galactic_frame && ai.galactic_frame() - saved.galactic_frame < galactic_ai_batch_window);
    const bool story_frame_ok = served_frame(story.galactic_frame());
    const bool instances_ok = unique_instances(ai, story);
    if (!ai_tick_ok || !story_tick_ok || !ai_frame_ok || !story_frame_ok || !instances_ok) {
        // Name the failing conditions and both host clocks against the barrier: the one message covers five checks.
        const auto number = [](const std::uint64_t value) { return std::to_string(value); };
        std::string detail = std::string(" [failed:") + (ai_tick_ok ? "" : " ai-tick") + (story_tick_ok ? "" : " story-tick")
            + (ai_frame_ok ? "" : " ai-frame") + (story_frame_ok ? "" : " story-frame") + (instances_ok ? "" : " instances")
            + "; ai tick " + number(ai.completed_tick()) + " frame " + number(ai.galactic_frame())
            + ", story tick " + number(story.completed_tick()) + " frame " + number(story.galactic_frame())
            + ", barrier tick " + number(saved.completed_tick) + " frame " + number(saved.galactic_frame)
            + ", pause depth " + number(static_cast<std::uint64_t>(world.clock().pause_depth))
            + (timer_held ? ", timer held" : "") + (tail_lag ? ", victory tail" : "") + "]";
        return refuse("saved AI/story service clocks or instance identities do not match the barrier" + detail);
    }
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
    auto impl = std::make_unique<Impl>(std::move(shell).value(), std::move(ai), std::move(story), std::move(objects));
    impl->pending = saved.pending;
    impl->next_sequence = saved.next_sequence;
    impl->exhausted_seats = saved.exhausted_seats;
    auto digest = impl->script_hash();
    if (!digest || digest.value() != saved.scripts_sha256) return refuse("saved script barrier digest differs");
    impl->world = std::move(world);
    impl->world.defer_history_service(true);
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
core::Result<void> ScriptedGalacticSession::set_object_scheduler(ScriptScheduler objects) {
    if (impl_->stepped || impl_->objects || impl_->world.completed_tick() != 0 || objects.completed_tick() != 0
        || !objects.uses_galactic_clock() || !unique_instances(impl_->ai, objects) || !unique_instances(impl_->story, objects))
        return core::Result<void>::failure(error(codes::invalid_request, "object scheduler requires a fresh disjoint world host"));
    impl_->objects.emplace(std::move(objects));
    return core::Result<void>::success();
}
core::Result<void> ScriptedGalacticSession::set_object_engine(std::shared_ptr<GalacticObjectScriptEngine> engine) {
    if (impl_->stepped || !impl_->objects || !engine)
        return core::Result<void>::failure(error(codes::invalid_request, "object engine requires an installed object scheduler before stepping"));
    impl_->object_engine = std::move(engine);
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
        const auto history_tick = impl.world.clock().galactic_frame;
        auto stepped = gc_profile::timed(gc_profile::Phase::tick_galaxy_step, [&] { return impl.world.step(executor, local_conflict_pending); });
        if (!stepped) return abort(stepped.error());
        out.world = std::move(stepped).value();
        if (impl.ai_engine ? impl.ai_engine->admits_service(out.world) : out.world.work.serviced) {
            // #6933: the AI barrier's state writes republish the snapshot once, on the next read or at the barrier's end.
            struct SnapshotBatch {
                sim::galactic::GalacticSession& session; bool open{true};
                explicit SnapshotBatch(sim::galactic::GalacticSession& value) noexcept : session(value) { session.begin_snapshot_batch(); }
                void finish() { if (open) { open = false; session.end_snapshot_batch(); } }
                ~SnapshotBatch() { if (open) { try { session.end_snapshot_batch(); } catch (...) {} } }
            };
            auto serviced = [&] {
                SnapshotBatch batch(impl.world);
                auto result = gc_profile::timed_ai([&] { return impl.service(executor, out.world, impl.ai, impl.ai_engine, false); });
                batch.finish();
                return result;
            }();
            if (!serviced) return abort(serviced.error());
            out.ai = std::move(serviced).value();
            impl.pending = out.ai->commands;
            // WGP-146: one admitted History turn after all committed player AI,
            // outside its parallel phases, before the global story reader.
            if (auto recorded = gc_profile::timed(gc_profile::Phase::tick_history, [&] {
                    return impl.world.service_admitted_history(history_tick, executor); }); !recorded) return abort(recorded.error());
            out.world.snapshot = impl.world.snapshot();
        }
        auto serviced = gc_profile::timed(gc_profile::Phase::tick_story_service, [&] { return impl.service(executor, out.world, impl.story, impl.story_engine, true); });
        if (!serviced) return abort(serviced.error());
        out.story = std::move(serviced).value();
        // The one provisional service-order assumption is recorded in the
        // galactic object-script host design; no binding chooses its own phase.
        if (impl.objects && out.world.work.serviced) {
            if (!impl.object_engine) return abort(error(codes::invalid_request, "object scheduler has no engine"));
            auto object_result = gc_profile::timed(gc_profile::Phase::tick_object_scripts, [&] {
                return impl.object_engine->service(executor, impl.world, *impl.objects); });
            if (!object_result) return abort(object_result.error());
            out.objects = std::move(object_result).value();
            if (impl.story_command_committer) {
                if (auto committed = impl.story_command_committer(*out.objects); !committed) return abort(committed.error());
            }
            impl.world.add_runtime_load_notes(out.objects->load_notes);
            impl.pending.insert(impl.pending.end(), out.objects->commands.begin(), out.objects->commands.end());
            out.world.snapshot = impl.world.snapshot();
            if (!unique_instances(impl.ai_ids(), *impl.objects) || !unique_instances(impl.story, *impl.objects))
                return abort(error(codes::invalid_request, "object instance IDs overlap another galactic host"));
        }
        if (!unique_instances(impl.ai_ids(), impl.story))
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

void ScriptedGalacticSession::set_global_value_committer(std::function<void(ServiceReport&)> committer) {
    impl_->global_value_committer = std::move(committer);
}

void ScriptedGalacticSession::set_story_command_committer(std::function<core::Result<void>(ServiceReport&)> committer) {
    impl_->story_command_committer = std::move(committer);
}

void ScriptedGalacticSession::set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept {
    impl_->world.set_state_hasher(hasher);
    impl_->hasher = std::move(hasher);
}
gc::GalacticSession& ScriptedGalacticSession::parent_world() noexcept { return impl_->world; }
const gc::GalacticSession& ScriptedGalacticSession::world() const noexcept { return impl_->world; }
ScriptScheduler& ScriptedGalacticSession::ai_scheduler() noexcept { return impl_->ai; }
std::uint64_t ScriptedGalacticSession::ai_instance_list_reads() const noexcept { return impl_->ai_instance_list_reads; }
bool ScriptedGalacticSession::ai_instance_copy_matches() const {
    const auto& impl = *impl_;
    return impl.ai_ids_revision != impl.ai.instance_revision() || impl.ai_ids_copy == impl.ai.instances();
}
ScriptScheduler& ScriptedGalacticSession::story_scheduler() noexcept { return impl_->story; }
ScriptScheduler* ScriptedGalacticSession::object_scheduler() noexcept { return impl_->objects ? &*impl_->objects : nullptr; }
std::uint64_t ScriptedGalacticSession::completed_tick() const noexcept { return impl_->world.completed_tick(); }
std::size_t ScriptedGalacticSession::pending_script_commands() const noexcept { return impl_->pending.size(); }
gc::GalacticReplay ScriptedGalacticSession::record() const { return impl_->world.record(); }
core::Result<std::string> ScriptedGalacticSession::script_state_hash() { return impl_->script_hash(); }
core::Result<std::string> ScriptedGalacticSession::script_sync_hash() { return impl_->script_hash(true); }
bool ScriptedGalacticSession::uses_sync_hash() const noexcept { return impl_->ai_engine && impl_->ai_engine->uses_sync_hash(); }
core::Result<std::string> ScriptedGalacticSession::save_story() { return impl_->story.save(); }
core::Result<GalacticScriptSave> ScriptedGalacticSession::save_scripts() {
    if (impl_->failure) return core::Result<GalacticScriptSave>::failure(*impl_->failure);
    auto a = impl_->ai.save();
    if (!a) return core::Result<GalacticScriptSave>::failure(a.error());
    auto s = impl_->story.save();
    if (!s) return core::Result<GalacticScriptSave>::failure(s.error());
    auto digest = impl_->script_hash();
    if (!digest) return core::Result<GalacticScriptSave>::failure(digest.error());
    std::optional<std::string> objects;
    if (impl_->objects) {
        auto saved = impl_->objects->save();
        if (!saved) return core::Result<GalacticScriptSave>::failure(saved.error());
        objects = std::move(saved).value();
    }
    return core::Result<GalacticScriptSave>::success({completed_tick(), impl_->world.clock().galactic_frame,
        std::move(a).value(), std::move(s).value(), impl_->pending, impl_->next_sequence, impl_->exhausted_seats,
        impl_->world.state_sha256(), std::move(digest).value(), std::move(objects)});
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
core::Result<void> ScriptedGalacticSession::bind_ground_abilities() {
    return register_verb("galactic.ground_ability", [owner = impl_.get()](const ScriptCommand& command) {
        using Result = core::Result<GalacticOrder>;
        if (command.arguments.size() != 3)
            return Result::failure(error(codes::command_unroutable, "ground activation requires player, actor and planet"));
        const auto* player = std::get_if<Handle>(&command.arguments[0].data);
        const auto* actor = std::get_if<Handle>(&command.arguments[1].data);
        const auto* planet = std::get_if<Handle>(&command.arguments[2].data);
        if (!player || player->kind != foc::handle_player || !actor || actor->kind != foc::handle_game_object || !actor->id
            || !planet || planet->kind != foc::handle_game_object || !planet->id)
            return Result::failure(error(codes::command_unroutable, "ground activation requires typed live identities"));
        const auto snapshot = owner->world.snapshot();
        const auto seats = snapshot->seats();
        const auto seat = std::find_if(seats.begin(), seats.end(), [player](const auto& row) {
            return row.player == player->id && row.active && row.kind == gc::SeatKind::ai;
        });
        if (seat == seats.end())
            return Result::failure(error(codes::command_unroutable, "ground activation requires an active AI seat"));
        return Result::success({seat->seat_id, gc::DeployGroundAbility{actor->id, planet->id}});
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
std::span<const std::string_view> missing_galactic_api_names() noexcept {
    return std::span<const std::string_view>(detail::missing_galactic_globals);
}
core::Result<void> register_missing_galactic_method(ScriptScheduler& scripts, std::uint32_t kind, std::string_view method) {
    return scripts.register_method(kind, method, missing(std::string(method)));
}

} // namespace eawr::script::authoritative
