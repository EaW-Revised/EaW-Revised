#include "eawr/presentation/ui/ability_buttons.hpp"

#include <algorithm>
#include <array>
#include <map>

namespace eawr::presentation::ui {
namespace {

// AB-04: the engine's icon per UnitAbilityType value (FoC debug build); empty where it has none.
constexpr std::array<std::string_view, ability_count> ability_icons{
    "", "i_sa_defend_mode.tga", "i_sa_deploy.tga", "i_sa_interdict.tga", "", "i_sa_barrage_area.tga",
    "i_sa_hunt.tga", "i_sa_spread_out.tga", "", "i_sa_power_to_engines.tga", "i_sa_lure.tga",
    "i_sa_rocket_attack.tga", "", "i_sa_force_crush.tga", "i_sa_force_push.tga", "i_sa_force_lighting.tga",
    "i_sa_flame_thrower.tga", "i_sa_jetpack_jump.tga", "i_sa_capture_vehicles.tga", "i_sa_force_protect.tga",
    "i_sa_hack_turret.tga", "i_sa_repair_vehicle.tga", "i_sa_sticky_bomb.tga", "i_sa_electronic_scramble.tga",
    "i_sa_area_heal.tga", "i_sa_join_me.tga", "", "i_sa_tow_cable_attack.tga", "i_sa_sensor_ping.tga",
    "i_sa_power_to_weapons.tga", "i_sa_tractor_beam.tga", "i_sa_fire_energy_weapon.tga", "i_sa_missile_jammer.tga",
    "i_sa_evasive_maneuvers.tga", "i_sa_lucky_shot.tga", "i_sa_Harmonic_bomb.tga", "i_sa_drop_bomb.tga",
    "i_sa_all_ships_concentrate_fire.tga", "i_sa_deploy_stormtroopers.tga", "i_sa_self_destruct.tga",
    "i_sa_sprint.tga", "i_sa_ion_cannon_shot.tga", "i_sa_weaken_enemy.tga", "i_sa_s_foil_mode.tga",
    "i_sa_cover_me.tga", "i_sa_maximum_firepower.tga", "i_sa_capture_vehicles.tga", "i_sa_weaken_enemy.tga",
    "i_sa_force_cloak.tga", "i_sa_stun.tga", "i_sa_contaminate.tga", "i_sa_berserker.tga", "i_sa_force_sight.tga",
    "i_sa_saber_throw.tga", "i_sa_force_cloak.tga", "i_sa_fire_energy_weapon.tga", "i_sa_laser_defense.tga",
    "i_sa_force_confuse.tga", "i_sa_leech_shields.tga", "i_sa_tactical_bribe.tga", "i_sa_swap_weapons.tga",
    "i_sa_cluster_bomb.tga", "i_sa_full_salvo.tga", "i_sa_sensor_jamming.tga", "i_sa_place_remote_bomb.tga",
    "i_sa_detonate_remote_bomb.tga", "i_sa_infection.tga", "i_sa_proximity_mines.tga", "i_sa_stim_pack.tga",
    "i_sa_drain_life.tga", "i_sa_blast.tga", "i_sa_buzz_droids.tga", "i_sa_shield_flare.tga", "i_sa_summon.tga",
    "i_sa_corrupt_systems.tga", "i_sa_deploy_squad.tga", ""};

// AB-09: a target first (an object, a position or passable terrain).
constexpr std::array<std::string_view, 30> targeted_abilities{
    "BARRAGE", "TRACTOR_BEAM", "ENERGY_WEAPON", "LUCKY_SHOT", "CONCENTRATE_FIRE", "ION_CANNON_SHOT", "SUPER_LASER",
    "FORCE_TELEKINESIS", "FORCE_LIGHTNING", "FLAME_THROWER", "CAPTURE_VEHICLE", "TARGETED_HACK", "STICKY_BOMB",
    "CABLE_ATTACK", "MAXIMUM_FIREPOWER", "BERSERKER", "SABER_THROW", "LEECH_SHIELDS", "PLACE_REMOTE_BOMB",
    "INFECTION", "JET_PACK", "DISTRACT", "FOW_REVEAL_PING", "WEAKEN_ENEMY", "BUZZ_DROIDS", "CORRUPT_SYSTEMS",
    "TARGETED_INVULNERABILITY", "TARGETED_REPAIR", "TACTICAL_BRIBE", "FIRE_LOBBING_SUPERWEAPON"};

// AB-10: FoC's default key map for the ability commands (0 none, 1 Shift, 2 Ctrl, 3 Alt).
struct Hotkey {
    std::string_view ability;
    char key{};
    std::uint8_t modifier{};
};
constexpr Hotkey hotkeys[] = {
    {"DEPLOY", 'D', 2}, {"SPREAD_OUT", 'Z', 2}, {"JET_PACK", 'J', 2}, {"STICKY_BOMB", 'K', 0},
    {"UNTARGETED_STICKY_BOMB", 'K', 2}, {"ROCKET_ATTACK", 'G', 2}, {"CABLE_ATTACK", 'F', 2},
    {"FOW_REVEAL_PING", 'O', 2}, {"POWER_TO_WEAPONS", 'B', 2}, {"SELF_DESTRUCT", 'X', 2}, {"DEPLOY_TROOPERS", 'H', 2},
    {"MAXIMUM_FIREPOWER", 'M', 2}, {"SPRINT", 'N', 2}, {"LURE", 'C', 2}, {"PLACE_REMOTE_BOMB", 'E', 2},
    {"DETONATE_REMOTE_BOMB", 'R', 2}, {"LASER_DEFENSE", 'L', 2}, {"STIM_PACK", 'S', 2}, {"SWAP_WEAPONS", 'I', 2},
    {"DRAIN_LIFE", 'T', 2}, {"PROXIMITY_MINES", 'P', 2}, {"DEFEND", 'O', 1}, {"TRACTOR_BEAM", 'T', 1},
    {"INTERDICT", 'G', 1}, {"BARRAGE", 'B', 1}, {"HUNT", 'H', 1}, {"TURBO", 'E', 1}, {"LURE", 'L', 1},
    {"MISSILE_SHIELD", 'M', 1}, {"SPOILER_LOCK", 'W', 1}, {"ION_CANNON_SHOT", 'I', 1}, {"SENSOR_JAMMING", 'J', 1},
    {"STEALTH", 'C', 1}, {"LEECH_SHIELDS", 'N', 1}, {"BUZZ_DROIDS", 'Z', 1}, {"SUPER_LASER", 'A', 1},
    {"CLUSTER_BOMB", 'K', 1}, {"LASER_DEFENSE", 'P', 1}, {"SELF_DESTRUCT", 'S', 1}, {"FULL_SALVO", 'F', 1},
    {"DEPLOY_SQUAD", 'Q', 1}, {"LUCKY_SHOT", 'K', 3}, {"REPLENISH_WINGMEN", 'W', 3}, {"FORCE_TELEKINESIS", 'H', 3},
    {"FORCE_WHIRLWIND", 'P', 3}, {"FORCE_LIGHTNING", 'L', 3}, {"AREA_EFFECT_CONVERT", 'O', 3},
    {"CAPTURE_VEHICLE", 'V', 3}, {"EJECT_VEHICLE_THIEF", 'Y', 3}, {"ENERGY_WEAPON", 'X', 3}, {"HARMONIC_BOMB", 'M', 3},
    {"WEAKEN_ENEMY", '[', 3}, {"INVULNERABILITY", ']', 3}, {"AREA_EFFECT_STUN", '/', 3}, {"TARGETED_HACK", ',', 3},
    {"TARGETED_REPAIR", '.', 3}, {"FLAME_THROWER", 'J', 3}, {"CONCENTRATE_FIRE", ';', 3}, {"SABER_THROW", 'T', 3},
    {"FORCE_CLOAK", 'C', 3}, {"FORCE_CONFUSE", 'U', 3}, {"FORCE_SIGHT", 'S', 3}, {"BERSERKER", 'E', 3},
    {"INFECTION", 'I', 3}, {"DRAIN_LIFE", 'D', 3}, {"SUMMON", 'R', 3}, {"BLAST", 'G', 3}, {"SHIELD_FLARE", 'F', 3},
    {"STUN", 'N', 3}, {"TACTICAL_BRIBE", 'B', 3}, {"FULL_SALVO", 'A', 3}, {"SENSOR_JAMMING", 'Q', 3},
};

struct Group {
    std::uint32_t ability{};
    std::uint32_t second{ability_none};
    std::size_t first_column{};
    std::size_t last_column{};
    std::vector<std::size_t> units; // card unit indices, card order
};

} // namespace

std::string_view to_string(const AbilityStatus status) noexcept {
    switch (status) {
    case AbilityStatus::ready: return "ready";
    case AbilityStatus::active: return "active";
    case AbilityStatus::recharging: return "recharging";
    case AbilityStatus::disabled: return "disabled";
    }
    return "ready";
}

std::string_view to_string(const AbilityRequest::Kind kind) noexcept {
    switch (kind) {
    case AbilityRequest::Kind::activate: return "activate";
    case AbilityRequest::Kind::deactivate: return "deactivate";
    case AbilityRequest::Kind::autofire_on: return "autofire_on";
    case AbilityRequest::Kind::autofire_off: return "autofire_off";
    }
    return "activate";
}

std::string_view ability_icon(const std::uint32_t ability) noexcept {
    return ability < ability_icons.size() ? ability_icons[ability] : std::string_view{};
}

bool ability_targeted(const std::uint32_t ability) noexcept {
    const std::string_view name = ability_name(ability);
    return !name.empty() && std::find(targeted_abilities.begin(), targeted_abilities.end(), name) != targeted_abilities.end();
}

AbilityBar ability_bar(const CardLayout& layout, const std::span<const CardUnit> units, const AbilityState& state,
                       const std::function<std::string(const CardUnit&, bool)>& icon) {
    AbilityBar bar;
    // AB-01: one group per ability, in slot order; its columns are the columns of its cards.
    std::map<std::uint32_t, Group> groups;
    std::vector<std::uint32_t> order;
    for (const UnitCard& card : layout.cards) {
        if (card.ability == ability_none || card.unit >= units.size()) continue;
        auto [found, inserted] = groups.try_emplace(card.ability);
        Group& group = found->second;
        if (inserted) {
            group.ability = card.ability;
            group.first_column = card.slot / 2;
            order.push_back(card.ability);
        }
        group.last_column = card.slot / 2;
    }
    for (const std::uint32_t ability : order) {
        Group& group = groups[ability];
        // The group's units are every card unit of the ability, stacked or not (the engine's lists).
        group.units = layout.by_ability[ability];
        for (const std::size_t index : group.units) {
            if (units[index].second_ability != ability_none) group.second = units[index].second_ability;
        }
    }
    for (const std::uint32_t ability : order) {
        const Group& group = groups[ability];
        const bool two = group.second != ability_none;
        for (const bool second : {false, true}) {
            if (second && !two) break;
            const std::uint32_t shown = second ? group.second : group.ability;
            AbilityButton button;
            button.component = group.first_column + group.last_column + (second ? 1U : 0U);
            button.ability = shown;
            button.second = second;
            button.shifted = two;
            bool hidden = false;
            bool all_disabled = true;
            bool all_autofire = true;
            double recharge = 0.0;
            bool has_dial = false;
            for (const std::size_t index : group.units) {
                const CardUnit& unit = units[index];
                const std::uint32_t own = second ? unit.second_ability : unit.ability;
                if (second && own != shown) continue;
                const auto current = state.state(unit.id, shown);
                // AB-02: a unit without a state for the ability hides the button.
                if (!current) {
                    hidden = true;
                    break;
                }
                if (current->status != AbilityStatus::disabled) all_disabled = false;
                if (!current->autofire) all_autofire = false;
                // AB-05, AB-11: a timed ability that is on draws its dial too (the share of its duration left).
                if (current->status == AbilityStatus::recharging || (current->status == AbilityStatus::active && current->recharge < 1.0)) {
                    has_dial = true;
                    recharge = std::max(recharge, current->recharge);
                }
                button.units.push_back(unit.id);
                if (button.icon.empty() && icon) button.icon = icon(unit, second);
            }
            if (hidden || button.units.empty()) continue;
            if (button.icon.empty()) button.icon = std::string(ability_icon(shown));
            button.disabled = all_disabled;
            button.autofire = all_autofire;
            // AB-05: no unit recharging counts as complete.
            button.recharge = has_dial ? std::clamp(recharge, 0.0, 1.0) : 1.0;
            bar.buttons.push_back(std::move(button));
        }
    }
    // AB-08: the marks on each card, from the card's (first) unit.
    for (const UnitCard& card : layout.cards) {
        if (card.unit >= units.size()) continue;
        const CardUnit& unit = units[card.unit];
        for (const bool second : {false, true}) {
            const std::uint32_t ability = second ? unit.second_ability : unit.ability;
            if (ability == ability_none) continue;
            const auto current = state.state(unit.id, ability);
            if (!current) continue;
            CardAbilityMark mark;
            mark.slot = card.slot;
            mark.second = second;
            // AB-08: the icon shows while the ability is active or recharging, and on autofire too.
            if (current->status == AbilityStatus::active || current->status == AbilityStatus::recharging || current->autofire) {
                mark.icon = icon ? icon(unit, second) : std::string();
                if (mark.icon.empty()) mark.icon = std::string(ability_icon(ability));
            }
            if (current->status == AbilityStatus::recharging || (current->status == AbilityStatus::active && current->recharge < 1.0)) {
                mark.dial = std::clamp(current->recharge, 0.0, 1.0);
            }
            mark.autofire = current->autofire;
            if (mark.icon.empty() && !mark.dial && !mark.autofire) continue;
            bar.marks.push_back(std::move(mark));
        }
    }
    return bar;
}

std::optional<AbilityRequest> ability_click(const AbilityButton& button, const bool right, const AbilityState& state) {
    if (button.units.empty() || button.ability == ability_none) return std::nullopt;
    AbilityRequest request;
    request.ability = button.ability;
    request.units = button.units;
    if (right) {
        // AB-09: off when every unit is on autofire, else on.
        bool all = true;
        for (const sim::EntityId unit : button.units) {
            const auto current = state.state(unit, button.ability);
            if (current && !current->autofire) all = false;
        }
        request.kind = all ? AbilityRequest::Kind::autofire_off : AbilityRequest::Kind::autofire_on;
        return request;
    }
    bool all_active = true;
    bool any_ready = false;
    for (const sim::EntityId unit : button.units) {
        const auto current = state.state(unit, button.ability);
        if (!current) continue;
        if (current->status != AbilityStatus::active) all_active = false;
        if (current->status == AbilityStatus::ready) any_ready = true;
    }
    if (all_active) {
        request.kind = AbilityRequest::Kind::deactivate;
        return request;
    }
    // Nothing ready (all recharging or disabled): the engine plays its refusal and sends nothing.
    if (!any_ready) return std::nullopt;
    request.kind = AbilityRequest::Kind::activate;
    request.targeted = ability_targeted(button.ability);
    return request;
}

std::optional<std::uint32_t> ability_hotkey(const char key, const Modifiers modifiers) noexcept {
    const int held = (modifiers.shift ? 1 : 0) + (modifiers.ctrl ? 1 : 0) + (modifiers.alt ? 1 : 0);
    if (held > 1) return std::nullopt;
    const std::uint8_t modifier = modifiers.shift ? 1 : modifiers.ctrl ? 2 : modifiers.alt ? 3 : 0;
    for (const Hotkey& hotkey : hotkeys) {
        if (hotkey.key == key && hotkey.modifier == modifier) {
            const std::uint32_t ability = ability_index(hotkey.ability);
            if (ability != ability_none) return ability;
        }
    }
    return std::nullopt;
}

void ReadyAbilities::set_units(const std::span<const CardUnit> units) {
    known_.clear();
    known_.reserve(units.size());
    for (const CardUnit& unit : units) known_.push_back({unit.id, unit.ability, unit.second_ability});
}

std::optional<UnitAbilityState> ReadyAbilities::state(const sim::EntityId unit, const std::uint32_t ability) const {
    if (ability == ability_none) return std::nullopt;
    for (const Staged& staged : staged_) {
        if (staged.unit == unit && staged.ability == ability) return staged.state;
    }
    for (const Known& known : known_) {
        if (known.unit == unit && (known.first == ability || known.second == ability)) return UnitAbilityState{};
    }
    return std::nullopt;
}

} // namespace eawr::presentation::ui
