#include "world_ui_view.hpp"

#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "world_ui_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace world_ui_detail;

std::optional<sim::EntityId> WorldUiView::icon_at(const std::array<float, 2> point) const {
    std::optional<sim::EntityId> found;
    for (const Icon& icon : icons_) {
        if (point[0] >= icon.min_x && point[0] <= icon.max_x && point[1] >= icon.min_y && point[1] <= icon.max_y) {
            found = icon.squadron;
        }
    }
    return found;
}

std::optional<std::array<float, 2>> WorldUiView::icon_centre(const sim::EntityId squadron) const {
    for (const Icon& icon : icons_) {
        if (icon.squadron == squadron) return std::array<float, 2>{0.5F * (icon.min_x + icon.max_x), 0.5F * (icon.min_y + icon.max_y)};
    }
    return std::nullopt;
}

std::vector<ui::BattleUnit> WorldUiView::icon_units() const {
    std::vector<ui::BattleUnit> units;
    units.reserve(icons_.size());
    for (const Icon& icon : icons_) {
        ui::BattleUnit unit;
        unit.entity = icon.squadron;
        unit.type = icon.type;
        unit.own = icon.own;
        unit.hostile = icon.hostile;
        unit.selectable = icon.selectable;
        unit.screen = std::array<float, 2>{0.5F * (icon.min_x + icon.max_x), 0.5F * (icon.min_y + icon.max_y)};
        units.push_back(unit);
    }
    return units;
}

std::vector<ui::SquadronIcon> WorldUiView::box_icons() const {
    std::vector<ui::SquadronIcon> result;
    result.reserve(icons_.size());
    for (const Icon& icon : icons_) {
        result.push_back({icon.squadron, icon.own, icon.selectable, {icon.min_x, icon.min_y, icon.max_x, icon.max_y}});
    }
    return result;
}

std::optional<ui::BattleUnit> WorldUiView::icon_hit(const std::array<float, 2> point) const {
    std::optional<ui::BattleUnit> found;
    for (const Icon& icon : icons_) {
        if (point[0] >= icon.min_x && point[0] <= icon.max_x && point[1] >= icon.min_y && point[1] <= icon.max_y) {
            ui::BattleUnit unit;
            unit.entity = icon.squadron;
            unit.type = icon.type;
            unit.own = icon.own;
            unit.hostile = icon.hostile;
            found = unit;
        }
    }
    return found;
}

void WorldUiView::write_report(std::ostream& output) const {
    const auto list = [&output](const std::vector<std::string>& rows) {
        output << "[";
        for (std::size_t index = 0; index < rows.size(); ++index) output << (index ? ", " : "") << "\"" << rows[index] << "\"";
        output << "]";
    };
    output << "  \"world_ui\": {\"ring\": \"" << ring_source_ << "\", \"ring_loaded\": " << (ring_.is_valid() ? "true" : "false")
           << ", \"atlas_loaded\": " << (atlas_.is_valid() ? "true" : "false") << ", \"types\": " << types_.size()
           << ", \"circles\": " << circles_drawn_ << ", \"max_circles\": " << max_circles_
           << ", \"health_bars\": " << health_bars_ << ", \"shield_bars\": " << shield_bars_
           << ", \"max_health_bars\": " << max_health_bars_ << ", \"reticles\": " << reticles_drawn_
           << ", \"reticle_flashes\": " << flashes_started_
           << ", \"tracked_reticles\": " << tracked_reticles_ << ", \"max_reticles\": " << max_reticles_
           << ", \"reticle_size\": [" << reticle_size_[0] << ", " << reticle_size_[1] << "]"
           << ", \"icons\": " << icons_.size() << ", \"grid_icons\": " << grid_icons_
           << ", \"max_grid_icons\": " << max_grid_icons_ << ", \"combat_cells\": " << grid_.cells().size()
           << ", \"bar_rows\": ";
    list(bar_rows_);
    output << ", \"icon_rows\": ";
    list(icon_rows_);
    output << ", \"gripper_points\": [";
    bool first_gripper = true;
    for (const auto& [squadron, gripper] : grippers_) {
        output << (first_gripper ? "" : ", ") << "{\"squadron\": " << squadron
               << ", \"position\": [" << gripper.motion.position[0] << ", " << gripper.motion.position[1]
               << ", " << gripper.motion.position[2] << "], \"speed\": " << gripper.motion.speed
               << ", \"arrival_samples_dropped\": " << gripper.arrival_samples_dropped << ", \"arrival_samples\": [";
        for (std::size_t index = 0; index < gripper.arrival_sample_count; ++index) {
            const auto& sample = gripper.arrival_samples[index];
            output << (index ? ", " : "") << "{\"tick\": " << sample.tick
                   << ", \"arriving\": " << (sample.arriving ? "true" : "false")
                   << ", \"drawn\": " << (sample.drawn ? "true" : "false");
            const auto position = [&](const char* name, const auto& at) {
                output << ", \"" << name << "\": [" << at[0] << ", " << at[1] << ", " << at[2] << "]";
            };
            position("desired", sample.desired);
            position("craft_centre", sample.craft_centre);
            position("anchor", sample.anchor);
            output << "}";
        }
        output << "]}";
        first_gripper = false;
    }
    output << "]";
    // #632: the flags drawn last frame (squadron:x=,y= of the flag's corner), and every squadron that ever showed one.
    output << ", \"flag_rows\": ";
    list(flag_rows_);
    output << ", \"flags_seen\": ";
    list(std::vector<std::string>(flags_seen_.begin(), flags_seen_.end()));
    output << ", \"ability_overlays\": [";
    for (std::size_t index = 0; index < ability_rows_.size(); ++index) {
        const auto& row = ability_rows_[index];
        output << (index ? ", " : "") << "{\"entity\": " << row.entity << ", \"squadron\": "
               << (row.squadron ? "true" : "false") << ", \"ability\": \"" << ui::ability_name(row.ability)
               << "\", \"icon\": \"" << ui::ability_icon(row.ability) << "\", \"centre\": [" << row.x << ", " << row.y
               << "], \"size\": [" << row.width << ", " << row.height << "]}";
    }
    output << "], \"control_groups\": [";
    for (std::size_t index = 0; index < group_rows_.size(); ++index) {
        const auto& row = group_rows_[index];
        output << (index ? ", " : "") << "{\"entity\": " << row.entity << ", \"squadron\": "
               << (row.squadron ? "true" : "false") << ", \"number\": " << row.group
               << ", \"centre\": [" << row.x << ", " << row.y << "]}";
    }
    output << "]";
    output << ", \"unresolved\": ";
    std::vector<std::string> unresolved;
    for (const std::string& row : unresolved_) {
        std::string clean;
        for (const char character : row) clean += character == '"' || character == '\\' ? '\'' : character;
        unresolved.push_back(clean);
    }
    list(unresolved);
    output << "},\n";
}

} // namespace eawr::presentation::godot_backend
