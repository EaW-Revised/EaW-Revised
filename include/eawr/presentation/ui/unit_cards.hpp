#pragma once

#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// #425 (P2-20b, #83): the selected units as FoC unit cards in the tactical command bar
// (docs/behaviour/foc-unit-cards.md). Engine-free and presentation only: the cards read the local
// player's selection and the last snapshot's health, and a card click only changes the selection,
// which never reaches a command, the replay or a hash.
//
// The rules follow the FoC debug build's tactical selection update, its
// stacking and its select and deselect handling, docs/behaviour/foc-unit-cards.md (card slots `s_select_00`
// to `s_select_23`, their `s_health_*` and `s_shield_*` bars and the `special_border_*` column
// borders of CommandBarComponents.xml).
namespace eawr::presentation::ui {

// UnitAbilityType, the order the command bar groups cards in. `ability_index` gives a unit
// table's ability name (`POWER_TO_WEAPONS`, any case) its value; unknown names are none.
inline constexpr std::uint32_t ability_none = 0;
inline constexpr std::uint32_t ability_count = 77; // ABILITY_COUNT
[[nodiscard]] std::uint32_t ability_index(std::string_view name) noexcept;
// The UnitAbilityType name of a value (`POWER_TO_WEAPONS`), empty past the enum.
[[nodiscard]] std::string_view ability_name(std::uint32_t ability) noexcept;

// One thing a card can show: a unit, or a squadron (a homogeneous team) standing for its craft.
struct CardUnit {
    sim::EntityId id{};                 // the unit, or the squadron's container
    std::vector<sim::EntityId> members; // what selecting the card selects: the unit, or the live craft
    std::string type;                   // object type name (a squadron's is the squadron type)
    std::uint32_t ability{ability_none}; // the type's first unit ability (Get_Unit_Ability)
    std::uint8_t ability_status{};      // ability state; units of one type stack only when it matches
    bool squadron{};
    double health{1.0};                 // Get_Display_Health_Percent, or Get_Team_Health for a squadron
    std::optional<double> shield;       // Get_Shield_Percent for a shielded unit; never for a squadron
    std::uint32_t second_ability{ability_none}; // #454: the type's second unit ability (a second button)
};

// A selected entity as the viewer sees it, before squadrons fold into their team card.
struct SquadronOf {
    sim::EntityId container{};
    std::string type;         // the squadron type
    std::uint32_t ability{ability_none};
    bool homogeneous{true};   // every craft of the squadron type is one type (CARD-1)
    std::vector<sim::EntityId> members; // its live craft, in roster order
    double health{1.0};       // the mean of its live craft's health (CARD-3, the debug build)
    std::uint32_t second_ability{ability_none};
};
struct SelectedUnit {
    sim::EntityId entity{};
    std::string type;
    std::uint32_t ability{ability_none};
    double health{1.0};
    std::optional<double> shield;
    std::optional<SquadronOf> squadron; // the craft's squadron, when it has one
    std::uint32_t second_ability{ability_none};
};

// The card units of a selection, in selection order. A craft of a homogeneous squadron is not
// shown: its squadron's card is, once, where its first selected craft stands. A craft of a mixed
// squadron shows as itself.
[[nodiscard]] std::vector<CardUnit> card_units(std::span<const SelectedUnit> selection);

// Update_Tactical_Selections: its bars have Max_Bar_Level + 1 levels.
[[nodiscard]] std::int32_t health_level(double health, std::int32_t max_bar_level) noexcept;

enum class BorderPiece : std::uint8_t { full, left, centre, right }; // Icon_Alternate_Texture_Name order

struct UnitCard {
    std::size_t slot{};      // s_select_<slot>
    std::size_t unit{};      // index into the card units
    std::uint32_t ability{ability_none};
    std::uint32_t count{1};  // units the card stands for
    bool stacked{};          // shows "x<count>" and no bars
    std::int32_t health_level{}; // bars of a card that is not stacked
    std::optional<double> shield;
};

struct CardBorder {
    std::size_t column{}; // special_border_<column>: a column is slots 2c and 2c + 1
    BorderPiece piece{BorderPiece::full};
};

struct CardLayout {
    std::vector<UnitCard> cards; // in slot order
    std::vector<CardBorder> borders;
    std::size_t groups{};        // abilities with at least one card unit (NumGroups)
    // Card units per ability in selection order (ShipTypes): a click selects one of these lists.
    std::array<std::vector<std::size_t>, ability_count> by_ability{};
    [[nodiscard]] const UnitCard* at(std::size_t slot) const noexcept;
};

// Fills `slots` card slots (the command bar's 24; a column holds two). Card units group by ability in
// UnitAbilityType order, each group starting at a new column. Units of one type and ability state
// stack; while the cards (plus one blank slot per odd-sized group) exceed the slots, the largest
// stack not yet collapsed becomes one "x<count>" card, the first of equal ones first.
[[nodiscard]] CardLayout layout_unit_cards(std::span<const CardUnit> units, std::size_t slots,
                                           std::int32_t max_bar_level = 10);

// A left release on the card at `slot` (Component_Logic_Tactical_Select); the selection it leaves.
// Without Shift a single card selects its unit when the selection has one ability group, else the
// card's whole ability group; a stacked card selects every card unit of its type. With Shift the
// card's unit (or, stacked, every card unit of its type) leaves the selection. Nothing changes on a
// slot without a card.
[[nodiscard]] std::optional<std::vector<sim::EntityId>> card_click(const CardLayout& layout,
    std::span<const CardUnit> units, std::size_t slot, bool shift, std::span<const sim::EntityId> selection);

} // namespace eawr::presentation::ui
