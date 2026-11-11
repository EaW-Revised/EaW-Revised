#include "eawr/units/content_selection.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/units/domain_tags.hpp"
#include "eawr/units/unit_tables.hpp"
#include "unit_internal.hpp"

#include <algorithm>
#include <cstdint>
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

std::string motion_decimal(const Fixed value) {
    const auto raw = value.raw();
    const auto magnitude = raw < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(raw)
                                   : static_cast<std::uint64_t>(raw);
    const auto scale = static_cast<std::uint64_t>(Fixed::scale);
    auto text = (raw < 0 ? std::string("-") : std::string{}) + std::to_string(magnitude / scale);
    auto remainder = magnitude % scale;
    if (remainder) text += '.';
    while (remainder) {
        remainder *= 10;
        text += static_cast<char>('0' + remainder / scale);
        remainder %= scale;
    }
    return text;
}

Fixed motion_scalar(const UnitTables& tables, std::string_view tag, core::LoadNoteBuffer& notes) {
    for (const auto& constant : tables.constants.scalars)
        if (constant.tag == tag && constant.value) return *constant.value;
    // Stock GameConstants.xml defaults; omitted globals retain defaults (LSA-P5).
    const std::pair<std::string_view, std::string_view> defaults[] = {
        {"Object_Max_Speed_Multiplier_Space", "1.2"}, {"Object_Max_Speed_Multiplier_Land", "1.56"},
        {"MaxRotationsSpace", "24"}, {"XYExpansionDistanceSpace", "300"},
        {"TurnInPlaceSlowdownCorvette", "2"}, {"TurnInPlaceSlowdownFrigate", "3"},
        {"TurnInPlaceSlowdownCapital", "4"}, {"WaitOperatorSpeedCoefficient", ".2"},
        {"WaitOperatorBaseFrameTime", "100"}, {"WaitOperatorCostCoefficient", ".8"},
        {"MinObstacleCostSpace", "15"}, {"CurrentPathCostCoefficientSpace", ".66"},
        {"OccupationRadiusCoefficientSpace", "1.2"}, {"SpacePathFailureDistanceCutoffCoefficient", ".25"},
        {"SpacePathFailureMaxExpansionsCoefficient", "1.7"}, {"SpacePathFailureRotationExpansionIncrement", ".5"},
        {"SpacePathFailureForwardExpansionIncrement", ".5"}, {"SpacePathfindMaxExpansions", "3500"},
        {"SpacePathingTries", "6"}, {"SpaceObjectTrackingInterval", "90"},
        {"SpaceObjectTrackingTreeCount", "45"}, {"DestinationSearchRadiusIncrementSpace", "50"},
        {"MovementReevaluationFrameCount", "10"}, {"Space_Guard_Range", "750"},
        {"Nebula_Ability_Disable_Time", "5"}
    };
    for (const auto& [name, text] : defaults) {
        if (name != tag) continue;
        const auto value = Fixed::from_decimal(text).value();
        core::LoadNote note;
        note.code = name.starts_with("TurnInPlace") || name.starts_with("Object_Max")
            || name == "MaxRotationsSpace" || name == "XYExpansionDistanceSpace"
            || name == "MovementReevaluationFrameCount" || name == "Space_Guard_Range"
            || name == "Nebula_Ability_Disable_Time" ? "EAWR-LOAD-0150" : "EAWR-LOAD-0158";
        note.object = "motion rules";
        note.tag = std::string(tag);
        note.classification = core::LoadCategory::retail_accepts;
        note.retail = "LSA-P5";
        note.action = core::LoadAction::default_used;
        note.effective_value = motion_decimal(value);
        note.diagnostic_code = std::string(diagnostic_codes::motion);
        note.message = note.code == "EAWR-LOAD-0150"
            ? "motion needs Object_Max_Speed_Multiplier_Space, MaxRotationsSpace, XYExpansionDistanceSpace "
              "and TurnInPlaceSlowdownCorvette, -Frigate and -Capital"
            : "motion needs " + std::string(tag);
        notes.append(std::move(note));
        return value;
    }
    return {};
}

Fixed motion_product(Fixed value, Fixed scale, const std::string& object, const char* tag,
                     const char* code, core::LoadNoteBuffer& notes) {
    auto product = sim::math::multiply(value, scale);
    if (product) return product.value();
    const auto raw = (value.raw() < 0) != (scale.raw() < 0) ? INT64_MIN : INT64_MAX;
    core::LoadNote note;
    note.code = code;
    note.object = object;
    note.tag = tag;
    note.authored_value = motion_decimal(value);
    const auto limit = (std::string_view(code) == "EAWR-LOAD-0157" ? tactical::max_motion_coordinate
                                                                            : tactical::max_motion_rate) * Fixed::scale;
    note.effective_value = motion_decimal(Fixed::from_raw(raw < 0 ? 0 : limit));
    note.action = core::LoadAction::clamped;
    note.diagnostic_code = std::string(diagnostic_codes::motion);
    note.message = object + " " + tag + ": " + product.error().message;
    notes.append(std::move(note));
    return Fixed::from_raw(raw);
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

[[nodiscard]] Fixed magnitude(const Fixed value) noexcept { return value.raw() < 0 ? Fixed::from_raw(value.raw() == INT64_MIN ? INT64_MAX : -value.raw()) : value; }

// AV-05 (research E71-15, E71-18, E71-19): the hard half extents are the custom extents when
// both are set, else per axis the custom value or the model's collision half extent, times
// Scale_Factor; the soft radius is Custom_Soft_Footprint_Radius, else Space_Obstacle_Radius,
// else the larger collision half extent, times Scale_Factor.
[[nodiscard]] std::optional<tactical::Footprint> footprint_of(const std::string& id, const std::string& layer_name,
    const std::optional<Fixed>& scale_factor, const SpaceFootprint& data, core::LoadNoteBuffer& load_notes) {
    const auto layer = space_layer(layer_name);
    const Fixed scale = scale_factor.value_or(Fixed::from_raw(Fixed::scale));
    const auto times = [&](const Fixed value, const char* tag) {
        return motion_product(magnitude(value), scale, id, tag, "EAWR-LOAD-0157", load_notes);
    };
    const auto positive = [](const std::optional<Fixed>& value) { return value && value->raw() > 0; };
    tactical::Footprint footprint;
    footprint.type_id = assets::object_type_crc(id);
    footprint.layer = layer;
    footprint.obstacle = data.space_obstacle;
    footprint.asteroid_field = data.hazard.asteroid_field;
    footprint.ion_storm = data.hazard.ion_storm;
    footprint.nebula = data.hazard.nebula;
    footprint.impassable_asteroid = data.hazard.impassable_asteroid;
    footprint.obstacle_offset = {data.hazard.obstacle_offset.x, data.hazard.obstacle_offset.y};
    // ML-24: a static non-obstacle with an authored zero obstacle radius remains scenery.
    // Its inherited layer must not activate the model-bound collision fallback.
    const bool scenery = layer == tactical::SpaceLayer::static_object && !data.space_obstacle
        && data.obstacle_radius == std::optional<Fixed>{Fixed{}};
    const Fixed box_x = scenery ? Fixed{} : data.collision_x.value_or(Fixed{});
    const Fixed box_y = scenery ? Fixed{} : data.collision_y.value_or(Fixed{});
    if (positive(data.custom_hard_x) && positive(data.custom_hard_y)) {
        footprint.x_extent = times(*data.custom_hard_x, "Custom_Hard_XExtent");
        footprint.y_extent = times(*data.custom_hard_y, "Custom_Hard_YExtent");
    } else if (!scenery && data.collision_x) {
        footprint.x_extent = times(positive(data.custom_hard_x) ? *data.custom_hard_x : box_x, "Custom_Hard_XExtent");
        footprint.y_extent = times(positive(data.custom_hard_y) ? *data.custom_hard_y : box_y, "Custom_Hard_YExtent");
    }
    // WWP-21: model geometry must exist before either soft-radius override is read.
    // The loader supplies collision extents only after resolving the type's model.
    if (data.collision_x) {
        if (positive(data.custom_soft_radius)) {
            footprint.radius = times(*data.custom_soft_radius, "Custom_Soft_Footprint_Radius");
        } else if (positive(data.obstacle_radius)) {
            footprint.radius = times(*data.obstacle_radius, "Space_Obstacle_Radius");
        } else if (!scenery) {
            footprint.radius = times(std::max(magnitude(box_x), magnitude(box_y)), "collision geometry");
        }
    }
    const auto clamp = [&](Fixed& value, const std::int64_t minimum, const std::int64_t maximum, const char* tag) {
        if (value.raw() >= minimum && value.raw() <= maximum) return;
        core::LoadNote note;
        note.code = "EAWR-LOAD-0157";
        note.object = id;
        note.tag = tag;
        note.authored_value = motion_decimal(value);
        note.diagnostic_code = std::string(diagnostic_codes::motion);
        note.message = "extents and radius must be in [0, 262144]; obstacle offsets in [-262144, 262144]";
        core::LoadNoteBuffer temporary;
        const auto effective = core::clamp_note(value.raw(), minimum, maximum, std::move(note), temporary);
        core::LoadNoteCollector collector;
        collector.merge(temporary);
        for (auto emitted : collector.snapshot()) {
            emitted.effective_value = motion_decimal(Fixed::from_raw(effective));
            load_notes.append(std::move(emitted));
        }
        value = Fixed::from_raw(effective);
    };
    const auto limit = tactical::max_motion_coordinate * Fixed::scale;
    clamp(footprint.x_extent, 0, limit, "Custom_Hard_XExtent");
    clamp(footprint.y_extent, 0, limit, "Custom_Hard_YExtent");
    clamp(footprint.radius, 0, limit, "Custom_Soft_Footprint_Radius");
    clamp(footprint.obstacle_offset.x, -limit, limit, "Space_Obstacle_Offset");
    clamp(footprint.obstacle_offset.y, -limit, limit, "Space_Obstacle_Offset");
    return footprint;
}

void append_unit_motion_profiles(const UnitTables& tables, tactical::MotionTable& table,
    const std::optional<Fixed>& multiplier, const std::optional<Fixed>& corvette,
    const std::optional<Fixed>& frigate, const std::optional<Fixed>& capital, core::LoadNoteBuffer& load_notes) {
    const ContentTypeSelection type_selection(tables);
    const auto scaled = [&](const Fixed value, const std::string& object, const char* tag) {
        return motion_product(value, *multiplier, object, tag, "EAWR-LOAD-0155", load_notes);
    };
    for (const auto& unit : tables.units) {
        if (!type_selection.keeps(unit)) continue;
        if (unit.kind != UnitKind::ship || !unit.movement.max_speed || !unit.movement.max_rate_of_turn) continue;
        const auto& movement = unit.movement;
        tactical::MotionProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        profile.max_speed = scaled(*movement.max_speed, unit.id, "Max_Speed");
        // Without an override the engine accelerates at the maximum speed per frame.
        profile.acceleration = movement.acceleration ? scaled(*movement.acceleration, unit.id, "OverrideAcceleration")
                                                     : profile.max_speed;
        profile.deceleration = movement.deceleration ? scaled(*movement.deceleration, unit.id, "OverrideDeceleration")
                                                     : profile.max_speed;
        profile.rate_of_turn = scaled(*movement.max_rate_of_turn, unit.id, "Max_Rate_Of_Turn");
        // BK-01: a type without the tags keeps the engine's defaults, a roll rate of 2 and a bank
        // angle of 70 degrees; the roll rate scales like the rate of turn.
        profile.roll_rate = scaled(movement.max_rate_of_roll.value_or(Fixed::from_raw(2 * Fixed::scale)),
                                   unit.id, "Max_Rate_Of_Roll");
        profile.bank_angle = movement.bank_turn_angle.value_or(Fixed::from_raw(70 * Fixed::scale));
        profile.turn_in_place_slowdown = Fixed::from_raw(Fixed::scale);
        const auto layer = space_layer(movement.space_layer);
        if (layer == tactical::SpaceLayer::corvette) {
            profile.turn_in_place_slowdown = *corvette;
        } else if (layer == tactical::SpaceLayer::frigate) {
            profile.turn_in_place_slowdown = *frigate;
        } else if (layer == tactical::SpaceLayer::capital || layer == tactical::SpaceLayer::super_capital) {
            profile.turn_in_place_slowdown = *capital;
        }
        table.profiles.push_back(profile);
    }
}

std::optional<core::Diagnostic> append_avoidance_footprints(const UnitTables& tables, tactical::MotionTable& table,
    const std::optional<Fixed>& rotations, core::LoadNoteBuffer& load_notes) {
    const auto scalar = [&](std::string_view tag) -> std::optional<Fixed> {
        return motion_scalar(tables, tag, load_notes);
    };
    const auto count = [&](const std::string_view tag) -> std::optional<std::uint32_t> {
        const auto value = *scalar(tag);
        const std::int64_t maximum = tag == "SpacePathfindMaxExpansions" ? tactical::max_avoidance_expansions
            : tag == "SpacePathingTries" ? tactical::max_avoidance_tries
            : tag == "SpaceObjectTrackingInterval" ? tactical::max_avoidance_tracking_interval
            : tactical::max_avoidance_tracking_windows;
        const auto effective = std::clamp<std::int64_t>(value.trunc_to_integer(), 1, maximum);
        if (value.raw() != effective * Fixed::scale) {
            core::LoadNote note;
            note.code = "EAWR-LOAD-0158";
            note.object = "motion rules";
            note.tag = std::string(tag);
            note.authored_value = motion_decimal(value);
            note.effective_value = std::to_string(effective);
            note.action = core::LoadAction::clamped;
            note.diagnostic_code = std::string(diagnostic_codes::motion);
            note.message = "avoidance rules are out of range";
            load_notes.append(std::move(note));
        }
        return static_cast<std::uint32_t>(effective);
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
        const FootprintTypeSelection selection(tables);
        const auto note_loser = [&](const auto& type) {
            const auto* winner = selection.find(type.id);
            core::LoadNote note;
            note.code = "EAWR-LOAD-0454";
            note.object = type.id;
            note.tag = "name";
            note.authored_value = type.id;
            note.effective_value = winner->name();
            note.action = core::LoadAction::skipped_object;
            note.diagnostic_code = std::string(diagnostic_codes::motion);
            note.message = "two footprints share type ID " + std::to_string(assets::object_type_crc(type.id));
            load_notes.append(std::move(note));
        };
        for (const auto& unit : tables.units) {
            const auto* winner = selection.find(unit.id);
            if (!selection.keeps(unit) && winner->obstacle) note_loser(unit);
        }
        for (const auto& obstacle : tables.obstacles)
            if (!selection.keeps(obstacle)) note_loser(obstacle);
        for (const auto& [id, candidate] : selection.winners()) {
            (void)id;
            if (const auto* unit = candidate.unit) {
                if (unit->kind == UnitKind::squadron || unit->kind == UnitKind::craft || unit->planetary_weapon) continue;
                if (space_layer(unit->movement.space_layer) == tactical::SpaceLayer::none) continue;
                if (auto footprint = footprint_of(unit->id, unit->movement.space_layer, unit->scale_factor, unit->footprint, load_notes)) {
                    footprint->asteroid_damage = unit->footprint.hazard.asteroid_damage;
                    footprint->locomotor = unit->locomotion;
                    table.footprints.push_back(*footprint);
                }
            } else {
                const auto& obstacle = *candidate.obstacle;
                if (space_layer(obstacle.space_layer) == tactical::SpaceLayer::none) continue;
                if (auto footprint = footprint_of(obstacle.id, obstacle.space_layer, obstacle.scale_factor, obstacle.footprint,
                        load_notes)) table.footprints.push_back(*footprint);
            }
        }
    }
    return std::nullopt;
}

void append_craft_motion_profiles(const UnitTables& tables, tactical::MotionTable& table,
    const std::optional<Fixed>& multiplier, std::optional<core::Diagnostic>& error) {
    const ContentTypeSelection type_selection(tables);
    const auto scaled = [&](const Fixed value, const std::string& what) {
        auto product = sim::math::multiply(value, *multiplier);
        if (!product) {
            if (!error) error = failure(what + ": " + product.error().message);
            return Fixed{};
        }
        return product.value();
    };
    auto& squadrons = table.squadrons;
    for (const auto& unit : tables.units) {
        if (!type_selection.keeps(unit)) continue;
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
}

void append_squadron_motion_profiles(const UnitTables& tables, tactical::MotionTable& table,
    const std::optional<Fixed>& multiplier, std::optional<core::Diagnostic>& error) {
    const ContentTypeSelection type_selection(tables);
    const auto scaled = [&](const Fixed value, const std::string& what) {
        auto product = sim::math::multiply(value, *multiplier);
        if (!product) {
            if (!error) error = failure(what + ": " + product.error().message);
            return Fixed{};
        }
        return product.value();
    };
    auto& squadrons = table.squadrons;
    const auto is_craft = [&squadrons](const tactical::TypeId id) {
        return std::any_of(squadrons.craft.begin(), squadrons.craft.end(),
            [id](const tactical::CraftProfile& craft) { return craft.type_id == id; });
    };
    for (const auto& unit : tables.units) {
        if (!type_selection.keeps(unit)) continue;
        const bool solo = unit.kind == UnitKind::craft && is_craft(assets::object_type_crc(unit.id));
        if (!solo && (unit.kind != UnitKind::squadron || unit.members.empty())) continue;
        tactical::SquadronProfile squadron;
        squadron.max_squad_size = unit.max_squad_size;
        squadron.type_id = assets::object_type_crc(unit.id);
        // WHE-SQ-02: a parentless fighter uses its own flight/order state; no extra entity is created.
        if (solo) {
            squadron.members.push_back(squadron.type_id);
            squadron.offsets.push_back(Vec3{});
        }
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
        squadron.autonomous_move_extension_vs_attacker = unit.autonomous_move_extension_vs_attacker.value_or(Fixed{});
        // WMV-44: only an explicit zero maximum returns the authored override directly.
        // An omitted maximum retains the debug build's nonzero type default and speed ratio.
        const auto& member_movement = solo ? unit.movement : unit.team_movement;
        const Fixed formation_deceleration = member_movement.deceleration.value_or(Fixed{});
        // WMV-44/E2675-01: the debug-build type maximum defaults to 0.25.
        const Fixed type_maximum = member_movement.max_speed.value_or(Fixed::from_raw(Fixed::scale / 4));
        squadron.formation_type_max_speed = scaled(type_maximum, unit.id + " formation Max_Speed");
        squadron.formation_deceleration = type_maximum.raw() == 0
            ? formation_deceleration : scaled(formation_deceleration, unit.id + " formation OverrideDeceleration");
        squadrons.squadrons.push_back(std::move(squadron));
    }
    std::sort(squadrons.craft.begin(), squadrons.craft.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    std::sort(squadrons.squadrons.begin(), squadrons.squadrons.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
}

void append_spawner_motion_profiles(const UnitTables& tables, tactical::MotionTable& table,
    const SpawnPolicy& spawn_policy, const bool& campaign,
    const Fixed& one, std::optional<core::Diagnostic>& error, core::LoadNoteBuffer& load_notes) {
    const ContentTypeSelection type_selection(tables);
    auto& squadrons = table.squadrons;
    const auto times = [&](const Fixed value, const Fixed factor, const std::string& what) {
        auto product = sim::math::multiply(value, factor);
        if (!product) {
            if (!error) error = failure(what + ": " + product.error().message);
            return Fixed{};
        }
        return product.value();
    };
    const auto has_squadron = [&squadrons](const tactical::TypeId id) {
        return std::any_of(squadrons.squadrons.begin(), squadrons.squadrons.end(),
            [id](const tactical::SquadronProfile& squadron) { return squadron.type_id == id; });
    };
    for (const auto& unit : tables.units) {
        if (!type_selection.keeps(unit)) continue;
        if (!unit.spawner || (unit.kind != UnitKind::ship && unit.kind != UnitKind::craft
                && unit.kind != UnitKind::station)) continue;
        tactical::SpawnerProfile spawner;
        spawner.type_id = assets::object_type_crc(unit.id);
        const auto entries = [&](const auto& starting, const auto& reserves) {
            std::vector<tactical::SpawnEntryProfile> result;
            for (const auto& entry : starting) {
                const auto id = assets::object_type_crc(entry.squadron);
                // WSL-04/16a: generic spawners accept ordinary ships in either tactical mode.
                // Marker-created skirmish stations suppress garrisons on the live object instead.
                // #5527: a spawn entry whose type is not a Squadron object is one single ship in either
                // flight model. A FIGHTER_LOCOMOTOR type that is not a Squadron (kind craft, e.g. BFC's
                // Raider_Corvette) has a solo squadron profile of its own, so has_squadron() is true for
                // it, but it still launches as a single ship placed by the free-spot search (WSL-16).
                const auto* spawned = type_selection.find(entry.squadron);
                const bool ship = spawned != nullptr
                    && ((spawned->kind == UnitKind::ship && !has_squadron(id)) || (spawned->kind == UnitKind::craft && has_squadron(id)));
                if (!has_squadron(id) && !ship) continue;
                tactical::SpawnEntryProfile row;
                row.squadron = id;
                row.ship = ship;
                row.arrival = ship && spawned->campaign_hyperspace;
                row.starting = entry.count;
                row.reserve = 0;
                if (campaign || spawn_policy.mode == SpawnPolicy::Mode::skirmish) {
                    const auto reserve = std::find_if(reserves.begin(), reserves.end(), [&](const auto& candidate) {
                        return assets::object_type_crc(candidate.squadron) == id;
                    });
                    if (reserve != reserves.end()) row.reserve = reserve->count;
                }
                result.push_back(row);
            }
            return result;
        };
        spawner.entries = entries(unit.spawner->starting, unit.spawner->reserves);
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
        spawner.mobile = unit.kind != UnitKind::station;
        // WSL-26: the registered station role is DUMMY_STAR_BASE in either
        // behavior list; STARBASE is not a registered behavior alias.
        const auto starbase = [](const auto& names) {
            return std::any_of(names.begin(), names.end(), [](const auto& name) { return detail::iequals(name, "DUMMY_STAR_BASE"); });
        };
        spawner.starbase = starbase(unit.footprint.hazard.behavior) || starbase(unit.footprint.hazard.space_behavior);
        const bool base_present = !spawner.entries.empty() || (spawner.starbase && !spawner.bays.empty());
        bool campaign_launches = false;
        const auto select = [&](const auto& tiers, const auto& fallback, const unsigned selected_tech) -> const std::vector<SpawnEntry>& {
                const bool authored = std::any_of(tiers.begin(), tiers.end(), [](const auto& rows) { return !rows.empty(); });
                if (!authored) return fallback; // explicit synthetic Tech0 profiles
                for (auto tech = static_cast<int>(selected_tech); tech >= 0; --tech)
                    if (!tiers[static_cast<std::size_t>(tech)].empty()) return tiers[static_cast<std::size_t>(tech)];
                static const std::vector<SpawnEntry> empty;
                return empty;
        };
        if (spawn_policy.mode == SpawnPolicy::Mode::skirmish) {
            std::array<tactical::SpawnerProfile, 7> tiers;
            bool any = false;
            for (unsigned tech = 0; tech < 6; ++tech) {
                tiers[tech] = spawner;
                tiers[tech].entries = entries(select(unit.spawner->campaign_starting, unit.spawner->starting, tech),
                    select(unit.spawner->campaign_reserves, unit.spawner->reserves, tech));
                any = any || !tiers[tech].entries.empty();
            }
            tiers[6] = spawner;
            tiers[6].entries.clear();
            if (any) squadrons.tech_spawners.emplace(spawner.type_id, std::move(tiers));
            campaign_launches = any && !spawner.bays.empty();
        }
        if (campaign) for (const auto& owner : spawn_policy.owners) {
            auto selected = spawner;
            selected.entries = entries(select(unit.spawner->campaign_starting, unit.spawner->starting, owner.tech),
                select(unit.spawner->campaign_reserves, unit.spawner->reserves, owner.tech));
            if (!selected.entries.empty() && !spawner.bays.empty()) campaign_launches = true;
            // An empty selected tier overrides a populated base profile as well.
            if (base_present || !selected.entries.empty())
                squadrons.campaign_spawners.emplace(std::pair{selected.type_id, owner.owner}, std::move(selected));
        }
        // #5559: a hangar that authors a starting complement but can never launch it (every entry dropped in
        // every mode, or no fighter bay on a ship) is reported once at load, instead of failing silently in a
        // battle. A campaign tier that resolves does launch (find_spawner picks it), so it is not reported.
        if (!unit.spawner->starting.empty() && !spawner.starbase
            && ((spawner.entries.empty() && !campaign_launches) || spawner.bays.empty())) {
            core::LoadNote note;
            note.code = "EAWR-LOAD-0456";
            note.object = unit.id;
            note.tag = "Starting_Spawned_Units_Tech_0";
            note.authored_value = unit.spawner->starting.front().squadron;
            note.effective_value = "no launch";
            note.action = core::LoadAction::runtime_degraded;
            note.diagnostic_code = std::string(diagnostic_codes::motion);
            note.message = spawner.entries.empty() && !spawner.bays.empty() ? "no authored starting squadron resolves to a flyable type; the hangar launches nothing"
                                                   : "the type has a starting complement but no fighter bay hardpoint; the hangar launches nothing";
            load_notes.append(std::move(note));
        }
        if (base_present || campaign_launches) squadrons.spawners.push_back(std::move(spawner));
    }
    std::sort(squadrons.spawners.begin(), squadrons.spawners.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
}

} // namespace

core::Result<tactical::Footprint> detail::space_footprint(const std::string& id,
    const std::optional<Fixed>& scale_factor, const SpaceFootprint& data, core::LoadNoteBuffer& load_notes) {
    const auto footprint = footprint_of(id, {}, scale_factor, data, load_notes);
    return core::Result<tactical::Footprint>::success(*footprint);
}

core::Result<tactical::MotionTable> motion_table(const UnitTables& tables, const SpawnPolicy& spawn_policy,
    std::vector<core::Diagnostic>* const notes, core::LoadNoteBuffer* load_notes) {
    core::LoadNoteBuffer load;
    using Result = core::Result<tactical::MotionTable>;
    const ContentTypeSelection type_selection(tables);
    type_selection.emit_notes(tables, diagnostic_codes::motion, load_notes);
    const bool campaign = spawn_policy.mode == SpawnPolicy::Mode::campaign;
    for (const auto& owner : spawn_policy.owners)
        if (owner.tech >= Spawner{}.campaign_starting.size())
            return Result::failure(failure("garrison owner tech outside 0..5"));
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    const std::optional<Fixed> multiplier = motion_scalar(tables, domain_tag(tables.domain, DomainTag::speed_multiplier), load);
    auto rotations_value = motion_scalar(tables, "MaxRotationsSpace", load);
    if (rotations_value < Fixed::from_raw(2 * Fixed::scale)
        || rotations_value > Fixed::from_raw(tactical::max_avoidance_rotations * Fixed::scale)) {
        core::LoadNote note;
        note.code = "EAWR-LOAD-0151";
        note.object = "motion rules";
        note.tag = "MaxRotationsSpace";
        note.authored_value = motion_decimal(rotations_value);
        rotations_value = Fixed::from_raw(std::clamp(rotations_value.raw(), 2 * Fixed::scale,
            tactical::max_avoidance_rotations * Fixed::scale));
        note.effective_value = motion_decimal(rotations_value);
        note.action = core::LoadAction::clamped;
        note.diagnostic_code = std::string(diagnostic_codes::motion);
        note.message = "the arc angle must be in (0, 180] degrees";
        load.append(std::move(note));
    }
    const std::optional<Fixed> rotations = rotations_value;
    const auto expansion = motion_scalar(tables, "XYExpansionDistanceSpace", load);
    const std::optional<Fixed> corvette = motion_scalar(tables, "TurnInPlaceSlowdownCorvette", load);
    const std::optional<Fixed> frigate = motion_scalar(tables, "TurnInPlaceSlowdownFrigate", load);
    const std::optional<Fixed> capital = motion_scalar(tables, "TurnInPlaceSlowdownCapital", load);
    std::optional<core::Diagnostic> error;

    tactical::MotionTable table;
    auto arc = sim::math::divide(Fixed::from_raw(360 * Fixed::scale), *rotations);
    if (!arc) return Result::failure(failure("MaxRotationsSpace: " + arc.error().message));
    table.rules.arc_degrees = arc.value();
    table.rules.expansion_distance = expansion;
    // OR-06, OR-14: authored intervals use the retail integer conversion (SKI-26, LSA-P1),
    // then degrade to the supported frame range before unsigned storage.
    if (const std::optional<Fixed> interval = motion_scalar(tables, "MovementReevaluationFrameCount", load)) {
        const auto effective = std::clamp<std::int64_t>(interval->trunc_to_integer(), 1, tactical::max_motion_reevaluation_frames);
        table.rules.reevaluation_frames = static_cast<std::uint32_t>(effective);
        if (interval->raw() != effective * Fixed::scale) {
            core::LoadNote note;
            note.code = "EAWR-LOAD-0152";
            note.object = "motion rules";
            note.tag = "MovementReevaluationFrameCount";
            note.authored_value = motion_decimal(*interval);
            note.effective_value = std::to_string(effective);
            note.action = core::LoadAction::clamped;
            note.diagnostic_code = std::string(diagnostic_codes::motion);
            note.message = "MovementReevaluationFrameCount must be a whole number of frames, 1 to 65536";
            load.append(std::move(note));
        }
    }
    if (const auto max_waypoints = scalar("MaxWaypointsPerPath")) {
        // HCB-58 (#4992): FoC reads the constant with atoi into a 32-bit int and the queue compares the count
        // against it as unsigned, so a negative authoring (-1) wraps to 4294967295, no practical limit,
        // and 0 refuses every queued point. A value with a fraction keeps its whole part.
        const auto whole = std::clamp<std::int64_t>(max_waypoints->trunc_to_integer(), INT32_MIN, INT32_MAX);
        table.rules.max_waypoints = static_cast<std::uint32_t>(static_cast<std::int32_t>(whole));
    }
    if (const std::optional<Fixed> guard = motion_scalar(tables, "Space_Guard_Range", load)) {
        table.rules.guard_range = *guard;
    }
    append_unit_motion_profiles(tables, table, multiplier, corvette, frigate, capital, load);
    if (auto footprint_error = append_avoidance_footprints(tables, table, rotations, load))
        return Result::failure(std::move(*footprint_error));
    // #75: craft flight, squadron types and hangars (docs/behaviour/space-fighters.md FL, FM).
    table.nebula_disable_seconds = motion_scalar(tables, "Nebula_Ability_Disable_Time", load);
    for (const auto& unit : tables.units) {
        if (!type_selection.keeps(unit)) continue;
        if (unit.footprint.hazard.nebula_service) table.nebula_service_types.push_back(assets::object_type_crc(unit.id));
    }
    std::sort(table.nebula_service_types.begin(), table.nebula_service_types.end());
    auto& squadrons = table.squadrons;
    // FO-11 (#599): a squadron's lane steer on a group move; without the constants it is off.
    squadrons.side_error_min = scalar("FormationMinimumSideError").value_or(Fixed{});
    squadrons.side_error_max = scalar("FormationMaximumSideError").value_or(Fixed{});
    const Fixed one = Fixed::from_raw(Fixed::scale);
    append_craft_motion_profiles(tables, table, multiplier, error);
    append_squadron_motion_profiles(tables, table, multiplier, error);
    append_spawner_motion_profiles(tables, table, spawn_policy, campaign, one, error, load);

    if (error) return Result::failure(*error);
    std::sort(table.profiles.begin(), table.profiles.end(),
              [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    for (std::size_t index = 1; index < table.profiles.size(); ++index) {
        if (table.profiles[index].type_id == table.profiles[index - 1].type_id) {
            return Result::failure(failure("two unit types share type ID " + std::to_string(table.profiles[index].type_id)));
        }
    }
    core::LoadNoteBuffer normalized_notes;
    auto normalized = tactical::prepare_motion(table, normalized_notes);
    if (!normalized) return Result::failure(failure(normalized.error().message));
    table = std::move(normalized.value());
    core::LoadNoteCollector collector;
    collector.merge(normalized_notes);
    collector.merge(load);
    for (auto note : collector.snapshot()) {
        for (const auto& unit : tables.units) {
            if (!type_selection.keeps(unit)) continue;
            if (std::to_string(assets::object_type_crc(unit.id)) == note.object) note.object = unit.id;
            if (unit.id != note.object) continue;
            if (note.tag == "TurnInPlaceSlowdown") {
                const auto layer = space_layer(unit.movement.space_layer);
                note.tag = layer == tactical::SpaceLayer::corvette ? "TurnInPlaceSlowdownCorvette"
                    : layer == tactical::SpaceLayer::frigate ? "TurnInPlaceSlowdownFrigate" : "TurnInPlaceSlowdownCapital";
                for (const auto& constant : tables.constants.scalars)
                    if (constant.tag == note.tag) note.authored_value = constant.text;
            }
            const auto* profile = table.find(assets::object_type_crc(unit.id));
            if (profile && note.code == "EAWR-LOAD-0155") {
                const auto& movement = unit.movement;
                if (note.tag == "Max_Speed") {
                    note.authored_value = motion_decimal(*movement.max_speed);
                    note.effective_value = motion_decimal(profile->max_speed);
                } else if (note.tag == "Max_Rate_Of_Turn") {
                    note.authored_value = motion_decimal(*movement.max_rate_of_turn);
                    note.effective_value = motion_decimal(profile->rate_of_turn);
                } else if (note.tag == "OverrideAcceleration") {
                    note.authored_value = motion_decimal(movement.acceleration.value_or(*movement.max_speed));
                    note.effective_value = motion_decimal(profile->acceleration);
                } else if (note.tag == "OverrideDeceleration") {
                    note.authored_value = motion_decimal(movement.deceleration.value_or(*movement.max_speed));
                    note.effective_value = motion_decimal(profile->deceleration);
                } else if (note.tag == "Max_Rate_Of_Roll") {
                    note.authored_value = motion_decimal(movement.max_rate_of_roll.value_or(Fixed::from_raw(2 * Fixed::scale)));
                    note.effective_value = motion_decimal(profile->roll_rate);
                }
            }
        }
        for (const auto& obstacle : tables.obstacles)
            if (std::to_string(assets::object_type_crc(obstacle.id)) == note.object) note.object = obstacle.id;
        for (const auto& constant : tables.constants.scalars)
            if (note.object == "motion rules" && constant.tag == note.tag) note.authored_value = constant.text;
        if (note.tag == "MovementReevaluationFrameCount")
            note.effective_value = std::to_string(table.rules.reevaluation_frames);
        if (load_notes) load_notes->append(std::move(note));
    }
    if (notes) {
        for (const auto& footprint : table.footprints) {
            if (footprint.layer != tactical::SpaceLayer::static_object || footprint.locomotor
                || table.find(footprint.type_id) != nullptr) continue;
            auto note = failure("static footprint " + std::to_string(footprint.type_id)
                + ": no locomotor; destination-search rings are not built or validated");
            note.severity = core::Severity::info;
            notes->push_back(std::move(note));
        }
    }
    return Result::success(std::move(table));
}

} // namespace eawr::units
