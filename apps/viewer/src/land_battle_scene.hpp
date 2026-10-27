#pragma once

#include "space_populate.hpp"
#include "eawr/sim/tactical/land_terrain.hpp"

namespace eawr::presentation::godot_backend {

// Model slots share the same renderer operations in both battle domains.
// Land changes the asset domain and the source-space ground query.
class LandBattleScene final : public SpacePopulation {
public:
    LandBattleScene(Options options, std::shared_ptr<const sim::tactical::LandTerrain> terrain)
        : SpacePopulation(std::move(options)), terrain_(std::move(terrain)) {}
    [[nodiscard]] sim::tactical::BattleDomain domain() const noexcept override {
        return sim::tactical::BattleDomain::land;
    }
    [[nodiscard]] std::optional<ui::Vec3f> ground_point(const ui::PickRay& ray) const noexcept override;
private:
    std::shared_ptr<const sim::tactical::LandTerrain> terrain_;
};

} // namespace eawr::presentation::godot_backend
