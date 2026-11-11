#include "eawr/campaign/galactic_world_immediate_host.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "../script/foc/galactic_ai_economic_replay.hpp"

#include <limits>
#include <optional>
#include <utility>

namespace eawr::campaign {
namespace {
namespace auth = script::authoritative;
namespace gc = sim::galactic;
namespace ga = script::foc::galactic_ai;
using Values = core::Result<auth::ValueList>;
template<class T> core::Result<T> refused(std::string message, std::string_view code = auth::codes::invalid_request) {
    core::Diagnostic error; error.code = std::string(code); error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}
std::optional<std::uint64_t> handle_id(const auth::Value& value, std::uint32_t kind) {
    const auto* handle = std::get_if<auth::Handle>(&value.data);
    return handle && handle->kind == kind && handle->id ? std::optional(handle->id) : std::nullopt;
}
auth::Value object_value(std::uint64_t id) { return auth::Value{auth::Handle{script::foc::handle_game_object, id}}; }

struct Shared {
    gc::GalacticSession* world{};
    GalacticWorldImmediateRules rules;
    std::size_t max_requests{};
};

struct WorldBatch final : auth::ImmediateEffectState, GalacticWorldBatch {
    std::shared_ptr<const Shared> shared;
    gc::GalacticSession::NativeActionShadow batch;
    std::vector<std::pair<gc::NativeEffectKind, ga::economic_replay::Bytes>> effects;
    std::vector<gc::GalacticSession::CreditNotification> credits;
    std::vector<std::function<core::Result<void>(gc::GalacticSession&)>> publish_writes;
    WorldBatch(std::shared_ptr<const Shared> owner, gc::GalacticSession::NativeActionShadow shadow)
        : shared(std::move(owner)), batch(std::move(shadow)) {}

    gc::GalacticSession::NativeActionShadow& shadow() noexcept override { return batch; }
    const gc::GalacticSession::NativeActionShadow& shadow() const noexcept override { return batch; }
    const gc::GalacticSession& world() const noexcept override { return *shared->world; }
    void on_publish(std::function<core::Result<void>(gc::GalacticSession&)> write) override { publish_writes.push_back(std::move(write)); }
    std::unique_ptr<auth::ImmediateEffectState> clone() const override { return std::make_unique<WorldBatch>(*this); }

    // The actor as this batch sees it: an existing actor of the world, or one the batch created.
    const gc::CensusObject* actor(std::uint64_t id) const {
        if (const auto* row = batch.actor(id)) return row;
        return batch.created_object(id);
    }

    Values spawn(const auth::ValueList& args) {
        // RGC-176..178: Spawn_Unit(player, planet, type) through the journaled native creation; the script receives the new object
        // of the requested type (a fleet creation yields its first ship); an empty list is a null creation.
        const auto player = args.size() == 3 ? handle_id(args[0], script::foc::handle_player) : std::nullopt;
        const auto planet = args.size() == 3 ? handle_id(args[1], script::foc::handle_game_object) : std::nullopt;
        const auto type = args.size() == 3 ? handle_id(args[2], script::foc::handle_type) : std::nullopt;
        if (!player || !planet || !type || *player > std::numeric_limits<gc::PlayerId>::max())
            return refused<auth::ValueList>("Spawn_Unit requires a player, planet and type");
        if (!shared->rules.placement) return refused<auth::ValueList>("Spawn_Unit requires the campaign's creation placement");
        ga::ActionRequest request; request.kind = ga::ActionKind::magic; request.player = static_cast<gc::PlayerId>(*player);
        request.target = *planet; request.type = *type;
        const sim::InlineExecutor inline_executor;
        const auto* chosen = shared->rules.executor ? shared->rules.executor() : nullptr;
        const auto& executor = chosen ? *chosen : static_cast<const sim::PartitionExecutor&>(inline_executor);
        const auto& placement = shared->rules.placement;
        auto made = batch.create_force({request.player, request.target, request.type, {}}, [&](const auto& view, auto frontier) {
            return placement(request, view, frontier);
        }, executor);
        if (!made) return Values::failure(made.error());
        effects.emplace_back(gc::NativeEffectKind::magic, ga::economic_replay::magic(request, made.value()));
        if (made.value().empty()) return Values::success({});
        if (!made.value().front()) return refused<auth::ValueList>("magic identity exceeds the Lua handle range");
        return Values::success({auth::Value{std::vector<auth::Value>{object_value(made.value().front())}}});
    }
    Values prevent(const auth::ValueList& args) {
        // RGC-178 / L-57: only an actor whose owner has an AI pool; any other owner (a human seat) is a warning without effect,
        // never a script error. The second element names the refusal: "actor" (no such actor) or "pool" (no AI pool).
        const auto id = args.size() == 2 ? handle_id(args[0], script::foc::handle_game_object) : std::nullopt;
        const auto* value = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
        if (!id || !value) return refused<auth::ValueList>("Prevent_AI_Usage requires an actor and Boolean");
        const auto* row = actor(*id);
        if (!row) return Values::success({auth::Value{false}, auth::Value{std::string("actor")}});
        if (!shared->rules.ai_players.contains(row->owner)) return Values::success({auth::Value{false}, auth::Value{std::string("pool")}});
        if (auto staged = batch.stage_actor_ai_usage(*id, *value, std::nullopt); !staged) return Values::failure(staged.error());
        return Values::success({auth::Value{true}});
    }
    Values prefer_ground(const auth::ValueList& args) {
        const auto id = args.size() == 2 ? handle_id(args[0], script::foc::handle_game_object) : std::nullopt;
        const auto* value = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
        if (!id || !value) return refused<auth::ValueList>("Set_Prefer_Ground_Over_Space requires an actor and Boolean");
        const auto* row = actor(*id);
        if (!row || row->dead || row->delete_pending)
            return refused<auth::ValueList>("Set_Prefer_Ground_Over_Space requires a live actor");
        if (auto staged = batch.stage_actor_ai_usage(*id, std::nullopt, *value); !staged) return Values::failure(staged.error());
        return Values::success({});
    }
    Values give_money(const auth::ValueList& args) {
        const auto player_id = args.size() == 2 ? handle_id(args[0], script::foc::handle_player) : std::nullopt;
        const auto* number = args.size() == 2 ? std::get_if<script::numeric::LuaNumber>(&args[1].data) : nullptr;
        if (!player_id || !number) return refused<auth::ValueList>("Give_Money requires a player and numeric amount");
        if (*player_id > std::numeric_limits<gc::PlayerId>::max()) return refused<auth::ValueList>("cash player exceeds identity range");
        const auto player = static_cast<gc::PlayerId>(*player_id);
        const auto multiplier = shared->rules.credit_multipliers.find(player);
        if (multiplier == shared->rules.credit_multipliers.end()) return refused<auth::ValueList>("cash requires the player's selected multiplier");
        auto amount = script::numeric::to_fixed(*number);
        if (!amount) return Values::failure(amount.error());
        auto actual = batch.add_credits(player, amount.value(), multiplier->second);
        if (!actual) return Values::failure(actual.error());
        const auto balance = batch.credits(player);
        if (!balance) return refused<auth::ValueList>("cash names a player without an account");
        credits.push_back({gc::GalacticSession::CreditNotification::Kind::windfall, player, *balance, actual.value(), {}});
        effects.emplace_back(gc::NativeEffectKind::cash, ga::economic_replay::cash(player, amount.value(), multiplier->second, actual.value()));
        return Values::success({});
    }

    Values apply(std::string_view action, const auth::ValueList& args) override {
        if (action == "galactic.spawn_unit") return spawn(args);
        if (action == "galactic.story_prevent_ai_usage") return prevent(args);
        if (action == "galactic.prefer_ground") return prefer_ground(args);
        if (action == "galactic.give_money") return give_money(args);
        if (const auto extension = shared->rules.verbs.find(action); extension != shared->rules.verbs.end()) return extension->second(*this, args);
        return refused<auth::ValueList>("unknown world immediate action " + std::string(action), auth::codes::missing_api);
    }
    Values query(std::string_view name, const auth::ValueList& args) const override {
        if (const auto extension = shared->rules.queries.find(name); extension != shared->rules.queries.end()) return extension->second(*this, args);
        if (name == "galactic.credits") {
            const auto player = args.size() == 1 ? handle_id(args[0], script::foc::handle_player) : std::nullopt;
            if (!player || *player > std::numeric_limits<gc::PlayerId>::max()) return refused<auth::ValueList>("invalid credit query player");
            const auto amount = batch.credits(static_cast<gc::PlayerId>(*player));
            return amount ? Values::success({auth::Value::number(script::numeric::from_fixed(*amount))})
                : refused<auth::ValueList>("credit query has no account");
        }
        if (name != "galactic.created_valid" && name != "galactic.created_type" && name != "galactic.created_owner"
            && name != "galactic.created_position")
            return refused<auth::ValueList>("unknown world immediate query " + std::string(name), auth::codes::missing_api);
        const auto id = args.size() == 1 ? handle_id(args[0], script::foc::handle_game_object) : std::nullopt;
        const auto* object = id ? batch.created_object(*id) : nullptr;
        if (!object) return Values::success({}); // existing and dead handles use the ordinary completed view
        if (name == "galactic.created_valid") return Values::success({auth::Value{true}});
        if (name == "galactic.created_type") return Values::success({auth::Value{auth::Handle{script::foc::handle_type, object->original_type}}});
        if (name == "galactic.created_owner") return Values::success({auth::Value{auth::Handle{script::foc::handle_player, object->owner}}});
        const auto* planet = batch.planet(object->planet);
        if (!planet) return refused<auth::ValueList>("created object has no planet");
        std::vector<auth::Value> position{auth::Value::number(script::numeric::from_fixed(planet->position.x)),
            auth::Value::number(script::numeric::from_fixed(planet->position.y)), auth::Value::number(script::numeric::from_fixed(planet->position.z))};
        return Values::success({auth::Value{std::move(position)}});
    }
};

struct WorldHost final : auth::ImmediateEffectHost {
    std::shared_ptr<const Shared> shared;
    explicit WorldHost(std::shared_ptr<const Shared> owner) : shared(std::move(owner)) {}
    core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override {
        using Result = core::Result<std::unique_ptr<auth::ImmediateEffectState>>;
        auto shadow = shared->world->begin_native_actions(shared->max_requests);
        if (!shadow) return Result::failure(shadow.error());
        return Result::success(std::make_unique<WorldBatch>(shared, std::move(shadow).value()));
    }
    core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState> state) override {
        const auto* batch = dynamic_cast<const WorldBatch*>(state.get());
        if (!batch || batch->shared != shared) return refused<void>("world immediate publication requires its own batch");
        auto& world = *shared->world;
        // Births reach the AI as ordinary actor-change notifications (not prepared here), with the staged AI-usage writes in
        // call order among them.
        if (auto committed = world.commit_native_actions(batch->batch, false, true); !committed) return committed;
        for (const auto& [kind, payload] : batch->effects)
            if (auto recorded = world.append_native_effect(kind, payload); !recorded) return recorded;
        for (const auto& change : batch->batch.actor_changes()) {
            if (change.prevent_ai_usage || change.prefer_ground)
                if (auto saved = world.set_story_ai_usage(change.actor, change.prevent_ai_usage, change.prefer_ground); !saved) return saved;
        }
        for (const auto& notice : batch->credits)
            if (auto notified = world.notify_credit(notice); !notified) return notified;
        for (const auto& write : batch->publish_writes)
            if (auto written = write(world); !written) return written;
        return core::Result<void>::success();
    }
};
} // namespace

std::shared_ptr<script::authoritative::ImmediateEffectHost> make_galactic_world_immediate_host(
    sim::galactic::GalacticSession& world, GalacticWorldImmediateRules rules, std::size_t max_requests) {
    auto shared = std::make_shared<Shared>();
    shared->world = &world; shared->rules = std::move(rules); shared->max_requests = max_requests;
    return std::make_shared<WorldHost>(std::move(shared));
}
} // namespace eawr::campaign
