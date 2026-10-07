#include "ai_engine_internal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace eawr::script::foc::ai {

using namespace engine_internal;

// EX-11: every 0.1 s each unfinished build task takes a free store object of its type, the one
// the plan's potential plan reserved first; a type the free store no longer holds fails.
void Engine::prepare_producers() const {
    if (producers_prepared_ || host_->world == nullptr || host_->view == nullptr) return;
    producers_prepared_ = true;
    const auto add = [&](const ViewUnit& unit) {
        const auto owner = std::find_if(host_->world->players().begin(), host_->world->players().end(),
            [&](const auto& player) { return player.player_id == unit.owner; });
        if (owner == host_->world->players().end()) return;
        const bool pad = host_->world->pads().contains(unit.id);
        for (const auto& player : host_->world->players()) {
            const auto* menu = host_->world->economy().menu(unit.type, pad ? player.faction_id : owner->faction_id);
            if (menu == nullptr) continue;
            for (const auto& option : menu->options) {
                if (build_option(player.player_id, option.type, unit.id) == nullptr) continue;
                auto& candidates = producers_[{player.player_id, option.type}];
                if (candidates.empty() || candidates.back() != unit.id) candidates.push_back(unit.id);
            }
        }
    };
    // Preserve pad-first and ascending entity order; live admission and reservations are
    // rechecked by producer(), since earlier plans can reserve a pad in this same service.
    for (const auto& [id, state] : host_->world->pads()) {
        static_cast<void>(state);
        ++producer_work_.entities;
        if (const auto* unit = host_->view->find(id)) add(*unit);
    }
    for (const auto& unit : host_->view->units) {
        ++producer_work_.entities;
        const auto* info = host_->type(unit.type);
        if (info != nullptr && info->star_base && !host_->world->pads().contains(unit.id)) add(unit);
    }
}

sim::EntityId Engine::producer(const tactical::PlayerId player, const tactical::TypeId type, const sim::EntityId preferred) const {
    prepare_producers();
    const auto found = producers_.find({player, type});
    if (found == producers_.end()) return 0;
    for (const auto id : found->second) {
        ++producer_work_.candidates;
        if (host_->world->pads().contains(id)) {
            if (preferred != 0 && preferred != id) continue;
            bool reserved = pad_reservations_.contains(id);
            for (const auto& [plan_id, plan] : running_) {
                static_cast<void>(plan_id);
                reserved = reserved || std::find(plan.reserved_pads.begin(), plan.reserved_pads.end(), id) != plan.reserved_pads.end();
            }
            if (reserved) continue;
        }
        if (host_->world->build_allowed(player, id, type)) return id;
    }
    return 0;
}

const tactical::BuildOption* Engine::build_option(const tactical::PlayerId player, const tactical::TypeId type,
    const sim::EntityId producer_id) const {
    if (host_->world == nullptr) return nullptr;
    return host_->world->build_option(player, producer_id, type);
}

void Engine::observe_purchases(PlayerAi& player, const tactical::TacticalSnapshot& snapshot) {
    // WAS-26/WBP-48: consume only accepted station work. A pad consumes its
    // reservation after precheck; a request yielding no child refunds that cost once.
    // Read every tick, so queue cancellation before the next execution service cannot
    // be mistaken for an unstarted purchase and refunded a second time on release.
    for (const auto& event : snapshot.events()) {
        if ((event.kind != tactical::EventKind::order_accepted && event.kind != tactical::EventKind::order_rejected)
            || (event.order != tactical::OrderKind::buy && event.order != tactical::OrderKind::pad_build)) continue;
        if (event.player != player.player) continue;
        const auto task = std::find_if(player.tasks.begin(), player.tasks.end(), [&](const auto& entry) {
            return entry.source == 2 && entry.issued == event.tick && !entry.acknowledged
                && entry.producer == event.unit && entry.pad_build == (event.order == tactical::OrderKind::pad_build);
        });
        if (task == player.tasks.end()) continue;
        task->acknowledged = true;
        const auto force = taskforces_.find(task->taskforce);
        const auto* plan = force != taskforces_.end() ? this->plan(force->second.plan) : nullptr;
        if (event.kind == tactical::EventKind::order_rejected) {
            if (task->pad_build && task->prepaid) change_wallet(event.player, task->generic_cost);
            if (task->pad_build) {
                pad_reservations_.erase(task->producer);
                if (plan != nullptr) if (auto running = running_.find(plan->id); running != running_.end())
                    std::erase(running->second.reserved_pads, task->producer);
            }
            task->finished = task->failed = true;
        } else if (!task->pad_build && task->prepaid && plan != nullptr) {
            const auto funds = player.reserved_credits.find(plan->goal);
            if (funds != player.reserved_credits.end())
                funds->second = std::max(Real{}, funds->second - task->generic_cost);
        }
    }
}

void Engine::service_execution(PlayerAi& player, const bool direct_pad_only) {
    std::set<std::uint64_t> touched;
    for (BuildTask& task : player.tasks) {
        if (direct_pad_only && !task.direct_pad) continue;
        if (task.finished) { touched.insert(task.block); continue; }
        auto tf = taskforces_.find(task.taskforce);
        if (tf == taskforces_.end()) {
            task.finished = task.failed = true;
            continue;
        }
        const Plan* plan = this->plan(tf->second.plan);
        const auto* account = host_->economy(player.player);
        touched.insert(task.block);
        if (task.source == 1) {
            tf->second.pooled.push_back({task.type, task.pool_token});
            task.finished = true;
            continue;
        }
        if (task.source == 2) {
            if (plan == nullptr || account == nullptr) { task.finished = task.failed = true; continue; }
            if (task.issued == 0) {
                if (task.producer == 0) task.producer = producer(player.player, task.type);
                const auto* option = build_option(player.player, task.type, task.producer);
                task.prepaid = player.reserved_credits.contains(plan->goal);
                if (option == nullptr || (!task.prepaid && host_->credits(player.player) < option->price)
                    || !host_->world->build_allowed(player.player, task.producer, task.type)) {
                    task.finished = task.failed = true; continue;
                }
                task.pad_build = option->kind == tactical::BuildKind::structure;
                if (task.pad_build) {
                    const auto reservation = pad_reservations_.find(task.producer);
                    if (task.prepaid && (reservation == pad_reservations_.end() || reservation->second.consumed
                        || reservation->second.player != player.player || reservation->second.goal != plan->goal
                        || reservation->second.type != task.type)) {
                        task.finished = task.failed = true; continue;
                    }
                    const auto* pad = host_->view->find(task.producer);
                    const auto state = host_->world->pads().find(task.producer);
                    const auto* profile = pad != nullptr ? host_->world->economy().pads.point(pad->type) : nullptr;
                    if (profile == nullptr || state == host_->world->pads().end()
                        || !tactical::pad_construction_allowed(*profile, state->second, player.player,
                            host_->world->players(), host_->view->capture_candidates,
                            host_->world->economy().pads, task.producer, pad->position)) {
                        task.finished = task.failed = true; continue;
                    }
                }
                const auto* info = host_->type(task.type);
                task.generic_cost = fixed(info != nullptr && info->tactical_cost ? *info->tactical_cost : option->price);
                if (task.pad_build && task.prepaid) {
                    auto& funds = player.reserved_credits.at(plan->goal);
                    funds = std::max(Real{}, funds - task.generic_cost);
                    pad_reservations_.at(task.producer).consumed = true;
                    pad_reservations_.at(task.producer).issued = static_cast<std::uint64_t>(frame_);
                }
                if (task.direct_pad) {
                    for (std::size_t index = 0; index < tf->second.types.size(); ++index) {
                        if (tf->second.types[index] == task.type && tf->second.producers[index] == task.producer) {
                            tf->second.types[index] = 0;
                            break;
                        }
                    }
                }
                task.completion = 1;
                if (const auto built = account->lifetime.find(task.type); built != account->lifetime.end()) task.completion += built->second;
                for (const auto& queue : account->queues) for (const auto& entry : queue) if (entry.type == task.type) ++task.completion;
                for (const auto& other : player.tasks) {
                    if (&other != &task && other.type == task.type && other.issued == static_cast<std::uint64_t>(frame_)) ++task.completion;
                }
                authoritative::ScriptCommand buy;
                buy.issuer = plan->instance;
                buy.sequence = order_sequence_++;
                buy.verb = std::string(option->kind == tactical::BuildKind::structure ? verb_pad_build : verb_buy);
                buy.arguments = {Value::number(real(player.player)), Value{authoritative::Handle{handle_game_object, task.producer}},
                    Value{authoritative::Handle{handle_type, task.type}}};
                if (!task.pad_build) buy.arguments.push_back(Value{task.prepaid});
                orders_.push_back(std::move(buy));
                task.issued = static_cast<std::uint64_t>(frame_);
                task.child_floor = host_->world->next_entity_id();
                record(plan, player.player, "buy", std::to_string(task.type));
            } else if (task.pad_build) {
                const auto pad = host_->world->pads().find(task.producer);
                const auto* profile = host_->world->economy().pads.child(task.type);
                const auto* child = pad != host_->world->pads().end() ? host_->view->find(pad->second.constructed) : nullptr;
                const auto constructor = pad != host_->world->pads().end()
                    ? host_->world->construction().find(pad->second.under_construction) : host_->world->construction().end();
                if (pad != host_->world->pads().end() && pad->second.constructed >= task.child_floor
                    && child != nullptr && profile != nullptr && child->type == profile->constructed && child->owner == player.player) {
                    task.finished = true;
                    tf->second.units.push_back(pad->second.constructed);
                } else if (task.issued < static_cast<std::uint64_t>(frame_)
                    && (constructor == host_->world->construction().end() || constructor->second.builder != player.player)) {
                    // WBP-18/27/48: rejection, cancellation or death clears the constructor
                    // link. The production block must fail and relinquish its pad.
                    task.finished = task.failed = true;
                }
                if (task.finished) {
                    if (auto running = running_.find(tf->second.plan); running != running_.end())
                        std::erase(running->second.reserved_pads, task.producer);
                }
            } else if (const auto built = account->lifetime.find(task.type); built != account->lifetime.end() && built->second >= task.completion) {
                task.finished = true;
                // SAE-03: new tactical purchases finish their build task without a
                // TaskForce reservation. Other plans may select them from the pool.
            } else {
                bool queued = false;
                for (const auto& queue : account->queues) for (const auto& entry : queue) queued = queued || entry.type == task.type;
                if (!queued && task.issued < static_cast<std::uint64_t>(frame_)) task.finished = task.failed = true;
            }
            continue;
        }
        const ViewUnit* chosen = nullptr;
        for (const ViewUnit& unit : host_->view->units) {
            if (unit.type != task.type || !in_freestore(unit, player.player)) continue;
            const auto reservation = player.reserved.find(unit.id);
            if (plan != nullptr && reservation != player.reserved.end() && reservation->second == plan->goal) {
                chosen = &unit;
                break;
            }
        }
        if (chosen == nullptr) {
            for (const ViewUnit& unit : host_->view->units) {
                if (unit.type != task.type || !in_freestore(unit, player.player) || player.reserved.contains(unit.id)) continue;
                chosen = &unit;
                break;
            }
        }
        touched.insert(task.block);
        if (chosen == nullptr) {
            task.finished = task.failed = true;
            continue;
        }
        task.object = chosen->id;
        task.finished = true;
        tf->second.units.push_back(chosen->id);
        tf->second.had_units = true;
        player.assigned[chosen->id] = tf->second.id;
    }
    for (const std::uint64_t id : touched) {
        auto block = blocks_.find(id);
        if (block == blocks_.end()) continue;
        bool done = true;
        bool any = false;
        for (const BuildTask& task : player.tasks) {
            if (task.block != id) continue;
            done = done && task.finished;
            any = any || (task.finished && !task.failed);
        }
        if (!done) continue;
        block->second.finished = true;
        const auto tf = taskforces_.find(block->second.taskforce);
        block->second.result = any || (tf != taskforces_.end() && (!tf->second.units.empty() || !tf->second.pooled.empty()));
        if (tf != taskforces_.end()) {
            if (const Plan* plan = this->plan(tf->second.plan)) {
                record(plan, player.player, "produced", tf->second.name + " " + std::to_string(tf->second.units.size()) + " units"
                    + members_of(tf->second.units));
                if (!any && tf->second.units.empty()) emit_call(*plan, tf->second, "No_Units_Remaining", {});
            }
        }
    }
    // WBP-48: an early refusal retains the allocation as well as its refundable funds.
    for (const auto& task : player.tasks) if (task.finished && task.source == 2 && task.issued != 0
        && (!direct_pad_only || task.direct_pad)) {
        pad_reservations_.erase(task.producer);
        const auto tf = taskforces_.find(task.taskforce);
        if (tf != taskforces_.end()) if (auto running = running_.find(tf->second.plan); running != running_.end())
            std::erase(running->second.reserved_pads, task.producer);
    }
    // A pad-only service must leave other finished tasks for ordinary execution
    // to settle their blocks, including station refusals observed earlier this tick.
    std::erase_if(player.tasks, [direct_pad_only](const BuildTask& task) {
        return task.finished && (!direct_pad_only || task.direct_pad);
    });
    if (!direct_pad_only) service_reinforcements(player);
}

std::optional<math::Vec3> reinforcement_candidate(const math::Vec3& requested, const std::uint32_t attempt,
    const math::Fixed yaw, const std::optional<std::array<math::Fixed, 4>>& bounds) {
    return tactical::reinforcement_search_candidate(requested, attempt, yaw, bounds);
}

void Engine::service_reinforcements(PlayerAi& player) {
    if (host_->world == nullptr) return;
    const auto players = host_->world->players();
    const auto owner = std::lower_bound(players.begin(), players.end(), player.player,
        [](const tactical::Player& entry, const tactical::PlayerId id) { return entry.player_id < id; });
    if (owner == players.end() || owner->player_id != player.player) return;
    for (auto& [id, block] : blocks_) {
        static_cast<void>(id);
        if (block.kind != Block::Kind::reinforce || block.finished) continue;
        const auto tf = taskforces_.find(block.taskforce);
        const auto* plan = tf != taskforces_.end() ? this->plan(tf->second.plan) : nullptr;
        if (plan == nullptr || plan->player != player.player) continue;
        if (block.reinforcing != 0) {
            if (const auto result = host_->world->reinforcement_search_result(player.player, block.id)) {
                block.reinforcement_attempt = result->next_attempt;
                work_.reinforcement_candidates += result->candidates;
                work_.reinforcement_search_ns += result->nanoseconds;
            }
            const auto& units = host_->view->units;
            const auto* account = host_->economy(player.player);
            const bool consumed = account != nullptr
                && std::find(account->pool_tokens.begin(), account->pool_tokens.end(), block.pool_token) == account->pool_tokens.end();
            const auto first = std::upper_bound(units.begin(), units.end(), block.entity_floor,
                [](sim::EntityId key, const ViewUnit& unit) { return key < unit.id; });
            const auto arrived = std::find_if(first, units.end(), [&](const ViewUnit& unit) {
                return consumed && unit.owner == player.player
                    && unit.purchase_token == block.pool_token
                    && (unit.purchase_type != 0 ? unit.purchase_type : unit.type) == block.reinforcing
                    && !unit.craft && !player.assigned.contains(unit.id)
                    // EX-13: the arrival signal attaches the ship, not its hyperspace spawn.
                    && !host_->world->arrivals().contains(unit.id);
            });
            if (arrived != units.end()) {
                tf->second.units.push_back(arrived->id);
                tf->second.had_units = true;
                player.assigned[arrived->id] = tf->second.id;
                const auto pooled = std::find_if(tf->second.pooled.begin(), tf->second.pooled.end(),
                    [&](const auto& purchase) { return purchase.token == block.pool_token; });
                if (pooled != tf->second.pooled.end()) tf->second.pooled.erase(pooled);
                auto& count = player.reserved_pool[block.reinforcing];
                if (count != 0) --count;
                player.reserved_pool_tokens.erase(block.pool_token);
                record(plan, player.player, "reinforced", std::to_string(arrived->id));
                block.reinforcing = 0;
                block.reinforcement_attempt = 0;
            } else if (!consumed && block.issued < static_cast<std::uint64_t>(frame_)) {
                block.reinforcing = 0; // rejected command: keep the pooled reservation and retry
            }
        }
        if (block.reinforcing != 0) continue;
        if (tf->second.pooled.empty()) {
            block.finished = std::none_of(tf->second.units.begin(), tf->second.units.end(),
                [&](const auto unit) { return host_->world->arrivals().contains(unit); });
            continue;
        }
        const auto purchase = tf->second.pooled.front();
        const auto type = purchase.type;
        // SAE-03/10: the live search is evaluated on workers in the authoritative tick.
        // The replay retains only its resolved ordinary command, with the usual admission.
        authoritative::ScriptCommand reinforce;
        reinforce.issuer = plan->instance;
        reinforce.sequence = order_sequence_++;
        reinforce.verb = std::string(verb_reinforce);
        reinforce.arguments = {Value::number(real(player.player)), Value{authoritative::Handle{handle_game_object, 0}},
            Value{authoritative::Handle{handle_type, type}}, detail::position_value(block.destination),
            Value{authoritative::Handle{handle_type, purchase.token}},
            Value{authoritative::Handle{handle_block, block.id}}, Value::number(real(block.reinforcement_attempt))};
        orders_.push_back(std::move(reinforce));
        block.entity_floor = host_->world->next_entity_id() - 1;
        block.reinforcing = type;
        block.pool_token = purchase.token;
        block.issued = static_cast<std::uint64_t>(frame_);
    }
}

} // namespace eawr::script::foc::ai
