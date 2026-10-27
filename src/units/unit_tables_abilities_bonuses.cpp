#include "unit_tables_decode.hpp"

namespace eawr::units::unit_tables_detail {

void Loader::load_bonus_abilities(Object& object, UnitType& unit, const std::string& name, const AbilityFields& child_text) {
    const auto& node = child_text.node;
    if (input_.campaign_scene && (iequals(node.name, "Enhance_Defense_Ability") || iequals(node.name, "Battlefield_Modifier_Ability"))
        && iequals(child_text("Activation_Style"), automatic_activation_name(input_.domain))) {
        BattlefieldBonus bonus; bonus.name = name;
        bonus.enabled = boolean(child_text("Initially_Enabled")).value_or(true);
        bonus.all_allies = boolean(child_text("Apply_To_All_Allies")).value_or(false);
        const auto category = number(child_text("Stacking_Category"));
        if (category && category->raw() >= 0 && category->trunc_to_integer() <= std::numeric_limits<std::uint32_t>::max())
            bonus.category = static_cast<std::uint32_t>(category->trunc_to_integer());
        if (iequals(node.name, "Enhance_Defense_Ability")) bonus.defence = number(child_text("Enhancement_Percentage")).value_or(Fixed{});
        else if (const auto multiplier = number(child_text("FOW_Reveal_Range_Multiplier"))) {
            const auto delta = sim::math::subtract(*multiplier, Fixed::from_raw(Fixed::scale));
            if (delta) bonus.reveal_delta = delta.value();
            else report_.missing(unit.id, "FOW_Reveal_Range_Multiplier", {}, "range multiplier overflows");
        }
        if (bonus.defence != Fixed{} || bonus.reveal_delta != Fixed{}) unit.production.battlefield_bonuses.push_back(std::move(bonus));
    }
    if (iequals(node.name, "Battlefield_Modifier_Ability") && iequals(child_text("Activation_Style"), automatic_activation_name(input_.domain))) {
        // WST-03: the debug build adds (multiplier - 1) to the cloak-duration list only when
        // the multiplier is not 1 (its default).
        const auto text = child_text("Stealth_Duration_Multiplier");
        const auto multiplier = number(text);
        if (!text.empty() && !multiplier) {
            report_.missing(unit.id, "Stealth_Duration_Multiplier", text, "not a decimal");
        } else if (multiplier && multiplier->raw() != Fixed::scale) {
            StealthDurationBonus bonus;
            bonus.multiplier = *multiplier;
            bonus.all_allies = boolean(child_text("Apply_To_All_Allies")).value_or(false);
            const auto category = number(child_text("Stacking_Category"));
            if (category && category->raw() >= 0
                && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
                bonus.stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
            }
            unit.production.stealth_duration_bonuses.push_back(bonus);
        }

        UnitType::SpecialUnlock unlock;
        unlock.enable = boolean(child_text("Enable_Special_Weapons")).value_or(false);
        unlock.all_allies = boolean(child_text("Apply_To_All_Allies")).value_or(false);
        unlock.reverse = boolean(child_text("Reverse_Application_Logic")).value_or(false);
        if (unlock.enable) unit.special_unlocks.push_back(unlock);
    }
    if (iequals(node.name, "Combat_Bonus_Ability") && iequals(child_text("Activation_Style"), automatic_activation_name(input_.domain))) {
        CombatBonus bonus;
        bonus.name = name;
        bonus.filter = unit.special_abilities.back().filter;
        bonus.enabled = unit.special_abilities.back().initially_enabled.value_or(true);
        bonus.specific_faction = child_text("Specific_Faction");
        for (const auto tag : {"Unique_Space_Unit", "Unique_Ground_Unit"}) {
            const auto container = trim(object.text(tag));
            if (!container.empty()) bonus.excluded_containers.push_back(assets::object_type_crc(container));
        }
        bonus.types = tokens(child_text("Applicable_Unit_Types"));
        bonus.categories = mask(child_text("Applicable_Unit_Categories"));
        const auto category = number(child_text("Stacking_Category"));
        if (category && category->raw() >= 0 && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
            bonus.stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
        }
        constexpr std::string_view tags[]{"Health_Bonus_Percentage", "Damage_Bonus_Percentage",
            "Energy_Pool_Bonus_Percentage", "Shield_Bonus_Percentage", "Defense_Bonus_Percentage",
            "Movement_Speed_Bonus_Percentage", "Fire_Range_Bonus_Percentage"};
        for (std::size_t index = 0; index < bonus.percentages.size(); ++index) {
            bonus.percentages[index] = number(child_text(tags[index])).value_or(Fixed{});
        }
        unit.production.combat_bonuses.push_back(std::move(bonus));
    } else if (iequals(node.name, "Enable_Ability")) {
        // WRB-05: the upgrade object's switch for a nested ability of its holder's units.
        AbilityEnable enable;
        enable.ability_name = child_text("Ability_Name");
        enable.types = tokens(child_text("Applicable_Unit_Types"));
        enable.categories = mask(child_text("Applicable_Unit_Categories"));
        enable.all_allies = boolean(child_text("Affects_All_Allies")).value_or(false);
        if (enable.ability_name.empty()) report_.missing(unit.id, "Ability_Name", name, "an Enable_Ability needs a name");
        else unit.production.ability_enables.push_back(std::move(enable));
    } else if (iequals(node.name, "Starbase_Upgrade_Ability")
        && iequals(child_text("Activation_Style"), "Skirmish_Automatic")) {
        unit.production.level_up = true;
    } else if (iequals(node.name, "Income_Stream_Ability")) {
        IncomeStream stream;
        stream.name = name;
        stream.base_value = number(child_text("Base_Income_Value"));
        stream.interval_seconds = number(child_text("Base_Interval_In_Secs"));
        stream.split_with_allies = boolean(child_text("Split_Income_With_Allies")).value_or(false);
        stream.full_amount_to_everyone = boolean(child_text("Full_Amount_To_Everyone")).value_or(false);
        if (!stream.base_value || !stream.interval_seconds) {
            report_.missing(unit.id, "Income_Stream_Ability", name, "no Base_Income_Value or Base_Interval_In_Secs");
        }
        unit.production.income.push_back(std::move(stream));
    } else if (iequals(node.name, "Income_Stream_Mod_Ability")) {
        IncomeBonus bonus;
        bonus.name = name;
        bonus.additive = number(child_text("Income_Additive_Value"));
        bonus.multiplier = number(child_text("Income_Multiplier"));
        bonus.target_source = child_text("Target_Stream_Source");
        bonus.interval_multiplier = number(child_text("Interval_Multiplier"));
        bonus.activation_style = child_text("Activation_Style");
        bonus.all_allied_sources = boolean(child_text("Affects_All_Allied_Sources")).value_or(false);
        bonus.reverse = boolean(child_text("Reverse_Application_Logic")).value_or(false);
        const auto category = number(child_text("Stacking_Category"));
        if (category && category->raw() >= 0
            && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
            bonus.stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
        }
        unit.production.income_bonuses.push_back(std::move(bonus));
    }
}

} // namespace eawr::units::unit_tables_detail
