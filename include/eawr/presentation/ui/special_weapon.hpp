#pragma once

#include "eawr/sim/tactical/snapshot.hpp"

namespace eawr::presentation::ui {

// MC-10/11: keep a world craft pick; a squadron icon supplies no special target.
[[nodiscard]] inline sim::EntityId special_weapon_pick_target(
    const sim::EntityId entity, const sim::EntityId part, const bool squadron_icon = false) {
    if (squadron_icon) return sim::invalid_entity_id;
    return part != sim::invalid_entity_id ? part : entity;
}

enum class SpecialWeaponAvailability { unavailable, purchased, charging, ready };
struct SpecialWeaponButton {
    sim::EntityId source{};
    std::uint32_t target_action{};
    sim::math::Fixed readiness{};
    std::uint32_t pulses_left{};
    bool permitted{};
    bool targeting{};
    SpecialWeaponAvailability availability{SpecialWeaponAvailability::unavailable};
    [[nodiscard]] bool enabled() const { return availability == SpecialWeaponAvailability::ready; }
    // WSW-29: zero readiness supplies a full dial; disabled state is independent.
    [[nodiscard]] double dial() const {
        return readiness.raw() > 0 ? static_cast<double>(readiness.raw()) / sim::math::Fixed::scale : 1.0;
    }
};
[[nodiscard]] inline SpecialWeaponAvailability special_weapon_availability(
    bool running, bool source, bool permitted, sim::math::Fixed readiness) {
    if (!running || !source || !permitted) return SpecialWeaponAvailability::unavailable;
    if (readiness.raw() <= 0) return SpecialWeaponAvailability::purchased;
    return readiness.raw() < sim::math::Fixed::scale
        ? SpecialWeaponAvailability::charging : SpecialWeaponAvailability::ready;
}
[[nodiscard]] inline const char* to_string(SpecialWeaponAvailability state) {
    switch (state) {
    case SpecialWeaponAvailability::unavailable: return "unavailable";
    case SpecialWeaponAvailability::purchased: return "purchased";
    case SpecialWeaponAvailability::charging: return "charging";
    case SpecialWeaponAvailability::ready: return "ready";
    }
    return "unavailable";
}
} // namespace eawr::presentation::ui
