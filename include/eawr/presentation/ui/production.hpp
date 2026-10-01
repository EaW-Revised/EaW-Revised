#pragma once

#include "eawr/sim/tactical/economy.hpp"
#include "eawr/data/xml.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// #530 (docs/behaviour/space-purchasing.md, PU-60 to PU-75): the local player's station production
// in the tactical command bar. Engine-free and presentation only: the layouts read the last
// snapshot's economy view and the station's menu, and a click becomes a buy, cancel or reinforce
// command through the live session (never a change here).
//
// The build buttons take the unit card slots (`s_select_00` on) while the player's own station is
// the selection; the two build queues are the `tqueue` slots (upgrades 00 to 04, units 05 to 09);
// the reinforcement pool is the pane `i_main_reinforce` with its `r_RRCC` slots (CommandBarComponents.xml).
namespace eawr::presentation::ui {
// WR-14: stored RGB times 1.5; authored alpha is validated but never used as opacity.
using ReinforcementColours = std::array<std::array<float, 3>, 2>; // good, bad
[[nodiscard]] core::Result<ReinforcementColours> reinforcement_colours(const vfs::Vfs& filesystem);

// PU-61: a button's state, the parameter the command bar gives it (3 normal, 0 queue full,
// 2 unaffordable). A button that cannot be produced is disabled without its own state.
enum class BuildButtonState : std::uint8_t { queue_full = 0, unaffordable = 2, normal = 3 };

struct BuildButton {
    std::size_t slot{};                 // s_select_<slot>
    std::size_t option{};               // index into the station menu's options
    sim::tactical::TypeId type{};
    std::int64_t price{};               // PU-62: the price shown, whole credits rounded to nearest
    bool enabled{};                     // PU-61: producible, affordable and its queue has room
    bool room{};                        // PU-62: its queue has room (the price's colour)
    BuildButtonState state{BuildButtonState::normal};
};

// PU-60, PU-61: one button per menu option in list order, in `slots` card slots. An option the
// session never builds (PU-20) shows disabled. `credits` and `queue_sizes` are the local player's.
[[nodiscard]] std::vector<BuildButton> layout_build_buttons(const sim::tactical::StationMenu& menu,
    sim::math::Fixed credits, const std::array<std::size_t, sim::tactical::build_queue_count>& queue_sizes,
    std::size_t max_queue, std::size_t slots);

inline constexpr std::size_t queue_slot_count = 5; // per queue: tqueue00..04, tqueue05..09

// PU-63: the tqueue component of entry `index` of `queue` (upgrades 0 to 4, units 5 to 9), or
// nothing past its five slots.
[[nodiscard]] std::optional<std::size_t> queue_component(sim::tactical::BuildQueue queue, std::size_t index) noexcept;

struct QueueSlot {
    std::size_t component{};            // tqueue<component>
    sim::tactical::BuildQueue queue{sim::tactical::BuildQueue::units};
    std::size_t index{};                // entry index in its queue (the cancel command's)
    sim::tactical::TypeId type{};
    double progress{1.0};               // PU-64: the front's completed fraction, 1 for the others
    std::optional<std::string> percent; // PU-64: "<n>%" on the front entry only
};

// PU-63, PU-64: the queued entries at `frame` (the snapshot's completed tick), front first.
[[nodiscard]] std::vector<QueueSlot> layout_build_queue(
    const std::array<std::vector<sim::tactical::QueueEntry>, sim::tactical::build_queue_count>& queues,
    std::uint64_t frame);

inline constexpr std::size_t pool_columns = 4;   // r_RR00..r_RR03
inline constexpr std::size_t pool_row_limit = 5; // r_00CC..r_04CC

struct PoolSlot {
    std::size_t slot{};                 // row * 4 + column: r_<row><column>
    sim::tactical::TypeId type{};
    std::uint32_t count{};
    std::string text;                   // PU-66: "x<n>" when more than one, else empty
    bool enabled{};                     // PU-66: its population fits the room
};

// PU-66: the pool folded by type, in first-completion order, one slot each up to `slots`.
// `population_of` gives a type's population value.
[[nodiscard]] std::vector<PoolSlot> layout_pool(std::span<const sim::tactical::TypeId> pool,
    std::uint32_t population, std::uint32_t population_cap,
    const std::function<std::uint32_t(sim::tactical::TypeId)>& population_of,
    std::size_t slots = pool_columns * pool_row_limit);

// PU-67: the pane's extra rows past the first for `filled` slots (0 to 4).
[[nodiscard]] std::size_t pool_rows(std::size_t filled) noexcept;

// PU-07, PU-65: whole credits, rounded down; the population "<used>/<cap>".
[[nodiscard]] std::string credits_text(sim::math::Fixed credits);
[[nodiscard]] std::string population_text(std::uint32_t population, std::uint32_t cap);

} // namespace eawr::presentation::ui
