// FoC tactical AI goal, planning, execution and learning systems (#449,
// docs/behaviour/foc-tactical-ai.md "Goal system" GS-xx, "Plans and TaskForces" PL-xx and EX-xx,
// "Learning" LS-xx, "Damage tracking" DT-xx). Engine logic runs serially on the tick barrier: one
// per-player goal loop over shared learning, reservation and random state, whose order is part of
// the rules (GS-01), so it is not partitioned; the per-entity work it reads (the tick's
// simulation) is.

#include "ai_engine_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>

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
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.3f", std::bit_cast<double>(value.repr));
    return buffer;
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

const TaskForce* Engine::taskforce(std::uint64_t id) const {
    const auto found = taskforces_.find(id);
    return found == taskforces_.end() ? nullptr : &found->second;
}

const Plan* Engine::plan_of_instance(std::uint64_t instance) const {
    const auto found = plan_of_instance_.find(instance);
    return found == plan_of_instance_.end() ? nullptr : plan(found->second);
}

const Plan* Engine::plan(std::uint64_t id) const {
    const auto found = running_.find(id);
    return found == running_.end() ? nullptr : &found->second;
}

const Block* Engine::block(std::uint64_t id) const {
    const auto found = blocks_.find(id);
    if (found == blocks_.end()) return nullptr;
    if (found->second.waiting_on != 0) {
        const auto active = blocks_.find(found->second.waiting_on);
        return active == blocks_.end() ? nullptr : &active->second;
    }
    return &found->second;
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
    if (host_->world != nullptr && host_->world->arrivals().contains(unit.id)) return false;
    const AiType* type = host_->type(unit.type);
    if (type == nullptr || !type->locomotor || type->star_base) return false;
    const PlayerAi* ai = player_ai(player);
    // SAE-11: an admitted purchase stays reserved until its TaskForce attaches it.
    if (ai != nullptr && unit.purchase_token != 0 && ai->reserved_pool_tokens.contains(unit.purchase_token)) return false;
    return ai == nullptr || !ai->assigned.contains(unit.id);
}

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
    if (host_->setup.journal) {
        host_->setup.journal->plans.push_back(
            PlanEvent{line.tick, line.player, line.plan, line.goal, line.target, line.event, line.detail});
    }
    records_.push_back(std::move(line));
}

} // namespace eawr::script::foc::ai
