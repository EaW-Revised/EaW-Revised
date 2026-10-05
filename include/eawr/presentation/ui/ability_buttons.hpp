#pragma once

// #454 (P2 gap 3, #83): the ability buttons of the tactical command bar, the ability marks on the unit
// cards and the ability hotkeys (docs/behaviour/foc-ability-buttons.md). Engine-free.
//
// Interface to the simulation (#76). The buttons read each unit's ability state through AbilityState
// and hand clicks and hotkeys to AbilityCommands as AbilityRequests. Until the simulation has ability
// state, the viewer implements both with a stand-in (every ability a card unit's type has is ready,
// no autofire; a request only adds a report line). #76 implements them over the session's snapshot
// and its ability commands; nothing here changes then.

#include "eawr/presentation/ui/selection.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// AB-03: one unit's state of one of its abilities.
enum class AbilityStatus : std::uint8_t { ready, active, recharging, disabled };
[[nodiscard]] std::string_view to_string(AbilityStatus status) noexcept;
struct UnitAbilityState {
    AbilityStatus status{AbilityStatus::ready};
    double recharge{1.0}; // recharging: the countdown's completion, 0 to 1; active and timed: the share of its
                          // duration left (the dial fills as it runs down), 1 when untimed
    bool autofire{};
    friend constexpr bool operator==(const UnitAbilityState&, const UnitAbilityState&) noexcept = default;
};

// What the simulation (#76) will implement: the state of `ability` (a UnitAbilityType value) on
// `unit` (a card unit: a unit, or a squadron's container), or nothing when the unit lacks it.
class AbilityState {
public:
    virtual ~AbilityState() = default;
    [[nodiscard]] virtual std::optional<UnitAbilityState> state(sim::EntityId unit, std::uint32_t ability) const = 0;
};

// A click's or hotkey's request for one ability group.
struct AbilityRequest {
    enum class Kind : std::uint8_t { activate, deactivate, autofire_on, autofire_off };
    Kind kind{Kind::activate};
    std::uint32_t ability{ability_none};
    std::vector<sim::EntityId> units; // the group's card units, in card order
    bool targeted{};                  // activate: the ability first takes a target (AB-09)
    // #561 (AB-11): the target unit a targeted activation was aimed at; none while it still
    // waits for one.
    sim::EntityId target{sim::invalid_entity_id};
    std::optional<sim::math::Vec3> position{}; // world-point abilities; zero retains owner-position fallback
};
[[nodiscard]] std::string_view to_string(AbilityRequest::Kind kind) noexcept;
class AbilityCommands {
public:
    virtual ~AbilityCommands() = default;
    virtual void request(const AbilityRequest& request) = 0;
};

// AB-04: the engine's icon for an ability type; empty when the table has none.
[[nodiscard]] std::string_view ability_icon(std::uint32_t ability) noexcept;
// AB-09: abilities that take a target before they fire.
[[nodiscard]] bool ability_targeted(std::uint32_t ability) noexcept;

// AB-01..AB-07: one ability button.
struct AbilityButton {
    std::size_t component{};     // special_button_<component>
    std::uint32_t ability{ability_none};
    bool second{};               // the group's second ability
    bool shifted{};              // the group has two buttons: both move 14 shell units left
    std::string icon;
    bool disabled{};
    double recharge{1.0};        // the dial's completion; 1 draws no dial
    bool autofire{};             // every unit on autofire: the animated outline
    std::vector<sim::EntityId> units; // the group's card units
    std::string_view disabled_reason{}; // RG-03: borrowed from immutable process-lifetime policy text
};
// AB-08: an ability mark on a unit card.
struct CardAbilityMark {
    std::size_t slot{};
    bool second{};
    std::string icon;            // active, recharging or on autofire: the icon at 0.75 scale; empty otherwise
    std::optional<double> dial;  // recharging: the completion
    bool autofire{};             // the card's overlay box
};
struct AbilityBar {
    std::vector<AbilityButton> buttons;
    std::vector<CardAbilityMark> marks;
};
inline constexpr float ability_button_shift = 14.0F; // shell units (AB-01)
inline constexpr float card_mark_scale = 0.75F;      // AB-08

// The buttons and card marks of a card layout. `icon` names a card unit's icon for its first
// (false) or second (true) ability: the type's Alternate_Icon_Name, else ability_icon().
[[nodiscard]] AbilityBar ability_bar(const CardLayout& layout, std::span<const CardUnit> units,
                                     const AbilityState& state,
                                     const std::function<std::string(const CardUnit& unit, bool second)>& icon);

// AB-09: a left (activate or deactivate) or right (autofire) release on `button`; nothing when no
// unit of the group can take the request.
[[nodiscard]] std::optional<AbilityRequest> ability_click(const AbilityButton& button, bool right,
                                                          const AbilityState& state);

// AB-10: the ability FoC's default key map binds to a key (an upper-case letter, or `[`, `]`, `;`,
// `'`, `,`, `.`, `/` and `\`) with exactly these modifiers; nothing when it binds none.
[[nodiscard]] std::optional<std::uint32_t> ability_hotkey(char key, Modifiers modifiers) noexcept;

// The stand-in state until #76: every ability of a card unit is ready and off autofire; `overrides`
// stages other states per card unit and ability (the gallery and the eye-check runs).
class ReadyAbilities final : public AbilityState {
public:
    struct Staged {
        sim::EntityId unit{};
        std::uint32_t ability{ability_none};
        UnitAbilityState state;
    };
    void set_units(std::span<const CardUnit> units);
    void stage(std::vector<Staged> staged) { staged_ = std::move(staged); }
    [[nodiscard]] std::optional<UnitAbilityState> state(sim::EntityId unit, std::uint32_t ability) const override;

private:
    struct Known {
        sim::EntityId unit{};
        std::uint32_t first{ability_none};
        std::uint32_t second{ability_none};
    };
    std::vector<Known> known_;
    std::vector<Staged> staged_;
};

} // namespace eawr::presentation::ui
