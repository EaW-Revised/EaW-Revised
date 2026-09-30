#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"
#include "unit_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

// The #70 motion table: the unit tables' movement values in the shape the tactical session binds
// (docs/behaviour/space-movement.md, MV-01).
namespace eawr::units {
namespace {

namespace tactical = sim::tactical;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::motion);
    diagnostic.message = std::move(message);
    return diagnostic;
}

// Space_Layer names (FoC's SpaceLayerType); anything else is no layer (AV-U6).
[[nodiscard]] tactical::SpaceLayer space_layer(const std::string& text) {
    const auto name = detail::trim(text);
    if (detail::iequals(name, "Capital")) return tactical::SpaceLayer::capital;
    if (detail::iequals(name, "Frigate")) return tactical::SpaceLayer::frigate;
    if (detail::iequals(name, "Corvette")) return tactical::SpaceLayer::corvette;
    if (detail::iequals(name, "StaticObject")) return tactical::SpaceLayer::static_object;
    if (detail::iequals(name, "SuperCapital")) return tactical::SpaceLayer::super_capital;
    return tactical::SpaceLayer::none;
}

[[nodiscard]] Fixed magnitude(const Fixed value) noexcept { return value.raw() < 0 ? Fixed::from_raw(-value.raw()) : value; }

// AV-05 (research E71-15, E71-18, E71-19): the hard half extents are the custom extents when
// both are set, else per axis the custom value or the model's collision half extent, times
// Scale_Factor; the soft radius is Custom_Soft_Footprint_Radius, else Space_Obstacle_Radius,
// else the larger collision half extent, times Scale_Factor.
[[nodiscard]] std::optional<tactical::Footprint> footprint_of(const std::string& id, const std::string& layer_name,
    const std::optional<Fixed>& scale_factor, const SpaceFootprint& data, std::optional<core::Diagnostic>& error) {
    const auto layer = space_layer(layer_name);
    if (layer == tactical::SpaceLayer::none) return std::nullopt;
    const Fixed scale = scale_factor.value_or(Fixed::from_raw(Fixed::scale));
    const auto times = [&](const Fixed value) {
        auto product = sim::math::multiply(magnitude(value), scale);
        if (!product) {
            if (!error) error = failure(id + " footprint: " + product.error().message);
            return Fixed{};
        }
        return product.value();
    };
    const auto positive = [](const std::optional<Fixed>& value) { return value && value->raw() > 0; };
    tactical::Footprint footprint;
    footprint.type_id = assets::object_type_crc(id);
    footprint.layer = layer;
    footprint.obstacle = data.space_obstacle;
    const Fixed box_x = data.collision_x.value_or(Fixed{});
    const Fixed box_y = data.collision_y.value_or(Fixed{});
    if (positive(data.custom_hard_x) && positive(data.custom_hard_y)) {
        footprint.x_extent = times(*data.custom_hard_x);
        footprint.y_extent = times(*data.custom_hard_y);
    } else if (data.collision_x) {
        footprint.x_extent = times(positive(data.custom_hard_x) ? *data.custom_hard_x : box_x);
        footprint.y_extent = times(positive(data.custom_hard_y) ? *data.custom_hard_y : box_y);
    }
    if (positive(data.custom_soft_radius)) {
        footprint.radius = times(*data.custom_soft_radius);
    } else if (positive(data.obstacle_radius)) {
        footprint.radius = times(*data.obstacle_radius);
    } else if (data.collision_x) {
        footprint.radius = times(std::max(magnitude(box_x), magnitude(box_y)));
    }
    return footprint;
}

} // namespace

core::Result<tactical::MotionTable> motion_table(const UnitTables& tables) {
    using Result = core::Result<tactical::MotionTable>;
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    const auto multiplier = scalar("Object_Max_Speed_Multiplier_Space");
    const auto rotations = scalar("MaxRotationsSpace");
    const auto expansion = scalar("XYExpansionDistanceSpace");
    const auto corvette = scalar("TurnInPlaceSlowdownCorvette");
    const auto frigate = scalar("TurnInPlaceSlowdownFrigate");
    const auto capital = scalar("TurnInPlaceSlowdownCapital");
    if (!multiplier || !rotations || !expansion || !corvette || !frigate || !capital) {
        return Result::failure(failure("motion needs Object_Max_Speed_Multiplier_Space, MaxRotationsSpace, "
                                       "XYExpansionDistanceSpace and TurnInPlaceSlowdownCorvette, -Frigate and "
                                       "-Capital"));
    }
    std::optional<core::Diagnostic> error;
    const auto scaled = [&](const Fixed value, const std::string& what) {
        auto product = sim::math::multiply(value, *multiplier);
        if (!product) {
            if (!error) error = failure(what + ": " + product.error().message);
            return Fixed{};
        }
        return product.value();
    };

    tactical::MotionTable table;
    auto arc = sim::math::divide(Fixed::from_raw(360 * Fixed::scale), *rotations);
    if (!arc) return Result::failure(failure("MaxRotationsSpace: " + arc.error().message));
    table.rules.arc_degrees = arc.value();
    table.rules.expansion_distance = *expansion;
    for (const auto& unit : tables.units) {
        if (unit.kind != UnitKind::ship || !unit.movement.max_speed || !unit.movement.max_rate_of_turn) continue;
        const auto& movement = unit.movement;
        tactical::MotionProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        profile.max_speed = scaled(*movement.max_speed, unit.id + " Max_Speed");
        // Without an override the engine accelerates at the maximum speed per frame.
        profile.acceleration = movement.acceleration ? scaled(*movement.acceleration, unit.id + " OverrideAcceleration")
                                                     : profile.max_speed;
        profile.deceleration = movement.deceleration ? scaled(*movement.deceleration, unit.id + " OverrideDeceleration")
                                                     : profile.max_speed;
        profile.rate_of_turn = scaled(*movement.max_rate_of_turn, unit.id + " Max_Rate_Of_Turn");
        // BK-01: a type without the tags keeps the engine's defaults, a roll rate of 2 and a bank
        // angle of 70 degrees; the roll rate scales like the rate of turn.
        profile.roll_rate = scaled(movement.max_rate_of_roll.value_or(Fixed::from_raw(2 * Fixed::scale)),
                                   unit.id + " Max_Rate_Of_Roll");
        profile.bank_angle = movement.bank_turn_angle.value_or(Fixed::from_raw(70 * Fixed::scale));
        profile.turn_in_place_slowdown = Fixed::from_raw(Fixed::scale);
        if (movement.space_layer == "Corvette") {
            profile.turn_in_place_slowdown = *corvette;
        } else if (movement.space_layer == "Frigate") {
            profile.turn_in_place_slowdown = *frigate;
        } else if (movement.space_layer == "Capital" || movement.space_layer == "SuperCapital") {
            profile.turn_in_place_slowdown = *capital;
        }
        table.profiles.push_back(profile);
    }
    // #71: the path finder's constants and the footprints, when the constants are loaded.
    const auto count = [&](const std::string_view tag) -> std::optional<std::uint32_t> {
        const auto value = scalar(tag);
        if (!value || value->raw() <= 0 || value->raw() % Fixed::scale != 0) return std::nullopt;
        return static_cast<std::uint32_t>(value->raw() / Fixed::scale);
    };
    const auto wait_speed = scalar("WaitOperatorSpeedCoefficient");
    const auto wait_frames = scalar("WaitOperatorBaseFrameTime");
    const auto wait_cost = scalar("WaitOperatorCostCoefficient");
    const auto obstacle_cost = scalar("MinObstacleCostSpace");
    const auto path_cost = scalar("CurrentPathCostCoefficientSpace");
    const auto occupation = scalar("OccupationRadiusCoefficientSpace");
    const auto cutoff = scalar("SpacePathFailureDistanceCutoffCoefficient");
    const auto expansions_factor = scalar("SpacePathFailureMaxExpansionsCoefficient");
    const auto rotation_step = scalar("SpacePathFailureRotationExpansionIncrement");
    const auto forward_step = scalar("SpacePathFailureForwardExpansionIncrement");
    const auto expansions = count("SpacePathfindMaxExpansions");
    const auto tries = count("SpacePathingTries");
    const auto interval = count("SpaceObjectTrackingInterval");
    const auto windows = count("SpaceObjectTrackingTreeCount");
    const auto search_increment = scalar("DestinationSearchRadiusIncrementSpace");
    if (wait_speed && wait_frames && wait_cost && obstacle_cost && path_cost && occupation && cutoff && expansions_factor
        && rotation_step && forward_step && expansions && tries && interval && windows && search_increment) {
        table.avoidance = tactical::AvoidanceRules{*rotations, *wait_speed, *wait_frames, *wait_cost, *obstacle_cost,
            *path_cost, *occupation, *cutoff, *expansions_factor, *rotation_step, *forward_step, *expansions, *tries,
            *interval, *windows, *search_increment};
        for (const auto& unit : tables.units) {
            if (unit.kind == UnitKind::squadron || unit.kind == UnitKind::craft) continue;
            if (auto footprint = footprint_of(unit.id, unit.movement.space_layer, unit.scale_factor, unit.footprint, error)) {
                table.footprints.push_back(*footprint);
            }
        }
        for (const auto& obstacle : tables.obstacles) {
            if (auto footprint = footprint_of(obstacle.id, obstacle.space_layer, obstacle.scale_factor, obstacle.footprint,
                    error)) {
                table.footprints.push_back(*footprint);
            }
        }
        std::sort(table.footprints.begin(), table.footprints.end(),
            [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
        for (std::size_t index = 1; index < table.footprints.size(); ++index) {
            if (table.footprints[index].type_id == table.footprints[index - 1].type_id) {
                return Result::failure(failure("two footprints share type ID "
                    + std::to_string(table.footprints[index].type_id)));
            }
        }
    }
    // #75: craft flight, squadron types and hangars (docs/behaviour/space-fighters.md FL, FM).
    auto& squadrons = table.squadrons;
    // FO-11 (#599): a squadron's lane steer on a group move; without the constants it is off.
    squadrons.side_error_min = scalar("FormationMinimumSideError").value_or(Fixed{});
    squadrons.side_error_max = scalar("FormationMaximumSideError").value_or(Fixed{});
    const Fixed one = Fixed::from_raw(Fixed::scale);
    const auto times = [&](const Fixed value, const Fixed factor, const std::string& what) {
        auto product = sim::math::multiply(value, factor);
        if (!product) {
            if (!error) error = failure(what + ": " + product.error().message);
            return Fixed{};
        }
        return product.value();
    };
    for (const auto& unit : tables.units) {
        const auto& movement = unit.movement;
        if (unit.kind == UnitKind::craft && movement.max_speed && movement.max_rate_of_turn) {
            tactical::CraftProfile craft;
            craft.type_id = assets::object_type_crc(unit.id);
            craft.max_speed = scaled(*movement.max_speed, unit.id + " Max_Speed");
            craft.min_speed = scaled(movement.min_speed.value_or(Fixed{}), unit.id + " Min_Speed");
            craft.rate_of_turn = scaled(*movement.max_rate_of_turn, unit.id + " Max_Rate_Of_Turn");
            craft.lift = scaled(movement.max_lift.value_or(Fixed{}), unit.id + " Max_Lift");
            // FM-01: thrust is not scaled; a craft without it changes speed by its maximum speed.
            craft.thrust = movement.max_thrust.value_or(craft.max_speed);
            craft.roll_rate = scaled(movement.max_rate_of_roll.value_or(Fixed::from_raw(2 * Fixed::scale)),
                                     unit.id + " Max_Rate_Of_Roll");
            craft.bank_angle = movement.bank_turn_angle.value_or(Fixed::from_raw(70 * Fixed::scale));
            craft.strafe_distance = unit.strafe_distance.value_or(Fixed{});
            craft.out_of_combat_defense = unit.out_of_combat_defense.value_or(Fixed{});
            craft.layer_z = movement.layer_z_adjust.value_or(Fixed{});
            craft.follow_distance = unit.follow_distance.value_or(Fixed{});
            craft.attack_distance = unit.targeting_max_attack_distance.value_or(Fixed{});
            // #447 SP-01: the chance and time as authored.
            if (unit.spin_away_on_death && unit.spin_away_chance && unit.spin_away_time) {
                craft.spin_away = tactical::SpinAwayProfile{*unit.spin_away_chance, *unit.spin_away_time};
            }
            squadrons.craft.push_back(craft);
        }
    }
    const auto is_craft = [&squadrons](const tactical::TypeId id) {
        return std::any_of(squadrons.craft.begin(), squadrons.craft.end(),
            [id](const tactical::CraftProfile& craft) { return craft.type_id == id; });
    };
    for (const auto& unit : tables.units) {
        if (unit.kind != UnitKind::squadron || unit.members.empty()) continue;
        tactical::SquadronProfile squadron;
        squadron.type_id = assets::object_type_crc(unit.id);
        bool flyable = true;
        for (const auto& member : unit.members) {
            const auto id = assets::object_type_crc(member.craft);
            flyable = flyable && is_craft(id);
            squadron.members.push_back(id);
            squadron.offsets.push_back(member.offset.value_or(Vec3{}));
        }
        if (!flyable) continue; // a member type without flight data: the squadron is not flown
        squadron.guard_chase_range = unit.guard_chase_range.value_or(Fixed{});
        squadron.idle_chase_range = unit.idle_chase_range.value_or(Fixed{});
        squadron.attack_move_response_range = unit.attack_move_response_range.value_or(Fixed{});
        squadron.formation_tolerance = unit.formation_error_tolerance.value_or(Fixed{});
        squadrons.squadrons.push_back(std::move(squadron));
    }
    std::sort(squadrons.craft.begin(), squadrons.craft.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    std::sort(squadrons.squadrons.begin(), squadrons.squadrons.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    const auto has_squadron = [&squadrons](const tactical::TypeId id) {
        return std::any_of(squadrons.squadrons.begin(), squadrons.squadrons.end(),
            [id](const tactical::SquadronProfile& squadron) { return squadron.type_id == id; });
    };
    for (const auto& unit : tables.units) {
        if (!unit.spawner || (unit.kind != UnitKind::ship && unit.kind != UnitKind::station)) continue;
        tactical::SpawnerProfile spawner;
        spawner.type_id = assets::object_type_crc(unit.id);
        for (const auto& entry : unit.spawner->starting) {
            const auto id = assets::object_type_crc(entry.squadron);
            if (!has_squadron(id)) continue;
            tactical::SpawnEntryProfile row;
            row.squadron = id;
            row.starting = entry.count;
            // SK-36 (owner, #179 Q2): the M2 fixture ignores Reserve_Spawned_Units_Tech_0, so each
            // entry launches its starting squadrons once and a lost one is not replaced. The
            // hangar keeps the retail reserve rule (FL-02, FL-08) for a table that sets one.
            row.reserve = 0;
            spawner.entries.push_back(row);
        }
        const auto delay = times(unit.spawner->delay_seconds.value_or(Fixed{}), Fixed::from_raw(30 * Fixed::scale),
                                 unit.id + " Spawned_Squadron_Delay_Seconds");
        spawner.delay_frames = delay.raw() > 0 ? static_cast<std::uint32_t>(delay.raw() / Fixed::scale) : 0U;
        // FL-05: bay points and spawn vectors in model space, scaled and turned like the fire points.
        const Fixed scale = unit.scale_factor.value_or(one);
        const auto turn = [](const Vec3& value) { return Vec3{Fixed::from_raw(-value.y.raw()), value.x, value.z}; };
        for (std::uint32_t index = 0; index < unit.hardpoints.size(); ++index) {
            const auto& hardpoint = unit.hardpoints[index];
            if (hardpoint.type != HardpointType::fighter_bay || !hardpoint.attachment.position) continue;
            const auto& at = *hardpoint.attachment.position;
            const auto axis = hardpoint.bay_axis.value_or(Vec3{one, Fixed{}, Fixed{}});
            const Fixed flyout = hardpoint.fighter_bay_flyout_distance.value_or(one);
            tactical::BayProfile bay;
            bay.hardpoint = index;
            bay.position = turn(Vec3{times(at.x, scale, unit.id + " bay"), times(at.y, scale, unit.id + " bay"),
                times(at.z, scale, unit.id + " bay")});
            bay.spawn_vector = turn(Vec3{times(axis.x, flyout, unit.id + " bay"), times(axis.y, flyout, unit.id + " bay"),
                times(axis.z, flyout, unit.id + " bay")});
            spawner.bays.push_back(bay);
        }
        spawner.mobile = unit.kind == UnitKind::ship;
        if (!spawner.entries.empty()) squadrons.spawners.push_back(std::move(spawner));
    }
    std::sort(squadrons.spawners.begin(), squadrons.spawners.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });

    if (error) return Result::failure(*error);
    std::sort(table.profiles.begin(), table.profiles.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    for (std::size_t index = 1; index < table.profiles.size(); ++index) {
        if (table.profiles[index].type_id == table.profiles[index - 1].type_id) {
            return Result::failure(failure("two unit types share type ID " + std::to_string(table.profiles[index].type_id)));
        }
    }
    auto valid = tactical::validate_motion(table);
    if (!valid) return Result::failure(failure(valid.error().message));
    return Result::success(std::move(table));
}

} // namespace eawr::units
