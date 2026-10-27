#include "unit_tables_decode.hpp"

namespace eawr::units::unit_tables_detail {

void Loader::load_special_healing(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::force_healing) {
        const auto read = [&](const std::string_view tag, const Fixed fallback = {}) {
            const auto value = child_text(tag);
            if (value.empty()) return fallback;
            const auto parsed = number(value);
            if (!parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
            return parsed.value_or(fallback);
        };
        const auto frame_count = [&](const Fixed seconds) {
            const auto bounded = std::clamp<std::int64_t>(seconds.raw(), 0,
                static_cast<std::int64_t>(sim::tactical::max_ability_frames) * Fixed::scale
                    / sim::tactical::logical_frames_per_second);
            return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                (bounded * sim::tactical::logical_frames_per_second + Fixed::scale / 2) / Fixed::scale,
                0, sim::tactical::max_ability_frames));
        };
        special.heal_range = std::max(read("Heal_Range"), Fixed{});
        special.heal_percent = std::max(read("Heal_Percent"), Fixed{});
        special.heal_amount = read("Heal_Amount");
        auto interval = read("Heal_Interval_In_Secs");
        if (interval.raw() <= 0) interval = Fixed::from_raw(10 * Fixed::scale); // WSH-06
        special.heal_interval_frames = frame_count(interval);
        special.single_target_heal = boolean(child_text("Single_Target_Heal")).value_or(false);
        special.initially_enabled = special.initially_enabled.value_or(true);
        special.heal_sfx = child_text("SFXEvent_Target_Affected");
        special.heal_particle = child_text("Target_Particle_Effect");
        special.heal_target_bones = tokens(child_text("Target_Particle_Bone_Name"));
        special.heal_owner_bone = child_text("Owner_Particle_Bone_Name");
        special.heal_lightning = child_text("Lighting_Effect_Name");
        const auto colour = [&](const std::string_view tag) {
            std::array<Fixed, 3> result{};
            const auto values = tokens(child_text(tag));
            for (std::size_t i = 0; i < std::min(values.size(), result.size()); ++i)
                result[i] = number(values[i]).value_or(Fixed{});
            return result;
        };
        if (iequals(child_text("Target_Light_Effect_Type"), "Color_Flash")) {
            special.heal_target_colour = colour("Target_Light_Effect_Color");
            special.heal_target_light_frames = frame_count(read("Target_Light_Effect_Duration"));
            special.heal_target_light_pulses = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                read("Target_Light_Effect_Pulse_Count", Fixed::from_raw(Fixed::scale)).raw() / Fixed::scale, 1, 65535));
        }
        if (iequals(child_text("Owner_Light_Effect_Type"), "Color_Flash")) {
            special.heal_owner_colour = colour("Owner_Light_Effect_Color");
            special.heal_owner_light_frames = frame_count(read("Owner_Light_Effect_Duration"));
            special.heal_owner_light_pulses = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                read("Owner_Light_Effect_Pulse_Count", Fixed::from_raw(Fixed::scale)).raw() / Fixed::scale, 1, 65535));
        }
    }
}

void Loader::load_special_beam(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::concentrate_fire
        || special_kind == sim::tactical::SpecialAbilityKind::tractor_beam
        || special_kind == sim::tactical::SpecialAbilityKind::energy_weapon) {
        const auto read_percent = [&](const std::string_view tag) {
            const auto value = child_text(tag);
            const auto parsed = number(value);
            if (!value.empty() && !parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
            return parsed.value_or(Fixed{});
        };
        special.target_damage_increase = read_percent("Target_Damage_Increase_Percent");
        special.target_speed_decrease = read_percent("Target_Speed_Decrease_Percent");
        if (special_kind != sim::tactical::SpecialAbilityKind::concentrate_fire) {
            special.beam_min_range = read_percent("Activation_Min_Range");
            special.beam_max_range = read_percent("Activation_Max_Range");
            special.damage_per_frame = read_percent("Damage_Per_Frame");
            special.beam_owner_particle = child_text("Owner_Particle_Effect");
            special.beam_owner_bone = child_text("Owner_Particle_Bone_Name");
            if (special_kind == sim::tactical::SpecialAbilityKind::tractor_beam)
                special.doubled_speed_categories = enum_bits(tables_.categories, {"Corvette"},
                    unit.id, "Tractor_Corvette_Exception", report_);
        }
        const auto text = child_text("Stacking_Category");
        const auto category = number(text);
        if (category && category->raw() >= 0 && category->raw() % Fixed::scale == 0
            && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
            special.concentrate_stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
        } else if (!text.empty()) report_.missing(unit.id, "Stacking_Category", text, "not an unsigned integer");
    }
}

void Loader::load_special_blast(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::blast) {
        // WHE-35/36, FC-62: the handler's admitted-service charge and direct multiplier.
        sim::tactical::ChargedBlastProfile blast;
        const auto read = [&](const std::string_view tag, const Fixed fallback) {
            const auto value = child_text(tag);
            if (value.empty()) {
                report_.missing(unit.id, std::string(tag), value, "required BLAST field is absent");
                return fallback;
            }
            const auto parsed = number(value);
            if (!parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
            return parsed.value_or(fallback);
        };
        const auto charge = read("Charge_Up_Seconds", Fixed{});
        if (charge.raw() < 0 || charge.raw() > 3600 * Fixed::scale)
            report_.missing(unit.id, "Charge_Up_Seconds", child_text("Charge_Up_Seconds"), "outside supported charge range");
        blast.charge_frames = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            charge.raw(), 0, 3600 * Fixed::scale) * sim::tactical::logical_frames_per_second / Fixed::scale);
        blast.damage_multiplier = read("Damage_Multiplier", Fixed::from_raw(Fixed::scale));
        blast.charging_effect = child_text("Charging_Effect");
        blast.charged_effect = child_text("Charged_Effect");
        blast.activate_sfx = child_text("SFXEvent_Activate");
        special.charged_blast = std::move(blast);
    }
}

void Loader::load_special_stealth(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::stealth) {
        // WST-12: the cloak's presentation, its fade time and end colour.
        const auto transition = child_text("Stealth_Transition_Time");
        if (!transition.empty()) {
            const auto parsed = number(transition);
            if (parsed && parsed->raw() >= 0) special.stealth_transition = *parsed;
            else report_.missing(unit.id, "Stealth_Transition_Time", transition, "not a non-negative decimal");
        }
        const auto colour = tokens(child_text("Stealth_Color"));
        for (std::size_t channel = 0; channel < colour.size() && channel < special.stealth_colour.size(); ++channel) {
            const auto parsed = number(colour[channel]);
            if (!parsed) { report_.missing(unit.id, "Stealth_Color", colour[channel], "not a decimal"); continue; }
            special.stealth_colour[channel] = static_cast<std::uint8_t>(
                std::clamp<std::int64_t>(parsed->raw() / Fixed::scale, 0, 255));
        }
    }
}

void Loader::load_special_leech(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::leech_shields) {
        // WLS-01..05: the named handler owns its range, duration, drain and beam.
        const auto read = [&](const std::string_view tag) {
            const auto value = child_text(tag);
            const auto parsed = number(value);
            if (!value.empty() && !parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
            return parsed.value_or(Fixed{});
        };
        special.beam_min_range = read("Activation_Min_Range");
        special.beam_max_range = read("Activation_Max_Range");
        const auto duration = read("Duration_In_Secs");
        const auto bounded_duration = std::clamp<std::int64_t>(duration.raw(), 0,
            static_cast<std::int64_t>(sim::tactical::max_ability_frames) * Fixed::scale
                / sim::tactical::logical_frames_per_second);
        special.leech_duration_frames = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            bounded_duration * sim::tactical::logical_frames_per_second / Fixed::scale,
            0, sim::tactical::max_ability_frames));
        const auto damage = read("Shield_Damage_Per_Second");
        special.damage_per_frame = Fixed::from_raw(damage.raw() / 30 + (damage.raw() % 30 + 15) / 30);
        special.leech_damage_bonus = read("Damage_Multiplier");
        special.beam_owner_bone = child_text("Beam_Bone_Name");
        special.leech_beam_frames = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            read("Beam_Frames").raw() / Fixed::scale, 0, sim::tactical::max_ability_frames));
        special.leech_beam_width = read("Beam_Width");
        special.leech_texture = child_text("Beam_Texture_Name");
        special.leech_lightning = child_text("Beam_Effect_Name");
        special.leech_effect = child_text("Leech_Effect");
        const auto colour = tokens(child_text("Beam_Color"));
        for (std::size_t channel = 0; channel < colour.size() && channel < special.leech_colour.size(); ++channel) {
            const auto parsed = number(colour[channel]);
            if (!parsed) { report_.missing(unit.id, "Beam_Color", colour[channel], "not a decimal"); continue; }
            special.leech_colour[channel] = static_cast<std::uint8_t>(
                std::clamp<std::int64_t>(parsed->raw() / Fixed::scale, 0, 255));
        }
    }
}

void Loader::load_special_corrupt(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::corrupt_systems) {
        const auto read = [&](const std::string_view tag) {
            const auto value = child_text(tag);
            const auto parsed = number(value);
            if (!parsed || parsed->raw() < 0) report_.missing(unit.id, std::string(tag), value, "expected a nonnegative decimal");
            return parsed.value_or(Fixed{});
        };
        special.corrupt_min_range = read("Activation_Min_Range");
        special.corrupt_max_range = read("Activation_Max_Range");
        const auto duration = std::min(read("Duration_In_Seconds"), Fixed::from_raw(
            static_cast<std::int64_t>(sim::tactical::max_ability_frames) * Fixed::scale
                / sim::tactical::logical_frames_per_second));
        special.corrupt_duration_frames = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            duration.raw() / Fixed::scale * sim::tactical::logical_frames_per_second
                + duration.raw() % Fixed::scale * sim::tactical::logical_frames_per_second / Fixed::scale, 1,
            sim::tactical::max_ability_frames));
        special.corrupt_drain_per_frame = Fixed::from_raw(read("Shield_Drain_Per_Second").raw()
            / sim::tactical::logical_frames_per_second);
        special.corrupt_inaccuracy = read("Inaccuracy_Radius");
        special.corrupt_target_particle = child_text("Target_Particle_Effect");
        special.corrupt_owner_particle = child_text("Owner_Particle_Effect");
        special.corrupt_activate_sfx = child_text("SFXEvent_Activate");
    }
}

void Loader::load_special_laser_defense(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::laser_defense) {
        // WLD-02..WLD-09, WLD-12: the point-defence handler's own timing, reach and look.
        const auto read_number = [&](const std::string_view tag) {
            const auto value = child_text(tag);
            const auto parsed = number(value);
            if (!value.empty() && !parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
            return parsed.value_or(Fixed{});
        };
        // The engine counts whole frames of 1/30 s: trunc(seconds x 30), as AB-04.
        const auto read_frames = [&](const std::string_view tag) {
            const auto seconds = read_number(tag);
            if (seconds.raw() <= 0) return std::uint32_t{0};
            const auto product = static_cast<std::uint64_t>(seconds.raw()) * sim::tactical::logical_frames_per_second;
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(
                product / Fixed::scale, sim::tactical::max_ability_frames));
        };
        special.defence_duration_frames = read_frames("Defense_Duration_In_Secs");
        special.defence_look_frames = read_frames("Recharge_Time_In_Secs");
        special.defence_radius = read_number("Protection_Radius");
        special.defence_min_travel = read_number("Projectile_Dist_Travelled");
        special.defence_beam_width = read_number("Laser_Beam_Width");
        const auto beam_frames = child_text("Laser_Beam_Frames");
        if (const auto parsed = read_integer(beam_frames, report_, unit.id, "Laser_Beam_Frames")) {
            if (*parsed <= 0 || *parsed > static_cast<std::int64_t>(sim::tactical::max_ability_frames))
                report_.note(unit.id, "Laser_Beam_Frames", beam_frames, "PR-C4: frame count retained without a positive ability-duration restriction");
            special.defence_beam_frames = static_cast<std::uint32_t>(*parsed);
        }
        bool every_category_known = true;
        for (const auto& category : tokens(child_text("Projectile_Types_Targeted"))) {
            if (iequals(category, "MISSILE")) special.defence_categories |= sim::tactical::defence_category_missile;
            else if (iequals(category, "ROCKET")) special.defence_categories |= sim::tactical::defence_category_rocket;
            else if (iequals(category, "MPTL_ROCKET")) special.defence_categories |= sim::tactical::defence_category_mptl_rocket;
            else {
                every_category_known = false;
                report_.missing(unit.id, "Projectile_Types_Targeted", category, "projectile category is not modelled");
            }
        }
        // AB-26: a handler with a category the sim cannot match stays out of the table.
        if (!every_category_known) special.defence_categories = 0;
        const auto colour = tokens(child_text("Laser_Color"));
        for (std::size_t channel = 0; channel < colour.size() && channel < special.defence_colour.size(); ++channel) {
            const auto parsed = number(colour[channel]);
            if (!parsed) { report_.missing(unit.id, "Laser_Color", colour[channel], "not a decimal"); continue; }
            special.defence_colour[channel] = static_cast<std::uint8_t>(
                std::clamp<std::int64_t>(parsed->raw() / Fixed::scale, 0, 255));
        }
        special.defence_bones = tokens(child_text("Bone_Names"));
        special.defence_beam_texture = child_text("Laser_Beam_Texture");
        special.defence_lightning_effect = child_text("Lightning_Effect_Name");
        special.defence_zap_sfx = child_text("Zap_SFXEvent");
    }
}

void Loader::load_special_redirect(UnitType& unit, sim::tactical::SpecialAbilityProfile& special, const AbilityFields& child_text) {
    const auto special_kind = special.kind;
    if (special_kind == sim::tactical::SpecialAbilityKind::redirect_blaster) {
        // WRB-01..WRB-04: Validate_Data's ranges; a value outside them is reported and cleared.
        const auto read_range = [&](const std::string_view tag, const std::int64_t whole_max, const Fixed fallback,
                                    const bool clamp = false) {
            const auto value = child_text(tag);
            const auto parsed = number(value);
            if (value.empty()) return fallback;
            if (!parsed) { report_.missing(unit.id, std::string(tag), value, "not a decimal"); return fallback; }
            if (parsed->raw() < 0 || parsed->raw() > whole_max * Fixed::scale) {
                report_.missing(unit.id, std::string(tag), value, "outside the authored range");
                return clamp ? Fixed::from_raw(std::clamp<std::int64_t>(parsed->raw(), 0, whole_max * Fixed::scale)) : Fixed{};
            }
            return *parsed;
        };
        special.redirect_chance = read_range("Redirect_Chance", 1, Fixed{});
        special.block_chance = read_range("Block_Chance", 1, Fixed{});
        special.reaction_arc = read_range("Reaction_Arc_In_Degrees", 360, Fixed{});
        special.max_redirect_angle = read_range("Max_Projectile_Redirection_Angle_In_Degrees", 180,
            Fixed::from_raw(180 * Fixed::scale), true);
        const auto turn = child_text("Turn_To_Face_Unblockable_Shots");
        if (!turn.empty()) {
            const auto parsed = boolean(turn);
            if (!parsed) report_.missing(unit.id, "Turn_To_Face_Unblockable_Shots", turn, "not a boolean");
            else special.turn_to_face_unblockable = *parsed;
        }
        special.redirect_activate_sfx = child_text("SFXEvent_Activate");
    }
}

} // namespace eawr::units::unit_tables_detail
