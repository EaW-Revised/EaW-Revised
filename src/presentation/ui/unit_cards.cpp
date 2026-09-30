#include "eawr/presentation/ui/unit_cards.hpp"

#include <algorithm>
#include <cmath>

namespace eawr::presentation::ui {
namespace {

// UnitAbilityType in enum order, without the ABILITY_ prefix (FoC debug build).
constexpr std::array<std::string_view, ability_count> ability_names{
    "NONE", "DEFEND", "DEPLOY", "INTERDICT", "HARASS", "BARRAGE", "HUNT", "SPREAD_OUT", "AFTERBURNER",
    "TURBO", "LURE", "ROCKET_ATTACK", "AVOID_DANGER", "FORCE_TELEKINESIS", "FORCE_WHIRLWIND",
    "FORCE_LIGHTNING", "FLAME_THROWER", "JET_PACK", "CAPTURE_VEHICLE", "TARGETED_INVULNERABILITY",
    "TARGETED_HACK", "TARGETED_REPAIR", "STICKY_BOMB", "AREA_EFFECT_STUN", "AREA_EFFECT_HEAL",
    "AREA_EFFECT_CONVERT", "DISTRACT", "CABLE_ATTACK", "FOW_REVEAL_PING", "POWER_TO_WEAPONS",
    "TRACTOR_BEAM", "ENERGY_WEAPON", "MISSILE_SHIELD", "INVULNERABILITY", "LUCKY_SHOT", "HARMONIC_BOMB",
    "UNTARGETED_STICKY_BOMB", "CONCENTRATE_FIRE", "DEPLOY_TROOPERS", "SELF_DESTRUCT", "SPRINT",
    "ION_CANNON_SHOT", "WEAKEN_ENEMY", "SPOILER_LOCK", "REPLENISH_WINGMEN", "MAXIMUM_FIREPOWER",
    "EJECT_VEHICLE_THIEF", "FIRE_LOBBING_SUPERWEAPON", "STEALTH", "STUN", "RADIOACTIVE_CONTAMINATE",
    "BERSERKER", "FORCE_SIGHT", "SABER_THROW", "FORCE_CLOAK", "SUPER_LASER", "LASER_DEFENSE",
    "FORCE_CONFUSE", "LEECH_SHIELDS", "TACTICAL_BRIBE", "SWAP_WEAPONS", "CLUSTER_BOMB", "FULL_SALVO",
    "SENSOR_JAMMING", "PLACE_REMOTE_BOMB", "DETONATE_REMOTE_BOMB", "INFECTION", "PROXIMITY_MINES",
    "STIM_PACK", "DRAIN_LIFE", "BLAST", "BUZZ_DROIDS", "SHIELD_FLARE", "SUMMON", "CORRUPT_SYSTEMS",
    "DEPLOY_SQUAD", "GARRISON_NONE"};

[[nodiscard]] bool same_name(const std::string_view a, const std::string_view b) noexcept {
    const auto upper = [](const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c; };
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return upper(x) == upper(y); });
}

// Add_Ship_To_Stack: one stack per type and ability state, in first-seen order; a stack keeps the
// ability of its first unit.
struct Stack {
    std::string_view type;
    std::uint8_t status{};
    std::uint32_t ability{ability_none};
    std::vector<std::size_t> units;
    bool collapsed{};
};

} // namespace

std::uint32_t ability_index(const std::string_view name) noexcept {
    for (std::size_t index = 0; index < ability_names.size(); ++index) {
        if (same_name(name, ability_names[index])) return static_cast<std::uint32_t>(index);
    }
    return ability_none;
}

std::string_view ability_name(const std::uint32_t ability) noexcept {
    return ability < ability_names.size() ? ability_names[ability] : std::string_view{};
}

std::vector<CardUnit> card_units(const std::span<const SelectedUnit> selection) {
    std::vector<CardUnit> out;
    for (const SelectedUnit& unit : selection) {
        if (unit.squadron && unit.squadron->homogeneous) {
            const SquadronOf& squadron = *unit.squadron;
            const bool shown = std::any_of(out.begin(), out.end(),
                [&](const CardUnit& card) { return card.squadron && card.id == squadron.container; });
            if (shown) continue;
            CardUnit card;
            card.id = squadron.container;
            card.members = squadron.members;
            card.type = squadron.type;
            card.ability = squadron.ability;
            card.second_ability = squadron.second_ability;
            card.squadron = true;
            card.health = squadron.health;
            out.push_back(std::move(card));
            continue;
        }
        CardUnit card;
        card.id = unit.entity;
        card.members = {unit.entity};
        card.type = unit.type;
        card.ability = unit.ability;
        card.second_ability = unit.second_ability;
        card.health = unit.health;
        card.shield = unit.shield;
        out.push_back(std::move(card));
    }
    return out;
}

std::int32_t health_level(const double health, const std::int32_t max_bar_level) noexcept {
    // ceil(percent * Max_Bar_Level) in single precision, as the engine computes it, then
    // Set_Current_Level clamps to [0, Max_Bar_Level].
    const float clamped = std::isfinite(health) ? static_cast<float>(std::clamp(health, 0.0, 1.0)) : 0.0F;
    const auto level = static_cast<std::int32_t>(std::ceil(clamped * static_cast<float>(max_bar_level)));
    return std::clamp(level, 0, std::max(0, max_bar_level));
}

const UnitCard* CardLayout::at(const std::size_t slot) const noexcept {
    const auto found = std::find_if(cards.begin(), cards.end(), [slot](const UnitCard& card) { return card.slot == slot; });
    return found == cards.end() ? nullptr : &*found;
}

CardLayout layout_unit_cards(const std::span<const CardUnit> units, const std::size_t slots,
                             const std::int32_t max_bar_level) {
    CardLayout out;
    std::vector<Stack> stacks;
    std::size_t cards = 0;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const CardUnit& unit = units[index];
        const std::uint32_t ability = unit.ability < ability_count ? unit.ability : ability_none;
        ++cards;
        out.by_ability[ability].push_back(index);
        const auto stack = std::find_if(stacks.begin(), stacks.end(), [&](const Stack& candidate) {
            return candidate.type == unit.type && candidate.status == unit.ability_status;
        });
        if (stack == stacks.end()) stacks.push_back({unit.type, unit.ability_status, ability, {index}, false});
        else stack->units.push_back(index);
    }
    // An odd-sized group leaves the rest of its column blank.
    for (const auto& group : out.by_ability) {
        if (group.size() % 2 != 0) ++cards;
        if (!group.empty()) ++out.groups;
    }
    // Too many cards: collapse the largest stack still shown one by one, the first of equal ones.
    while (cards > slots) {
        Stack* largest = nullptr;
        std::size_t largest_count = 1;
        for (Stack& stack : stacks) {
            if (!stack.collapsed && stack.units.size() > largest_count) {
                largest = &stack;
                largest_count = stack.units.size();
            }
        }
        if (largest == nullptr) break;
        largest->collapsed = true;
        cards -= largest_count - 1;
    }
    std::size_t slot = 0;
    for (std::uint32_t ability = 0; ability < ability_count && slot < slots; ++ability) {
        const std::size_t first_column = slot / 2;
        bool placed = false;
        for (const Stack& stack : stacks) {
            if (slot >= slots) break;
            if (stack.ability != ability) continue;
            const std::size_t shown = stack.collapsed ? 1 : stack.units.size();
            for (std::size_t index = 0; index < shown && slot < slots; ++index) {
                const CardUnit& unit = units[stack.units[index]];
                UnitCard card;
                card.slot = slot;
                card.unit = stack.units[index];
                card.ability = ability;
                card.stacked = stack.collapsed;
                card.count = stack.collapsed ? static_cast<std::uint32_t>(stack.units.size()) : 1U;
                if (!card.stacked) {
                    card.health_level = health_level(unit.health, max_bar_level);
                    if (!unit.squadron && unit.shield) card.shield = std::clamp(*unit.shield, 0.0, 1.0);
                }
                out.cards.push_back(card);
                placed = true;
                ++slot;
            }
        }
        if (placed) {
            const std::size_t last_column = (slot - 1) / 2;
            for (std::size_t column = first_column; column <= last_column; ++column) {
                BorderPiece piece = BorderPiece::centre;
                if (first_column == last_column) piece = BorderPiece::full;
                else if (column == first_column) piece = BorderPiece::left;
                else if (column == last_column) piece = BorderPiece::right;
                out.borders.push_back({column, piece});
            }
        }
        if (slot % 2 != 0) ++slot; // the blank slot below the group's last card
    }
    return out;
}

std::optional<std::vector<sim::EntityId>> card_click(const CardLayout& layout, const std::span<const CardUnit> units,
                                                     const std::size_t slot, const bool shift,
                                                     const std::span<const sim::EntityId> selection) {
    const UnitCard* card = layout.at(slot);
    if (card == nullptr || card->unit >= units.size()) return std::nullopt;
    const CardUnit& clicked = units[card->unit];
    std::vector<std::size_t> chosen;
    if (card->stacked || shift) {
        if (card->stacked) {
            for (std::size_t index = 0; index < units.size(); ++index) {
                if (units[index].type == clicked.type) chosen.push_back(index);
            }
        } else {
            chosen.push_back(card->unit);
        }
    } else if (layout.groups == 1) {
        chosen.push_back(card->unit);
    } else {
        chosen = layout.by_ability[card->ability];
    }
    std::vector<sim::EntityId> members;
    for (const std::size_t index : chosen) {
        for (const sim::EntityId member : units[index].members) {
            if (std::find(members.begin(), members.end(), member) == members.end()) members.push_back(member);
        }
    }
    if (!shift) return members;
    // Tactical_Deselect_Object: everything else stays, in selection order.
    std::vector<sim::EntityId> kept;
    for (const sim::EntityId entity : selection) {
        if (std::find(members.begin(), members.end(), entity) == members.end()) kept.push_back(entity);
    }
    return kept;
}

} // namespace eawr::presentation::ui
