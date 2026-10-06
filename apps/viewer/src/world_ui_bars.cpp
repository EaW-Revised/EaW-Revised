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

namespace {

[[nodiscard]] Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] Vec3 normalized(const Vec3& value) {
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!(length > 1.0e-6F)) return {0.0F, 0.0F, 1.0F};
    return {value[0] / length, value[1] / length, value[2] / length};
}

// The camera's up axis in the source basis (+Z up); the render basis is source (x, z, -y).
[[nodiscard]] Vec3 camera_up_source(const camera::TacticalFrame& frame) {
    const Vec3 forward = normalized(sub(frame.target, frame.eye));
    const Vec3 right = normalized(cross(forward, frame.up));
    const Vec3 up = cross(right, forward);
    return {up[0], -up[2], up[1]};
}

// The world-space bounds of a unit's pick box (FoC: the model's world-space bounds).
struct Bounds final {
    Vec3 centre{};
    Vec3 half{};
};
[[nodiscard]] Bounds world_bounds(const ui::UnitBox& box) {
    Vec3 low{1.0e30F, 1.0e30F, 1.0e30F};
    Vec3 high{-1.0e30F, -1.0e30F, -1.0e30F};
    const auto& m = box.model_to_world;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 local{(corner & 1) ? box.high[0] : box.low[0], (corner & 2) ? box.high[1] : box.low[1],
                         (corner & 4) ? box.high[2] : box.low[2]};
        for (std::size_t row = 0; row < 3; ++row) {
            const float world = m[row * 4] * local[0] + m[row * 4 + 1] * local[1] + m[row * 4 + 2] * local[2] + m[row * 4 + 3];
            low[row] = std::min(low[row], world);
            high[row] = std::max(high[row], world);
        }
    }
    Bounds bounds;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        bounds.centre[axis] = 0.5F * (low[axis] + high[axis]);
        bounds.half[axis] = 0.5F * (high[axis] - low[axis]);
    }
    return bounds;
}
} // namespace

void WorldUiView::draw_bars(const Frame& frame, const RID canvas_item, const float ui_scale) {
    health_bars_ = 0;
    shield_bars_ = 0;
    bar_rows_.clear();
    if (!frame.camera) return;
    const auto snapshot = frame.live->battle_frame().latest;
    const auto& squadron_of = frame.live->squadron_of();
    // WU-16 (#502): the unit under the pointer (for a squadron, the craft), then the selection.
    // Hovering a squadron's icon adds none of its craft (its own bar, drawn with the icon, already
    // shows the squadron's health).
    struct Candidate final {
        const ui::BattleUnit* unit{};
        sim::EntityId entity{};
    };
    std::vector<Candidate> candidates;
    const auto add = [&candidates](const ui::BattleUnit& unit, const sim::EntityId entity) {
        if (std::none_of(candidates.begin(), candidates.end(), [entity](const Candidate& c) { return c.entity == entity; })) {
            candidates.push_back({&unit, entity});
        }
    };
    const auto& units = *frame.units;
    if (frame.hovered && *frame.hovered < units.size()) {
        const ui::BattleUnit& unit = units[*frame.hovered];
        add(unit, unit.part != sim::invalid_entity_id ? unit.part : unit.entity);
    }
    for (const ui::BattleUnit& unit : units) {
        // WU-16 (#502): only these reach the bar rules; an unhovered, unselected unit shows none
        // even at critical health, and hovering a squadron's icon adds none of its craft.
        const bool craft = unit.part != sim::invalid_entity_id;
        const bool selected = !craft && frame.selection->contains(unit.entity);
        if (ui::bar_candidate(false, selected)) add(unit, craft ? unit.part : unit.entity);
    }
    const Vec3 up = camera_up_source(*frame.camera);
    const Vec3 eye{frame.camera->eye[0], -frame.camera->eye[2], frame.camera->eye[1]};
    for (const Candidate& candidate : candidates) {
        const ui::BattleUnit& unit = *candidate.unit;
        const auto* instance = instance_of(snapshot.get(), candidate.entity);
        if (instance == nullptr) continue;
        const auto type = types_.find(instance->type_id);
        if (type == types_.end()) continue;
        const auto* pad = frame.live->pad_view(candidate.entity);
        const bool capturing = pad && pad->state.progress.raw() > 0;
        const auto* construction = frame.live->pad_construction(candidate.entity);
        if (!instance->durability && !capturing) continue;
        const sim::tactical::InstanceDurability empty{};
        const auto& durability = instance->durability ? *instance->durability : empty;
        const bool craft = squadron_of.contains(candidate.entity);
        ui::BarUnit rules;
        rules.neutral = unit.neutral;
        rules.admitted = type->second.bar_admitted;
        rules.selected = !craft && frame.selection->contains(candidate.entity);
        rules.hovered = frame.hovered && *frame.hovered < units.size()
            && (units[*frame.hovered].part != sim::invalid_entity_id ? units[*frame.hovered].part : units[*frame.hovered].entity)
                == candidate.entity;
        rules.squadron_member = craft;
        const float max_hull = to_float(durability.max_hull);
        rules.has_health = max_hull > 0.0F;
        rules.health = rules.has_health ? to_float(durability.hull) / max_hull : 0.0F;
        rules.shielded = type->second.shielded && durability.max_shields && to_float(*durability.max_shields) > 0.0F;
        rules.hide_health_bar = type->second.hide_health_bar;
        const ui::BarVisibility shown = ui::bar_visibility(rules);
        if (!shown.health && !shown.shield && !capturing && !construction) continue;
        // WU-18: the anchor over the unit's bounds along the camera's up axis. A craft of a
        // squadron uses its own pick volume.
        const Bounds bounds = world_bounds(unit.box);
        const float lift = ui::bar_anchor_lift(bounds.half, up, type->second.bounds_scale);
        const Vec3 anchor{bounds.centre[0] + up[0] * lift, bounds.centre[1] + up[1] * lift, bounds.centre[2] + up[2] * lift};
        auto screen = frame.project(anchor);
        if (!screen) continue;
        if ((*screen)[0] < 0.0F || (*screen)[0] > frame.viewport[0]
            || (*screen)[1] < 0.0F || (*screen)[1] > frame.viewport[1]) continue;
        (*screen)[0] = std::trunc((*screen)[0]);
        (*screen)[1] = std::trunc((*screen)[1]);
        const float distance = std::sqrt((unit.position[0] - eye[0]) * (unit.position[0] - eye[0])
            + (unit.position[1] - eye[1]) * (unit.position[1] - eye[1]) + (unit.position[2] - eye[2]) * (unit.position[2] - eye[2]));
        const float scale = ui::bar_scale(distance) * ui_scale;
        const float width = ui::bar_width(type->second.bar) * scale;
        const float height = ui::bar_height * scale;
        float y = (*screen)[1];
        std::string row = std::to_string(candidate.entity) + ":";
        if (shown.shield) {
            const float max_shields = to_float(*durability.max_shields);
            const float shields = durability.shields ? to_float(*durability.shields) / max_shields : 0.0F;
            bar(canvas_item, (*screen)[0], y, width, height, shields, ui::shield_bar_colour);
            y += height * ui::health_bar_spacing;
            ++shield_bars_;
            row += "s" + std::to_string(ui::bar_level(shields));
        }
        if (shown.health) {
            const int level = ui::bar_level(rules.health);
            bar(canvas_item, (*screen)[0], y, width, height, rules.health, ui::health_bar_colour(level));
            ++health_bars_;
            row += "h" + std::to_string(level);
            y += height * ui::health_bar_spacing;
        }
        // WBP-37: reuse world-bar geometry; exact stock capture artwork remains U-BP-7.
        if (capturing) {
            const bool neutralizing = instance->owner != frame.live->economy().pads.neutral;
            const double progress = static_cast<double>(pad->state.progress.raw()) / sim::math::Fixed::scale;
            const auto tint = frame.live->player_colour(neutralizing ? instance->owner : pad->state.target)
                .value_or(ui::Rgb{255, 255, 255});
            bar(canvas_item, (*screen)[0], y, width, height,
                static_cast<float>(ui::capture_fill(progress, neutralizing)), tint);
            row += "c" + std::to_string(progress);
        } else if (construction) {
            const double progress = ui::pad_time_progress(frame.live->presented_tick(), construction->start_frame,
                                                         construction->finish_frame);
            const auto tint = frame.live->player_colour(construction->builder).value_or(ui::Rgb{255, 255, 255});
            bar(canvas_item, (*screen)[0], y, width, height, static_cast<float>(progress), tint);
            row += "b" + std::to_string(progress);
        }
        const std::array<float, 2> bracket{std::trunc((*screen)[0]), std::trunc((*screen)[1])};
        const auto parent = squadron_of.find(candidate.entity);
        draw_abilities(frame, canvas_item, candidate.entity, type->second, frame.live->is_ally_of_local(instance->owner),
                       bracket, false, height, ui_scale);
        // WU-45: tactical text occupies the foreground layer despite earlier submission.
        draw_group(frame, canvas_item, parent != squadron_of.end() ? parent->second : candidate.entity,
                   bracket, false, ui_scale);
        bar_rows_.push_back(std::move(row));
    }
    max_health_bars_ = std::max(max_health_bars_, health_bars_);
}

void WorldUiView::draw_reticles(const Frame& frame, const RID canvas_item) {
    reticles_drawn_ = 0;
    tracked_reticles_ = 0;
    reticle_rects_.clear();
    tooltip_hit_.reset();
    tooltip_rect_ = {};
    // The unit whose reticles show: the one under the pointer (WU-30), and the unit of a reticle that
    // is still flashing from an order (WU-42), which shows only that reticle.
    struct Shown final {
        const ui::BattleUnit* unit{};
        bool only_flash{};
    };
    std::vector<Shown> shown;
    if (frame.hovered && *frame.hovered < frame.units->size()) {
        const ui::BattleUnit& hovered = (*frame.units)[*frame.hovered];
        if (hovered.part == sim::invalid_entity_id && hovered.selectable && !hovered.neutral) {
            shown.push_back({&hovered, false}); // WU-30, WSU-63: a squadron or neutral prop has none
        }
    }
    if (flash_) {
        const bool listed = std::any_of(
            shown.begin(), shown.end(), [&](const Shown& entry) { return entry.unit->entity == flash_->entity; });
        if (!listed) {
            for (const ui::BattleUnit& unit : *frame.units) {
                if (unit.entity == flash_->entity && unit.part == sim::invalid_entity_id) {
                    shown.push_back({&unit, true});
                    break;
                }
            }
        }
    }
    const auto snapshot = frame.live->battle_frame().latest;
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (const Shown& entry_unit : shown) {
        const ui::BattleUnit& unit = *entry_unit.unit;
        if (unit.neutral || !unit.selectable) continue;
        const auto* instance = instance_of(snapshot.get(), unit.entity);
        const auto view = frame.live->unit_frame(unit.entity);
        const auto type = types_.find(unit.type);
        if (instance == nullptr || !instance->durability || !view || type == types_.end()) continue;
        const Vec3 position{static_cast<float>(view->position[0]), static_cast<float>(view->position[1]),
                            static_cast<float>(view->position[2])};
        struct Placed final {
            std::size_t index{};
            Rect2 rect;
            bool flashing{};
        };
        std::vector<Placed> placed;
        const auto& statuses = instance->durability->hardpoints;
        for (std::size_t index = 0; index < type->second.hardpoints.size() && index < statuses.size(); ++index) {
            const HardpointUi& hardpoint = type->second.hardpoints[index];
            const bool flashing = flash_ && flash_->entity == unit.entity && flash_->hardpoint == index;
            if (entry_unit.only_flash && !flashing) continue;
            // WU-30: every targetable hardpoint that still stands.
            if (!hardpoint.targetable || hardpoint.texture.empty()
                || statuses[index].state == sim::tactical::HardpointState::destroyed
                || (unit.hostile && !statuses[index].enabled)) {
                continue;
            }
            // WU-34: on the attachment point as the ship is drawn, bank and pitch included.
            const Vec3 world = ui::hardpoint_reticle_anchor(position, static_cast<float>(view->yaw_degrees),
                                                            static_cast<float>(view->pitch_degrees),
                                                            static_cast<float>(view->roll_degrees), hardpoint.local);
            const auto screen = frame.project(world);
            if (!screen) continue;
            // WU-31: a fixed share of the screen, whatever the camera distance.
            const ui::ReticleRect base = ui::hardpoint_reticle_rect(*screen, frame.viewport);
            // WU-42: a flashing reticle starts at half size and toggles every third render service.
            const float scale = flashing && (flash_->services / 3U) % 2U == 0U ? 0.5F : 1.0F;
            placed.push_back({index,
                Rect2(base.x + base.width * (1.0F - scale) * 0.5F, base.y + base.height * (1.0F - scale) * 0.5F,
                    base.width * scale, base.height * scale),
                flashing});
        }
        // WU-32: the hardpoint under the pointer shows the tracked art (the last reticle drawn over
        // the pointer, as the later one is on top). WU-41 picks by the same rectangles.
        std::optional<std::size_t> tracked;
        if (frame.pointer && !entry_unit.only_flash) {
            for (const Placed& entry : placed) {
                if (entry.rect.has_point(Vector2((*frame.pointer)[0], (*frame.pointer)[1]))) tracked = entry.index;
            }
        }
        for (const Placed& entry : placed) {
            const HardpointUi& hardpoint = type->second.hardpoints[entry.index];
            const bool is_tracked = (tracked && *tracked == entry.index) || entry.flashing;
            const auto texture = reticles_.find(is_tracked ? hardpoint.texture + "_tracked" : hardpoint.texture);
            if (texture == reticles_.end() || texture->second.is_null()) continue;
            const float health = hardpoint.max_health > 0.0F ? to_float(statuses[entry.index].health) / hardpoint.max_health : 1.0F;
            rendering->canvas_item_add_texture_rect(canvas_item, entry.rect, texture->second->get_rid(), false,
                                                    colour(ui::hardpoint_reticle_tint(health, !statuses[entry.index].enabled)));
            ++reticles_drawn_;
            if (is_tracked) ++tracked_reticles_;
            if (!entry.flashing) reticle_size_ = {entry.rect.size.x, entry.rect.size.y};
            if (!entry_unit.only_flash) {
                reticle_rects_.push_back({unit.entity, static_cast<std::uint32_t>(entry.index), entry.rect.position.x,
                    entry.rect.position.y, entry.rect.position.x + entry.rect.size.x,
                    entry.rect.position.y + entry.rect.size.y});
            }
            // WU-51: the actual hover, never an attack-order flash, owns the tooltip.
            if (tracked && *tracked == entry.index && !hardpoint.tooltip.empty()) {
                tooltip_hit_ = ReticleHit{unit.entity, static_cast<std::uint32_t>(entry.index)};
                tooltip_health_ = hardpoint.max_health > 0.0F ? std::clamp(health, 0.0F, 1.0F) : 0.0F;
                const int percent = static_cast<int>(std::floor(tooltip_health_ * 100.0F + 0.5F));
                const int pixels = ui::font_pixels({tooltip_points_, false, 1.0},
                    static_cast<std::uint32_t>(frame.viewport[1])).em_height;
                if (tooltip_line_.is_null()) tooltip_line_.instantiate();
                if (&hardpoint != tooltip_source_ || percent != tooltip_percent_ || pixels != tooltip_pixels_) {
                    tooltip_source_ = &hardpoint;
                    tooltip_percent_ = percent;
                    tooltip_text_ = hardpoint.tooltip + " - " + std::to_string(percent) + "%";
                    tooltip_pixels_ = pixels;
                    tooltip_line_->clear();
                    tooltip_line_->add_string(String::utf8(tooltip_text_.c_str()), tooltip_font_, pixels);
                    static_cast<void>(tooltip_line_->get_size());
                }
            }
        }
    }
    // WU-52/53: one cached text line and health bar, positioned after the reticle pass.
    if (tooltip_hit_ && frame.pointer && tooltip_font_.is_valid()) {
        const Vector2 padding(frame.viewport[0] * 0.0035F, frame.viewport[1] * 0.0035F);
        const Vector2 text_size = tooltip_line_->get_size();
        const int small_pixels = ui::font_pixels({tooltip_small_points_, false, 1.0},
            static_cast<std::uint32_t>(frame.viewport[1])).em_height;
        const float row_height = tooltip_small_font_->get_height(small_pixels);
        const float bar_width = frame.viewport[0] * 0.1F;
        const Vector2 content_size(std::max(text_size.x, bar_width), text_size.y + row_height);
        const Vector2 size = content_size + 2.0F * padding;
        // WU-53: clamp content extents; the frame adds padding after placement.
        const Vector2 origin(std::min((*frame.pointer)[0], frame.viewport[0] * 0.98F - content_size.x),
                             std::min((*frame.pointer)[1] + 32.0F, frame.viewport[1] * 0.98F - content_size.y));
        const Rect2 rect(origin, size);
        rendering->canvas_item_add_rect(canvas_item, rect, colour({15, 25, 45}, 240.0F / 255.0F));
        const float edge = frame.viewport[1] * 0.0014F;
        const Color border = colour({51, 113, 190}, 200.0F / 255.0F);
        rendering->canvas_item_add_rect(canvas_item, Rect2(origin, Vector2(size.x, edge)), border);
        rendering->canvas_item_add_rect(canvas_item, Rect2(origin.x, origin.y + size.y - edge, size.x, edge), border);
        rendering->canvas_item_add_rect(canvas_item, Rect2(origin, Vector2(edge, size.y)), border);
        rendering->canvas_item_add_rect(canvas_item, Rect2(origin.x + size.x - edge, origin.y, edge, size.y), border);
        tooltip_line_->draw(canvas_item, origin + padding, colour({51, 113, 190}));
        const Vector2 bar_origin = origin + padding + Vector2(0.0F, text_size.y + row_height * 0.3F);
        rendering->canvas_item_add_rect(canvas_item, Rect2(bar_origin, Vector2(bar_width, row_height * 0.4F)), colour({112, 0, 0}));
        if (tooltip_health_ > 0.0F) rendering->canvas_item_add_rect(canvas_item,
            Rect2(bar_origin, Vector2(bar_width * tooltip_health_, row_height * 0.4F)), colour({24, 168, 42}));
        tooltip_rect_ = {origin.x, origin.y, size.x, size.y};
    }
    max_reticles_ = std::max(max_reticles_, reticles_drawn_);
}

std::optional<WorldUiView::ReticleHit> WorldUiView::reticle_at(const std::array<float, 2> point) const {
    // The last reticle drawn over the point is the one on top (WU-32); the hit is its hardpoint's unit.
    std::optional<ReticleHit> found;
    for (const Reticle& reticle : reticle_rects_) {
        if (point[0] >= reticle.min_x && point[0] < reticle.max_x && point[1] >= reticle.min_y && point[1] < reticle.max_y) {
            found = ReticleHit{reticle.entity, reticle.hardpoint};
        }
    }
    return found;
}

std::optional<std::array<float, 2>> WorldUiView::reticle_centre(const sim::EntityId entity, const std::uint32_t hardpoint) const {
    for (const Reticle& reticle : reticle_rects_) {
        // 0xffffffff asks for the first reticle drawn on the unit (the lowest hardpoint index).
        if (reticle.entity == entity && (reticle.hardpoint == hardpoint || hardpoint == 0xffffffffU)) {
            return std::array<float, 2>{0.5F * (reticle.min_x + reticle.max_x), 0.5F * (reticle.min_y + reticle.max_y)};
        }
    }
    return std::nullopt;
}

void WorldUiView::flash_reticle(const sim::EntityId entity, const std::uint32_t hardpoint) {
    // WU-42: FoC's flash lasts 60 render services.
    flash_ = Flash{entity, hardpoint, 0U};
    ++flashes_started_;
}

void WorldUiView::service() {
    if (!flash_) return;
    if (++flash_->services >= flash_frames) flash_.reset();
}

} // namespace eawr::presentation::godot_backend
