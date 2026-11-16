#include "session_impl.hpp"
#include "session_land_formation.hpp"
#include "session_land_garrison.hpp"
#include "tactical_internal.hpp"
#include "eawr/core/text.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/sim/tactical/land_abilities.hpp"

#include <algorithm>
#include <limits>
#include <string>

// Land ability dispatch, group G1 (#3238, docs/behaviour/walks/land-abilities.md): the unit switch with its enabled gate
// (WLAB-02, -05, -06, -07), company delegation (WLAB-03), the timer choice per kind on the shared slot timers (WLAB-09), the
// kinds with no no-target branch (WLAB-10), the handler lookup by name (WLAB-11), the range context of a targeted order
// (WLAB-77) and the per-tick expiry. The effects of the kinds are not here: a group registers them in
// LandSessionContent::ability_effects, and a kind with no registered effect fails closed.
namespace eawr::sim::tactical {
namespace {

enum class Why : std::uint8_t {
    applied,
    unit_gone,          // the addressed unit is not live
    not_held,           // no unit holds the ability (WLAB-02)
    already_on,         // a switch-on while the ability is on (WLAB-02)
    not_on,             // a switch-off of an ability that is not on
    no_autofire,        // the ability does not support autofire
    no_target_branch,   // WLAB-10: no unit-level branch for a no-target switch
    no_handler,         // WLAB-11: the nested handler the ability names is not bound
    no_effect,          // no effect registered for the kind: it fails closed
    admit_refused,      // the effect refused the switch-on
    target_gone,        // the named target is not live
    bad_target,         // the target is an object or a point the kind does not take, or on the wrong side
    too_far, too_close, // WLAB-77
    gate,               // WLAB-06, WLAB-07: `refusal` names the step
    not_ready,          // the shared timer says the ability is not ready
};
struct Outcome {
    Why why{Why::applied};
    AbilityRefusal refusal{AbilityRefusal::none};
    [[nodiscard]] bool ok() const noexcept { return why == Why::applied; }
};

[[nodiscard]] std::string describe(const Outcome& outcome) {
    switch (outcome.why) {
    case Why::applied: return "applied";
    case Why::unit_gone: return "the unit is not live";
    case Why::not_held: return "no unit of the squad holds the ability";
    case Why::already_on: return "the ability is already on";
    case Why::not_on: return "the ability is not on";
    case Why::no_autofire: return "the ability does not support autofire";
    case Why::no_target_branch: return "the kind starts only through a targeted order (WLAB-10)";
    case Why::no_handler: return "the nested handler named by GUI_Activated_Ability_Name is not bound (WLAB-11)";
    case Why::no_effect: return "the kind has no land effect yet";
    case Why::admit_refused: return "the effect refused the switch-on";
    case Why::target_gone: return "the target is not live";
    case Why::bad_target: return "the target is not one this kind takes (WLAB-77)";
    case Why::too_far: return "the target is out of range (WLAB-77)";
    case Why::too_close: return "the target is too close (WLAB-77)";
    case Why::gate: return std::string("the enabled gate refused: ") + std::string(to_string(outcome.refusal));
    case Why::not_ready: return "the ability is not ready";
    }
    return "refused";
}

// The units of one squad, ascending id: the members that are still live.
[[nodiscard]] std::vector<EntityId> live_members(const session_detail::LandSession& land, const std::vector<LiveUnit>& live,
    const std::size_t squad) {
    std::vector<EntityId> result;
    for (const auto& member : land.movement.members()) {
        if (member.squad != squad) continue;
        if (session_detail::land_live_unit(live, member.object) != nullptr) result.push_back(member.object);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace

// WLAB-08: whether the unit is Force disabled at `tick`: the last scan that reached it (the stamp) is within the window. A
// command of tick t runs before that tick's scan, so the newest stamp it sees is from tick t - 1: the stock three-frame window
// refuses the Force-cancelled kinds for the three commands after the last scan and accepts the fourth.
bool TacticalSession::Impl::land_force_disabled(const EntityId unit, const std::uint64_t tick) const {
    const auto found = land->force_disable_stamps.find(unit);
    return found != land->force_disable_stamps.end() && tick >= found->second
        && tick - found->second <= land->content.force_disable_frames;
}

std::optional<std::uint64_t> TacticalSession::land_force_disable_stamp(const EntityId unit) const {
    if (!impl_->land) return std::nullopt;
    const auto found = impl_->land->force_disable_stamps.find(unit);
    return found != impl_->land->force_disable_stamps.end() ? std::optional(found->second) : std::nullopt;
}

// WLAB-12..15, 37: the inputs the active abilities of a unit supply to the land contexts (see the declaration). A container's
// spread-out slot widens the spacing of its members' rows (WLAB-13); the multipliers are each unit's own.
std::optional<TacticalSession::LandAbilityInputs> TacticalSession::land_ability_inputs(const EntityId unit) const {
    if (!impl_->land) return std::nullopt;
    const auto& registry = std::as_const(impl_->registry);
    const auto held = [&](const EntityId id) -> std::pair<const UnitAbilityProfile*, const AbilityState*> {
        const auto found = impl_->handles.find(id);
        if (found == impl_->handles.end()) return {nullptr, nullptr};
        const auto* able = registry.try_get<Abilities>(found->second);
        const auto* profile = impl_->abilities.find(registry.get<Identity>(found->second).type_id);
        return {profile, able != nullptr ? &able->value : nullptr};
    };
    if (!impl_->handles.contains(unit)) return std::nullopt;
    LandAbilityInputs inputs;
    if (const auto [profile, state] = held(unit); profile != nullptr && state != nullptr) {
        inputs.speed = ability_multiplier(*profile, *state, AbilityModifier::speed);
        inputs.cause_damage = ability_multiplier(*profile, *state, AbilityModifier::cause_damage);
        inputs.take_damage = ability_multiplier(*profile, *state, AbilityModifier::take_damage);
        inputs.weapon_delay = ability_multiplier(*profile, *state, AbilityModifier::weapon_delay);
        for (std::size_t index = 0; index < state->slots.size() && index < profile->abilities.size(); ++index)
            if (state->slots[index].active && profile->abilities[index].modifiers.allow_target_diminishing_firepower)
                inputs.allow_target_diminishing_firepower = profile->abilities[index].modifiers.allow_target_diminishing_firepower;
    }
    // The company container of a member is the squad object its movement member belongs to; a container is its own.
    const auto& land = *impl_->land;
    EntityId container = unit;
    const auto members = land.movement.members();
    const auto member = std::find_if(members.begin(), members.end(), [&](const auto& row) { return row.object == unit; });
    if (member != members.end()) {
        const auto squads = land.movement.squads();
        if (member->squad < squads.size()) container = squads[member->squad].object;
    }
    if (land.content.spread_out_spacing) {
        if (const auto [profile, state] = held(container); profile != nullptr && state != nullptr) {
            const auto index = ability_slot(*profile, AbilityKind::spread_out);
            if (index && *index < state->slots.size() && state->slots[*index].active) inputs.spacing = *land.content.spread_out_spacing;
        }
    }
    return inputs;
}

// WLAB-16/17: the ability motion record of a live unit (see the declaration).
std::optional<TacticalSession::LandAbilityMotionView> TacticalSession::land_ability_motion(const EntityId unit) const {
    if (!impl_->land || !impl_->handles.contains(unit)) return std::nullopt;
    LandAbilityMotionView view;
    if (const auto found = impl_->land->ability_motion.find(unit); found != impl_->land->ability_motion.end()) {
        view.locomotor = found->second.locomotor; view.deployed = found->second.deploy != 0;
        view.jump_pending = found->second.jump_phase == 1; view.jump_point = found->second.jump_point;
        view.jump_phase = found->second.jump_phase; view.jump_position = found->second.jump_position;
        view.projectile = found->second.projectile;
    }
    return view;
}

// WLAB-08: the Force disabling scan. Every live carrier (a type with a Force-disabling row) reaches every live unit of ANY owner
// inside its radius, its own side included, that holds at least one Force-cancelled kind, and stamps it with this tick. The
// roster is the live units, so a dead unit or one inside a garrison host is skipped. Stamps older than the window are dropped
// first, so the saved map stays small. Serial: one pass over the carriers, then one over the units that hold such a kind.
void TacticalSession::Impl::land_force_disable_service(const std::vector<LiveUnit>& live) {
    auto& stamps = land->force_disable_stamps;
    const auto tick = completed_tick;
    const auto window = static_cast<std::uint64_t>(land->content.force_disable_frames);
    std::erase_if(stamps, [&](const auto& stamp) { return tick > stamp.second && tick - stamp.second > window; });
    const auto& rows = land->content.force_disablers;
    if (rows.empty()) return;
    struct Source { math::Vec3 position; math::Fixed range; };
    std::vector<Source> sources;
    for (const auto& unit : live) {
        const auto row = std::lower_bound(rows.begin(), rows.end(), unit.state.type_id,
            [](const auto& entry, const TypeId type) { return entry.type < type; });
        if (row != rows.end() && row->type == unit.state.type_id) sources.push_back({unit.state.position, row->range});
    }
    if (sources.empty()) return;
    for (const auto& unit : live) {
        if (!unit.abilities) continue;
        const auto* profile = abilities.find(unit.state.type_id);
        if (profile == nullptr || std::none_of(profile->abilities.begin(), profile->abilities.end(),
                [](const auto& ability) { return ability_kind_flags(ability.kind).force_cancelled; })) continue;
        for (const auto& source : sources) {
            math::Fixed dx, dy, distance;
            if (!math::try_subtract(unit.state.position.x, source.position.x, dx)
                || !math::try_subtract(unit.state.position.y, source.position.y, dy)
                || !math::try_length(math::Vec2{dx, dy}, distance)) continue;
            if (distance > source.range) continue;
            stamps[unit.state.entity_id] = tick;
            break;
        }
    }
}

// WLAB-71: the permanent weapon swap. A unit whose type has the ability available (a row with a permanent index) holds its
// current projectile index at Weapon_Index. Reading (DEV-23): the index is set when the unit is first seen and written again only
// when it differs, so a battle that never moves it saves nothing and no tick rewrites state; a SWAP_WEAPONS on the same unit is put
// back by the next tick. Team containers never carry it: a container is a squad object, not a weapon holder. Serial, one pass.
void TacticalSession::Impl::land_weapon_swap_service(const std::vector<LiveUnit>& live) {
    const auto& rows = land->content.weapon_swaps;
    if (std::none_of(rows.begin(), rows.end(), [](const auto& row) { return row.permanent.has_value(); })) return;
    const auto squads = land->movement.squads();
    for (const auto& unit : live) {
        const auto row = std::lower_bound(rows.begin(), rows.end(), unit.state.type_id,
            [](const auto& entry, const TypeId type) { return entry.type < type; });
        if (row == rows.end() || row->type != unit.state.type_id || !row->permanent) continue;
        if (std::any_of(squads.begin(), squads.end(), [&](const auto& squad) { return squad.object == unit.state.entity_id; })) continue;
        const auto found = land->ability_motion.find(unit.state.entity_id);
        const std::uint8_t current = found != land->ability_motion.end() ? found->second.projectile : std::uint8_t{0};
        if (current == *row->permanent) continue;
        auto& record = land->ability_motion[unit.state.entity_id];
        record.projectile = *row->permanent;
        if (record.idle()) land->ability_motion.erase(unit.state.entity_id);
    }
}

namespace {

// The gate input of one unit for one kind. A subsystem the land session does not have yet is the failing value, never the
// passing one: no deploy-troopers behaviour, no special-weapon hardpoint, no summon, no base power, no fighter bay, and a
// remote-bomb cap of zero (so PLACE_REMOTE_BOMB fails the cap test as well as having no handler). Limbo is false: a unit
// that is not live (inside a garrison host) is not addressable at all.
[[nodiscard]] AbilityGateInput gate_input(const session_detail::LandSession& land, const bool force_disabled,
    const UnitAbilityProfile& profile, const AbilityState& state, const AbilitySlot& slot, const EntityId unit,
    const std::uint64_t tick) {
    AbilityGateInput input;
    input.force_disabled = force_disabled;
    input.abilities_cancelled = state.special.service_cancelled; // WLAB-06 (5): the manager's cancel flag
    input.recharging = !slot.active && tick < slot.ready_tick;   // WLAB-06 (7)
    // WLAB-06 (9): a transit state holds the locomotor: an entrant still walking into, or fading into, a garrison host.
    const auto entry = land.garrison_entries.find(unit);
    input.locomotor_hold = entry != land.garrison_entries.end()
        && (entry->second.phase == session_detail::LandGarrisonPhase::moving
            || entry->second.phase == session_detail::LandGarrisonPhase::fading);
    input.movement_locked = ability_movement_disabled(profile, state); // WLAB-07
    return input;
}

} // namespace

// One ability command to its squads (WLAB-03). A refusal is a diagnostic per squad; the command stays in the replay record.
void TacticalSession::Impl::land_ability_command(std::vector<LiveUnit>& live, const PlayerCommand& command,
    std::vector<core::Diagnostic>& diagnostics) {
    // WLAB-77: a barrage order names a point; it routes as a point-targeted switch-on of the kind.
    AbilityPayload converted;
    const auto* payload = std::get_if<AbilityPayload>(&command.payload);
    if (payload == nullptr) {
        const auto* area = std::get_if<AreaAbilityPayload>(&command.payload);
        if (area == nullptr) return;
        converted = AbilityPayload{area->ability, AbilityAction::activate, invalid_entity_id, 0xffffffffU, area->point};
        payload = &converted;
    }
    const auto tick = completed_tick;
    const auto& effects = land->content.ability_effects;
    // WLAB-15: what an effect may ask of the session. The self-damage is a hull loss that cannot kill: U-LAB-3 (whether the
    // retail hit can) is unverified, and a kill needs the death path, so the hull stops at the smallest positive value
    // (marked reading, DEV-19). It is miscellaneous damage with no armour, shield or damage-mode factor applied.
    struct World final : LandAbilityWorld {
        Impl& session;
        std::vector<LiveUnit>& units;
        World(Impl& impl, std::vector<LiveUnit>& live) : session(impl), units(live) {}
        // WLAB-16: the planar distance from the unit to the point.
        std::optional<math::Fixed> planar_distance(const EntityId id, const math::Vec3 point) const override {
            const auto* unit = session_detail::land_live_unit(units, id);
            math::Fixed dx, dy, distance;
            if (unit == nullptr || !math::try_subtract(point.x, unit->state.position.x, dx)
                || !math::try_subtract(point.y, unit->state.position.y, dy) || !math::try_length(math::Vec2{dx, dy}, distance))
                return std::nullopt;
            return distance;
        }
        // WLAB-16 (#7084): a unit that is flying or coming down takes no second jump (the effect refuses it first).
        [[nodiscard]] bool jump_in_flight(const EntityId id) const override {
            const auto found = session.land->ability_motion.find(id);
            return found != session.land->ability_motion.end() && found->second.jump_phase >= 2;
        }
        // WLAB-16: the jump is pending for the land movement (#7084); a type with a secondary locomotor switches to it.
        void request_jump(const EntityId id, const math::Vec3 point) override {
            const auto* unit = session_detail::land_live_unit(units, id);
            if (unit == nullptr || jump_in_flight(id)) return;
            auto& record = session.land->ability_motion[id];
            const auto& several = session.land->content.multi_locomotor_types;
            if (std::binary_search(several.begin(), several.end(), unit->state.type_id)) record.locomotor = 1;
            record.jump_phase = 1; record.jump_point = point; record.jump_frames = 0;
            record.jump_speed = math::Fixed{}; record.jump_position = math::Vec3{};
        }
        // WLAB-17: the unit's squad leaves its movement and ends on a stop order (a degenerate move), then the phase follows.
        void set_deployed(const EntityId id, const bool on) override {
            auto& land = *session.land;
            const auto members = land.movement.members();
            const auto member = std::find_if(members.begin(), members.end(), [&](const auto& row) { return row.object == id; });
            const auto squads = land.movement.squads();
            if (member != members.end() && member->squad < squads.size()) {
                const auto index = member->squad;
                const auto squad_id = squads[index].object;
                if (auto* squad = session_detail::land_live_unit(units, squad_id)) {
                    session_detail::release_land_formation_member(land, squad_id);
                    land.searches.erase(squad_id);
                    static_cast<void>(land.movement.drop_order(index));
                    static_cast<void>(land.movement.set_formation(index, 0, {}));
                    squad->state.order = Order{};
                    squad->state.order.issued_tick = session.completed_tick;
                    squad->state.order.kind = OrderKind::stop;
                    for (const auto& other : members) {
                        if (other.squad != index) continue;
                        if (auto* child = session_detail::land_live_unit(units, other.object)) child->state.order = squad->state.order;
                    }
                }
            }
            auto& record = session.land->ability_motion[id];
            record.deploy = on ? 1 : 0;
            if (record.idle()) session.land->ability_motion.erase(id);
        }
        // WLAB-19: the toggle needs a type with more than one projectile type; the index lives in the unit's motion record.
        void swap_weapon(const EntityId id) override {
            const auto* unit = session_detail::land_live_unit(units, id);
            if (unit == nullptr) return;
            const auto& swaps = session.land->content.weapon_swaps;
            const auto row = std::lower_bound(swaps.begin(), swaps.end(), unit->state.type_id,
                [](const auto& entry, const TypeId type) { return entry.type < type; });
            if (row == swaps.end() || row->type != unit->state.type_id) return;
            auto& record = session.land->ability_motion[id];
            record.projectile = record.projectile == 0 ? 1 : 0;
            if (record.idle()) session.land->ability_motion.erase(id);
        }
        void damage_self(const EntityId id, const math::Fixed share) override {
            auto* unit = session_detail::land_live_unit(units, id);
            const auto* health = unit != nullptr ? session.health_profile(*unit) : nullptr;
            if (unit == nullptr || health == nullptr || !unit->durability || unit->durability->hull.raw() <= 0) return;
            math::Fixed amount, after;
            if (!math::try_multiply(health->max_hull, share, amount) || amount.raw() <= 0) return;
            const auto before = unit->durability->hull;
            if (!math::try_subtract(before, amount, after) || after.raw() < 1) after = math::Fixed::from_raw(1);
            unit->durability->hull = after;
            session.track_damage(*unit, before, unit->durability->shields);
        }
    } world{*this, live};
    const auto kind = payload->ability;
    const auto& flags = ability_kind_flags(kind);
    const bool targeted = payload->target != invalid_entity_id || payload->position.has_value();

    // One unit's switch (WLAB-02 .. WLAB-11, WLAB-77). `team_check` makes the gate recurse over the container's members.
    const auto switch_unit = [&](LiveUnit& unit, const std::vector<EntityId>* team_check) -> Outcome {
        const auto* profile = unit.abilities ? abilities.find(unit.state.type_id) : nullptr;
        const auto index = profile ? ability_slot(*profile, kind) : std::nullopt;
        // WLAB-02: a kind outside 1 to 75, and a unit without the ability, are refused.
        if (!index || ability_vocabulary_index(kind) == 0) return {Why::not_held, AbilityRefusal::none};
        const auto& ability = profile->abilities[*index];
        auto& slot = unit.abilities->slots[*index];
        switch (payload->action) {
        case AbilityAction::autofire_on:
        case AbilityAction::autofire_off:
            if (!ability.supports_autofire) return {Why::no_autofire, AbilityRefusal::none};
            slot.autofire = payload->action == AbilityAction::autofire_on;
            return {};
        case AbilityAction::deactivate: {
            // WLAB-02: a switch-off never needs the gate. An instant kind never stays on, so it is never on to switch off.
            if (!slot.active) return {Why::not_on, AbilityRefusal::none};
            const auto changed = deactivate_ability(ability, slot, tick);
            if (!changed.changed) return {Why::not_on, AbilityRefusal::none};
            if (const auto* effect = effects ? effects->find(kind) : nullptr)
                effect->switched(AbilityUse{unit.state.entity_id, kind, tick, payload->target, payload->position, nullptr, &ability, &world}, false);
            return {};
        }
        case AbilityAction::activate: break;
        }
        // WLAB-02: a switch-on while the ability is on is refused. (The one exception, BARRAGE while its barrage area is
        // active, needs a barrage area; the land session has none, so it never applies: DEV-15.)
        if (slot.active) return {Why::already_on, AbilityRefusal::none};
        // WLAB-10: a kind with no unit-level branch fails a no-target switch; only a targeted order reaches its handler.
        if (flags.route == AbilityRoute::targeted_only && !targeted) return {Why::no_target_branch, AbilityRefusal::none};
        // WLAB-06, WLAB-07: the enabled gate, with its recursion over a team container's members.
        auto input = gate_input(*land, land_force_disabled(unit.state.entity_id, tick), *profile, *unit.abilities, slot,
            unit.state.entity_id, tick);
        if (team_check != nullptr && flags.team) {
            for (const auto member_id : *team_check) {
                const auto* member = session_detail::land_live_unit(live, member_id);
                if (member == nullptr) continue;
                const auto* member_profile = member->abilities ? abilities.find(member->state.type_id) : nullptr;
                AbilityGateInput member_input;
                if (member_profile != nullptr) {
                    // The member's own state when it holds the kind, else its general state.
                    const auto member_index = ability_slot(*member_profile, kind);
                    static const AbilitySlot idle{};
                    member_input = gate_input(*land, land_force_disabled(member_id, tick), *member_profile, *member->abilities,
                        member_index ? member->abilities->slots[*member_index] : idle, member_id, tick);
                } else {
                    member_input.force_disabled = land_force_disabled(member_id, tick);
                }
                if (land_ability_enabled(kind, member_input, true, true) != AbilityRefusal::none) {
                    input.team_member_fails = true;
                    break;
                }
            }
        }
        if (const auto refusal = land_ability_enabled(kind, input, true, true); refusal != AbilityRefusal::none)
            return {Why::gate, refusal};
        // WLAB-11: the kinds that look their nested handler up by GUI_Activated_Ability_Name and apply it with no target.
        const SpecialAbilityProfile* handler = nullptr;
        if (flags.route == AbilityRoute::handler_by_name) {
            const auto found = std::find_if(profile->special.begin(), profile->special.end(),
                [&](const auto& special) { return core::iequals(special.name, ability.gui_activated_ability_name); });
            if (ability.gui_activated_ability_name.empty() || found == profile->special.end()) return {Why::no_handler, AbilityRefusal::none};
            handler = &*found;
        }
        // The effect of the kind. Without one the kind does nothing yet, so the switch fails closed.
        const auto* effect = effects ? effects->find(kind) : nullptr;
        if (effect == nullptr) return {Why::no_effect, AbilityRefusal::none};
        // WLAB-77: a targeted order is range-checked through the ability's context, with the holder's weapon range.
        if (targeted) {
            const auto rule = effect->target_rule();
            math::Vec3 aim = payload->position.value_or(math::Vec3{});
            if (payload->target != invalid_entity_id) {
                const auto* target = session_detail::land_live_unit(live, payload->target);
                if (target == nullptr) return {Why::target_gone, AbilityRefusal::none};
                const bool hostile = players_hostile(snapshot_players, unit.state.owner, target->state.owner);
                if (!rule.object || (rule.relation == AbilityTargetRelation::hostile && !hostile)
                    || (rule.relation == AbilityTargetRelation::friendly && hostile)) return {Why::bad_target, AbilityRefusal::none};
                aim = target->state.position;
            } else if (!rule.point) return {Why::bad_target, AbilityRefusal::none};
            math::Fixed dx, dy, distance;
            if (!math::try_subtract(aim.x, unit.state.position.x, dx) || !math::try_subtract(aim.y, unit.state.position.y, dy)
                || !math::try_length(math::Vec2{dx, dy}, distance)) return {Why::too_far, AbilityRefusal::none};
            const auto* fighting = combat.find(unit.state.type_id);
            const auto weapon_max = fighting != nullptr && fighting->max_attack_distance ? *fighting->max_attack_distance
                                                                                         : math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
            switch (land_ability_range(effect->range(), true, distance, weapon_max)) {
            case AbilityRange::in_range: break;
            case AbilityRange::too_far: return {Why::too_far, AbilityRefusal::none};
            case AbilityRange::too_close: return {Why::too_close, AbilityRefusal::none};
            }
        }
        const AbilityUse use{unit.state.entity_id, kind, tick, payload->target, payload->position, handler, &ability, &world};
        if (!effect->admit(use)) return {Why::admit_refused, AbilityRefusal::none};
        // The shared timers (WAB-03, WAB-05): the countdown starts, and WLAB-05: an instant kind never stays on.
        const AbilityGate gate;
        if (!activate_ability(ability, slot, gate, tick).changed) return {Why::not_ready, AbilityRefusal::none};
        switch (ability_timer(kind, ability.expiration_frames != 0)) {
        case AbilityTimer::none: break; // DEPLOY and SWAP_WEAPONS run no timer
        case AbilityTimer::expiration: break;
        case AbilityTimer::recharge_at_switch:
            slot.active = false;
            slot.expires_tick = 0;
            slot.ready_tick = tick + ability.recharge_frames;
            break;
        }
        effect->switched(use, true);
        return {};
    };

    for (const auto id : command.units) {
        auto* unit = session_detail::land_live_unit(live, id);
        const auto squads = land->movement.squads();
        const auto squad = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == id; });
        if (unit == nullptr || unit->state.owner != command.key.player_id || squad == squads.end()) {
            diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                "land ability command requires an owned squad"));
            continue;
        }
        const auto members = live_members(*land, live, static_cast<std::size_t>(squad - squads.begin()));
        Outcome result;
        // WLAB-12: SPREAD_OUT acts only on a company container; on any other object it succeeds and does nothing.
        const bool container = std::any_of(land->content.companies.begin(), land->content.companies.end(),
            [&](const auto& row) { return row.type == unit->state.type_id; });
        if (kind == AbilityKind::spread_out && !container) {
            continue;
        } else if (flags.team) {
            // WLAB-03: a team ability acts on the container itself, whose gate needs every member to pass.
            result = switch_unit(*unit, &members);
        } else {
            // WLAB-03: any other kind is delegated: each member that holds the ability is commanded, and one failing member
            // fails the command. The container's own switch runs only for SPREAD_OUT; for every other kind the container only
            // reports that some member had the ability.
            bool any_held = false;
            Outcome first_failure;
            const auto try_unit = [&](LiveUnit& target) {
                const auto* target_profile = target.abilities ? abilities.find(target.state.type_id) : nullptr;
                if (target_profile == nullptr || !ability_slot(*target_profile, kind)) return;
                any_held = true;
                const auto outcome = switch_unit(target, nullptr);
                if (!outcome.ok() && first_failure.ok()) first_failure = outcome;
            };
            for (const auto member_id : members) try_unit(*session_detail::land_live_unit(live, member_id));
            if (kind == AbilityKind::spread_out) try_unit(*unit);
            result = any_held ? first_failure : Outcome{Why::not_held, AbilityRefusal::none};
        }
        if (!result.ok())
            diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                "land ability " + std::string(to_string(kind)) + " refused: " + describe(result)));
    }
}

// WLAB-09, WAB-03: every tick, the abilities whose time limit is up end and start their recharge, in ascending unit id
// (the roster is sorted). Each ending is announced to the kind's effect so it can undo what the switch-on did. Serial: the
// work is one test per unit that holds an ability, and the effect calls are ordered side effects.
void TacticalSession::Impl::land_ability_service(std::vector<LiveUnit>& live) {
    const auto tick = completed_tick;
    const auto* effects = land->content.ability_effects.get();
    std::vector<std::uint8_t> was_active;
    for (auto& unit : live) {
        if (!unit.abilities) continue;
        const auto* profile = abilities.find(unit.state.type_id);
        if (profile == nullptr) continue;
        auto& state = *unit.abilities;
        was_active.assign(state.slots.size(), 0);
        bool any = false;
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            was_active[index] = state.slots[index].active ? 1 : 0;
            any = any || state.slots[index].active;
        }
        if (!any) continue;
        const auto changed = expire_abilities(*profile, state, tick);
        if (!changed.changed || effects == nullptr) continue;
        for (std::size_t index = 0; index < state.slots.size() && index < profile->abilities.size(); ++index) {
            if (!was_active[index] || state.slots[index].active) continue;
            if (const auto* effect = effects->find(profile->abilities[index].kind))
                effect->switched(AbilityUse{unit.state.entity_id, profile->abilities[index].kind, tick, invalid_entity_id,
                    std::nullopt, nullptr, &profile->abilities[index], nullptr}, false);
        }
    }
}

} // namespace eawr::sim::tactical
