#pragma once

#include "eawr/campaign/fleets.hpp"
#include "eawr/sim/galactic/control.hpp"

namespace eawr::campaign {
// CGS-16 / RGC-19: positive alignment effects produce raw transition time.
[[nodiscard]] core::Result<sim::math::Fixed> prepare_control_alignment_multiplier(
    sim::math::Fixed strength, bool like, sim::math::Fixed maximum);
struct ControlTypeBinding {
    sim::galactic::TypeId id{};
    std::string original_type;
    bool skips_combat{}, ground_capable{}; // live predicate owners, WGM-38 / WGM-U09
};
struct ControlObjectBinding { sim::EntityId id{}; std::string original_type; };
[[nodiscard]] core::Result<std::vector<sim::galactic::GroundCapabilityType>> prepare_ground_capability_types(
    const units::GalacticTypeTable&, std::span<const FleetTypeBinding>);
// Bind authored quantities into explicit runtime presence. Player/faction leaning,
// faction caps, protection and alignment/modifier lifetime are
// resolved inputs from their owners, never guessed parser defaults (WGM-U11).
[[nodiscard]] core::Result<sim::galactic::PoliticalControlSetup> bind_control_data(
    const data::galactic::GalacticData& data, const units::GalacticTypeTable& types,
    sim::galactic::PoliticalControlSetup presence, std::span<const ControlTypeBinding> objects,
    std::span<const ControlObjectBinding> planets, std::span<const ControlObjectBinding> routes);
// CGS-02 / WGM-C04: immutable complete placement presence, no trade influence,
// no conquest rewards. Retain the returned current/target state at tick zero.
[[nodiscard]] core::Result<sim::galactic::PoliticalControlSetup> prepare_initial_control(
    const sim::galactic::GalacticSetup&, sim::galactic::PoliticalControlSetup);
} // namespace eawr::campaign
