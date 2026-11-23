#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/tactical/instance.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

namespace eawr::presentation::ui {

using BattleRgb = std::array<std::uint8_t, 3>;

[[nodiscard]] inline std::optional<BattleRgb> gc_model_colour(const bool participant_units,
        const std::optional<BattleRgb> faction, const std::optional<BattleRgb> participant) {
    return participant_units && participant ? participant : faction;
}

// Read the live controller every frame: a gift changes the seat without changing its owner.
[[nodiscard]] inline std::optional<BattleRgb> gc_instance_model_colour(
    const bool participant_units, const sim::tactical::TacticalInstance& instance,
    const std::optional<BattleRgb> faction, const std::span<const sim::galactic::Seat> seats,
    const std::map<sim::SeatId, BattleRgb>& colours) {
    if (!participant_units) return faction;
    auto seat = instance.controller_seat;
    if (seat.value == 0) {
        const auto fallback = std::find_if(seats.begin(), seats.end(), [&](const auto& row) {
            return row.player == instance.owner && row.active && row.kind == sim::SeatKind::human;
        });
        if (fallback != seats.end()) seat = fallback->seat_id;
    }
    const auto row = std::find_if(seats.begin(), seats.end(), [&](const auto& value) { return value.seat_id == seat; });
    if (row != seats.end() && row->kind == sim::SeatKind::human) {
        if (const auto colour = colours.find(seat); colour != colours.end()) return colour->second;
    }
    return faction;
}

// The inputs are the saved galactic seat and side tables, in their canonical order.
// An absent result leaves AI, script, and unknown seats on their faction colour.
[[nodiscard]] inline std::optional<BattleRgb> gc_participant_colour(
    const sim::SeatId seat, const std::span<const sim::galactic::Seat> seats,
    const std::span<const sim::galactic::TeamLead> teams,
    const std::span<const BattleRgb> palette, const bool ally_shades) {
    const auto row = std::find_if(seats.begin(), seats.end(), [seat](const auto& value) { return value.seat_id == seat; });
    if (row == seats.end() || row->kind != sim::SeatKind::human || row->human_side_id == 0 || palette.empty()) return {};
    const auto side = std::find_if(teams.begin(), teams.end(), [&](const auto& value) { return value.side == row->human_side_id; });
    if (side == teams.end()) return {};
    const auto rank = static_cast<std::size_t>(side - teams.begin());
    std::size_t ally_rank{};
    std::size_t extra_rank{};
    for (const auto& candidate : seats) {
        if (candidate.kind != sim::SeatKind::human || candidate.human_side_id == 0 || !(candidate.seat_id < seat)) continue;
        if (candidate.human_side_id == row->human_side_id) ++ally_rank;
        const auto previous = std::count_if(seats.begin(), seats.end(), [&](const auto& other) {
            return other.kind == sim::SeatKind::human && other.human_side_id == candidate.human_side_id
                && other.seat_id < candidate.seat_id;
        });
        if (previous != 0) ++extra_rank;
    }
    const auto index = !ally_shades && ally_rank != 0 ? teams.size() + extra_rank : rank;
    if (index >= palette.size()) return {};
    auto colour = palette[index];
    if (ally_shades && ally_rank != 0) {
        // Move toward the more distant endpoint so even a white or black base has distinct shades.
        const auto step = std::min<std::size_t>(ally_rank, 7);
        const auto total = static_cast<unsigned>(colour[0]) + colour[1] + colour[2];
        for (auto& channel : colour) {
            channel = total > 382U ? static_cast<std::uint8_t>(channel - channel * step / 9U)
                                   : static_cast<std::uint8_t>(channel + (255U - channel) * step / 9U);
        }
    }
    return colour;
}

} // namespace eawr::presentation::ui
