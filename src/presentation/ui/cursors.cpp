#include "eawr/presentation/ui/cursors.hpp"
#include <cmath>

namespace eawr::presentation::ui {
std::string_view battle_cursor(const CursorInput& input) noexcept {
    if (input.dragging) return "POINTER_DRAG_SELECT_MODE";
    if (input.camera_pan) return "POINTER_MAP_SCROLLING_MODE";
    if (input.camera_rotate) return "POINTER_MAP_ROTATING_MODE";
    if (input.hud) return "POINTER_NORMAL";
    if (input.mode == OrderMode::attack) return input.passable && input.hostile && input.own_selection
        ? "POINTER_ATTACK_ONLY_MODE_UNIT_TARGETED" : "POINTER_ATTACK_ONLY_MODE_NO_UNIT_TARGETED";
    if (input.mode == OrderMode::move) return input.passable
        ? "POINTER_MOVE_ONLY_MODE_PASSABLE_TARGETED" : "POINTER_MOVE_ONLY_MODE_NO_PASSABLE_TARGETED";
    if (!input.passable) return "POINTER_CANT_MOVE";
    switch (input.ability) {
    case CursorTarget::enemy: return input.ability_valid ? "POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT"
        : "POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT_INVALID";
    case CursorTarget::friendly: return input.ability_valid ? "POINTER_TARGET_SPECIAL_ABILITY_TO_FRIENDLY_OBJECT"
        : "POINTER_TARGET_SPECIAL_ABILITY_TO_FRIENDLY_OBJECT_INVALID";
    case CursorTarget::terrain: return input.ability_valid ? "POINTER_TARGET_SPECIAL_ABILITY_TO_PASSABLE_TERRAIN"
        : "POINTER_TARGET_SPECIAL_ABILITY_TO_PASSABLE_TERRAIN_INVALID";
    case CursorTarget::space_position: return input.ability_valid ? "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION"
        : "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION_INVALID";
    case CursorTarget::none: break;
    }
    if (input.placing) return input.placement_valid ? "POINTER_REINFORCEMENTS_LANDING_POINT" : "POINTER_CANT_MOVE";
    if (input.repair) return "POINTER_REPAIR_HARDPOINT";
    if (input.own_selection && input.hostile) return "POINTER_ATTACK";
    if ((input.selectable || input.movable_selection) && (input.mode == OrderMode::guard || (input.ctrl && input.alt)))
        return "POINTER_GUARD";
    if (input.selectable) return "POINTER_SELECT";
    if (!input.movable_selection) return "POINTER_NORMAL";
    if (input.alt) return "POINTER_WAYPOINT_PLACEMENT";
    if (input.ctrl || input.mode == OrderMode::attack_move) return "POINTER_ATTACK_MOVE";
    return "POINTER_MOVE";
}
void CursorHistory::record(const std::uint64_t frame, const std::string_view id, const std::string_view hover) {
    if (id_ == id && hover_ == hover) return;
    if (samples_.size() < 256) samples_.push_back({frame, std::string(id), std::string(hover)});
    id_ = id;
    hover_ = hover;
}
std::size_t cursor_frame(const double seconds, const std::uint32_t delay, const std::size_t frames) noexcept {
    if (frames < 2 || !std::isfinite(seconds) || seconds <= 0.0) return 0;
    const double period = (static_cast<double>(delay) + 1.0) * 0.016;
    return static_cast<std::size_t>(std::fmod(std::floor(seconds / period), static_cast<double>(frames)));
}
}
