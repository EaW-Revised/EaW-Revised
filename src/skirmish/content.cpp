#include "eawr/skirmish/start.hpp"

#include "skirmish_internal.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace eawr::skirmish {

core::Result<SessionContent> session_content(
    const units::UnitTables& tables, const std::span<const sim::tactical::PlayerId> humans) {
    using ContentResult = core::Result<SessionContent>;
    SessionContent content;
    content.sensors = sensor_table(tables);
    auto durability = units::durability_table(tables);
    if (!durability) return ContentResult::failure(durability.error());
    content.durability = std::move(durability).value();
    auto motion = units::motion_table(tables);
    if (!motion) return ContentResult::failure(motion.error());
    content.motion = std::move(motion).value();
    auto combat = units::combat_table(tables);
    if (!combat) return ContentResult::failure(combat.error());
    content.combat = std::move(combat).value();
    auto abilities = units::ability_table(tables, humans);
    if (!abilities) return ContentResult::failure(abilities.error());
    content.abilities = std::move(abilities).value();
    return ContentResult::success(std::move(content));
}

namespace detail {

std::optional<std::int64_t> q24_from_binary32_bits(const std::uint32_t bits) noexcept {
    const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
    if ((bits >> 31U) != 0U || exponent == 0U || exponent == 0xFFU) return std::nullopt;
    const std::int64_t mantissa = static_cast<std::int64_t>((bits & 0x7FFFFFU) | 0x800000U);
    // value = mantissa * 2^(exponent - 150); raw = value * 2^24.
    const int shift = static_cast<int>(exponent) - 150 + static_cast<int>(sim::math::Fixed::fractional_bits);
    if (shift >= 0) {
        if (shift > 62 - 24) return std::nullopt;
        return mantissa << shift;
    }
    return shift <= -24 ? std::int64_t{0} : mantissa >> -shift;
}

} // namespace detail

core::Result<std::optional<sim::tactical::FogRules>> fog_rules(const StartInputs& inputs) {
    using FogResult = core::Result<std::optional<sim::tactical::FogRules>>;
    if (!inputs.map_extents) return FogResult::success(std::nullopt);
    const auto width = detail::q24_from_binary32_bits(inputs.map_extents->first);
    const auto height = detail::q24_from_binary32_bits(inputs.map_extents->second);
    const auto cell = inputs.fog_cell_size.raw();
    if (!width || !height || *width <= 0 || *height <= 0 || cell <= 0) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::input);
        diagnostic.message = "the map's declared extents (binary32 bits " + std::to_string(inputs.map_extents->first) + ", "
            + std::to_string(inputs.map_extents->second) + ") or the fog cell size (Q24 raw " + std::to_string(cell)
            + ") are not positive";
        return FogResult::failure(std::move(diagnostic));
    }
    // V-18: cells rounded up and at most 512 a side, as FoC sizes its fog grid.
    const auto cells = [&](const std::int64_t extent) {
        return static_cast<std::uint32_t>(std::min<std::int64_t>((extent + cell - 1) / cell, 512));
    };
    auto step = sim::tactical::fog_ramp_down_step(inputs.fog_regrow_seconds);
    if (!step) return FogResult::failure(step.error());
    sim::tactical::FogRules rules;
    rules.map_left = sim::math::Fixed::from_raw(-(*width / 2));
    rules.map_top = sim::math::Fixed::from_raw(*height / 2);
    rules.cell_size = inputs.fog_cell_size;
    rules.cells_wide = cells(*width);
    rules.cells_tall = cells(*height);
    rules.ramp_down_step = step.value();
    if (auto valid = sim::tactical::validate_fog_rules(rules); !valid) return FogResult::failure(valid.error());
    return FogResult::success(rules);
}

sim::tactical::VictoryRules victory_rules(const sim::tactical::TacticalSetup& setup, const units::UnitTables& tables,
    const std::span<const sim::tactical::PlayerId> humans) {
    sim::tactical::VictoryRules rules;
    rules.condition = sim::tactical::VictoryCondition::enemy_starbase_destroyed;
    for (const auto& type : tables.units) {
        if (type.kind == units::UnitKind::station && type.victory_relevant) rules.starbase_types.push_back(type_id(type.id));
    }
    std::sort(rules.starbase_types.begin(), rules.starbase_types.end());
    rules.starbase_types.erase(std::unique(rules.starbase_types.begin(), rules.starbase_types.end()),
        rules.starbase_types.end());
    for (const auto& player : setup.players) {
        if (!player.commandable()) continue;
        rules.contenders.push_back(player.player_id);
        if (std::find(humans.begin(), humans.end(), player.player_id) != humans.end()) {
            rules.humans.push_back(player.player_id);
        }
    }
    std::sort(rules.contenders.begin(), rules.contenders.end());
    std::sort(rules.humans.begin(), rules.humans.end());
    return rules;
}

sim::tactical::VictoryRules victory_rules(const SkirmishStart& start, const units::UnitTables& tables) {
    std::vector<sim::tactical::PlayerId> humans;
    for (const auto& player : start.players) {
        if (player.lobby && player.human) humans.push_back(player.player.player_id);
    }
    return victory_rules(start.setup, tables, humans);
}

std::vector<sim::tactical::PlayerId> human_slots(const Fixture& fixture) {
    std::vector<sim::tactical::PlayerId> humans;
    for (const auto& slot : fixture.slots) {
        if (slot.human) humans.push_back(slot.slot);
    }
    return humans;
}

} // namespace eawr::skirmish
