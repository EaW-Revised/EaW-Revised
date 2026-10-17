// FoC tactical AI goal, planning, execution and learning systems (#449,
// docs/behaviour/foc-tactical-ai.md "Goal system" GS-xx, "Plans and TaskForces" PL-xx and EX-xx,
// "Learning" LS-xx, "Damage tracking" DT-xx). Engine logic runs serially on the tick barrier: one
// per-player goal loop over shared learning, reservation and random state, whose order is part of
// the rules (GS-01), so it is not partitioned; the per-entity work it reads (the tick's
// simulation) is.

#include "ai_engine_internal.hpp"

#include <algorithm>
#include <atomic>
#include <utility>
#include "eawr/script/numeric/decimal.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/script/foc/ai_diagnostics.hpp"

namespace eawr::script::foc::ai {
namespace engine_internal {

using authoritative::ScriptEvent;
using authoritative::Value;
using authoritative::ValueList;

const Real huge = real(1000000000000000000LL); // 1e18, the marshallers' reject weight

Real fixed(math::Fixed value) { return numeric::from_fixed(value); }

// GS-01: a serviced system's next frame, frame + max(1, (int)(delay * fps)).
std::int64_t next_frame(std::int64_t frame, Real delay) {
    return frame + std::max<std::int64_t>(1, truncate(to_single(delay * real(fps))));
}

std::string text_of(const Value& value) {
    const std::string* text = std::get_if<std::string>(&value.data);
    return text == nullptr ? std::string() : *text;
}

std::optional<std::uint64_t> handle_id(const Value& value, std::uint32_t kind) {
    const auto* found = std::get_if<authoritative::Handle>(&value.data);
    if (found == nullptr || found->kind != kind) return std::nullopt;
    return found->id;
}

std::optional<Real> number_of(const Value& value) {
    const auto* found = std::get_if<numeric::LuaNumber>(&value.data);
    if (found == nullptr) return std::nullopt;
    return *found;
}

std::string show(Real value) {
    numeric::decimal::FormatSpec spec;
    spec.conversion = 'f';
    spec.precision = 3;
    return numeric::decimal::format(value.repr, spec);
}

Real xy_distance_squared(const math::Vec3& a, Real x, Real y) {
    const Real dx = fixed(a.x) - x;
    const Real dy = fixed(a.y) - y;
    return to_single(dx * dx + dy * dy);
}

// The journal names a TaskForce's members, so a viewer run can follow them (eye checks).
std::string members_of(const std::vector<sim::EntityId>& units) {
    std::string text = " [";
    for (std::size_t index = 0; index < units.size(); ++index) {
        if (index > 0) text += ',';
        text += std::to_string(units[index]);
    }
    return text + "]";
}
} // namespace engine_internal

Engine::Engine(std::shared_ptr<Host> host, AiData data, std::vector<PlanDef> plans)
    : host_(std::move(host)), data_(std::move(data)), plans_(std::move(plans)) {}

// ---- Reads ------------------------------------------------------------------------------------

void Engine::rebuild_runtime_index() {
    runtime_index_ = {};
    for (const auto& owner : players_) {
        for (const auto& [id, row] : owner.running) { (void)row; index_runtime(runtime_index_.plans, id, owner.player); }
        for (const auto& [id, row] : owner.taskforces) { (void)row; index_runtime(runtime_index_.taskforces, id, owner.player); }
        for (const auto& [id, row] : owner.blocks) { (void)row; index_runtime(runtime_index_.blocks, id, owner.player); }
        for (const auto& [id, row] : owner.plan_of_instance) { (void)row; index_runtime(runtime_index_.instances, id, owner.player); }
    }
}

const TaskForce* Engine::taskforce(std::uint64_t id) const {
    const auto found = runtime_index_.taskforces.find(id);
    if (found == runtime_index_.taskforces.end()) return nullptr;
    note_handle(found->second);
    const auto* owner = player_ai(found->second);
    if (!owner) return nullptr;
    const auto row = owner->taskforces.find(id);
    return row == owner->taskforces.end() ? nullptr : &row->second;
}

const Plan* Engine::plan_of_instance(std::uint64_t instance) const {
    const auto found = runtime_index_.instances.find(instance);
    if (found == runtime_index_.instances.end()) return nullptr;
    note_handle(found->second);
    const auto* owner = player_ai(found->second);
    if (!owner) return nullptr;
    const auto row = owner->plan_of_instance.find(instance);
    if (row == owner->plan_of_instance.end()) return nullptr;
    const auto running = owner->running.find(row->second);
    return running == owner->running.end() ? nullptr : &running->second;
}

const Plan* Engine::plan(std::uint64_t id) const {
    const auto found = runtime_index_.plans.find(id);
    if (found == runtime_index_.plans.end()) return nullptr;
    note_handle(found->second);
    const auto* owner = player_ai(found->second);
    if (!owner) return nullptr;
    const auto row = owner->running.find(id);
    return row == owner->running.end() ? nullptr : &row->second;
}

Plan* Engine::mutable_plan(std::uint64_t id) {
    return const_cast<Plan*>(std::as_const(*this).plan(id));
}
Plan* Engine::mutable_plan_of_instance(std::uint64_t id) {
    return const_cast<Plan*>(std::as_const(*this).plan_of_instance(id));
}

const Block* Engine::raw_block(std::uint64_t id) const {
    const auto found = runtime_index_.blocks.find(id);
    if (found == runtime_index_.blocks.end()) return nullptr;
    note_handle(found->second);
    const auto* owner = player_ai(found->second);
    if (!owner) return nullptr;
    const auto row = owner->blocks.find(id);
    return row == owner->blocks.end() ? nullptr : &row->second;
}

const Block* Engine::block(std::uint64_t id) const {
    const auto* row = raw_block(id);
    return row && row->waiting_on ? raw_block(row->waiting_on) : row;
}

PlayerAi* Engine::positioning_owner(sim::EntityId unit) {
    if (host_->view) if (const auto* actor = host_->view->find(unit))
        if (auto* owner = player_ai(actor->owner)) return owner;
    return players_.empty() ? nullptr : &players_.front();
}

AttackPositioning* Engine::positioning(sim::EntityId unit) {
    // Rows outlive their issuing plan and can survive a unit's ownership change.
    for (auto& owner : players_) {
        const auto found = owner.positioning.find(unit);
        if (found != owner.positioning.end()) return &found->second;
    }
    return nullptr;
}

bool Engine::reserved(sim::EntityId object) const {
    for (const PlayerAi& player : players_) {
        if (player.reserved.contains(object) || player.assigned.contains(object)) return true;
    }
    return false;
}

std::optional<Real> Engine::global_value(const std::string& name) const {
    const auto found = globals_.find(name);
    if (found == globals_.end()) return std::nullopt;
    return found->second;
}

PlayerAi* Engine::player_ai(tactical::PlayerId player) {
    for (PlayerAi& entry : players_) {
        if (entry.player == player) return &entry;
    }
    return nullptr;
}

const PlayerAi* Engine::player_ai(tactical::PlayerId player) const {
    for (const PlayerAi& entry : players_) {
        if (entry.player == player) return &entry;
    }
    return nullptr;
}

const PlayerAi* Engine::player_of_slot(std::uint32_t slot) const {
    for (const PlayerAi& entry : players_) {
        if (entry.galactic && entry.slot == slot) return &entry;
    }
    return nullptr;
}

// S9-early: galactic identities come from the owner's range; the tactical engine counts engine-wide.
std::optional<std::uint64_t> Engine::allocate_target_id(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    if (!player.galactic) return next_target_id_++;
    if (player.next_target_local >= galactic_id_counter_limit) return std::nullopt;
    return galactic_id_base(player.slot) | player.next_target_local++;
}

std::optional<std::uint64_t> Engine::allocate_taskforce_id(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    if (!player.galactic) return next_taskforce_id_++;
    if (player.next_taskforce_local >= galactic_id_counter_limit) return std::nullopt;
    return galactic_id_base(player.slot) | player.next_taskforce_local++;
}

// Potential and running plans share this one allocator, so a reservation made under a potential
// plan can never resolve to a different goal after attachment.
std::optional<std::uint64_t> Engine::allocate_plan_id(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    if (!player.galactic) return next_plan_id_++;
    if (player.next_plan_local >= galactic_id_counter_limit) return std::nullopt;
    return galactic_id_base(player.slot) | player.next_plan_local++;
}

std::optional<std::uint64_t> Engine::allocate_instance_id(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    if (!player.galactic) return next_instance_++;
    if (player.next_instance_local >= galactic_instance_counter_limit) return std::nullopt;
    return galactic_instance_base(player.slot) + player.next_instance_local++;
}

namespace {
bool issued(const PlayerAi* owner, std::uint64_t id, std::uint64_t next_local) {
    return owner && (id & (galactic_id_counter_limit - 1)) != 0 && (id & (galactic_id_counter_limit - 1)) < next_local;
}
}

std::uint64_t Engine::galactic_chain_owner(std::uint64_t instance) const {
    // A freestore instance is created with its player's ID as its instance ID (galactic_ai_runtime.cpp
    // create_instance(player, ...)), so below the first plan instance the instance ID is the chain owner.
    if (instance < first_plan_instance) return instance;
    const auto* owner = player_of_slot(galactic_instance_slot(instance));
    return owner ? owner->player : 0;
}

bool Engine::galactic_target_issued(std::uint64_t id) const {
    const auto* owner = player_of_slot(galactic_id_slot(id));
    return owner && issued(owner, id, owner->next_target_local);
}

bool Engine::galactic_taskforce_issued(std::uint64_t id) const {
    const auto* owner = player_of_slot(galactic_id_slot(id));
    return owner && issued(owner, id, owner->next_taskforce_local);
}

bool Engine::galactic_plan_issued(std::uint64_t id) const {
    const auto* owner = player_of_slot(galactic_id_slot(id));
    return owner && issued(owner, id, owner->next_plan_local);
}

bool Engine::galactic_instance_issued(std::uint64_t instance) const {
    const auto* owner = player_of_slot(galactic_instance_slot(instance));
    return owner && instance >= galactic_instance_base(owner->slot)
        && instance - galactic_instance_base(owner->slot) < owner->next_instance_local;
}

Goal* Engine::find_goal(PlayerAi& player, std::uint64_t id) {
    for (Goal& goal : player.active) {
        if (goal.id == id) return &goal;
    }
    return nullptr;
}

// FH-13 with EX-02: the AI player's own moving objects, except squadron members, star bases and
// the units a TaskForce holds.
bool Engine::in_freestore(const ViewUnit& unit, tactical::PlayerId player) const {
    if (unit.owner != player || unit.craft) return false;
    // EX-13: arrival registers the ship with AI freestore, even after its plan times out.
    if (host_->ai_world->available() && host_->ai_world->arriving(unit.id)) return false;
    const AiType* type = host_->type(unit.type);
    if (type == nullptr || (!type->locomotor && !type->space_special_weapon) || type->star_base) return false;
    const PlayerAi* ai = player_ai(player);
    // SAE-11: an admitted purchase stays reserved until its TaskForce attaches it.
    if (ai != nullptr && unit.purchase_token != 0 && ai->reserved_pool_tokens.contains(unit.purchase_token)) return false;
    return ai == nullptr || !ai->assigned.contains(unit.id);
}

bool Engine::unit_service(const ViewUnit& unit, tactical::PlayerId player) const {
    if (!in_freestore(unit, player)) return false;
    const PlayerAi* ai = player_ai(player);
    return ai == nullptr || !ai->reserved.contains(unit.id);
}

bool Engine::execution_due(tactical::PlayerId player, std::int64_t frame) const {
    const PlayerAi* ai = player_ai(player);
    if (ai == nullptr) return true;
    const auto service_frame = staggered() && frame >= 0
        ? static_cast<std::int64_t>(staggered_clock_.enabled_frame(static_cast<std::uint64_t>(frame))) : frame;
    return ai->next_execution <= service_frame;
}

bool Engine::runs_player(tactical::PlayerId player) const { return player_ai(player) != nullptr; }

// PG-08: the combat power metric of an object's company type.
Real Engine::power(const ViewUnit& unit) const {
    const AiType* type = host_->type(unit.type);
    return type == nullptr ? Real{} : type->combat_power;
}

// DT-03: the attacker with the most accumulated threat on the objects (a squadron counts its
// craft's); ties go to the lowest attacker ID. Only live attackers count.
sim::EntityId Engine::deadly_enemy(const std::vector<sim::EntityId>& objects) const {
    std::map<sim::EntityId, Real> merged;
    const auto add = [&](sim::EntityId object) {
        const auto found = threats_.find(object);
        if (found == threats_.end()) return;
        for (const auto& [attacker, threat] : found->second) merged[attacker] = merged[attacker] + threat;
    };
    for (const sim::EntityId object : objects) {
        const ViewUnit* unit = host_->view->find(object);
        if (unit == nullptr) continue;
        if (unit->container) {
            for (const sim::EntityId member : unit->members) add(member);
        } else {
            add(object);
        }
    }
    sim::EntityId best{};
    Real most{};
    for (const auto& [attacker, threat] : merged) {
        if (host_->view->find(attacker) == nullptr) continue;
        if (best == 0 || most < threat) {
            best = attacker;
            most = threat;
        }
    }
    return best;
}


void Engine::record(const Plan* plan, tactical::PlayerId player, std::string event, std::string detail) {
    PlanRecord line;
    line.tick = static_cast<std::uint64_t>(frame_);
    line.player = player;
    if (plan != nullptr) {
        line.plan = plans_[plan->definition].name;
        if (const PlayerAi* ai = player_ai(plan->player)) {
            for (const Goal& goal : ai->active) {
                if (goal.id == plan->goal) line.goal = ai->functions[goal.function].goal_name;
            }
        }
        line.target = target_name(target(plan->target));
    }
    line.event = std::move(event);
    line.detail = std::move(detail);
    if (auto* turn = staged()) { turn->diagnostics.emplace_back(std::move(line)); return; }
    publish_record(std::move(line));
}

void Engine::publish_record(PlanRecord line) {
    if (host_->setup.journal) {
        host_->setup.journal->append_plan(
            PlanEvent{line.tick, line.player, line.plan, line.goal, line.target, line.event, line.detail});
    }
    append_ai_diagnostic(records_, records_dropped_, std::move(line));
}

// ---- Player effects (S-T3b) ---------------------------------------------------------------------

namespace {
// The turn the running thread executes. A player task binds its own, so nothing here is engine-wide.
struct TurnBinding {
    const Engine* engine{};
    PlayerEffects* effects{};
};
thread_local TurnBinding turn_binding;
std::atomic<Engine::EffectObserver> effect_observer{nullptr};

void add_counts(EffectCounts& to, const EffectCounts& from) {
    to.turns += from.turns; to.events += from.events; to.orders += from.orders; to.removals += from.removals;
    to.credit_deltas += from.credit_deltas; to.pad_effects += from.pad_effects;
    to.attach_intents += from.attach_intents; to.index_effects += from.index_effects;
    to.operations += from.operations; to.diagnostics += from.diagnostics; to.invalidations += from.invalidations;
    to.reads.credits += from.reads.credits; to.reads.pads += from.reads.pads; to.reads.pad_rows += from.reads.pad_rows;
    to.reads.handles += from.reads.handles;
    to.reads.row_scans += from.reads.row_scans;
}
} // namespace

Engine::TurnScope::TurnScope(Engine& engine, PlayerAi& player) noexcept
    : TurnScope(engine, player, engine.staggered() && !player.galactic) {}

Engine::TurnScope::TurnScope(Engine& engine, PlayerAi& player, const bool stage) noexcept
    : previous_engine_(turn_binding.engine), previous_effects_(turn_binding.effects) {
    player.effects.owner = player.player;
    turn_binding.engine = &engine;
    // The faithful schedule attaches plans (scheduler instances, identities) inside the pass and keeps writing
    // directly. The staggered tactical schedule and every galactic service call stage.
    turn_binding.effects = stage ? &player.effects : nullptr;
}

Engine::TurnScope::~TurnScope() {
    turn_binding.engine = previous_engine_;
    turn_binding.effects = previous_effects_;
}

Engine::BoundaryScope::BoundaryScope() noexcept : previous_effects_(turn_binding.effects) { turn_binding.effects = nullptr; }
Engine::BoundaryScope::~BoundaryScope() { turn_binding.effects = previous_effects_; }

PlayerEffects* Engine::staged() const noexcept {
    return turn_binding.engine == this ? turn_binding.effects : nullptr;
}

void Engine::note_handle(const tactical::PlayerId owner) const noexcept {
    if (auto* turn = staged(); turn != nullptr && owner != turn->owner) ++turn->reads.handles;
}

void Engine::queue_event(authoritative::ScriptEvent event) {
    if (auto* turn = staged()) { turn->scheduler.push_back(PlayerEffects::SchedulerEvent{std::move(event), false}); return; }
    event.key.sequence = sequence_++;
    events_.push_back(std::move(event));
}

void Engine::queue_order(authoritative::ScriptCommand order) {
    if (auto* turn = staged()) { turn->orders.push_back(std::move(order)); return; }
    order.sequence = order_sequence_++;
    orders_.push_back(std::move(order));
}

void Engine::stage_removal(authoritative::ScriptScheduler& scripts, const std::uint64_t instance) {
    if (auto* turn = staged()) { turn->scheduler.push_back(PlayerEffects::SchedulerRemoval{instance}); return; }
    static_cast<void>(scripts.remove_instance(instance));
}

void Engine::journal_plan(PlanEvent row, const bool raw) const {
    if (!host_->setup.journal) return;
    if (auto* turn = staged()) { turn->diagnostics.emplace_back(PlayerEffects::JournalPlan{std::move(row), raw}); return; }
    if (raw) host_->setup.journal->plans.push_back(std::move(row)); else host_->setup.journal->append_plan(std::move(row));
}

void Engine::note_pad_query(const tactical::PlayerId player, const bool open) const {
    if (auto* turn = staged()) {
        ++turn->pad_queries;
        if (open) turn->pad_open.insert(player);
        return;
    }
    ++input_counts_.pad_queries;
    if (open) pad_open_requested_.insert(player);
}

void Engine::journal_service(const AiServiceEvent& row) {
    if (!host_->setup.journal) return;
    if (auto* turn = staged()) { turn->diagnostics.emplace_back(row); return; }
    host_->setup.journal->append_service(row);
}

const PadReservation* Engine::pad_claim(const sim::EntityId pad) const {
    auto* turn = staged();
    for (auto owner = players_.rbegin(); owner != players_.rend(); ++owner) {
        const auto found = owner->effects.pads.find(pad);
        if (found == owner->effects.pads.end()) continue;
        if (turn != nullptr && &owner->effects != turn) ++turn->reads.pads;
        return found->second ? &*found->second : nullptr;
    }
    const auto found = pad_reservations_.find(pad);
    return found == pad_reservations_.end() ? nullptr : &found->second;
}

void Engine::set_pad_claim(const sim::EntityId pad, const PadReservation& claim) {
    if (auto* turn = staged()) { turn->pads[pad] = claim; return; }
    pad_reservations_[pad] = claim;
}

void Engine::release_pad_claim(const sim::EntityId pad) {
    if (auto* turn = staged()) { turn->pads[pad] = std::nullopt; return; }
    pad_reservations_.erase(pad);
}

void Engine::release_pad_claims(const tactical::PlayerId player, const std::uint64_t goal) {
    const auto held = [&](const PadReservation& claim) { return claim.player == player && claim.goal == goal; };
    auto* turn = staged();
    if (turn == nullptr) { std::erase_if(pad_reservations_, [&](const auto& entry) { return held(entry.second); }); return; }
    // Pads the stacked view could show as held: the committed claims and any staged claim, each answered by
    // the stacked lookup (the committed claims, then each earlier turn's net claims, then the own turn's).
    std::vector<sim::EntityId> pads;
    for (const auto& [pad, claim] : pad_reservations_) if (held(claim)) pads.push_back(pad);
    for (const auto& owner : players_) for (const auto& [pad, claim] : owner.effects.pads) if (claim && held(*claim)) pads.push_back(pad);
    std::sort(pads.begin(), pads.end());
    pads.erase(std::unique(pads.begin(), pads.end()), pads.end());
    for (const auto pad : pads) {
        const auto* claim = pad_claim(pad);
        if (claim != nullptr && held(*claim)) turn->pads[pad] = std::nullopt;
    }
}

std::int64_t Engine::uncommitted_credits(const tactical::PlayerId id) const noexcept {
    auto* turn = staged();
    std::int64_t raw = 0;
    for (const auto& owner : players_) {
        const auto found = owner.effects.credits.find(id);
        if (found == owner.effects.credits.end()) continue;
        raw += found->second.raw();
        if (turn != nullptr && &owner.effects != turn) ++turn->reads.credits;
    }
    return raw;
}

math::Fixed Engine::stacked_credits(const tactical::PlayerId player) const {
    const auto committed = host_->credit_changes.find(player);
    return math::Fixed::from_raw((committed != host_->credit_changes.end() ? committed->second.raw() : 0)
        + uncommitted_credits(player));
}

void Engine::retire_runtime(const RuntimeTable table, const std::uint64_t id) {
    if (auto* turn = staged()) { turn->index.push_back(PlayerEffects::IndexEffect{table, id}); return; }
    switch (table) {
    case RuntimeTable::plans: runtime_index_.plans.erase(id); break;
    case RuntimeTable::taskforces: runtime_index_.taskforces.erase(id); break;
    case RuntimeTable::blocks: runtime_index_.blocks.erase(id); break;
    case RuntimeTable::instances: runtime_index_.instances.erase(id); break;
    }
}

bool Engine::foreign_row(const RuntimeTable table, const std::uint64_t id) const {
    const auto* turn = staged();
    if (turn == nullptr) return false;
    const auto& index = table == RuntimeTable::plans ? runtime_index_.plans
        : table == RuntimeTable::taskforces ? runtime_index_.taskforces
        : table == RuntimeTable::blocks ? runtime_index_.blocks : runtime_index_.instances;
    const auto found = index.find(id);
    return found != index.end() && found->second != turn->owner;
}

void Engine::operate(const CrossOwnerOp& op) {
    if (auto* turn = staged()) ++turn->operations;
    apply_operation(op);
}

void Engine::apply_operation(const CrossOwnerOp& op) {
    const auto row_owner = [&](const std::map<std::uint64_t, tactical::PlayerId>& index) -> PlayerAi* {
        const auto found = index.find(op.id);
        return found == index.end() ? nullptr : player_ai(found->second);
    };
    switch (op.kind) {
    case CrossOwnerOp::Kind::taskforce_join:
    case CrossOwnerOp::Kind::taskforce_pool:
    case CrossOwnerOp::Kind::taskforce_consume: {
        auto* owner = row_owner(runtime_index_.taskforces);
        if (owner == nullptr) return;
        const auto found = owner->taskforces.find(op.id);
        if (found == owner->taskforces.end()) return;
        TaskForce& force = found->second;
        if (op.kind == CrossOwnerOp::Kind::taskforce_join) {
            force.units.push_back(op.first);
            if (op.flag) force.had_units = true;
        } else if (op.kind == CrossOwnerOp::Kind::taskforce_pool) {
            force.pooled.push_back({op.first, op.second});
        } else {
            for (std::size_t index = 0; index < force.types.size() && index < force.producers.size(); ++index) {
                if (force.types[index] == op.first && force.producers[index] == op.second) { force.types[index] = 0; break; }
            }
        }
        return;
    }
    case CrossOwnerOp::Kind::plan_release_pad:
        if (auto* owner = row_owner(runtime_index_.plans)) {
            const auto found = owner->running.find(op.id);
            if (found != owner->running.end()) std::erase(found->second.reserved_pads, static_cast<sim::EntityId>(op.first));
        }
        return;
    case CrossOwnerOp::Kind::block_finish:
        if (auto* owner = row_owner(runtime_index_.blocks)) {
            const auto found = owner->blocks.find(op.id);
            if (found != owner->blocks.end()) { found->second.finished = true; found->second.result = op.flag; }
        }
        return;
    }
}

void Engine::apply_native_records_changed(const std::uint32_t owner, const std::uint32_t kind) {
    auto& bits = native_record_dirty_groups_[owner];
    if (kind == touch_only_kind) { native_record_header_dirty_ = true; return; }
    bits |= std::uint64_t{1} << kind;
    if (owner != native_all_players && owner != 0 && kind <= 17) bits |= std::uint64_t{1} << 1;
    native_record_header_dirty_ = true;
}

void Engine::apply_native_record_changed(const std::uint32_t owner, const std::uint32_t kind, const std::uint64_t key) {
    core::RecordDigest::Bytes bytes;
    core::RecordDigest::append(bytes, key);
    native_record_dirty_keys_.emplace(owner, kind, std::move(bytes));
    if (owner != native_all_players && owner != 0 && kind <= 17) apply_native_records_changed(owner, 1);
    native_record_header_dirty_ = true;
}

void Engine::discard_effects(PlayerAi& player) noexcept {
    PlayerEffects& effects = player.effects;
    if (effects.tasks_revision != 0) {
        galactic_tasks_revision_ += effects.tasks_revision;
        if (galactic_tasks_revision_ < effects.tasks_revision) std::terminate();
        galactic_build_cache_.reset();
    }
    if (effects.revisions != 0) {
        native_revision_ += effects.revisions;
        if (native_revision_ < effects.revisions) std::terminate();
        native_record_header_dirty_ = true;
    }
    effects.clear(player.player);
}

core::Result<void> Engine::commit_effects(authoritative::ScriptScheduler& scripts) {
    // A staged submit fails here, not in the turn. The sequential pass stopped at that point, so nothing of the
    // failing turn after it and nothing of a later player applies; the barrier has failed and the engine is not
    // a continuation point.
    std::optional<core::Diagnostic> failed;
    for (PlayerAi& player : players_) {
        PlayerEffects& effects = player.effects;
        if (effects.scope_depth != 0) throw std::logic_error("AI turn left a native record scope open");
        const bool worked = effects.work.goals_evaluated != 0 || effects.work.maintenances != 0
            || effects.work.plans_attached != 0 || effects.work.plans_pumped != 0
            || effects.work.reinforcement_candidates != 0 || effects.work.reinforcement_search_ns != 0;
        if (failed || (effects.empty() && !worked && effects.reads == EffectReads{})) { discard_effects(player); continue; }
        EffectCounts counts;
        counts.turns = 1;
        counts.orders = effects.orders.size();
        counts.credit_deltas = effects.credits.size();
        counts.pad_effects = effects.pads.size();
        counts.attach_intents = effects.attaches.size();
        counts.index_effects = effects.index.size();
        counts.operations = effects.operations;
        counts.diagnostics = effects.diagnostics.size();
        counts.invalidations = effects.invalidations.size();
        counts.reads = effects.reads;
        // Scheduler effects in the order the turn made them; events take their sequence here.
        for (auto& effect : effects.scheduler) {
            if (auto* removal = std::get_if<PlayerEffects::SchedulerRemoval>(&effect)) {
                ++counts.removals;
                static_cast<void>(scripts.remove_instance(removal->instance));
                continue;
            }
            auto& staged_event = std::get<PlayerEffects::SchedulerEvent>(effect);
            ++counts.events;
            staged_event.event.key.sequence = sequence_++;
            if (!staged_event.submit) { events_.push_back(std::move(staged_event.event)); continue; }
            if (auto submitted = scripts.submit_event(std::move(staged_event.event)); !submitted) {
                failed = submitted.error();
                break;
            }
        }
        if (failed) { discard_effects(player); continue; }
        for (auto& order : effects.orders) {
            order.sequence = order_sequence_++;
            orders_.push_back(std::move(order));
        }
        for (const auto& [id, delta] : effects.credits) {
            const auto changed = math::add(host_->credit_changes[id], delta);
            if (!changed) throw std::runtime_error("AI wallet balance: " + changed.error().message);
            host_->credit_changes[id] = changed.value();
        }
        for (const auto& [pad, claim] : effects.pads) {
            if (claim) pad_reservations_[pad] = *claim; else pad_reservations_.erase(pad);
        }
        for (const auto& attach : effects.attaches) pending_attach_.push_back(attach);
        for (const auto& change : effects.index) {
            auto& table = change.table == RuntimeTable::plans ? runtime_index_.plans
                : change.table == RuntimeTable::taskforces ? runtime_index_.taskforces
                : change.table == RuntimeTable::blocks ? runtime_index_.blocks : runtime_index_.instances;
            table.erase(change.id);
        }
        for (auto& line : effects.diagnostics) {
            if (auto* record_line = std::get_if<PlanRecord>(&line)) {
                publish_record(std::move(*record_line));
            } else if (auto* plan_row = std::get_if<PlayerEffects::JournalPlan>(&line)) {
                if (!host_->setup.journal) continue;
                if (plan_row->raw) host_->setup.journal->plans.push_back(std::move(plan_row->row));
                else host_->setup.journal->append_plan(std::move(plan_row->row));
            } else if (host_->setup.journal) {
                host_->setup.journal->append_service(std::get<AiServiceEvent>(line));
            }
        }
        work_.goals_evaluated += effects.work.goals_evaluated;
        work_.maintenances += effects.work.maintenances;
        work_.plans_attached += effects.work.plans_attached;
        work_.plans_pumped += effects.work.plans_pumped;
        work_.reinforcement_candidates += effects.work.reinforcement_candidates;
        work_.reinforcement_search_ns += effects.work.reinforcement_search_ns;
        producer_work_.candidates += effects.producer_candidates;
        input_counts_.pad_queries += effects.pad_queries;
        pad_open_requested_.insert(effects.pad_open.begin(), effects.pad_open.end());
        for (const auto& change : effects.invalidations) {
            if (change.exact) apply_native_record_changed(change.owner, change.kind, change.key);
            else apply_native_records_changed(change.owner, change.kind);
        }
        if (effects.tasks_revision != 0) {
            galactic_tasks_revision_ += effects.tasks_revision;
            if (galactic_tasks_revision_ < effects.tasks_revision) std::terminate();
            galactic_build_cache_.reset();
        }
        if (effects.revisions != 0) {
            native_revision_ += effects.revisions;
            if (native_revision_ < effects.revisions) std::terminate();
            native_record_header_dirty_ = true;
        }
        add_counts(effect_totals_, counts);
        add_counts(effect_tick_, counts);
        effects.clear(player.player);
    }
    if (failed) return core::Result<void>::failure(*failed);
    return core::Result<void>::success();
}

void Engine::observe_effects(const EffectObserver observer) noexcept {
    effect_observer.store(observer, std::memory_order_relaxed);
}

void Engine::report_effects() const noexcept {
    if (const auto observer = effect_observer.load(std::memory_order_relaxed))
        observer(host_->view ? host_->view->tick : 0, effect_tick_);
}

} // namespace eawr::script::foc::ai

namespace eawr::script::foc::detail {

std::int64_t Host::uncommitted_credits(const tactical::PlayerId id) const noexcept {
    return engine != nullptr ? engine->uncommitted_credits(id) : 0;
}

} // namespace eawr::script::foc::detail
