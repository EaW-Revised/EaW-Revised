#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/units/galactic_types.hpp"

#include <string>
#include <vector>

namespace eawr::campaign {

struct GalaxyName {
    sim::EntityId id{};
    std::string name;
};
struct GalaxyModel {
    std::vector<sim::galactic::Planet> planets;
    std::vector<sim::galactic::Route> routes;
    std::vector<GalaxyName> names;
    sim::EntityId next_id{};
    sim::math::Fixed maximum_movement_distance;
    std::array<std::uint8_t, 32> content_identity{};
    // Admission warnings are observations, outside persistent graph identity.
    std::vector<core::Diagnostic> diagnostics;
};

// WGS-01/WGM-16/17: campaign-scoped graph objects from validated tables. Assign IDs in authored
// planet then route order from the shared allocator frontier. The caller supplies initial owner;
// faction/home/starting-force setup remains the campaign-session owner's lane (#1149).
// Missing graph inputs fail with provenance; no copied stock values or guessed parser defaults.
[[nodiscard]] core::Result<GalaxyModel> build_campaign_galaxy(const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const data::galactic::Campaign& campaign,
    sim::galactic::PlayerId initial_owner, sim::EntityId first_free);

} // namespace eawr::campaign
