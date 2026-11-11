#include "session_impl.hpp"
#include "session_tick.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;

const LandCompanyBinding* company_binding(const LandSessionContent& content, const TypeId type) noexcept {
    const auto found = std::find_if(content.companies.begin(), content.companies.end(),
        [type](const LandCompanyBinding& row) { return row.type == type; });
    return found != content.companies.end() ? &*found : nullptr;
}
}

// LC-48 (#5894): the Update_Tactical_Population_Cap query. The faction base capacity plus the capacity of every live object
// the player owns; multiplayer adds the live objects of its allies. The retail refresh cadence is not located, so the value
// is taken at the moment a reinforcement is asked for.
std::int64_t TacticalSession::Impl::land_capacity(const PlayerId owner, const UnitStage& staged) const {
    const auto& rules = land->content.reinforcement;
    std::int64_t total = rules.base_capacity;
    for (const auto& [id, unit] : staged) {
        static_cast<void>(id);
        if (unit.durability && unit.durability->hull.raw() <= 0) continue;
        const auto capacity = rules.capacity(unit.state.type_id);
        if (capacity == 0) continue;
        if (unit.state.owner == owner || (rules.multiplayer && allied(unit.state.owner, owner))) total += capacity;
    }
    return total;
}

// LC-46/47: Get_Max_Company_Count. An unknown player has no cap (0).
std::int64_t TacticalSession::Impl::land_company_cap(const PlayerId owner, const std::int64_t capacity) const {
    const auto& rules = land->content.reinforcement;
    if (rules.multiplayer) {
        const auto curve = rules.cap(owner);
        return curve ? std::min(capacity, *curve) : 0;
    }
    if (player_of(owner) == nullptr) return 0;
    std::int64_t maximum = rules.max_ground_forces;
    if (owner != rules.defender) maximum = std::min(capacity, maximum);
    return maximum;
}

// LC-48/50: Get_Current_Company_Count. Population shares of the owner's objects plus a whole company value for each of its
// companies still in flight; floor, plus one when the fraction exceeds Allow_Reinforcement_Percentage_Normalized.
std::int64_t TacticalSession::Impl::land_company_count(const PlayerId owner, const std::map<EntityId, PopulationShare>& owner_shares,
    const UnitStage& staged) const {
    const auto& rules = land->content.reinforcement;
    std::int64_t total = 0;
    for (const auto& [id, share] : owner_shares) {
        // A member killed earlier in this frame no longer counts, whether or not the economy service purged its share yet.
        // A member inside a garrison host (G-05) keeps counting against the cap.
        if (share.owner == owner && (staged.find(id) != staged.end() || land->garrison_hiding(id) != nullptr)) total += share.share;
    }
    for (const auto& landing : land->landings) {
        if (landing.owner != owner) continue;
        const auto* binding = company_binding(land->content, landing.company);
        if (binding != nullptr) total += population_share(binding->company.population_value.value_or(0), 1);
    }
    if (total <= 0) return 0;
    std::int64_t whole = total / population_share_scale;
    const std::int64_t fraction = total % population_share_scale;
    if (fraction * Fixed::scale > rules.allow_fraction.raw() * population_share_scale) ++whole;
    return whole;
}

// LC-42..51: Reinforce on land. Refusals mirror the retail order: pool, cap, placement. The company itself exists from the
// landing's finalize frame (LC-51); until then it is a charge against the cap and a record in the land section.
core::Result<void> TacticalSession::Impl::execute_land_reinforcement(const PlayerCommand& command, const std::uint64_t tick,
    UnitStage& staged, EconomyStage& stage, PlayerEconomy& ledger, const EconomyPlayer& player, Event& event,
    std::vector<Event>& events) {
    using Void = core::Result<void>;
    const auto finish = [&](const RejectReason reason) {
        event.reason = reason;
        if (reason != RejectReason::none) event.kind = EventKind::order_rejected;
        events.push_back(event);
        return Void::success();
    };
    const auto issuer = command.key.player_id;
    const auto& reinforce = std::get<ReinforcePayload>(command.payload);
    const auto& rules = land->content.reinforcement;
    const auto* binding = company_binding(land->content, reinforce.type);
    if (!rules.enabled || binding == nullptr || economy.disabled_types.contains(reinforce.type))
        return finish(RejectReason::cannot_produce);
    if (outcome) return finish(RejectReason::battle_decided);
    auto pooled = ledger.pool.end();
    if (reinforce.pool_token == 0) {
        pooled = std::find(ledger.pool.begin(), ledger.pool.end(), reinforce.type);
    } else {
        const auto token = std::find(ledger.pool_tokens.begin(), ledger.pool_tokens.end(), reinforce.pool_token);
        if (token != ledger.pool_tokens.end()) {
            const auto index = static_cast<std::size_t>(token - ledger.pool_tokens.begin());
            if (index < ledger.pool.size() && ledger.pool[index] == reinforce.type)
                pooled = ledger.pool.begin() + static_cast<std::ptrdiff_t>(index);
        }
    }
    if (pooled == ledger.pool.end()) return finish(RejectReason::not_in_pool);
    const auto pool_index = static_cast<std::size_t>(pooled - ledger.pool.begin());
    const auto pool_token = pool_index < ledger.pool_tokens.size() ? ledger.pool_tokens[pool_index] : std::uint64_t{0};
    // LC-46..48: Is_Room_For_Reinforcements. The company's population rounds to a whole number.
    const std::int64_t population = binding->company.population_value.value_or(0);
    const auto maximum = land_company_cap(issuer, land_capacity(issuer, staged));
    const auto current = land_company_count(issuer, stage.shares, staged);
    if (population > maximum - current) {
        stage.cues.push_back({issuer, reinforce.type, tick, BattleEconomyCue::Kind::unit_cap});
        return finish(RejectReason::no_population_room);
    }
    // LC-42/43: the whole company or nothing. The landing point is on the plane; terrain height is read where members stand.
    const math::Vec3 point{reinforce.position.x, reinforce.position.y, Fixed{}};
    // Can_Land_Reinforcements always uses the player's reinforcement facing; the command's own facing is not read (LR-FACING).
    const auto yaw = player.reinforcement_yaw;
    const auto admitted = land_company_admission(*binding, issuer, point, yaw, staged);
    if (!admitted) return Void::failure(admitted.error());
    if (!admitted.value()) return finish(RejectReason::invalid_position);
    // LC-51: the unload steps are one squad per step for a team company, one member per step otherwise.
    const auto plan = units::plan_land_company(binding->company);
    if (!plan) return finish(RejectReason::cannot_produce);
    const std::uint64_t steps = plan->forms_squads ? plan->squads.size() : plan->members.size();
    LandLanding landing;
    landing.sequence = land->next_landing++;
    landing.owner = issuer; landing.company = reinforce.type; landing.position = point; landing.yaw = yaw;
    landing.created_frame = tick; landing.pool_token = pool_token;
    landing.seat = command.origin == CommandOrigin::seat ? command.issuing_seat : SeatId{};
    // Waiting (delay), the landing clip, then one unload step per spawn call and a last one that finalizes.
    landing.finalize_frame = tick + rules.delay_frames + rules.landing_frames + rules.unload_frames * (steps + 1);
    land->landings.push_back(landing);
    ledger.pool_controllers.erase(pool_token);
    if (pool_index < ledger.pool_tokens.size())
        ledger.pool_tokens.erase(ledger.pool_tokens.begin() + static_cast<std::ptrdiff_t>(pool_index));
    ledger.pool.erase(pooled);
    ++ledger.pool_version;
    return finish(RejectReason::none);
}

// LC-52/53: the shared four-frame capture service over the land objects that carry a capture profile. The candidates are
// every live land body; eligibility, radius, contest and ownership retention are the shared rules (pads.cpp). The serial
// loop visits the capture points only, a handful per map, against the roster once per service frame.
core::Result<void> session_detail::Tick::land_capture(std::vector<LiveUnit>& survivors, std::vector<Event>& events) {
    auto& pads = impl_->pad_stage.values;
    if ((tick + 1) % 4 != 0 || pads.empty()) return core::Result<void>::success();
    const auto& rules = impl_->land->content.reinforcement;
    const auto neutral = impl_->economy.pads.neutral;
    const auto find = [&](const EntityId id) -> LiveUnit* {
        const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
            [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
        return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    std::vector<CaptureCandidate> bodies;
    std::vector<CaptureCandidate> near;
    for (auto& [id, state] : pads) {
        if (state.under_construction != invalid_entity_id || state.constructed != invalid_entity_id) continue;
        auto* point = find(id);
        if (point == nullptr || (point->durability && point->durability->hull.raw() <= 0)) continue;
        const auto* profile = impl_->economy.pads.point(point->state.type_id);
        if (profile == nullptr) continue;
        // LC-52: a single land battle opens capture after Land_Capture_Allowed_Countdown_Seconds, except a neutral point.
        if (!rules.multiplayer && tick < rules.capture_countdown_frames && point->state.owner != neutral) continue;
        if (bodies.empty()) {
            bodies.reserve(survivors.size());
            // LC-01: squad command wrappers are not physical bodies; their members carry influence.
            std::vector<EntityId> wrappers;
            for (const auto& squad : impl_->land->movement.squads()) wrappers.push_back(squad.object);
            std::sort(wrappers.begin(), wrappers.end());
            for (const auto& unit : survivors)
                if (!std::binary_search(wrappers.begin(), wrappers.end(), unit.state.entity_id))
                    bodies.push_back({SpaceBody{unit.state.entity_id, unit.state.owner, unit.state.position}, unit.state.type_id,
                        !unit.durability || unit.durability->hull.raw() > 0});
        }
        near.clear();
        for (const auto& candidate : bodies)
            if (within_range(point->state.position, candidate.body.position, profile->radius, RangeMetric::spatial)) near.push_back(candidate);
        auto next = state;
        const auto owner = service_capture(*profile, next, point->state.owner, neutral, impl_->setup.players, near,
            impl_->economy.pads, id, point->state.position);
        if (next != state) { impl_->pad_stage.touch(id); state = next; }
        if (owner == point->state.owner) continue;
        const auto before = point->state;
        std::map<EntityId, TypeId> containers;
        for (const auto& account : staging_->ledgers.value()) for (const auto& held : account.completed)
            if (const auto* host = find(held.station)) containers.emplace(held.station, host->state.type_id);
        std::vector<CombatBonuses> categories(impl_->bonus_categories.size());
        const auto bonuses = impl_->command_bonuses_for(*point, impl_->command_ledger, staging_->ledgers.value(), containers,
            categories, nullptr, true, owner);
        if (const auto changed = impl_->transfer_owner(*point, owner, bonuses); !changed) return core::Result<void>::failure(changed.error());
        track_victory_change(&before, &point->state, true);
        events.push_back(Event{tick, EventKind::pad_captured, owner, before.owner, id});
        if (const auto root = impl_->land->static_roots.find(id); root != impl_->land->static_roots.end()) root->second.owner = owner;
        if (const auto bound = impl_->land->bindings.find(id); bound != impl_->land->bindings.end()) bound->second.second = owner;
    }
    return core::Result<void>::success();
}

// LC-51: companies reach the ground. Each landing whose finalize frame has come creates its objects through the same
// creation path the setup roster uses, registers their population and becomes controllable at once (the retail finalize
// step unlocks, selects and registers every member).
core::Result<void> session_detail::Tick::land_reinforcements(std::vector<LiveUnit>& live, std::vector<Event>& events,
    std::vector<core::Diagnostic>& diagnostics) {
    auto& land = *impl_->land;
    if (land.landings.empty()) return core::Result<void>::success();
    // LR-OUTCOME: a decided battle (or one deciding this frame) finalizes nothing; a waiting transport does not keep a side
    // alive and its company never lands into a decided battle. The records end with the battle.
    if (impl_->outcome || pending_outcome_) { land.landings.clear(); return core::Result<void>::success(); }
    if (std::none_of(land.landings.begin(), land.landings.end(), [this](const LandLanding& row) { return row.finalize_frame <= tick; }))
        return core::Result<void>::success();
    core::tick_profile::Scope profile("serial.tactical.land-reinforce");
    const auto& terrain = *land.occupancy.terrain_handle();
    for (std::size_t index = 0; index < land.landings.size();) {
        if (land.landings[index].finalize_frame > tick) { ++index; continue; }
        const auto landing = land.landings[index];
        land.landings.erase(land.landings.begin() + static_cast<std::ptrdiff_t>(index));
        const auto* binding = company_binding(land.content, landing.company);
        // LR-REFUND: a landing that cannot finish never aborts the match. The owner gets the bought company back in its pool
        // (new token, same seat), an order_rejected event names the reason, and a warning says why; its population charge
        // ends with the record.
        const auto refuse = [&](const char* reason, const RejectReason why) {
            diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                std::string("land reinforcement ") + reason, {}, core::Severity::warning));
            auto& accounts = impl_->ledgers;
            const auto account = std::find_if(accounts.begin(), accounts.end(),
                [&](const PlayerEconomy& entry) { return entry.player == landing.owner; });
            if (account != accounts.end()) {
                const auto token = account->next_pool_token++;
                account->pool.push_back(landing.company);
                account->pool_tokens.push_back(token);
                if (landing.seat.value != 0) account->pool_controllers.emplace(token, landing.seat);
                ++account->pool_version;
                ++account->pool_additions;
                account->pool_addition_frame = tick;
            }
            Event rejected{tick, EventKind::order_rejected, landing.owner, landing.sequence};
            rejected.reason = why;
            events.push_back(rejected);
        };
        if (binding == nullptr) { refuse("has no company binding", RejectReason::cannot_produce); continue; }
        if (!impl_->next_id || impl_->next_id == std::numeric_limits<EntityId>::max()) { refuse("has no identity left", RejectReason::cannot_produce); continue; }
        const auto rotation = yaw_rotation(landing.yaw);
        if (!rotation) { refuse("has an unusable facing", RejectReason::invalid_position); continue; }
        UnitState entry{impl_->next_id++, binding->type, landing.owner,
            {landing.position.x, landing.position.y, terrain.height_at(landing.position.x, landing.position.y).value_or(Fixed{})},
            rotation.value(), {}};
        if (auto assigned = impl_->attribute_output(entry, landing.seat, landing.pool_token); !assigned) {
            refuse("has no eligible controller", RejectReason::unit_not_controlled); continue;
        }
        auto moving = land.movement.checkpoint();
        std::vector<LiveUnit> added;
        LandSpawnFilter filter;
        for (const auto& member : moving.members) filter.bodies.push_back({member.position, member.profile.radius});
        if (auto created = impl_->spawn_land_company(terrain, entry, *binding, moving, added, filter); !created) {
            refuse("could not create its company", RejectReason::invalid_position); continue;
        }
        if (added.empty()) { refuse("created no member", RejectReason::invalid_position); continue; }
        if (live.size() + added.size() > max_units) { refuse("exceeds the unit budget", RejectReason::no_population_room); continue; }
        std::sort(moving.members.begin(), moving.members.end(), [](const auto& a, const auto& b) { return a.object < b.object; });
        auto movement = LandMovement::from_checkpoint(std::move(moving));
        if (!movement) { refuse("produced an invalid roster", RejectReason::invalid_position); continue; }
        land.movement = std::move(*movement);
        // LC-02/48: each created member carries the company population divided by the company's member count.
        const auto share = population_share(binding->company.population_value.value_or(0),
            static_cast<std::uint32_t>(binding->company.members.size()));
        auto& shares = impl_->shares;
        const auto first = added.front().state.entity_id;
        for (auto& unit : added) {
            const auto id = unit.state.entity_id;
            land.bindings.emplace(id, std::pair{unit.state.type_id, unit.state.owner});
            land.landed.insert(std::upper_bound(land.landed.begin(), land.landed.end(), id), id);
            track_victory_change(nullptr, &unit.state, false);
        }
        for (const auto& member : land.movement.members())
            if (member.object >= first && share > 0) shares.emplace(member.object, PopulationShare{landing.owner, share});
        live.insert(live.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
        std::sort(live.begin(), live.end(), [](const LiveUnit& a, const LiveUnit& b) { return a.state.entity_id < b.state.entity_id; });
        events.push_back(Event{tick, EventKind::reinforcement_unloaded, landing.owner, landing.sequence, first});
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
