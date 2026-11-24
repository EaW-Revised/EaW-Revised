#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/tactical/instance.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <vector>

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
        else {
            // No human seat owns the faction: an allied AI seat of that faction stands in (its units are mates).
            const auto ai = std::find_if(seats.begin(), seats.end(), [&](const auto& row) {
                return row.player == instance.owner && row.active && row.kind == sim::SeatKind::ai;
            });
            if (ai != seats.end()) seat = ai->seat_id;
        }
    }
    if (const auto colour = colours.find(seat); colour != colours.end()) return colour->second;
    return faction;
}

// Owner rule (#8355, 11-24): the local seat and every enemy keep their faction's own (colour-off) look, so they get no
// entry; the team mates on the local side get tinted variants of the local faction's colour, a different one per mate
// (shades by default; with `ally_shades` off a palette colour of their own). AI seats the caller names in `ai_mates`
// (allied AI) are mates too and take the shades after the human mates. Script and unknown seats get no entry.
// The inputs are the saved galactic seat and side tables in their canonical order, so every peer derives the same table.
// The one switch for the enemy rule: true would give each enemy side a palette colour instead (not the owner's choice).
inline constexpr bool gc_enemy_humans_use_palette = false;

namespace detail {
[[nodiscard]] inline bool near_colour(const BattleRgb& a, const BattleRgb& b) {
    int distance = 0;
    for (std::size_t i = 0; i < 3; ++i) distance += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return distance < 90;
}

// Ranks 1-3 move toward the far endpoint (white for a dark base) in clearly separated steps, ranks 4-6 toward the
// near one, rank 7 almost all the way; so a mate is tellable from the base and from the other mates, whatever the base.
[[nodiscard]] inline BattleRgb gc_shade(BattleRgb colour, const std::size_t rank) {
    const auto total = static_cast<unsigned>(colour[0]) + colour[1] + colour[2];
    const bool light = total > 382U;
    const bool far_side = rank <= 3 || rank >= 7;
    const bool toward_white = light ? !far_side : far_side;
    const unsigned percent = rank <= 3 ? 30U + 25U * static_cast<unsigned>(rank - 1)
        : rank <= 6 ? 35U + 25U * static_cast<unsigned>(rank - 4) : 92U;
    for (auto& channel : colour) {
        channel = toward_white ? static_cast<std::uint8_t>(channel + (255U - channel) * percent / 100U)
                               : static_cast<std::uint8_t>(channel - channel * percent / 100U);
    }
    return colour;
}
} // namespace detail

[[nodiscard]] inline std::optional<BattleRgb> gc_participant_colour(
    const sim::SeatId seat, const std::span<const sim::galactic::Seat> seats,
    const std::span<const sim::galactic::TeamLead> teams,
    const std::span<const BattleRgb> palette, const bool ally_shades,
    const sim::SeatId local_seat = {}, const std::optional<BattleRgb> local_faction = {},
    const std::span<const sim::SeatId> ai_mates = {}) {
    const auto human = [](const auto& value) { return value.kind == sim::SeatKind::human && value.human_side_id != 0; };
    const auto row = std::find_if(seats.begin(), seats.end(), [seat](const auto& value) { return value.seat_id == seat; });
    if (row == seats.end()) return {};
    const auto side_known = [&](const auto side) {
        return std::any_of(teams.begin(), teams.end(), [side](const auto& team) { return team.side == side; });
    };
    const auto local = std::find_if(seats.begin(), seats.end(), [&](const auto& value) {
        return local_seat.value != 0 && value.seat_id == local_seat && human(value); });
    const auto own_side = local == seats.end() ? decltype(row->human_side_id){} : local->human_side_id;
    const bool own_colour = own_side != 0 && local_faction.has_value();
    if (row->kind == sim::SeatKind::ai) {
        const auto found = std::find(ai_mates.begin(), ai_mates.end(), seat);
        if (!own_colour || found == ai_mates.end()) return {};
        const auto human_mates = static_cast<std::size_t>(std::count_if(seats.begin(), seats.end(), [&](const auto& other) {
            return human(other) && other.human_side_id == own_side && other.seat_id != local_seat; }));
        return detail::gc_shade(*local_faction, human_mates + 1 + static_cast<std::size_t>(found - ai_mates.begin()));
    }
    if (!human(*row) || !side_known(row->human_side_id)) return {};
    const auto leads = [&](const auto& value) {
        // The seat whose colour is the side's own: the local seat on its side, else the lowest seat of the side.
        if (own_colour && value.human_side_id == own_side) return value.seat_id == local_seat;
        return std::none_of(seats.begin(), seats.end(), [&](const auto& other) {
            return human(other) && other.human_side_id == value.human_side_id && other.seat_id < value.seat_id; });
    };
    // The palette the other sides and the extra seats draw from, without the colour the local faction already uses.
    std::vector<BattleRgb> pool;
    for (const auto& entry : palette) if (!own_colour || !detail::near_colour(entry, *local_faction)) pool.push_back(entry);
    std::size_t enemy_sides = 0;
    std::size_t side_index = 0;
    for (const auto& team : teams) {
        if (own_colour && team.side == own_side) continue;
        if (team.side == row->human_side_id) side_index = enemy_sides;
        ++enemy_sides;
    }
    std::optional<BattleRgb> base;
    if (own_colour && row->human_side_id == own_side) base = local_faction;
    else if (gc_enemy_humans_use_palette && side_index < pool.size()) base = pool[side_index];
    if (!base) return {};
    std::size_t rank = 0; // 1-based among the side's seats that are not its lead
    std::size_t extra = 0; // among all such seats of every side, in seat order
    if (!leads(*row)) {
        for (const auto& other : seats) {
            if (!human(other) || !side_known(other.human_side_id) || leads(other)) continue;
            if (other.seat_id < seat) {
                ++extra;
                if (other.human_side_id == row->human_side_id) ++rank;
            }
        }
        ++rank;
    }
    if (rank == 0) return own_colour && row->human_side_id == own_side ? std::nullopt : base; // own: no tint
    if (ally_shades) return detail::gc_shade(*base, rank);
    const auto index = (gc_enemy_humans_use_palette ? enemy_sides : 0U) + extra;
    if (index >= pool.size()) return detail::gc_shade(*base, rank); // palette exhausted: fall back to a shade
    return pool[index];
}

} // namespace eawr::presentation::ui
