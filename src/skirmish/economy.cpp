#include "eawr/skirmish/start.hpp"

#include "eawr/skirmish/placement.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <string>
#include <utility>

// #530 (docs/behaviour/space-purchasing.md): the economy content of a skirmish from its start and
// the unit tables. Nothing here is per faction or per station level: a station's menu is its own
// type's data (#540 swaps the type), and income is a list of sources (#541 adds mining).
namespace eawr::skirmish {

namespace {

namespace tactical = sim::tactical;
using sim::math::Fixed;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    return detail::error(diagnostic_codes::input, "economy rules: " + std::move(message));
}

// PU-13: whole frames of `seconds` at 30 frames a second, rounded to nearest.
[[nodiscard]] std::uint32_t frames_of(const Fixed seconds) {
    const auto raw = seconds.raw() * 30;
    return static_cast<std::uint32_t>(std::max<std::int64_t>(0, (raw + Fixed::scale / 2) / Fixed::scale));
}

// PU-02: `value` per `interval_seconds`, paid every frame.
[[nodiscard]] core::Result<Fixed> per_frame(const Fixed value, const Fixed interval_seconds) {
    const auto frames = sim::math::multiply(interval_seconds, Fixed::from_raw(30 * Fixed::scale));
    if (!frames) return core::Result<Fixed>::failure(frames.error());
    return sim::math::divide(value, frames.value());
}

} // namespace

core::Result<tactical::EconomyRules> economy_rules(
    const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables) {
    using Result = core::Result<tactical::EconomyRules>;
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    const auto credits = scalar("MP_Default_Credits");
    const auto build_multiplier = scalar("Tactical_Build_Time_Multiplier");
    const auto vulnerability = scalar("Space_Elevated_Vulnerability_Factor");
    const auto vulnerable_seconds = scalar("Space_Elevated_Vulnerability_Duration");
    const auto collision_distance = scalar("Space_Reinforcement_Collision_Check_Distance");
    if (!credits || !build_multiplier || !vulnerability || !vulnerable_seconds || !collision_distance) {
        return Result::failure(failure("needs MP_Default_Credits, Tactical_Build_Time_Multiplier and the "
                                       "Space_Elevated_Vulnerability constants"));
    }
    tactical::EconomyRules rules;
    rules.collision_distance = *collision_distance; // WR-25

    // PU-01, PU-21, PU-34: the lobby players.
    for (const auto& player : start.players) {
        if (!player.lobby) continue;
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const StartFaction& entry) { return detail::iequals(entry.name, player.faction); });
        if (faction == inputs.factions.end() || !faction->space_unit_cap) {
            return Result::failure(failure("faction " + player.faction + " has no Space_Tactical_Unit_Cap"));
        }
        const auto marker = std::find_if(start.markers.begin(), start.markers.end(), [&](const StartMarker& entry) {
            return entry.use == MarkerUse::spawn && entry.player == player.player.player_id;
        });
        if (marker == start.markers.end()) {
            return Result::failure(failure("player " + std::to_string(player.player.player_id) + " has no spawn marker"));
        }
        rules.players.push_back(tactical::EconomyPlayer{
            player.player.player_id, *credits, *faction->space_unit_cap, !player.human, marker->yaw_degrees});
    }
    std::sort(rules.players.begin(), rules.players.end(),
        [](const tactical::EconomyPlayer& left, const tactical::EconomyPlayer& right) { return left.player < right.player; });

    for (const auto& type : tables.units) {
        const auto& production = type.production;
        // PU-10 to PU-13, PU-20: one menu per faction group of a station's list.
        for (const auto& group : production.buildable) {
            tactical::StationMenu menu;
            menu.station = type_id(type.id);
            menu.faction = faction_id(group.faction);
            for (const auto& name : group.types) {
                tactical::BuildOption option;
                option.type = type_id(name);
                const auto* built = tables.find(name);
                if (built == nullptr) {
                    // Not a table type: an upgrade object, listed but not built in M2 (#540).
                    option.kind = tactical::BuildKind::upgrade;
                    menu.options.push_back(option);
                    continue;
                }
                const auto& costs = built->production;
                option.queue = detail::iequals(costs.production_queue, "Tactical_Upgrades") ? tactical::BuildQueue::upgrades
                                                                                              : tactical::BuildQueue::units;
                option.price = costs.build_cost_multiplayer.value_or(Fixed{});
                if (costs.build_time_seconds) {
                    const auto seconds = sim::math::multiply(*costs.build_time_seconds, *build_multiplier);
                    if (!seconds) return Result::failure(seconds.error());
                    option.build_frames = frames_of(seconds.value());
                    option.ai_build_frames = option.build_frames; // SK-42: Normal's Space_Build_Time_Multiplier is 1
                }
                option.population = costs.population_value.value_or(0);
                option.available = option.price.raw() > 0 && option.build_frames > 0;
                menu.options.push_back(option);
            }
            rules.menus.push_back(std::move(menu));
        }
        // PU-02 to PU-04: the stream and the bonuses that target it.
        for (const auto& stream : production.income) {
            if (!stream.base_value || !stream.interval_seconds || stream.interval_seconds->raw() <= 0) continue;
            tactical::IncomeProfile income;
            income.source = type_id(type.id);
            auto base = per_frame(*stream.base_value, *stream.interval_seconds);
            if (!base) return Result::failure(base.error());
            income.per_frame = base.value();
            for (const auto& bonus : production.income_bonuses) {
                if (!bonus.additive || (!bonus.target_source.empty() && !detail::iequals(bonus.target_source, type.id))) {
                    continue;
                }
                const auto hardpoint = std::find_if(type.hardpoints.begin(), type.hardpoints.end(),
                    [&](const units::Hardpoint& entry) { return detail::iequals(entry.special_ability_name, bonus.name); });
                if (hardpoint == type.hardpoints.end()) continue; // enabled by nothing M2 has
                auto amount = per_frame(*bonus.additive, *stream.interval_seconds);
                if (!amount) return Result::failure(amount.error());
                income.bonuses.push_back(tactical::IncomeBonus{
                    amount.value(), static_cast<std::uint32_t>(hardpoint - type.hardpoints.begin())});
            }
            rules.income.push_back(std::move(income));
            break; // one stream per source type
        }
        // PU-31.
        if (production.reinforcement_prevention_radius && production.reinforcement_prevention_radius->raw() > 0) {
            rules.prevention.push_back(tactical::PreventionProfile{type_id(type.id), *production.reinforcement_prevention_radius});
        }
    }
    // PL-02, LZ-01: every unit and map object type's placement box and height, for a bought unit's
    // arrival. A super capital has no box (it is placed on the point and never blocks).
    const auto footprint = [&](const std::string& name, const std::optional<PlacementBox>& box,
                               const std::string& layer, const std::optional<Fixed>& height) {
        const auto id = type_id(name);
        if (std::any_of(rules.footprints.begin(), rules.footprints.end(),
                [id](const tactical::FootprintProfile& entry) { return entry.type == id; })) {
            return;
        }
        rules.footprints.push_back(tactical::FootprintProfile{
            id, detail::iequals(layer, "SuperCapital") ? std::nullopt : box, height.value_or(Fixed{})});
    };
    for (const auto& type : tables.units) {
        footprint(type.id, placement_box(type), type.movement.space_layer, type.movement.layer_z_adjust);
    }
    for (const auto& obstacle : tables.obstacles) {
        footprint(obstacle.id, placement_box(obstacle), obstacle.space_layer, std::nullopt);
    }
    std::sort(rules.footprints.begin(), rules.footprints.end(),
        [](const tactical::FootprintProfile& left, const tactical::FootprintProfile& right) { return left.type < right.type; });
    std::sort(rules.menus.begin(), rules.menus.end(), [](const tactical::StationMenu& left, const tactical::StationMenu& right) {
        return std::pair{left.station, left.faction} < std::pair{right.station, right.faction};
    });
    std::sort(rules.income.begin(), rules.income.end(),
        [](const tactical::IncomeProfile& left, const tactical::IncomeProfile& right) { return left.source < right.source; });
    std::sort(rules.prevention.begin(), rules.prevention.end(),
        [](const tactical::PreventionProfile& left, const tactical::PreventionProfile& right) { return left.type < right.type; });

    // PU-31: the map's declared extents about the origin, as the fog grid spans them.
    if (inputs.map_extents) {
        const auto width = detail::q24_from_binary32_bits(inputs.map_extents->first);
        const auto height = detail::q24_from_binary32_bits(inputs.map_extents->second);
        if (width && height && *width > 0 && *height > 0) {
            rules.bounds = std::array<Fixed, 4>{Fixed::from_raw(-(*width / 2)), Fixed::from_raw(-(*height / 2)),
                Fixed::from_raw(*width / 2), Fixed::from_raw(*height / 2)};
        }
    }
    rules.max_queue = 5; // PU-14: FoC's built-in Max_Build_Queue; its data does not set it
    rules.vulnerability = *vulnerability;
    rules.vulnerability_frames = frames_of(*vulnerable_seconds);
    if (auto valid = tactical::validate_economy(rules, start.setup.players); !valid) return Result::failure(valid.error());
    return Result::success(std::move(rules));
}

} // namespace eawr::skirmish
