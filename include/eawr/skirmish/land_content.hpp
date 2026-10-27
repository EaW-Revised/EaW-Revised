#pragma once

#include "eawr/skirmish/land_start.hpp"
#include "eawr/units/domain_tags.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/text.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace eawr::skirmish {

struct LandCollisionRank {
    std::string name;
    std::uint32_t order{};
};

// Unqualified defaults and instance occupation producers are supplied explicitly.
// Authored extents take precedence; an override cannot repair malformed XML.
struct LandStaticGate {
    std::string type;
    std::optional<sim::math::Fixed> scale_factor; // unverified absent constructor default
    std::uint32_t primary_passability{};
};

// LS shoot PR 1: the tags a land body reads for combat, durability and death presentation. A bad value there leaves an
// unresolved row against the object, but it never refuses the object's obstacle geometry.
[[nodiscard]] inline bool land_combat_field(const std::string_view field) {
    for (const auto tag : {"Tactical_Health", "Shield_Points", "Shield_Refresh_Rate", "Armor_Type", "Shield_Armor_Type",
        "Damage_Type", "Targeting_Priority_Set", "Targeting_Max_Attack_Distance", "Targeting_Min_Attack_Distance",
        "Targeting_Stickiness_Time_Threshold", "Idle_Chase_Range", "Guard_Chase_Range", "Attack_Move_Response_Range",
        "CategoryMask", "Property_Flags", "Attack_Category_Restrictions", "Special_Weapon_Valid_Targets",
        "Ranged_Target_Z_Adjust", "Projectile_Types", "Projectile_Damage", "Projectile_Fire_Recharge_Seconds",
        "Projectile_Fire_Pulse_Count", "Projectile_Fire_Pulse_Delay_Seconds", "Targeting_Fire_Inaccuracy", "Fires_Forward",
        "Turret_Rotate_Extent_Degrees", "Turret_Elevate_Extent_Degrees", "Death_Clone", "Death_Fade_Time",
        "Death_Persistence_Duration", "Collidable_By_Projectile_Living", "Collidable_By_Projectile_Dead", "Is_Valid_Target",
        "Is_Named_Hero", "Is_Generic_Hero"})
        if (core::iequals(field, tag)) return true;
    return false;
}

// Unverified gates are explicit data at the boundary, never inferred from enum positions.
// R2/R3: raw passability bit assignments. LR3-10: collision ordering is independent of masks.
// R13-17..21: animation completion remains a live caller input, not a guessed clip timer.
struct LandContentInputs {
    std::vector<sim::tactical::LandMovementMask> unverified_passability_bits;
    std::vector<LandCollisionRank> collision_class_order;
    sim::tactical::LandContactSettings contacts;
    std::vector<sim::tactical::LandOccupation> occupations;
    std::optional<sim::tactical::LandBounds> bounds;
    std::map<sim::tactical::PlayerId, std::uint32_t> last_known_capacity;
    std::vector<LandStaticGate> unverified_static_objects;
    // Content-error policy of the combat rows (#4589): skip_object leaves a type with a bad row out of the combat table
    // and names it in the binding notes; strict refuses the binding.
    units::ContentErrors content_errors{units::ContentErrors::skip_object};
};

// Menu-started land battles have no bindings file: derive the gates from the loaded authored tables.
// Explicit unverified assignment (R2/R3, LR3-10): Clear is bit 0, the other movement-class passabilities
// take the next bits in sorted name order, collision classes are ranked in sorted name order. It does not
// claim retail map-bit or collision-enum qualification; --eawr-land-bindings stays the override.
// Unverified service constants for menu-started battles (not measured retail values): the contact quota, reset
// interval, prediction seconds and minimum distance, and the last-known capacity every seat starts with.
inline constexpr std::uint32_t unverified_contact_quota = 4096;
inline constexpr std::uint32_t unverified_contact_reset_interval = 1;
inline constexpr std::uint32_t unverified_last_known_capacity = 50;
inline constexpr std::size_t max_passability_names = 32; // Clear plus the named bits must fit the uint32_t mask.
// LSL-02: the debug build's object type defaults for a tag a land member does not author.
inline constexpr sim::math::Fixed type_default_max_speed = sim::math::Fixed::from_raw(sim::math::Fixed::scale / 4); // 0.25
inline constexpr sim::math::Fixed type_default_max_rate_of_turn = sim::math::Fixed::from_raw((sim::math::Fixed::scale * 23 + 5) / 10); // 2.3
inline constexpr sim::math::Fixed type_default_scale_factor = sim::math::Fixed::from_raw(sim::math::Fixed::scale); // 1.0
// LSL-13: a converted land physical value (speed, rate, turn, footprint or static extent) saturates at 2^24 world units
// instead of overflowing; retail stores an infinity there. Far beyond any authored stock value or map.
inline constexpr sim::math::Fixed land_physical_limit = sim::math::Fixed::from_raw(sim::math::Fixed::scale << 24);
[[nodiscard]] inline sim::math::Fixed land_saturated_product(const sim::math::Fixed left, const sim::math::Fixed right) {
    sim::math::Fixed product;
    if (!sim::math::try_multiply(left, right, product))
        product = (left < sim::math::Fixed{}) != (right < sim::math::Fixed{})
            ? sim::math::Fixed::from_raw(-land_physical_limit.raw()) : land_physical_limit;
    return std::clamp(product, sim::math::Fixed::from_raw(-land_physical_limit.raw()), land_physical_limit);
}

[[nodiscard]] inline core::Result<LandContentInputs> authored_land_content_inputs(const units::UnitTables& tables) {
    LandContentInputs result;
    result.unverified_passability_bits.push_back({"Clear", 1});
    std::set<std::string> passabilities, collisions;
    for (const auto& row : tables.land_movement_classes) for (const auto& name : row.passabilities)
        if (name != "Clear") passabilities.insert(name);
    if (passabilities.size() + 1 > max_passability_names) {
        core::Diagnostic error; error.code = units::diagnostic_codes::motion;
        error.message = "authored land passability names exceed the 32-bit mask";
        return core::Result<LandContentInputs>::failure(std::move(error));
    }
    std::uint32_t bit = 2;
    for (const auto& name : passabilities) { result.unverified_passability_bits.push_back({name, bit}); bit <<= 1; }
    for (const auto& company : tables.land_companies) for (const auto& member : company.locomotion)
        collisions.insert(std::string(units::to_string(member.collision_class)));
    std::uint32_t order{};
    for (const auto& name : collisions) result.collision_class_order.push_back({name, order++});
    result.contacts = {unverified_contact_quota, unverified_contact_reset_interval, sim::math::Fixed::from_ratio(1, 3).value(), sim::math::Fixed::from_raw(sim::math::Fixed::scale)};
    for (std::uint32_t player = 1; player <= 8; ++player) result.last_known_capacity[player] = unverified_last_known_capacity;
    return core::Result<LandContentInputs>::success(std::move(result));
}

[[nodiscard]] inline core::Result<LandStaticBinding> build_land_static_binding(
    const units::UnitTables& tables, const units::LandStaticObject& object,
    const LandStaticGate& gate, std::span<const sim::tactical::LandMovementMask> bits) {
    using sim::math::Fixed;
    using Result = core::Result<LandStaticBinding>;
    const auto fail = [&](std::string field, std::string reason) {
        core::Diagnostic error; error.code = units::diagnostic_codes::motion;
        error.message = object.id + " " + field + ": " + reason;
        return Result::failure(std::move(error));
    };
    if (!object.land_obstacle || !core::iequals(gate.type, object.id))
        return fail("LandBehavior", "requires attached LAND_OBSTACLE behavior");
    for (const auto& row : tables.unresolved)
        if ((core::iequals(row.owner, object.id) || (!object.proxy_type.empty() && core::iequals(row.owner, object.proxy_type)))
            && !land_combat_field(row.field))
            return fail(row.field, row.reason);
    // LSL-08: a negative authored obstacle extent is kept as parsed; a non-positive extent rasterizes no cells.
    LandStaticBinding binding; binding.type = type_id(object.id);
    const auto scale = object.scale_factor ? object.scale_factor : gate.scale_factor;
    if (!scale) return fail("unverified-static-scale", "Scale_Factor is absent; constructor default remains unverified");
    // LSL-10: a zero or negative scale sizes the geometry by its magnitude (zero for zero; unverified).
    const auto size = *scale < Fixed{} ? Fixed::from_raw(-scale->raw()) : *scale;
    Fixed x_half, y_half, z_half;
    if (object.collision_bounds) {
        const auto& box = *object.collision_bounds;
        const auto half = [&](Fixed low, Fixed high, Fixed& value) {
            Fixed extent;
            if (high < low || !sim::math::try_subtract(high, low, extent)) return false;
            value = land_saturated_product(Fixed::from_raw(extent.raw() / 2), size); // LSL-13
            return true;
        };
        if (!half(box.min.x, box.max.x, x_half) || !half(box.min.y, box.max.y, y_half)
            || !half(box.min.z, box.max.z, z_half)) return fail("Land_Model_Name", "collision bounds overflow or are reversed");
    }
    Fixed width, height;
    if (!sim::math::try_add(y_half, y_half, width) || !sim::math::try_add(x_half, x_half, height))
        return fail("Land_Model_Name", "scaled obstacle dimensions overflow");
    const auto resolve = [](const std::optional<Fixed>& value, Fixed fallback) {
        // LSL-13: an authored extent or offset saturates at the land physical limit.
        return value && *value != Fixed{} ? std::clamp(*value, Fixed::from_raw(-land_physical_limit.raw()), land_physical_limit) : fallback;
    };
    binding.width = resolve(object.width, width); binding.height = resolve(object.height, height);
    binding.x_offset = resolve(object.x_offset, Fixed::from_raw(-y_half.raw()));
    binding.y_offset = resolve(object.y_offset, Fixed::from_raw(-x_half.raw()));
    binding.z_extent = z_half;
    binding.fine_tune = object.fine_tune.value_or(true); // WLPA-64
    binding.model_present = object.model_present;
    binding.scale_factor = *scale;
    binding.collision_meshes = object.collision_meshes;
    const auto bit = [&](std::string_view name) -> std::optional<std::uint32_t> {
        if (name.empty() || core::iequals(name, "None")) return 0;
        for (const auto& row : bits) if (core::iequals(row.name, name)) return row.bits;
        return {};
    };
    // WLPA-63: defaults/override are per attached behavior; raw enum bits remain an explicit input.
    for (const auto& behavior : object.behaviors) {
        std::string_view passability = "None";
        if (core::iequals(behavior, "LAND_OBSTACLE")) passability = "Obstacle";
        if (core::iequals(behavior, "WALL")) passability = "EmpireWall";
        if (!object.override_passability.empty() && !core::iequals(object.override_passability, "None"))
            passability = object.override_passability;
        auto value = bit(passability);
        if (!value && core::iequals(passability, "Obstacle") && gate.primary_passability) value = gate.primary_passability;
        if (!value) return fail("unverified-passability-bits", "missing " + std::string(passability));
        binding.primary_passability |= *value;
    }
    if (object.behaviors.empty()) {
        const auto obstacle = bit("Obstacle");
        if (!obstacle && !gate.primary_passability)
            return fail("unverified-passability-bits", "missing Obstacle");
        binding.primary_passability = obstacle.value_or(gate.primary_passability);
    }
    const auto secondary = bit(object.secondary_passability);
    if (!secondary) return fail("SecondaryOccupationPassability", "unverified-passability-bits gate is missing " + object.secondary_passability);
    binding.secondary_passability = *secondary;
    binding.water_bit = bit("Water").value_or(0);
    return Result::success(std::move(binding));
}

// LC-13 spacing remains in authored units: L5 applies the cited factor of 20 exactly once.
// All other physical profile values are world units/second (turns/second for turn rate).
[[nodiscard]] inline core::Result<LandStartBindings> build_land_bindings(const units::UnitTables& tables,
    std::span<const std::string> company_types, const LandContentInputs& inputs) {
    using namespace sim::tactical;
    using sim::math::Fixed;
    using Result = core::Result<LandStartBindings>;
    const auto fail = [](const std::string& owner, std::string_view field, const std::string& reason) {
        core::Diagnostic error;
        error.code = units::diagnostic_codes::motion;
        error.message = owner + " " + std::string(field) + ": " + reason;
        return Result::failure(std::move(error));
    };
    const auto equal = [](std::string_view a, std::string_view b) {
        return core::iequals(a, b);
    };
    // LSL-11: a negative formation spacing adds no spacing (unverified); the authored value stays in the company data.
    const auto spacing = [](const std::optional<Fixed>& value) {
        return value ? std::optional<Fixed>{std::clamp(*value, Fixed{}, land_physical_limit)} : value; // LSL-11/13
    };
    if (tables.domain != BattleDomain::land) return fail("land content", "domain", "requires a land unit table (BF-2)");
    for (const auto& row : tables.unresolved)
        if (equal(row.owner, "MovementClassType")) return fail(row.owner, row.field, row.reason);
    std::optional<Fixed> multiplier;
    const auto speed_tag = units::domain_tag(tables.domain, units::DomainTag::speed_multiplier);
    for (const auto& value : tables.constants.scalars) if (equal(value.tag, speed_tag)) multiplier = value.value;
    if (!multiplier || *multiplier <= Fixed{}) return fail("GameConstants", speed_tag, "required positive scalar is absent or invalid");
    LandStartBindings result;
    result.land.contacts = inputs.contacts;
    result.land.occupations = inputs.occupations;
    result.bounds = inputs.bounds;
    result.last_known_capacity = inputs.last_known_capacity;
    if (inputs.contacts.prediction_seconds < Fixed{} || inputs.contacts.minimum_prediction_distance < Fixed{})
        return fail("land content", "contacts", "negative prediction settings");
    std::uint32_t assigned_bits{};
    std::vector<std::string> bit_names;
    for (const auto& bit : inputs.unverified_passability_bits) {
        if (bit.name.empty() || !bit.bits || (bit.bits & (bit.bits - 1)) || (assigned_bits & bit.bits)
            || std::any_of(bit_names.begin(), bit_names.end(), [&](const auto& name) { return equal(name, bit.name); }))
            return fail("land content", "unverified-passability-bits", "expected unique names and disjoint single bits");
        assigned_bits |= bit.bits;
        bit_names.push_back(bit.name);
        if (equal(bit.name, "Clear")) result.land.clear_bit = bit.bits;
    }
    if (!result.land.clear_bit) return fail("land content", "unverified-passability-bits", "Clear bit must be supplied explicitly (R2/R3)");
    for (const auto& unresolved : tables.unresolved) {
        if (!land_combat_field(unresolved.field)) continue;
        if (std::any_of(tables.land_static_objects.begin(), tables.land_static_objects.end(),
            [&](const auto& object) { return equal(object.id, unresolved.owner); }))
            result.static_notes.push_back(unresolved.owner + ": " + unresolved.field + ": " + unresolved.reason);
    }
    std::set<std::string> static_gates;
    for (const auto& gate : inputs.unverified_static_objects) {
        if (gate.type.empty() || !static_gates.insert(core::to_lower(gate.type)).second)
            return fail("land content", "unverified-static-objects", "empty or duplicate type gate");
        const auto object = std::find_if(tables.land_static_objects.begin(), tables.land_static_objects.end(),
            [&](const auto& row) { return equal(row.id, gate.type); });
        if (object == tables.land_static_objects.end()) return fail(gate.type, "unverified-static-objects", "type is not loaded");
        auto binding = build_land_static_binding(tables, *object, gate, inputs.unverified_passability_bits);
        if (!binding) return Result::failure(binding.error());
        result.land.static_objects.push_back(binding.value().type);
        result.static_objects.push_back(std::move(binding).value());
    }
    for (const auto& object : tables.land_static_objects) {
        if (!object.land_obstacle) continue;
        if (static_gates.contains(core::to_lower(object.id))) continue;
        LandStaticGate gate; gate.type = object.id;
        auto binding = build_land_static_binding(tables, object, gate, inputs.unverified_passability_bits);
        if (!binding) { result.static_notes.push_back(binding.error().code + ": " + binding.error().message); continue; }
        result.land.static_objects.push_back(binding.value().type);
        result.static_objects.push_back(std::move(binding).value());
    }
    std::vector<std::string> collision_names;
    for (const auto& row : inputs.collision_class_order) {
        if (row.name.empty() || std::any_of(collision_names.begin(), collision_names.end(), [&](const auto& name) { return equal(name, row.name); }))
            return fail("land content", "collision-class-order", "empty or duplicate name");
        collision_names.push_back(row.name);
    }
    for (const auto& authored : tables.land_movement_classes) {
        if (authored.name.empty() || authored.passabilities.empty()
            || std::any_of(result.movement_masks.begin(), result.movement_masks.end(), [&](const auto& row) { return equal(row.name, authored.name); }))
            return fail("MovementClassType", authored.name, "empty or duplicate class");
        std::uint32_t mask{};
        for (const auto& name : authored.passabilities) {
            const auto bit = std::find_if(inputs.unverified_passability_bits.begin(), inputs.unverified_passability_bits.end(),
                [&](const auto& value) { return equal(name, value.name); });
            if (bit == inputs.unverified_passability_bits.end())
                return fail("MovementClassType", authored.name, "unverified-passability-bits gate is missing " + name);
            mask |= bit->bits;
        }
        result.movement_masks.push_back({authored.name, mask});
    }
    std::set<TypeId> company_ids;
    for (const auto& name : company_types) {
        const auto company = std::find_if(tables.land_companies.begin(), tables.land_companies.end(),
            [&](const auto& value) { return equal(value.id, name); });
        if (company == tables.land_companies.end()) return fail(name, "Company_Units", "company is absent or unresolved");
        if (!type_id(company->id) || !company_ids.insert(type_id(company->id)).second)
            return fail(name, "type", "zero, duplicate or colliding company identity");
        if (company->members.empty() || company->locomotion.size() != company->members.size())
            return fail(name, "locomotion", "requires one authored profile per company member");
        if (!units::plan_land_company(*company)) return fail(name, "Max_Squad_Size", "invalid company expansion plan");
        LandCompanyBinding binding;
        binding.type = type_id(company->id); binding.company = *company;
        binding.formation_spacing = spacing(company->formation_spacing).value_or(Fixed{});
        const auto frame_rate = Fixed::from_raw(logical_frames_per_second * Fixed::scale);
        // LSL-13: the conversion saturates at the land physical limit instead of refusing the company.
        const auto convert = [&](Fixed value, Fixed factor, Fixed& output) {
            output = land_saturated_product(land_saturated_product(value, *multiplier), factor);
            return true;
        };
        // LSL-02/03: an absent speed is the type default; zero is an immobile member, a negative one too (unverified).
        const auto member_speed = [](const units::LandCompanyMember& member) {
            return std::max(member.max_speed.value_or(type_default_max_speed), Fixed{});
        };
        // LC-02: creation may skip earlier members. Any survivor can become the first created.
        if (!equal(company->team_locomotor, "TEAM_LOCOMOTOR")
            && std::any_of(company->members.begin(), company->members.end(), [](const auto& member) { return member.create_team; })) {
            Fixed effective;
            const auto team_speed = std::max(company->team_max_speed.value_or(type_default_max_speed), Fixed{});
            auto base = team_speed;
            for (const auto& member : company->members) base = std::min(base, member_speed(member));
            if (!convert(base, frame_rate, effective))
                return fail(name, "Create_Team_Type/Max_Speed", "team speed overflows");
            binding.effective_team_max_speed = effective;
            if (!convert(team_speed, frame_rate, effective))
                return fail(name, "Create_Team_Type/Max_Speed", "team type speed overflows");
            binding.effective_team_type_max_speed = effective;
        }
        for (std::size_t i = 0; i < company->members.size(); ++i) {
            const auto& member = company->members[i];
            const auto& authored = company->locomotion[i];
            if (member.type.empty() || !type_id(member.type)) return fail(name, "Company_Units", "empty or zero member identity");
            // Invalid authored optional values must not fall through to an absence default.
            for (const auto& row : tables.unresolved) {
                if (!equal(row.owner, member.type) && !equal(row.owner, company->team_type) && !equal(row.owner, name)) continue;
                for (const auto tag : {"Max_Speed", "Max_Rate_Of_Turn", "OverrideAcceleration", "OverrideDeceleration",
                    "Scale_Factor", "Custom_Soft_Footprint_Radius", "Custom_Hard_XExtent", "Custom_Hard_YExtent",
                    "FormationSpacing", "Walk_Transition", "Has_Pre_Turn_Anim", "Stopped_Rate_Of_Turn", "UnitCollisionClass"})
                    if (equal(row.field, tag)) return fail(row.owner, row.field, row.reason);
            }
            if (member.stay_in_transport) return fail(member.type, "Stay_In_Transport_During_Ground_Battle", "transport-only member needs transport creation/unload");
            if (!member.unique_ground_unit.empty()) return fail(member.type, "Unique_Ground_Unit", "unique-ground creation path is not bound");
            if (member.locomotor.empty()) return fail(member.type, "LandBehavior", "no authored locomotor");
            const auto movement = std::find_if(result.movement_masks.begin(), result.movement_masks.end(),
                [&](const auto& row) { return equal(row.name, member.movement_class); });
            if (movement == result.movement_masks.end()) return fail(member.type, "MovementClass", "class has no authored mask");
            const auto collision = std::find_if(inputs.collision_class_order.begin(), inputs.collision_class_order.end(),
                [&](const auto& row) { return equal(row.name, units::to_string(authored.collision_class)); });
            if (collision == inputs.collision_class_order.end()) return fail(member.type, "UnitCollisionClass", "class has no explicit ordering binding");
            // LSL-02: absent values are the type defaults; LSL-03: zero speed or turn rate is kept (an immobile or
            // non-turning member), a negative one reads as zero (unverified). The authored data stays as parsed.
            const auto speed = member_speed(member);
            const auto turn = std::max(authored.rate_of_turn.value_or(type_default_max_rate_of_turn), Fixed{});
            const auto scale = authored.scale_factor.value_or(type_default_scale_factor);
            if ((authored.collision_x && *authored.collision_x < Fixed{}) || (authored.collision_y && *authored.collision_y < Fixed{}))
                return fail(member.type, "Land_Model_Name", "negative model collision extent");
            // LSL-04: a non-positive custom radius or extent is the unauthored default (zero): the model bounds apply.
            const auto positive = [](const std::optional<Fixed>& value) { return value && *value > Fixed{} ? value : std::optional<Fixed>{}; };
            auto radius = positive(authored.soft_radius);
            if (!radius) {
                if (!authored.collision_x || !authored.collision_y)
                    return fail(member.type, "Land_Model_Name", "soft footprint requires collision bounds or a positive authored radius");
                radius = std::max(*authored.collision_x, *authored.collision_y);
            }
            LandMemberBinding output;
            output.type = type_id(member.type); output.collision_class = collision->order;
            auto& p = output.locomotion;
            Fixed acceleration_factor, degrees_per_second;
            // LSL-09: a negative override rate takes the absent-override rule (maximum speed; unverified).
            const auto rate = [&](const std::optional<Fixed>& value) { return value && *value >= Fixed{} ? *value : speed; };
            // LSL-10: a zero or negative scale gives a footprint of the scaled magnitude (zero for zero; unverified).
            const auto scaled = [&](Fixed value, Fixed& product) {
                product = land_saturated_product(value, scale); // LSL-13
                if (product < Fixed{}) product = Fixed::from_raw(-product.raw());
                return true;
            };
            if (!sim::math::try_multiply(frame_rate, frame_rate, acceleration_factor)
                || !convert(speed, frame_rate, p.maximum_speed)
                || !convert(rate(authored.acceleration), acceleration_factor, p.acceleration)
                || !convert(rate(authored.deceleration), acceleration_factor, p.deceleration)
                || !convert(turn, frame_rate, degrees_per_second)
                || !sim::math::try_divide(degrees_per_second, Fixed::from_raw(360 * Fixed::scale), p.turn_rate)
                || !scaled(*radius, p.radius))
                return fail(member.type, "locomotion", "physical conversion overflows");
            output.formation_spacing = spacing(authored.formation_spacing).value_or(Fixed{});
            const auto hard = [&](const std::optional<Fixed>& custom, const std::optional<Fixed>& model) {
                return positive(custom) ? custom : model;
            };
            const auto hard_x = hard(authored.hard_x, authored.collision_x), hard_y = hard(authored.hard_y, authored.collision_y);
            if (!hard_x || !hard_y)
                return fail(member.type, "Custom_Hard_XExtent/Custom_Hard_YExtent", "hard footprint requires authored extents or model collision bounds");
            if (!scaled(*hard_x, output.hard_x) || !scaled(*hard_y, output.hard_y))
                return fail(member.type, "Custom_Hard_XExtent/Custom_Hard_YExtent", "scaled hard footprint overflows");
            p.walk_transition = authored.walk_transition;
            p.collision_class = authored.collision_class;
            // Preserve the exact terrain name used for lookup by the session.
            const auto movement_bits = movement->bits;
            if (std::none_of(result.movement_masks.begin(), result.movement_masks.end(),
                [&](const auto& row) { return row.name == member.movement_class; }))
                result.movement_masks.push_back({member.movement_class, movement_bits});
            binding.members.push_back(std::move(output));
        }
        result.land.companies.push_back(std::move(binding));
    }
    // LS shoot PR 1: the combat and durability rows of the loaded land bodies. A table without a damageable or armed
    // body binds none, so the movement-only content keeps empty tables. The rows cover the selected companies' members
    // and every loaded type that no company lists (statics); another company's members stay out.
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const auto& unit) { return unit.hull || unit.weapon; })) {
        std::set<std::string> chosen, listed;
        for (const auto& company : tables.land_companies) {
            const bool selected = std::any_of(company_types.begin(), company_types.end(),
                [&](const auto& name) { return equal(company.id, name); });
            for (const auto& member : company.members) {
                listed.insert(core::to_lower(member.type));
                if (selected) chosen.insert(core::to_lower(member.type));
            }
        }
        auto narrowed = tables;
        std::erase_if(narrowed.units, [&](const auto& unit) {
            const auto key = core::to_lower(unit.id);
            return listed.contains(key) && !chosen.contains(key);
        });
        auto durability = units::durability_table(narrowed);
        if (!durability) return Result::failure(durability.error());
        std::vector<core::Diagnostic> skipped;
        auto combat = units::combat_table(narrowed, inputs.content_errors, &skipped);
        if (!combat) return Result::failure(combat.error());
        result.durability = std::move(durability).value();
        result.combat = std::move(combat).value();
        for (const auto& row : skipped) result.static_notes.push_back(row.code + ": " + row.message);
    }
    // Bind the complete land-only authored inputs plus external gates. Space never calls this adapter.
    auto bytes = units::canonical_encoding(tables);
    const auto number = [&](std::uint64_t value) { for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift)); };
    const auto fixed = [&](Fixed value) { number(static_cast<std::uint64_t>(value.raw())); };
    const auto text = [&](const std::string& value) { number(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); };
    text("land-content-adapter-v5");
    number(result.static_objects.size());
    for (const auto& row : result.static_objects) {
        number(row.type); fixed(row.width); fixed(row.height); fixed(row.x_offset); fixed(row.y_offset);
        number(row.z_extent.has_value()); if (row.z_extent) fixed(*row.z_extent);
        number(row.primary_passability); number(row.secondary_passability); number(row.water_bit);
        number(row.fine_tune); number(row.model_present); fixed(row.scale_factor);
    }
    number(result.land.clear_bit);
    number(result.movement_masks.size());
    for (const auto& row : result.movement_masks) { text(row.name); number(row.bits); }
    number(result.land.companies.size());
    for (const auto& row : result.land.companies) {
        number(row.type); fixed(row.formation_spacing); number(row.effective_team_max_speed.has_value());
        if (row.effective_team_max_speed) fixed(*row.effective_team_max_speed);
        number(row.effective_team_type_max_speed.has_value());
        if (row.effective_team_type_max_speed) fixed(*row.effective_team_type_max_speed);
        number(row.members.size());
        for (const auto& member : row.members) {
            number(member.type); number(member.collision_class); number(member.formation_spacing.has_value());
            if (member.formation_spacing) fixed(*member.formation_spacing);
            fixed(member.hard_x); fixed(member.hard_y);
            const auto& p = member.locomotion;
            fixed(p.maximum_speed); fixed(p.acceleration); fixed(p.deceleration); fixed(p.turn_rate); fixed(p.radius); number(p.walk_transition); number(static_cast<std::uint8_t>(p.collision_class));
        }
    }
    number(inputs.contacts.complexity_quota); number(inputs.contacts.reset_interval);
    fixed(inputs.contacts.prediction_seconds); fixed(inputs.contacts.minimum_prediction_distance);
    number(inputs.occupations.size());
    for (const auto& row : inputs.occupations) {
        number(row.object); number(row.primary_passability); number(row.secondary_passability);
        number(row.primary_cells.size()); for (const auto cell : row.primary_cells) number(cell);
        number(row.secondary_cells.size()); for (const auto cell : row.secondary_cells) number(cell);
    }
    number(inputs.bounds.has_value());
    if (inputs.bounds) { fixed(inputs.bounds->min_x); fixed(inputs.bounds->min_y); fixed(inputs.bounds->max_x); fixed(inputs.bounds->max_y); }
    result.identity = core::sha256(bytes);
    return Result::success(std::move(result));
}

} // namespace eawr::skirmish
