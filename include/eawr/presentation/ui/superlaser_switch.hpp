#pragma once

#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/sim/tactical/instance.hpp"

#include <algorithm>
#include <span>

namespace eawr::presentation::ui {

// HCB-46/63: a sibling of the planet-destruction lever. Selection is irrelevant:
// the command bar finds a local countdown holder with SUPER_LASER in GC space.
struct SuperlaserSwitchView {
    bool visible{};
    bool enabled{};
    bool card_slot{}; // a planet-destruction lever occupies the ordinary switch panel
    sim::EntityId unit{};
    double recharge{};
    bool operator==(const SuperlaserSwitchView&) const = default;
};

[[nodiscard]] inline SuperlaserSwitchView superlaser_switch_view(
    const std::span<const sim::tactical::TacticalInstance> instances,
    const sim::tactical::PlayerId local_player, const AbilityState& abilities,
    const bool space, const bool tactical_production, const bool deathstar_active) {
    SuperlaserSwitchView view;
    if (!space || tactical_production || local_player == 0) return view;
    for (const auto& instance : instances) {
        if (instance.owner != local_player) continue;
        const auto holder = std::find_if(instance.abilities.begin(), instance.abilities.end(), [](const auto& status) {
            return status.kind == sim::tactical::AbilityKind::super_laser;
        });
        if (holder == instance.abilities.end()) continue;
        const auto state = abilities.state(instance.entity_id, ability_index("SUPER_LASER"));
        if (!state) continue;
        view.visible = true;
        view.enabled = state->status == AbilityStatus::ready;
        view.card_slot = deathstar_active;
        view.unit = instance.entity_id;
        // HCB-63: an active use forces the progress to zero, even for a timed ability.
        view.recharge = state->status == AbilityStatus::active ? 0.0 : std::clamp(state->recharge, 0.0, 1.0);
        break;
    }
    return view;
}

[[nodiscard]] inline std::optional<AbilityRequest> superlaser_switch_click(const SuperlaserSwitchView& view) {
    if (!view.visible || !view.enabled || view.unit == sim::invalid_entity_id) return std::nullopt;
    AbilityRequest request;
    request.ability = ability_index("SUPER_LASER");
    request.units = {view.unit};
    request.targeted = true;
    return request;
}

} // namespace eawr::presentation::ui
