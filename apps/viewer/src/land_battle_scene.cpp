#include "land_battle_scene.hpp"
#include "eawr/presentation/live/land_ground.hpp"

namespace eawr::presentation::godot_backend {

std::optional<ui::Vec3f> LandBattleScene::ground_point(const ui::PickRay& ray) const noexcept {
    return terrain_ ? live::land_ground_point(*terrain_, ray) : std::nullopt;
}

} // namespace eawr::presentation::godot_backend
