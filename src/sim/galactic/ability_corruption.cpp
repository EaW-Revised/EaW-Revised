#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "observer_internal.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
namespace {
using math::Fixed;
using CorruptionType = corruption_fixture::Type;
template<class Rows, class Id, class Key> auto find(Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
template<class Rows, class Value, class Key> void put(Rows& rows, Value value, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), key(value),
        [&](const auto& row, auto id) { return key(row) < id; });
    if (at != rows.end() && key(*at) == key(value)) *at = std::move(value);
    else rows.insert(at, std::move(value));
}
core::Result<void> invalid(const char* message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command, message));
}
void add(std::vector<TypeId>& rows, TypeId id) {
    if (!id) return;
    const auto at = std::lower_bound(rows.begin(), rows.end(), id);
    if (at == rows.end() || *at != id) rows.insert(at, id);
}
void remove(std::vector<TypeId>& rows, TypeId id) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id);
    if (at != rows.end() && *at == id) rows.erase(at);
}
}
std::span<const TypeId> GalacticSession::Impl::corruption_local_unlocks(PlanetId planet, PlayerId player) const {
    const auto* row = ability_host ? find(ability_host->planet_entitlements, std::pair{planet, player},
        [](const auto& r) { return std::pair{r.planet, r.player}; }) : nullptr;
    return row ? std::span<const TypeId>(row->build_unlocks) : std::span<const TypeId>{};
}
std::size_t GalacticSession::Impl::corruption_unit_count(std::span<const TypeId> alternatives, PlayerId player) const {
    // WGCH-49: eligibility pools the owner's alternatives, unlike the representative consumer.
    std::set<EntityId> identities;
    for (const auto& [id, live] : actor_records) {
        if (live.owner != player || live.dead || live.delete_pending
            || std::find(alternatives.begin(), alternatives.end(), live.original_type) == alternatives.end()) continue;
        const auto* policy = find(setup.ability_host->corruption_prerequisites, live.original_type, [](const auto& r) { return r.type; });
        if (policy && policy->consumable && !planets.contains(live.planet)) continue;
        EntityId counted = id;
        if (live.container && movement_inputs) {
            const auto parent = actor_records.find(live.container);
            const auto* carrier = parent == actor_records.end() ? nullptr : find(movement_inputs->types, parent->second.current_type, [](const auto& r) { return r.type; });
            if (carrier && carrier->transport) counted = live.container;
        }
        identities.insert(counted);
    }
    return identities.size();
}
std::shared_ptr<const CorruptionMenu> GalacticSession::Impl::publish_corruption_menu() const {
    if (!ability_host || !observer || !observer_projection || !economy) return {};
    for (const auto& menu : ability_host->preparations) {
        if (!menu.open || menu.kind != AbilityKind::corruption) continue;
        const auto actor = actor_records.find(menu.actor);
        if (actor == actor_records.end() || actor->second.owner != observer->local_player || actor->second.dead || actor->second.delete_pending) continue;
        if (std::none_of(observer_projection->begin(), observer_projection->end(), [&](const auto& row) {
            return row.player == observer->local_player && row.planet == menu.planet && row.discovered;
        })) continue;
        const auto* location = find(setup.ability_host->corruption_planets, menu.planet, [](const auto& r) { return r.planet; });
        const auto* account = find(economy->players, observer->local_player, [](const auto& r) { return r.player; });
        const auto* usage = find(ability_host->corruption_usage, observer->local_player, [](const auto& r) { return r.player; });
        if (!location || !account || !usage) continue;
        CorruptionMenu result; result.player = observer->local_player; result.actor = menu.actor; result.planet = menu.planet; result.preparation = menu.id;
        for (std::uint32_t i = 0; i < menu.corruption_choices.size(); ++i) {
            const auto& prepared = menu.corruption_choices[i]; const auto& choice = location->slots[prepared.slot];
            ObservedCorruptionChoice row; row.slot = prepared.slot + 1U; row.live_prepared = prepared_ability(menu, i); row.live_choice = choice;
            row.prepared.choice.type = choice.type; row.prepared.choice.credits = row.live_prepared->price;
            row.affordable = account->credits >= row.live_prepared->price;
            row.missions_satisfied = choice.required_mission_type == CorruptionType::none || usage->uses[static_cast<std::size_t>(choice.required_mission_type)] >= choice.mission_count;
            auto& live_choice = *row.live_choice;
            std::erase_if(live_choice.planet_types, [&](const auto type) {
                return std::none_of(setup.ability_host->corruption_planets.begin(), setup.ability_host->corruption_planets.end(), [&](const auto& p) { return p.type == type; });
            });
            row.planets_satisfied = live_choice.planet_types.empty() || std::any_of(setup.ability_host->corruption_planets.begin(), setup.ability_host->corruption_planets.end(),
                [&](const auto& p) {
                    const auto entity = planets.find(p.planet);
                    return entity != planets.end() && registry.get<session_detail::PlanetData>(entity->second).owner == result.player
                        && std::find(live_choice.planet_types.begin(), live_choice.planet_types.end(), p.type) != live_choice.planet_types.end();
                });
            row.units_satisfied = !choice.unit_count || choice.unit_types.empty()
                || corruption_unit_count(choice.unit_types, result.player) >= choice.unit_count;
            result.choices.push_back(std::move(row));
        }
        return std::make_shared<const CorruptionMenu>(std::move(result));
    }
    return {};
}
core::Result<bool> GalacticSession::Impl::begin_corruption(PlanetId planet, CorruptionType type, Fixed seconds) {
    using R = core::Result<bool>;
    auto* row = ability_host ? find(ability_host->transitions, planet, [](const auto& r) { return r.planet; }) : nullptr;
    const auto* economic = economy_corruption ? find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; }) : nullptr;
    if (!row || !economic || economic->destroyed) return R::success(false);
    if (type == CorruptionType::none && economic->corruption == CorruptionType::militia)
        return R::success(false); // named live revolt/retreat consumer seam; never clear a militia without its effects
    auto changed = row->transition;
    changed.current = economic->corruption;
    auto started = corruption_fixture::begin(changed, type, seconds, galactic_frames_per_second, clock.galactic_frame);
    if (!started || !started.value()) return started;
    row->transition = changed; corruption_due[changed.end].push_back(planet); touch_abilities();
    return R::success(true);
}
core::Result<void> GalacticSession::Impl::capture_corruption(PlanetId planet, PlayerId before, PlayerId after) {
    auto* row = ability_host ? find(ability_host->transitions, planet, [](const auto& r) { return r.planet; }) : nullptr;
    auto* economic = economy_corruption ? find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; }) : nullptr;
    if (!row || !economic) return core::Result<void>::success();
    const auto benefits = [&](PlayerId player) {
        const auto* account = find(economy_corruption->players, player, [](const auto& r) { return r.player; }); return account && account->benefits;
    };
    if (economic->siphon.active && economic->siphon.recipient == after) {
        economic->siphon.active = false; dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    const auto abort = [&] {
        const auto at = corruption_due.find(row->transition.end);
        if (at != corruption_due.end()) { std::erase(at->second, planet); if (at->second.empty()) corruption_due.erase(at); }
        row->transition.target = CorruptionType::none; row->transition.increasing = row->transition.decreasing = false;
        row->transition.start = row->transition.end = 0; touch_abilities();
    };
    bool request = false; CorruptionType target{};
    if (!benefits(before) && benefits(after) && !row->transition.increasing) {
        abort(); request = economic->corruption == CorruptionType::none; target = CorruptionType::racketeering;
    } else if (benefits(before) && !benefits(after)) {
        abort(); request = economic->corruption != CorruptionType::none;
    }
    if (!request) return core::Result<void>::success();
    auto begun = begin_corruption(planet, target, Fixed::from_integer(10).value()); // WGCH-168/WGM-195 code duration
    return begun ? core::Result<void>::success() : core::Result<void>::failure(begun.error());
}
core::Result<void> GalacticSession::Impl::finish_corruption_mission(std::uint64_t request, CorruptionMissionState state) {
    auto* mission = ability_host ? find(ability_host->missions, request, [](const auto& r) { return r.request; }) : nullptr;
    if (mission && native_replaying && mission->state == state && state != CorruptionMissionState::queued) return core::Result<void>::success();
    if (!mission || mission->state != CorruptionMissionState::queued || state == CorruptionMissionState::queued
        || state > CorruptionMissionState::cancelled) return invalid("corruption mission result requires an unsettled saved request");
    if (state == CorruptionMissionState::succeeded) {
        auto* usage = find(ability_host->corruption_usage, mission->player, [](const auto& r) { return r.player; });
        if (!usage || usage->uses[static_cast<std::size_t>(mission->choice.type)] == std::numeric_limits<std::uint32_t>::max())
            return invalid("corruption mission recipient or usage is unavailable");
        auto began = begin_corruption(mission->planet, mission->choice.type, Fixed::from_integer(5).value()); // WGCH-10
        if (!began) return core::Result<void>::failure(began.error());
        if (!began.value()) return core::Result<void>::success(); // WGCH-10 ordinary late transition refusal leaves the request unsettled
        ++usage->uses[static_cast<std::size_t>(mission->choice.type)];
    }
    mission->state = state;
    if (corruption_result_handler) {
        auto notified = corruption_result_handler(*mission);
        if (!notified) return notified;
    }
    if (auto* receipt = find(ability_host->results, mission->actor, [](const auto& r) { return r.actor; })) {
        receipt->outcome = state == CorruptionMissionState::succeeded ? AbilityOutcome::purchased
            : state == CorruptionMissionState::cancelled ? AbilityOutcome::cancelled : AbilityOutcome::refused;
    }
    touch_abilities(); return core::Result<void>::success();
}
core::Result<void> GalacticSession::Impl::service_corruption(const PartitionExecutor& executor, std::size_t preceding_events) {
    if (!ability_host || corruption_due.empty() || corruption_due.begin()->first > clock.galactic_frame)
        return core::Result<void>::success();
    struct Completion { PlanetId planet{}; corruption_fixture::Transition before, after; bool destroyed{}; };
    std::vector<Completion> copied;
    for (auto at = corruption_due.begin(); at != corruption_due.end() && at->first <= clock.galactic_frame; ++at)
        for (const auto id : at->second) {
            const auto* row = find(ability_host->transitions, id, [](const auto& r) { return r.planet; });
            const auto* economic = find(economy_corruption->planets, id, [](const auto& r) { return r.planet; });
            if (row && economic && row->transition.active() && row->transition.end == at->first)
                copied.push_back({id, row->transition, {}, economic->destroyed});
        }
    std::sort(copied.begin(), copied.end(), [](const auto& a, const auto& b) { return a.planet < b.planet; });
    auto staged = copied;
    const auto partitions = std::min(executor.worker_count(), std::max(std::size_t{1}, copied.size()));
    auto phase = executor.execute_phase("galactic-corruption-completion", partitions, [&](std::size_t partition) {
        const auto range = PartitionRange{partition * copied.size() / partitions, (partition + 1) * copied.size() / partitions};
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& input = copied[i];
            staged[i].after = {input.destroyed ? CorruptionType::none : input.before.target,
                CorruptionType::none, false, false, 0, 0, clock.galactic_frame};
        }
    });
    if (!phase) return phase;
    corruption_due.erase(corruption_due.begin(), corruption_due.upper_bound(clock.galactic_frame));
    for (const auto& item : staged) {
        auto* row = find(ability_host->transitions, item.planet, [](const auto& r) { return r.planet; });
        row->transition = item.after;
        if (item.destroyed) continue; // ruins abort; no reward or repeat callback
        auto applied = apply_corruption_completion(item.planet, item.before.current, item.after.current, {}, preceding_events);
        if (!applied) return applied;
    }
    touch_abilities(); return core::Result<void>::success();
}
core::Result<void> GalacticSession::Impl::apply_corruption_completion(PlanetId planet, CorruptionType old, CorruptionType next,
    std::optional<PlayerId> recipient, std::optional<std::size_t> preceding_events) {
    auto* economic = find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; });
    if (!economic || economic->destroyed) return invalid("corruption completion planet unavailable");
    economic->corruption = next;
    if (old == CorruptionType::intimidation || next == CorruptionType::intimidation)
        if (auto refreshed = refresh_planet_technology_price(planet); !refreshed) return refreshed;
    dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    // Publish the same authoritative type to existing movement/visibility inputs.
    // These are action commits, never an idle rescan or a second corruption owner.
    const bool corrupted = next != CorruptionType::none;
    if (movement_inputs) {
        const auto* before = find(movement_inputs->planets, planet, [](const auto& r) { return r.planet; });
        if (before && before->corrupted != corrupted) {
            auto updated = std::make_shared<MovementSetup>(*movement_inputs);
            find(updated->planets, planet, [](const auto& r) { return r.planet; })->corrupted = corrupted;
            movement_inputs = std::move(updated); dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        }
    }
    if (observer) {
        auto* location = find(observer->planets, planet, [](const auto& r) { return r.planet; });
        if (location) {
            location->corrupted = corrupted;
            const auto presence = observer_detail::presence(*observer, sorted_fleets(), movement_inputs.get(), actor_inputs());
            for (auto& record : observer->records) if (record.planet == planet) {
                const auto* player = find(observer->players, record.player, [](const auto& r) { return r.player; });
                if (player) observer_detail::refresh(record, *player, *location, presence, clock.galactic_frame);
            }
            dirty[static_cast<std::size_t>(StateSection::observer)] = true;
        }
    }
    invalidate_observer_projection();
    const auto* authored = find(setup.ability_host->corruption_planets, planet, [](const auto& r) { return r.planet; });
    const auto reward = [&](CorruptionType type) -> const CorruptionChoice* {
        if (!authored || type == CorruptionType::none) return nullptr;
        const auto at = std::find_if(authored->slots.begin(), authored->slots.end(), [&](const auto& s) { return s.type == type; });
        return at == authored->slots.end() ? nullptr : &*at;
    };
    if (corruption_completion_handler) {
        auto notified = corruption_completion_handler(planet, old, next, CorruptionCompletionPhase::state_committed, ability_host->planet_entitlements);
        if (!notified) return notified;
    }
    const auto* added = reward(next); const auto* removed = reward(old);
    const auto beneficiary = std::find_if(economy_corruption->players.begin(), economy_corruption->players.end(),
        [&](const auto& p) { return recipient ? p.player == *recipient : p.benefits; }); // WGCH-161: current first beneficiary, never retained actor owner
    // WGP-54/WGCH-75: a due completion voices the local owner first, otherwise
    // the designated beneficiary if local. Startup and ruins cleanup are silent.
    if (preceding_events && feedback_notice_handler && observer && observer->local_player) {
        const auto location = planets.find(planet);
        const auto owner = location == planets.end() ? 0 : registry.get<session_detail::PlanetData>(location->second).owner;
        const auto local = observer->local_player;
        if (owner == local || (beneficiary != economy_corruption->players.end() && beneficiary->player == local))
            feedback_notice_handler({next == CorruptionType::none ? GalacticFeedbackNotice::Kind::corruption_removed
                : GalacticFeedbackNotice::Kind::planet_corrupted, local, planet, *preceding_events});
    }
    for (const auto& player : setup.ability_host->corruption_players) {
        const auto key = std::pair{planet, player.player};
        auto* entitlements = find(ability_host->planet_entitlements, key, [](const auto& r) { return std::pair{r.planet, r.player}; });
        if (!entitlements) {
            put(ability_host->planet_entitlements, PlanetAbilityEntitlements{planet, player.player, {}, {}, {}},
                [](const auto& r) { return std::pair{r.planet, r.player}; });
            entitlements = find(ability_host->planet_entitlements, key, [](const auto& r) { return std::pair{r.planet, r.player}; });
        }
        add(entitlements->automatic_upgrades, player.automatic_upgrades[static_cast<std::size_t>(next)]);
        remove(entitlements->automatic_upgrades, player.automatic_upgrades[static_cast<std::size_t>(old)]);
    }
    if (corruption_completion_handler) {
        auto notified = corruption_completion_handler(planet, old, next, CorruptionCompletionPhase::automatic_upgrades, ability_host->planet_entitlements);
        if (!notified) return notified;
    }
    if (beneficiary != economy_corruption->players.end()) {
        const auto* player = find(setup.ability_host->corruption_players, beneficiary->player, [](const auto& r) { return r.player; });
        if (!player) return invalid("corruption beneficiary missing authored inputs");
        auto* entitlements = find(ability_host->planet_entitlements, std::pair{planet, player->player}, [](const auto& r) { return std::pair{r.planet, r.player}; });
        if (added && added->credit_reward > Fixed{}) {
            auto* account = find(economy->players, player->player, [](const auto& r) { return r.player; });
            if (!account) return invalid("corruption reward recipient unavailable");
            const auto multiplier = setup.fiscal ? setup.fiscal->players[static_cast<std::size_t>(account - economy->players.data())].positive_credit_multiplier : corruption_fixture::one;
            auto paid = adjusted_credits(account->credits, added->credit_reward, multiplier, account->credit_cap);
            if (!paid) return core::Result<void>::failure(paid.error());
            auto delta = math::subtract(paid.value(), account->credits);
            if (!delta) return core::Result<void>::failure(delta.error());
            if (credit_notification_handler) {
                auto notice = credit_notification_handler({CreditNotification::Kind::windfall, player->player, paid.value(), delta.value(), {}});
                if (!notice) return notice;
            }
            account->credits = paid.value();
            dirty[static_cast<std::size_t>(StateSection::players)] = true;
        }
        if (added) {
            add(entitlements->tactical_upgrades, added->tactical_upgrade);
            if (added->unlock_build_cost > Fixed{}) add(entitlements->build_unlocks, added->build_unlock);
        }
        if (removed) {
            remove(entitlements->tactical_upgrades, removed->tactical_upgrade);
            remove(entitlements->build_unlocks, removed->build_unlock);
        }
    }
    if (corruption_completion_handler) {
        auto notified = corruption_completion_handler(planet, old, next, CorruptionCompletionPhase::rewards, ability_host->planet_entitlements);
        if (!notified) return notified;
    }
    touch_abilities(); return core::Result<void>::success();
}
core::Result<void> GalacticSession::Impl::initialize_corruption() {
    if (!ability_host) return core::Result<void>::success();
    for (const auto& request : setup.ability_host->initial_corruption) {
        auto applied = apply_corruption_completion(request.planet, CorruptionType::none, request.type, request.recipient);
        if (!applied) return applied;
        auto* row = find(ability_host->transitions, request.planet, [](const auto& r) { return r.planet; });
        row->transition = {request.type, CorruptionType::none, false, false, 0, 0, 0};
    }
    return core::Result<void>::success();
}

void GalacticSession::Impl::prepare_corruption(const GameCommand& command, const CensusObject& actor,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto& request = std::get<PrepareGalacticAbility>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    const auto* target = find(setup.ability_host->corruption_planets, request.planet, [](const auto& r) { return r.planet; });
    const auto* economic = find(economy_corruption->planets, request.planet, [](const auto& r) { return r.planet; });
    const auto* state = find(ability_host->transitions, request.planet, [](const auto& r) { return r.planet; });
    const auto* profile = find(setup.ability_host->profiles, actor.current_type, [](const auto& r) { return r.type; });
    if (!target || !economic || !state || !profile || !profile->corruption || !control || !setup.ability_host->tutorial_corruption_allowed
        || economic->destroyed || economic->corruption != CorruptionType::none || state->transition.active()
        || !target->retains_influence || std::find(setup.ability_host->activated_corruption_abilities.begin(),
            setup.ability_host->activated_corruption_abilities.end(), profile->corruption->name) == setup.ability_host->activated_corruption_abilities.end()) return reject();
    const auto* political = find(control->planets, request.planet, [](const auto& r) { return r.planet; });
    const auto planet_entity = planets.find(request.planet);
    if (planet_entity == planets.end()) return reject();
    const auto object_owner = registry.get<session_detail::PlanetData>(planet_entity->second).owner;
    const auto* owner_player = find(control->players, object_owner, [](const auto& r) { return r.player; });
    if (!political || !owner_player || owner_player->neutral || object_owner == actor.owner) return reject();
    for (const auto& relation : control->relations)
        if (relation.player == actor.owner && relation.other == object_owner && relation.allied) return reject();
    if (actor.container) {
        EntityId parent = actor.container; std::set<EntityId> ancestry;
        while (parent) {
            if (!ancestry.insert(parent).second || ancestry.size() > galactic_max_objects) return reject();
            const auto fleet = fleets.find(parent);
            if (fleet != fleets.end()) {
                const auto& members = registry.get<session_detail::FleetData>(fleet->second).members;
                std::vector<FleetMember> pending_members(members.begin(), members.end()); std::set<EntityId> visited;
                std::size_t count{};
                while (!pending_members.empty()) {
                    const auto member = pending_members.back(); pending_members.pop_back();
                    if (!visited.insert(member.id).second || visited.size() > galactic_max_objects) return reject();
                    const auto* traits = movement_inputs ? find(movement_inputs->types, member.type, [](const auto& r) { return r.type; }) : nullptr;
                    if (movement_inputs && !traits) return reject();
                    if (traits && (traits->squadron || traits->fleet_container)) {
                        const auto* contents = find(movement_inputs->contents, member.id, [](const auto& r) { return r.container; });
                        if (!contents) return reject();
                        pending_members.insert(pending_members.end(), contents->children.begin(), contents->children.end());
                    } else if (++count > 1) return reject(); // WGCH-02: transport payloads are a separate lookup
                }
                break;
            }
            const auto contained = actor_records.find(parent);
            if (contained == actor_records.end()) return reject();
            parent = contained->second.container;
        }
    }
    AbilityPreparation prepared; prepared.actor = actor.id;
    prepared.planet = request.planet; prepared.kind = AbilityKind::corruption; prepared.open = true;
    for (std::size_t i = 0; i < target->slots.size(); ++i) {
        const auto& choice = target->slots[i];
        if (choice.type == CorruptionType::none) continue;
        prepared.corruption_choices.push_back({choice.type, static_cast<std::uint8_t>(i)});
        prepared.offers.push_back({static_cast<std::uint32_t>(i + 1), choice.price, false});
    }
    if (prepared.corruption_choices.empty()) return reject();
    prepared.id = ability_host->next_request++;
    put(ability_host->preparations, std::move(prepared), [](const auto& r) { return r.actor; }); touch_abilities();
    event.kind = GalacticEventKind::ability_prepared; event.value = ability_host->next_request - 1; events.push_back(event);
}
void GalacticSession::Impl::execute_corruption(const GameCommand& command, const CensusObject& actor,
    const PreparedAbility& prepared, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    auto* account = economy ? find(economy->players, actor.owner, [](const auto& r) { return r.player; }) : nullptr;
    auto* usage = find(ability_host->corruption_usage, actor.owner, [](const auto& r) { return r.player; });
    const auto* target = find(setup.ability_host->corruption_planets, prepared.planet, [](const auto& r) { return r.planet; });
    const auto* economic = find(economy_corruption->planets, prepared.planet, [](const auto& r) { return r.planet; });
    const auto* transition = find(ability_host->transitions, prepared.planet, [](const auto& r) { return r.planet; });
    if (!account || !usage || !target || !economic || !transition || economic->destroyed || transition->transition.active()
        || economic->corruption != CorruptionType::none || prepared.corruption.slot >= 3 || account->credits < prepared.price
        || !ability_consumption_handler) return reject();
    auto defeat = prepare_ability_defeat(actor, Fixed{}, true);
    if (!defeat) return reject();
    const auto& choice = target->slots[prepared.corruption.slot];
    if (choice.type != prepared.corruption.type) return reject();
    // WGCH-05/72: charge before the stale unit recheck, with no refund on later refusal.
    if (prepared.price > Fixed{}) {
        auto balance = math::subtract(account->credits, prepared.price), delta = math::negate(prepared.price);
        if (!balance || !delta) { failed = true; return; }
        if (credit_notification_handler) {
            auto notice = credit_notification_handler({CreditNotification::Kind::windfall, actor.owner, balance.value(), delta.value(), {}});
            if (!notice) { failed = true; return; }
        }
        account->credits = balance.value(); dirty[static_cast<std::size_t>(StateSection::players)] = true;
    }
    if (choice.unit_count && !choice.unit_types.empty() && corruption_unit_count(choice.unit_types, actor.owner) < choice.unit_count) return reject();
    struct Candidate { CensusObject actor; EntityId removal{}; bool consume{}, named{}; std::uint32_t weight{}; };
    std::vector<Candidate> candidates;
    // WGCH-124: the representative getter has no ownership filter; an enemy instance can select it.
    TypeId representative = choice.unit_types.empty() ? 0 : choice.unit_types.front();
    for (const auto type : choice.unit_types) {
        if (std::any_of(actor_records.begin(), actor_records.end(), [&](const auto& entry) {
            return entry.second.original_type == type && !entry.second.dead && !entry.second.delete_pending;
        })) { representative = type; break; }
    }
    if (representative && choice.unit_count) {
        const auto type = representative;
        std::set<EntityId> seen;
        const auto* policy = find(setup.ability_host->corruption_prerequisites, type, [](const auto& r) { return r.type; });
        for (const auto& [id, row] : actor_records) {
            if (row.owner != actor.owner || row.original_type != type || row.dead || row.delete_pending
                || (policy && policy->consumable && !planets.contains(row.planet))) continue;
            EntityId removal = id;
            if (policy && !policy->named && row.container && movement_inputs) {
                const auto parent = actor_records.find(row.container);
                const auto* carrier = parent == actor_records.end() ? nullptr : find(movement_inputs->types,
                    parent->second.current_type, [](const auto& r) { return r.type; });
                if (carrier && carrier->transport) removal = row.container;
            }
            if (seen.insert(removal).second) candidates.push_back({row, removal, policy && policy->consumable, policy && policy->named, 0});
        }
    }
    const auto count = std::min(candidates.size(), static_cast<std::size_t>(choice.unit_count));
    if (count && candidates.front().consume && !candidates.front().named) {
        for (auto& candidate : candidates) {
            auto draw = economy_draw(*economy_corruption, setup.seed);
            if (!draw) { failed = true; return; }
            candidate.weight = static_cast<std::uint32_t>(draw.value().raw() * 5 / Fixed::scale);
        }
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.weight > b.weight; });
        // U-03: unresolved selected or boundary tie never invents an ID ordering.
        for (std::size_t i = 1; i < candidates.size() && i <= count; ++i)
            if (candidates[i - 1].weight == candidates[i].weight) return reject();
    }
    if (count && candidates.front().consume && candidates.front().named && candidates.size() != 1) return reject(); // named-instance ambiguity seam
    for (std::size_t i = 0; i < count; ++i) {
        const auto& candidate = candidates[i]; if (!candidate.consume) continue;
        if (candidate.named) {
            auto prerequisite = prepare_ability_defeat(candidate.actor, Fixed{}, true);
            if (!prerequisite) return reject();
            // Named prerequisites remove only that instance, never siblings.
            prerequisite->companions.clear(); prerequisite->removed_objects.clear(); prerequisite->parent = HeroParent::none;
            if (auto consumed = ability_consumption_handler(*prerequisite); !consumed) { failed = true; return; }
        } else {
            HeroDefeat consumed; consumed.id = candidate.removal; consumed.type = consumed.respawn_type = candidate.actor.original_type; consumed.owner = candidate.actor.owner;
            std::vector<EntityId> nested{candidate.removal}; std::set<EntityId> visited;
            while (!nested.empty()) {
                const auto id = nested.back(); nested.pop_back();
                if (!visited.insert(id).second) continue;
                if (visited.size() > galactic_max_objects) { failed = true; return; }
                consumed.removed_objects.push_back(id);
                const auto* contents = movement_inputs ? find(movement_inputs->contents, id, [](const auto& r) { return r.container; }) : nullptr;
                if (contents) for (const auto& child : contents->children) nested.push_back(child.id);
            }
            auto lost = ability_consumption_handler(consumed);
            if (!lost) { failed = true; return; }
        }
    }
    const auto* player = find(economy_corruption->players, actor.owner, [](const auto& r) { return r.player; });
    const bool tactical = !choice.map.empty() && (choice.mode == 1 || choice.mode == 2) && player && player->human
        && !setup.ability_host->multiplayer && !setup.ability_host->fast_corruption;
    const auto request = ability_host->next_request++;
    if (tactical) {
        // WGCH-72: a failed mission queue can follow the debit and prerequisite consumption; the refusal is retail order.
        if (!corruption_mission_handler || ability_host->missions.size() >= galactic_max_objects) { touch_abilities(); return reject(); }
        LiveCorruptionMission mission{request, actor.id, prepared.planet, actor.owner, prepared.corruption, CorruptionMissionState::queued};
        auto queued = corruption_mission_handler(mission, choice);
        if (!queued) { failed = true; return; }
        if (!queued.value()) { touch_abilities(); return reject(); }
        ability_host->missions.push_back(mission);
    } else {
        if (usage->uses[static_cast<std::size_t>(choice.type)] == std::numeric_limits<std::uint32_t>::max()) { touch_abilities(); return reject(); }
        auto started = begin_corruption(prepared.planet, choice.type, Fixed::from_integer(5).value());
        if (!started) { failed = true; return; }
        if (!started.value()) { touch_abilities(); return reject(); }
        ++usage->uses[static_cast<std::size_t>(choice.type)];
    }
    if (auto consumed = ability_consumption_handler(*defeat); !consumed) { failed = true; return; }
    auto* retained = find(ability_host->preparations, actor.id, [](const auto& r) { return r.actor; }); retained->open = false;
    AbilityResult receipt{request, prepared.preparation, actor.id, prepared.planet, actor.owner, 0, AbilityOutcome::purchased};
    receipt.kind = AbilityKind::corruption;
    put(ability_host->results, std::move(receipt), [](const auto& r) { return r.actor; }); touch_abilities();
    event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event);
}
void GalacticSession::set_corruption_mission_handler(CorruptionMissionHandler handler) { impl_->corruption_mission_handler = std::move(handler); }
void GalacticSession::set_corruption_completion_handler(CorruptionCompletionHandler handler) { impl_->corruption_completion_handler = std::move(handler); }
void GalacticSession::set_corruption_result_handler(CorruptionResultHandler handler) { impl_->corruption_result_handler = std::move(handler); }
core::Result<bool> GalacticSession::begin_corruption(PlanetId planet, CorruptionType type, Fixed seconds) {
    if (!planet || type > CorruptionType::bonded_citizens || seconds < Fixed{}) return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_command, "invalid corruption transition input"));
    if (const auto* row = impl_->ability_host ? find(impl_->ability_host->transitions, planet, [](const auto& r) { return r.planet; }) : nullptr) {
        auto probe = row->transition;
        if (const auto* economic = impl_->economy_corruption ? find(impl_->economy_corruption->planets, planet, [](const auto& r) { return r.planet; }) : nullptr)
            probe.current = economic->corruption;
        auto admission = corruption_fixture::begin(probe, type, seconds, galactic_frames_per_second, impl_->clock.galactic_frame);
        if (!admission) return admission; // malformed arithmetic never enters the native journal
    }
    if (!impl_->native_replaying) {
        std::vector<std::uint8_t> payload; detail::put_u8(payload, 0); detail::put_u64(payload, planet);
        detail::put_u8(payload, static_cast<std::uint8_t>(type)); detail::put_i64(payload, seconds.raw());
        auto recorded = append_native_effect(NativeEffectKind::corruption_lifecycle, std::move(payload));
        if (!recorded) return core::Result<bool>::failure(recorded.error());
    }
    return impl_->begin_corruption(planet, type, seconds);
}
core::Result<void> GalacticSession::finish_corruption_mission(std::uint64_t request, CorruptionMissionState state) {
    const auto* mission = impl_->ability_host ? find(impl_->ability_host->missions, request, [](const auto& r) { return r.request; }) : nullptr;
    if (mission && impl_->native_replaying && mission->state == state && state != CorruptionMissionState::queued) return core::Result<void>::success();
    if (!mission || mission->state != CorruptionMissionState::queued || state == CorruptionMissionState::queued || state > CorruptionMissionState::cancelled)
        return invalid("corruption mission result requires an unsettled saved request");
    if (!impl_->native_replaying) {
        std::vector<std::uint8_t> payload; detail::put_u8(payload, 1); detail::put_u64(payload, request);
        detail::put_u8(payload, static_cast<std::uint8_t>(state));
        auto recorded = append_native_effect(NativeEffectKind::corruption_lifecycle, std::move(payload));
        if (!recorded) return recorded;
    }
    return impl_->finish_corruption_mission(request, state);
}
} // namespace eawr::sim::galactic
