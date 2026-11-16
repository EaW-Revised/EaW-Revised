#include "eawr/sim/tactical/land_abilities.hpp"

#include "eawr/core/text.hpp"

#include <algorithm>
#include <array>

namespace eawr::sim::tactical {
namespace {

using Kind = AbilityKind;

// WLAB-01: the vocabulary in engine order. The first 32 kinds keep the values the space game already uses; the others
// are the appended enumerators of types.hpp.
constexpr std::array<AbilityVocabularyEntry, ability_vocabulary_count> vocabulary{{
    {"DEFEND", Kind::defend}, {"DEPLOY", Kind::deploy}, {"INTERDICT", Kind::interdict}, {"HARASS", Kind::harass},
    {"BARRAGE", Kind::barrage}, {"HUNT", Kind::hunt}, {"SPREAD_OUT", Kind::spread_out},
    {"AFTERBURNER", Kind::afterburner}, {"TURBO", Kind::turbo}, {"LURE", Kind::lure},
    {"ROCKET_ATTACK", Kind::rocket_attack}, {"AVOID_DANGER", Kind::avoid_danger},
    {"FORCE_TELEKINESIS", Kind::force_telekinesis}, {"FORCE_WHIRLWIND", Kind::force_whirlwind},
    {"FORCE_LIGHTNING", Kind::force_lightning}, {"FLAME_THROWER", Kind::flame_thrower}, {"JET_PACK", Kind::jet_pack},
    {"CAPTURE_VEHICLE", Kind::capture_vehicle}, {"TARGETED_INVULNERABILITY", Kind::targeted_invulnerability},
    {"TARGETED_HACK", Kind::targeted_hack}, {"TARGETED_REPAIR", Kind::targeted_repair},
    {"STICKY_BOMB", Kind::sticky_bomb}, {"AREA_EFFECT_STUN", Kind::area_effect_stun},
    {"AREA_EFFECT_HEAL", Kind::area_effect_heal}, {"AREA_EFFECT_CONVERT", Kind::area_effect_convert},
    {"DISTRACT", Kind::distract}, {"CABLE_ATTACK", Kind::cable_attack}, {"FOW_REVEAL_PING", Kind::fow_reveal_ping},
    {"POWER_TO_WEAPONS", Kind::power_to_weapons}, {"TRACTOR_BEAM", Kind::tractor_beam},
    {"ENERGY_WEAPON", Kind::energy_weapon}, {"MISSILE_SHIELD", Kind::missile_shield},
    {"INVULNERABILITY", Kind::invulnerability}, {"LUCKY_SHOT", Kind::lucky_shot},
    {"HARMONIC_BOMB", Kind::harmonic_bomb}, {"UNTARGETED_STICKY_BOMB", Kind::untargeted_sticky_bomb},
    {"CONCENTRATE_FIRE", Kind::concentrate_fire}, {"DEPLOY_TROOPERS", Kind::deploy_troopers},
    {"SELF_DESTRUCT", Kind::self_destruct}, {"SPRINT", Kind::sprint}, {"ION_CANNON_SHOT", Kind::ion_cannon_shot},
    {"WEAKEN_ENEMY", Kind::weaken_enemy}, {"SPOILER_LOCK", Kind::spoiler_lock},
    {"REPLENISH_WINGMEN", Kind::replenish_wingmen}, {"MAXIMUM_FIREPOWER", Kind::maximum_firepower},
    {"EJECT_VEHICLE_THIEF", Kind::eject_vehicle_thief}, {"FIRE_LOBBING_SUPERWEAPON", Kind::fire_lobbing_superweapon},
    {"STEALTH", Kind::stealth}, {"STUN", Kind::stun}, {"RADIOACTIVE_CONTAMINATE", Kind::radioactive_contaminate},
    {"BERSERKER", Kind::berserker}, {"FORCE_SIGHT", Kind::force_sight}, {"SABER_THROW", Kind::saber_throw},
    {"FORCE_CLOAK", Kind::force_cloak}, {"SUPER_LASER", Kind::super_laser}, {"LASER_DEFENSE", Kind::laser_defense},
    {"FORCE_CONFUSE", Kind::force_confuse}, {"LEECH_SHIELDS", Kind::leech_shields},
    {"TACTICAL_BRIBE", Kind::tactical_bribe}, {"SWAP_WEAPONS", Kind::swap_weapons},
    {"CLUSTER_BOMB", Kind::cluster_bomb}, {"FULL_SALVO", Kind::full_salvo}, {"SENSOR_JAMMING", Kind::sensor_jamming},
    {"PLACE_REMOTE_BOMB", Kind::place_remote_bomb}, {"DETONATE_REMOTE_BOMB", Kind::detonate_remote_bomb},
    {"INFECTION", Kind::infection}, {"PROXIMITY_MINES", Kind::proximity_mines}, {"STIM_PACK", Kind::stim_pack},
    {"DRAIN_LIFE", Kind::drain_life}, {"BLAST", Kind::blast}, {"BUZZ_DROIDS", Kind::buzz_droids},
    {"SHIELD_FLARE", Kind::shield_flare}, {"SUMMON", Kind::summon}, {"CORRUPT_SYSTEMS", Kind::corrupt_systems},
    {"DEPLOY_SQUAD", Kind::deploy_squad},
}};

// WLAB-04: the kinds the table names as instant (not modal): every Force power but FORCE_CLOAK (which is modal), and with
// them JET_PACK, FLAME_THROWER, the bombs, TARGETED_*, DEPLOY_TROOPERS, DRAIN_LIFE, STUN and TACTICAL_BRIBE. That reading
// of the rule's list is the one WLAB-09 backs: these kinds recharge from the switch and never stay on. A kind the rule
// does not name keeps the default of an uninitialised entry (modal) until its own kind rule says otherwise.
constexpr bool instant(const Kind kind) noexcept {
    switch (kind) {
    case Kind::force_telekinesis: case Kind::force_whirlwind: case Kind::force_lightning:
    case Kind::area_effect_convert: case Kind::berserker: case Kind::force_sight: case Kind::saber_throw:
    case Kind::force_confuse: case Kind::jet_pack: case Kind::flame_thrower: case Kind::sticky_bomb:
    case Kind::untargeted_sticky_bomb: case Kind::harmonic_bomb: case Kind::place_remote_bomb:
    case Kind::detonate_remote_bomb: case Kind::cluster_bomb: case Kind::targeted_invulnerability:
    case Kind::targeted_hack: case Kind::targeted_repair: case Kind::deploy_troopers: case Kind::drain_life:
    case Kind::stun: case Kind::tactical_bribe:
        return true;
    default: return false;
    }
}
// WLAB-03: the team kinds act on the container.
constexpr bool team(const Kind kind) noexcept {
    return kind == Kind::ion_cannon_shot || kind == Kind::lucky_shot || kind == Kind::targeted_hack
        || kind == Kind::targeted_repair || kind == Kind::buzz_droids;
}
// WLAB-08: the kinds a Force-disabled unit cannot start.
constexpr bool force_cancelled(const Kind kind) noexcept {
    switch (kind) {
    case Kind::force_telekinesis: case Kind::force_whirlwind: case Kind::force_lightning:
    case Kind::area_effect_convert: case Kind::drain_life: case Kind::berserker: case Kind::force_sight:
    case Kind::saber_throw: case Kind::force_cloak: case Kind::force_confuse:
        return true;
    default: return false;
    }
}
// AB-09: the command bar asks for a target first.
constexpr bool targeted(const Kind kind) noexcept {
    switch (kind) {
    case Kind::barrage: case Kind::tractor_beam: case Kind::energy_weapon: case Kind::lucky_shot:
    case Kind::concentrate_fire: case Kind::ion_cannon_shot: case Kind::super_laser: case Kind::force_telekinesis:
    case Kind::force_lightning: case Kind::flame_thrower: case Kind::capture_vehicle: case Kind::targeted_hack:
    case Kind::sticky_bomb: case Kind::cable_attack: case Kind::maximum_firepower: case Kind::berserker:
    case Kind::saber_throw: case Kind::leech_shields: case Kind::place_remote_bomb: case Kind::infection:
    case Kind::jet_pack: case Kind::distract: case Kind::fow_reveal_ping: case Kind::weaken_enemy:
    case Kind::buzz_droids: case Kind::corrupt_systems: case Kind::targeted_invulnerability:
    case Kind::targeted_repair: case Kind::tactical_bribe: case Kind::fire_lobbing_superweapon:
        return true;
    default: return false;
    }
}
constexpr AbilityBehaviour behaviour(const Kind kind) noexcept {
    switch (kind) {
    case Kind::hunt: return AbilityBehaviour::hunt;
    case Kind::lure: return AbilityBehaviour::lure;
    case Kind::deploy_troopers: return AbilityBehaviour::deploy_troopers;
    case Kind::self_destruct: return AbilityBehaviour::self_destruct;
    default: return AbilityBehaviour::none;
    }
}
// WLAB-10: no unit-level branch. WLAB-11: the handler is looked up by name and applied with no target.
constexpr AbilityRoute route(const Kind kind) noexcept {
    switch (kind) {
    case Kind::force_telekinesis: case Kind::force_lightning: case Kind::flame_thrower: case Kind::targeted_hack:
    case Kind::targeted_repair: case Kind::cable_attack: case Kind::berserker: case Kind::saber_throw:
    case Kind::radioactive_contaminate: case Kind::tactical_bribe: case Kind::place_remote_bomb: case Kind::infection:
    case Kind::proximity_mines: case Kind::summon: case Kind::corrupt_systems: case Kind::buzz_droids:
    case Kind::harass: case Kind::afterburner:
        return AbilityRoute::targeted_only;
    case Kind::stun: case Kind::force_sight: case Kind::laser_defense: case Kind::force_confuse:
    case Kind::cluster_bomb: case Kind::drain_life: case Kind::blast: case Kind::shield_flare:
    case Kind::force_whirlwind:
        return AbilityRoute::handler_by_name;
    default: return AbilityRoute::unit;
    }
}
constexpr AbilityKindFlags make_flags(const Kind kind) noexcept {
    AbilityKindFlags flags;
    flags.modal = !instant(kind);
    flags.nebula_disabled = true;
    flags.team = team(kind);
    flags.force_cancelled = force_cancelled(kind);
    flags.targeted = targeted(kind);
    flags.behaviour = behaviour(kind);
    flags.route = route(kind);
    return flags;
}

constexpr auto make_table() noexcept {
    std::array<AbilityKindFlags, ability_vocabulary_count + 1> table{};
    for (std::size_t index = 0; index < vocabulary.size(); ++index) table[index + 1] = make_flags(vocabulary[index].kind);
    return table;
}
constexpr auto flag_table = make_table();
constexpr AbilityKindFlags default_flags{};

[[nodiscard]] bool same_name(const std::string_view a, const std::string_view b) noexcept {
    return core::iequals(a, b);
}

} // namespace

std::span<const AbilityVocabularyEntry> ability_vocabulary() noexcept { return vocabulary; }

AbilityKind land_ability_kind(const std::string_view name) noexcept {
    for (const auto& entry : vocabulary) if (same_name(entry.name, name)) return entry.kind;
    return AbilityKind::none;
}

std::uint32_t ability_vocabulary_index(const AbilityKind kind) noexcept {
    for (std::size_t index = 0; index < vocabulary.size(); ++index)
        if (vocabulary[index].kind == kind) return static_cast<std::uint32_t>(index + 1);
    return 0;
}

AbilityKind ability_kind_of_index(const std::uint32_t index) noexcept {
    return index >= 1 && index <= ability_vocabulary_count ? vocabulary[index - 1].kind : AbilityKind::none;
}

const AbilityKindFlags& ability_kind_flags(const AbilityKind kind) noexcept {
    const auto index = ability_vocabulary_index(kind);
    return index == 0 ? default_flags : flag_table[index];
}

AbilityTimer ability_timer(const AbilityKind kind, const bool has_expiration) noexcept {
    // WLAB-09: DEPLOY and SWAP_WEAPONS have no timer.
    if (kind == AbilityKind::deploy || kind == AbilityKind::swap_weapons) return AbilityTimer::none;
    // JET_PACK runs the expiration countdown only when Expiration_Seconds is above 0; without one it recharges at once.
    if (kind == AbilityKind::jet_pack) return has_expiration ? AbilityTimer::expiration : AbilityTimer::recharge_at_switch;
    // Every other modal kind (the listed SPREAD_OUT, SPRINT, STEALTH and the rest, and the modal kinds the rule does not list,
    // such as CAPTURE_VEHICLE) shares the slot countdown: it follows the authored Expiration_Seconds (WAB-03), so a kind that
    // authors none stays on until it is switched off. The instant kinds recharge from the switch.
    return ability_kind_flags(kind).modal ? AbilityTimer::expiration : AbilityTimer::recharge_at_switch;
}

std::string_view to_string(const AbilityRefusal refusal) noexcept {
    switch (refusal) {
    case AbilityRefusal::none: return "none";
    case AbilityRefusal::kind_out_of_range: return "kind out of range";
    case AbilityRefusal::in_nebula: return "in a nebula";
    case AbilityRefusal::force_disabled: return "Force disabled";
    case AbilityRefusal::limbo: return "in limbo";
    case AbilityRefusal::abilities_cancelled: return "abilities cancelled";
    case AbilityRefusal::excluded_behaviour: return "excluded behaviour";
    case AbilityRefusal::recharging: return "recharging";
    case AbilityRefusal::team_member: return "a team member fails the gate";
    case AbilityRefusal::locomotor_hold: return "locomotor hold";
    case AbilityRefusal::movement_locked: return "movement locked";
    case AbilityRefusal::in_limbo: return "in limbo for this kind";
    case AbilityRefusal::stunned: return "stunned";
    case AbilityRefusal::deploy_troopers: return "deploy-troopers behaviour unavailable";
    case AbilityRefusal::channelling: return "channelling another special ability";
    case AbilityRefusal::special_hardpoint: return "no live special-weapon hardpoint";
    case AbilityRefusal::remote_bomb_cap: return "remote bomb cap reached";
    case AbilityRefusal::summon: return "summon pending or live";
    case AbilityRefusal::base_power: return "no base power";
    case AbilityRefusal::deploy_squad: return "no ready spawn state or fighter bay";
    }
    return "unknown";
}

AbilityRefusal land_ability_enabled(const AbilityKind kind, const AbilityGateInput& input, const bool check_recharge,
    const bool check_nebula) noexcept {
    const auto index = ability_vocabulary_index(kind);
    if (index == 0) return AbilityRefusal::kind_out_of_range;                                      // (1)
    const auto& flags = flag_table[index];
    if (check_nebula && input.in_nebula && flags.nebula_disabled) return AbilityRefusal::in_nebula;  // (2)
    if (flags.force_cancelled && input.force_disabled) return AbilityRefusal::force_disabled;      // (3)
    if (input.limbo && !input.garrisoned) return AbilityRefusal::limbo;                            // (4)
    if (input.abilities_cancelled) return AbilityRefusal::abilities_cancelled;                     // (5)
    if (input.excluded_behaviour) return AbilityRefusal::excluded_behaviour;                       // (6)
    if (check_recharge && input.recharging) return AbilityRefusal::recharging;                     // (7)
    if (flags.team && input.team_member_fails) return AbilityRefusal::team_member;                 // (8)
    if (input.locomotor_hold) return AbilityRefusal::locomotor_hold;                               // (9)
    switch (kind) {                                                                                // WLAB-07
    case Kind::defend: case Kind::interdict: case Kind::hunt:
        if (input.movement_locked) return AbilityRefusal::movement_locked;
        break;
    case Kind::capture_vehicle: case Kind::targeted_invulnerability: case Kind::area_effect_stun:
    case Kind::area_effect_heal: case Kind::area_effect_convert: case Kind::corrupt_systems:
        if (input.limbo) return AbilityRefusal::in_limbo;
        break;
    case Kind::lucky_shot:
        if (input.stunned) return AbilityRefusal::stunned;
        break;
    case Kind::deploy_troopers:
        if (!input.deploy_troopers_ready) return AbilityRefusal::deploy_troopers;
        break;
    case Kind::force_sight:
        if (input.channelling) return AbilityRefusal::channelling;
        break;
    case Kind::leech_shields: case Kind::blast:
        if (!input.special_hardpoint_live) return AbilityRefusal::special_hardpoint;
        break;
    case Kind::place_remote_bomb:
        if (input.remote_bombs >= input.remote_bomb_cap) return AbilityRefusal::remote_bomb_cap;
        break;
    case Kind::summon:
        if (!input.summon_ready) return AbilityRefusal::summon;
        break;
    case Kind::fire_lobbing_superweapon:
        if (!input.base_power_ok) return AbilityRefusal::base_power;
        break;
    case Kind::deploy_squad:
        if (!input.deploy_squad_ready) return AbilityRefusal::deploy_squad;
        break;
    default: break; // every other kind passes
    }
    return AbilityRefusal::none;
}

AbilityRange land_ability_range(const AbilityRangeContext& context, const bool has_target, const math::Fixed distance,
    const math::Fixed weapon_max) noexcept {
    if (!has_target) return AbilityRange::in_range;
    const auto maximum = context.activation_max && context.activation_max->raw() > 0 ? *context.activation_max : weapon_max;
    if (distance > maximum) return AbilityRange::too_far;
    if (context.activation_min && distance < *context.activation_min) return AbilityRange::too_close;
    return AbilityRange::in_range;
}

const LandAbilityEffect* LandAbilityEffects::find(const AbilityKind kind) const noexcept {
    const auto found = std::lower_bound(rows.begin(), rows.end(), kind,
        [](const auto& row, const AbilityKind key) { return row.first < key; });
    return found != rows.end() && found->first == kind ? found->second.get() : nullptr;
}

} // namespace eawr::sim::tactical
