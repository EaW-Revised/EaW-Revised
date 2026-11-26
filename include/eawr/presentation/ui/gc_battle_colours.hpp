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

// GCC-08 (owner decision 11-25, legacy #8359): the order of a faction's human players is their lobby position in that
// faction's slot: ascending `lobby_slot` (the lobby's seat order, sealed into the roster every peer shares), then seat ID
// (a co-op seat admitted mid-campaign shares the faction's slot and joins behind it). Seats that left (`active` off) drop
// out, so when #1 leaves the next player becomes #1. Nothing here reads the host or the local seat.
[[nodiscard]] inline bool gc_slot_before(const sim::galactic::Seat& a, const sim::galactic::Seat& b) noexcept {
    return a.lobby_slot != b.lobby_slot ? a.lobby_slot < b.lobby_slot : a.seat_id < b.seat_id;
}
[[nodiscard]] inline bool gc_in_slot(const sim::galactic::Seat& row, const sim::galactic::PlayerId player) noexcept {
    return row.player == player && row.active && row.kind == sim::SeatKind::human && row.human_side_id != 0;
}
// The main player of `player`'s faction: #1 of its slot (GCC-08); zero when no human of the faction is left.
[[nodiscard]] inline sim::SeatId gc_main_seat(const std::span<const sim::galactic::Seat> seats,
        const sim::galactic::PlayerId player) noexcept {
    const sim::galactic::Seat* main = nullptr;
    for (const auto& row : seats)
        if (gc_in_slot(row, player) && (main == nullptr || gc_slot_before(row, *main))) main = &row;
    return main != nullptr ? main->seat_id : sim::SeatId{};
}

// Read the live controller every frame: a gift changes the seat without changing its owner.
[[nodiscard]] inline std::optional<BattleRgb> gc_instance_model_colour(
    const bool participant_units, const sim::tactical::TacticalInstance& instance,
    const std::optional<BattleRgb> faction, const std::span<const sim::galactic::Seat> seats,
    const std::map<sim::SeatId, BattleRgb>& colours) {
    if (!participant_units) return faction;
    auto seat = instance.controller_seat;
    if (seat.value == 0) {
        // No controller recorded: the faction's main seat (GCC-08) speaks for it.
        if (const auto main = gc_main_seat(seats, instance.owner); main.value != 0) seat = main;
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

// Only the 3D model look is co-op gated; the shared participant table also serves the UI.
[[nodiscard]] inline std::optional<BattleRgb> gc_coop_model_colour(
    const bool participant_units, const sim::tactical::TacticalInstance& instance,
    const std::optional<BattleRgb> faction, const std::span<const sim::galactic::Seat> seats,
    const std::map<sim::SeatId, BattleRgb>& colours, const sim::galactic::PlayerId view_player) {
    const bool coop = view_player != 0 && std::count_if(seats.begin(), seats.end(), [&](const auto& row) {
        return gc_in_slot(row, view_player);
    }) >= 2;
    return gc_instance_model_colour(participant_units && coop, instance, faction, seats, colours);
}

// Owner rule (#8355, 11-24; corrected #8359, 11-25): every enemy keeps its faction's own (colour-off) look, so it gets no
// entry. The main player of the local faction, #1 of the faction's lobby slot (GCC-08; not the host, not the local seat),
// takes the faction's COLOURED main team colour (Factions.xml `Color`, passed as `local_team_colour`; absent: no entry,
// the colour-off look). `local_seat` only picks which side is ours. The other players on the local side get tinted variants of
// the local faction's colour-off colour, a different one per mate (shades by default; with `ally_shades` off a palette
// colour of their own). AI seats the caller names in `ai_mates`
// (allied AI) are mates too and take the shades after the human mates. Script and unknown seats get no entry.
// The inputs are the saved galactic seat and side tables in their canonical order, so every peer derives the same table.
// PROVISIONAL (#8359): `coop_colours` is the local faction's Factions.xml `Coop_Player_Colours` list; entry n is the n-th mate
// (human mates in slot order, then allied AI), and a mate past the end of the list, or an empty list, takes the shade.
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
    const std::span<const sim::SeatId> ai_mates = {}, const std::span<const BattleRgb> coop_colours = {},
    const std::optional<BattleRgb> local_team_colour = {}) {
    // Mate number `rank` (1-based): the mod's list entry when it has one, else nothing (the caller shades).
    const auto listed = [&](const std::size_t rank) -> std::optional<BattleRgb> {
        if (rank == 0 || rank > coop_colours.size()) return std::nullopt;
        return coop_colours[rank - 1];
    };
    // GCC-08: a seat that left is out of the slot order and gets no entry.
    const auto human = [](const auto& value) {
        return value.kind == sim::SeatKind::human && value.human_side_id != 0 && value.active; };
    const auto row = std::find_if(seats.begin(), seats.end(), [seat](const auto& value) { return value.seat_id == seat; });
    if (row == seats.end()) return {};
    const auto side_known = [&](const auto side) {
        return std::any_of(teams.begin(), teams.end(), [side](const auto& team) { return team.side == side; });
    };
    const auto local = std::find_if(seats.begin(), seats.end(), [&](const auto& value) {
        return local_seat.value != 0 && value.seat_id == local_seat && human(value); });
    const auto own_side = local == seats.end() ? decltype(row->human_side_id){} : local->human_side_id;
    const bool own_colour = own_side != 0 && local_faction.has_value();
    // The main player of the local faction: #1 of its lobby slot (GCC-08), the same seat on every peer.
    const auto own_main = local == seats.end() ? sim::SeatId{} : gc_main_seat(seats, local->player);
    if (row->kind == sim::SeatKind::ai) {
        const auto found = std::find(ai_mates.begin(), ai_mates.end(), seat);
        if (!own_colour || found == ai_mates.end()) return {};
        const auto human_mates = static_cast<std::size_t>(std::count_if(seats.begin(), seats.end(), [&](const auto& other) {
            return human(other) && other.human_side_id == own_side && other.seat_id != own_main; }));
        const auto ai_rank = human_mates + 1 + static_cast<std::size_t>(found - ai_mates.begin());
        if (const auto mod = listed(ai_rank)) return mod;
        return detail::gc_shade(*local_faction, ai_rank);
    }
    if (!human(*row) || !side_known(row->human_side_id)) return {};
    const auto leads = [&](const auto& value) {
        // The seat whose colour is the side's own: the local faction's main seat (GCC-08), else #1 of the side's slot order.
        if (own_colour && value.human_side_id == own_side) return value.seat_id == own_main;
        return std::none_of(seats.begin(), seats.end(), [&](const auto& other) {
            return human(other) && other.human_side_id == value.human_side_id && gc_slot_before(other, value); });
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
    std::size_t rank = 0; // 1-based among the side's seats that are not its lead, in slot order (GCC-08)
    std::size_t extra = 0; // among all such seats of every side, in slot order
    if (!leads(*row)) {
        for (const auto& other : seats) {
            if (!human(other) || !side_known(other.human_side_id) || leads(other)) continue;
            if (gc_slot_before(other, *row)) {
                ++extra;
                if (other.human_side_id == row->human_side_id) ++rank;
            }
        }
        ++rank;
    }
    if (rank == 0) return own_colour && row->human_side_id == own_side ? local_team_colour : base; // main: team colour
    if (own_colour && row->human_side_id == own_side)
        if (const auto mod = listed(rank)) return mod;
    if (ally_shades) return detail::gc_shade(*base, rank);
    const auto index = (gc_enemy_humans_use_palette ? enemy_sides : 0U) + extra;
    if (index >= pool.size()) return detail::gc_shade(*base, rank); // palette exhausted: fall back to a shade
    return pool[index];
}

} // namespace eawr::presentation::ui
