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

namespace world_ui_detail {

// WU-12, WU-14: a one-pixel outlined bar: the black back over the full width, the
// filled part from the left, and a black outline around the back.
void bar(const RID item, const float centre_x, const float centre_y, const float width, const float height,
         const float fraction, const ui::Rgb& fill) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    const float left = centre_x - width * 0.5F;
    const float top = centre_y - height * 0.5F;
    const Color black(0.0F, 0.0F, 0.0F, 1.0F);
    rendering->canvas_item_add_rect(item, Rect2(left - 1.0F, top - 1.0F, width + 2.0F, height + 2.0F), black);
    const float filled = width * std::clamp(fraction, 0.0F, 1.0F);
    if (filled > 0.0F) rendering->canvas_item_add_rect(item, Rect2(left, top, filled, height), colour(fill));
}
} // namespace world_ui_detail

void WorldUiView::set_group_fonts(Ref<Font> icon, Ref<Font> bracket) {
    if (icon.is_valid()) icon_group_.font = std::move(icon);
    if (bracket.is_valid()) bracket_group_.font = std::move(bracket);
    icon_group_.pixels = bracket_group_.pixels = 0;
}

void WorldUiView::prepare_digits(GroupText& text, const float ui_scale) {
    const int pixels = ui::font_pixels({text.points, false, 1.0}, static_cast<std::uint32_t>(768.0F * ui_scale)).em_height;
    if (pixels == text.pixels || text.font.is_null()) return;
    text.pixels = pixels;
    // Shape ten cached digits only when the font or viewport size changes, never per unit.
    for (std::size_t group = 0; group < text.digits.size(); ++group) {
        if (text.digits[group].is_null()) text.digits[group].instantiate();
        text.digits[group]->clear();
        text.digits[group]->add_string(String::num_int64(static_cast<std::int64_t>(group)), text.font, pixels);
        static_cast<void>(text.digits[group]->get_size());
    }
}

void WorldUiView::draw_group(const Frame& frame, const RID canvas_item, const sim::EntityId entity,
                            const std::array<float, 2> anchor, const bool squadron, const float ui_scale) {
    const auto group = frame.selection->group_of(entity);
    if (!group) return;
    const GroupText& text = squadron ? icon_group_ : bracket_group_;
    const auto& digit = text.digits[*group];
    if (digit.is_null()) return;
    // XML +Y points up; reference offsets are relative to the component centre.
    const Vector2 centre(anchor[0] + text.offset[0] * ui_scale, anchor[1] - text.offset[1] * ui_scale);
    const Vector2 position = centre - digit->get_size() * 0.5F;
    if (text.outline) digit->draw_outline(canvas_item, position, 1, Color(0, 0, 0, 1));
    digit->draw(canvas_item, position, text.colour);
    group_rows_.push_back({entity, *group, squadron, centre.x, centre.y});
}

void WorldUiView::draw_abilities(const Frame& frame, const RID canvas_item, const sim::EntityId entity,
                                const TypeUi& type, const bool ally, const std::array<float, 2> anchor,
                                const bool squadron, const float bar_height_pixels, const float ui_scale) {
    if (!ability_art_enabled_ || frame.abilities == nullptr || atlas_.is_null()) return;
    const auto shown = ui::world_ability_overlays({entity, type.abilities, ally, true, true, squadron, !squadron},
                                                 *frame.abilities);
    const float scale = (squadron ? 1.0F : bracket_icon_scale_) * ui_scale;
    // WU-44: the additional lower effect is behind the primary icon.
    for (std::size_t draw = 0; draw < shown.size(); ++draw) {
        const std::size_t slot = shown.size() - 1 - draw;
        if (shown[slot] == ui::ability_none || !type.ability_regions[slot]) continue;
        const Rect2& region = *type.ability_regions[slot];
        const float height = region.size.y * scale;
        auto centre = anchor;
        std::array<float, 2> offset{};
        if (squadron) {
            offset = squadron_ability_offset_;
        } else {
            // Both quads share the primary icon's component point when both slots are active.
            const float primary_height = slot == 1 && shown[0] != ui::ability_none && type.ability_regions[0]
                ? type.ability_regions[0]->size.y * scale : height;
            centre[1] = ui::bracket_ability_y(centre[1], bar_height_pixels, primary_height, ui_scale);
            if (slot == 1 && shown[0] != ui::ability_none) {
                offset = second_ability_offset_;
            }
        }
        const auto rect = ui::world_ability_rect(centre, {region.size.x, region.size.y}, offset,
                                                squadron ? 1.0F : bracket_icon_scale_, ui_scale);
        RenderingServer::get_singleton()->canvas_item_add_texture_rect_region(canvas_item,
            Rect2(rect.x, rect.y, rect.width, rect.height), atlas_->get_rid(), region);
        ability_rows_.push_back({entity, shown[slot], squadron, rect.x + rect.width * 0.5F,
                                rect.y + rect.height * 0.5F, rect.width, rect.height});
    }
}

void WorldUiView::place_circles(const Frame& frame) {
    std::vector<std::pair<Vec3, float>> circles;
    if (ring_material_.is_valid()) {
        const auto& squadrons = frame.live->squadron_members();
        for (const sim::EntityId selected : frame.selection->units()) {
            // A selected squadron: every craft carries its own small circle (WU-03).
            const bool squadron = squadrons.contains(selected);
            for (const ui::BattleUnit& unit : *frame.units) {
                if (unit.entity != selected) continue;
                sim::tactical::TypeId type = unit.type;
                if (squadron) {
                    const auto view = frame.live->unit_frame(unit.part);
                    if (!view) continue;
                    type = view->type;
                }
                const auto found = types_.find(type);
                if (found == types_.end() || !found->second.circle_side) continue;
                Vec3 at = unit.position;
                if (squadron) {
                    const auto view = frame.live->unit_frame(unit.part);
                    at = {static_cast<float>(view->position[0]), static_cast<float>(view->position[1]),
                          static_cast<float>(view->position[2])};
                }
                circles.emplace_back(at, *found->second.circle_side);
            }
        }
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !scenario_.is_valid() || ring_mesh_.is_null()) {
        circles.clear();
    } else {
        while (circles_.size() < circles.size()) {
            const RID instance = rendering->instance_create2(ring_mesh_->get_rid(), scenario_);
            if (!instance.is_valid()) break;
            rendering->instance_geometry_set_cast_shadows_setting(instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            circles_.push_back(instance);
        }
        for (std::size_t index = 0; index < circles_.size(); ++index) {
            if (index >= circles.size()) {
                rendering->instance_set_visible(circles_[index], false);
                continue;
            }
            const auto& [at, side] = circles[index];
            // WU-02: flat at the unit's position (render basis: source x, z, -y), side x side.
            const Transform3D transform(Basis().scaled(Vector3(side, 1.0F, side)), Vector3(at[0], at[2], -at[1]));
            rendering->instance_set_transform(circles_[index], transform);
            rendering->instance_set_visible(circles_[index], true);
        }
        circles.resize(std::min(circles.size(), circles_.size()));
    }
    circles_drawn_ = circles.size();
    max_circles_ = std::max(max_circles_, circles_drawn_);
}

void WorldUiView::update_grid(const Frame& frame) {
    // WU-25 (#457): the sim flies the dogfights over its combat cells (space-fighters FD-01 to
    // FD-03) and publishes the cell each squadron records; the joined ones hold their icons, in
    // ascending squadron ID (the sim's service order). The grid starts at the world origin.
    const auto snapshot = frame.live->battle_frame().latest;
    std::vector<ui::CombatGrid::Record> records;
    if (snapshot != nullptr) {
        for (const sim::tactical::SquadronTarget& row : snapshot->squadron_targets()) {
            if (row.recorded) records.push_back({row.squadron, ui::CombatCell{row.cell_x, row.cell_y}, row.joined});
        }
    }
    grid_.adopt(records);
    std::vector<ui::CombatIconGrid::Cell> cells;
    for (const auto& occupied : grid_.cells()) {
        float height = 0.0F;
        const auto members = frame.live->squadron_members().find(occupied.squadrons.front());
        if (members != frame.live->squadron_members().end()) {
            for (const auto member : members->second) {
                const auto* craft = instance_of(snapshot.get(), member);
                if (craft == nullptr) continue;
                if (const auto type = types_.find(craft->type_id); type != types_.end()) height = type->second.layer_z;
                break;
            }
        }
        cells.push_back({occupied.cell, occupied.squadrons, height});
    }
    icon_grid_.update(cells);
}

void WorldUiView::draw_identity(const Frame& frame, const RID canvas_item, const sim::EntityId entity,
                                const TypeUi& type, const sim::tactical::PlayerId owner, const bool hostile,
                                const std::array<float, 2> screen, const float fraction, const float ui_scale) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    const bool selected = frame.selection->contains(entity);
    const bool hovered = frame.hovered_icon && *frame.hovered_icon == entity;
    const int level = ui::bar_level(fraction);
    const float side = ui::squadron_frame_side * ui_scale;
    const float x = screen[0], y = screen[1];
    const auto aligned_frame = ui::squadron_icon_rect(screen, side, identity_pixel_align_);
    const auto aligned_inner = ui::squadron_icon_rect(screen, 50.0F * 0.6F * ui_scale, identity_pixel_align_);
    identity_geometry_.push_back({entity, screen, aligned_frame, aligned_inner});
    const Rect2 frame_rect(aligned_frame.x, aligned_frame.y, aligned_frame.width, aligned_frame.height);
    // WU-21: the gripper frame tinted by the owner's colour, or the yellow select frame while
    // selected or under the pointer; the entity's icon inside it.
    const ui::Rgb tint = frame.live->player_colour(owner).value_or(ui::Rgb{255, 255, 255});
    const std::string frame_name = selected || hovered ? "i_button_unit_frame_gripper_select.tga"
                                                       : "i_button_unit_frame_gripper.tga";
    if (atlas_.is_valid()) {
        if (const auto region = atlas_region(frame_name)) {
            rendering->canvas_item_add_texture_rect_region(canvas_item, frame_rect, atlas_->get_rid(), *region,
                                                           selected || hovered ? Color(1, 1, 1, 1) : colour(tint));
        }
        if (!type.icon.empty()) {
            if (const auto region = atlas_region(type.icon)) {
                rendering->canvas_item_add_texture_rect_region(canvas_item,
                    Rect2(aligned_inner.x, aligned_inner.y, aligned_inner.width, aligned_inner.height), atlas_->get_rid(), *region);
            }
        }
    }
    const float bar_scale = ui::squadron_bar_scale * ui_scale;
    bar(canvas_item, x, y + ui::squadron_bar_offset * ui_scale, ui::bar_width(ui::BarSize::small) * bar_scale,
        ui::bar_height * bar_scale, fraction, ui::health_bar_colour(level));
    icons_.push_back({entity, skirmish::type_id(type.name), owner == frame.live->local_player(), hostile,
                      type.selectable, frame_rect.position.x, frame_rect.position.y,
                      frame_rect.position.x + frame_rect.size.x, frame_rect.position.y + frame_rect.size.y});
    icon_rows_.push_back(std::to_string(entity) + ":" + (selected ? "selected" : hovered ? "hovered" : "normal")
                         + ":" + std::to_string(level) + ":y=" + std::to_string(std::lround(y)));
}

void WorldUiView::draw_icons(const Frame& frame, const RID canvas_item, const float ui_scale) {
    icons_.clear();
    identity_geometry_.clear();
    icon_rows_.clear();
    flag_rows_.clear();
    RenderingServer* rendering = RenderingServer::get_singleton();
    const auto snapshot = frame.live->battle_frame().latest;
    const sim::tactical::PlayerId local = frame.live->local_player();
    std::erase_if(grippers_, [&](const auto& row) { return instance_of(snapshot.get(), row.first) == nullptr; });
    update_grid(frame);
    grid_icons_ = 0;
    // Retain missing draws too: grid-only samples cannot reveal a one-frame dropout.
    const auto sample_icon = [&](Gripper& gripper) -> IconSample& {
        const auto shown = frame.live->shown_frames();
        auto count = gripper.icon_sample_count;
        if (count == 0 || gripper.icon_samples[(count - 1) % gripper.icon_samples.size()].frame != shown) {
            ++gripper.icon_sample_count;
            ++count;
        }
        auto& sample = gripper.icon_samples[(count - 1) % gripper.icon_samples.size()];
        sample = {shown, frame.live->presented_tick(), {}, false, false};
        return sample;
    };
    for (auto& [squadron, gripper] : grippers_) {
        static_cast<void>(squadron);
        sample_icon(gripper);
    }
    for (const auto& [squadron, members] : frame.live->squadron_members()) {
        // WU-20: every squadron the local player sees has its icon; the pointer can grab it.
        std::vector<const ui::BattleUnit*> seen;
        for (const ui::BattleUnit& unit : *frame.units) {
            if (unit.entity == squadron && unit.part != sim::invalid_entity_id) seen.push_back(&unit);
        }
        if (seen.empty()) continue;
        Vec3 at{};
        if (const auto container = frame.live->unit_frame(squadron)) {
            at = {static_cast<float>(container->position[0]), static_cast<float>(container->position[1]),
                  static_cast<float>(container->position[2])};
        } else {
            for (const ui::BattleUnit* unit : seen) {
                for (std::size_t axis = 0; axis < 3; ++axis) at[axis] += unit->position[axis] / static_cast<float>(seen.size());
            }
        }
        const auto* container = instance_of(snapshot.get(), squadron);
        auto [anchor, inserted] = grippers_.try_emplace(squadron);
        auto& gripper = anchor->second;
        auto& icon_sample = sample_icon(gripper);
        if (inserted) gripper.motion.position = at;
        const auto cell = grid_.cell_of(squadron);
        const bool idle_grid = container != nullptr && container->squadron_idle_anchor.has_value();
        Vec3 desired = at;
        if (idle_grid) {
            const auto& idle = *container->squadron_idle_anchor;
            desired = {to_float(idle.x), to_float(idle.y), to_float(idle.z)};
        }
        std::optional<Vec3> landing;
        if (container != nullptr && container->arrival_exit) {
            const auto& exit = *container->arrival_exit;
            landing = Vec3{to_float(exit.x), to_float(exit.y), to_float(exit.z)};
        }
        const bool arrival_owns_anchor = ui::place_squadron_arrival_icon(gripper.motion, landing, at);
        std::optional<std::array<float, 2>> grid_screen;
        std::string gridded;
        if (!cell || (gripper.settled_cell && *gripper.settled_cell != *cell)) gripper.settled_cell.reset();
        if (cell && !arrival_owns_anchor) {
            // WSU-36 project policy: retain this slot and the initial anchor
            // height until the cell empties; a leave/rejoin never repacks it.
            const auto place = icon_grid_.position(*cell, squadron).value();
            const auto point = ui::combat_cell_point(*cell, {0.0F, 0.0F});
            if (const auto base = frame.project({point[0], point[1], place.height})) {
                const auto slot = place.offset;
                // WSU-36: truncate the cell projection before laying out the reference-pixel slots.
                const std::array<float, 2> target{std::trunc((*base)[0]) + slot[0] * ui_scale,
                                                 std::trunc((*base)[1]) + slot[1] * ui_scale};
                const std::array<float, 2> anchor_screen{std::trunc(target[0]),
                    std::trunc(target[1] - ui::squadron_icon_screen_offset(frame.viewport[1]))};
                if (frame.world_at_height) {
                    if (const auto world = frame.world_at_height(anchor_screen, at[2])) {
                        desired = *world;
                        if (ui::settle_squadron_icon(gripper.motion, desired, gripper_snap_distance_,
                                                    gripper.settled_cell.has_value())) {
                            gripper.settled_cell = cell;
                            grid_screen = target;
                            gridded = ":grid" + std::to_string(cell->x) + "," + std::to_string(cell->y);
                            ++grid_icons_;
                        }
                    }
                }
            }
        }
        gripper.desired = desired;
        gripper.idle = idle_grid;
        if (!arrival_owns_anchor && !inserted) {
            float fastest = 0.0F;
            float thrust = 0.0F;
            bool leader = true;
            for (const sim::EntityId member : members) {
                const auto* craft = instance_of(snapshot.get(), member);
                if (craft == nullptr) continue;
                if (leader) {
                    if (const auto type = types_.find(craft->type_id); type != types_.end()) thrust = type->second.max_thrust;
                    leader = false;
                }
                if (craft->craft_velocity_per_frame) {
                    const auto& velocity = *craft->craft_velocity_per_frame;
                    const Vector3 vector(to_float(velocity.x), to_float(velocity.y), to_float(velocity.z));
                    fastest = std::max(fastest, static_cast<float>(vector.length()));
                }
            }
            ui::slide_squadron_icon(gripper.motion, desired, thrust, fastest, idle_grid || cell.has_value(),
                                   frame.live->time().state() == ui::TimeState::fast_forward);
        }
        const bool arriving = container != nullptr && container->arrival.has_value();
        ArrivalIconSample* arrival_sample = nullptr;
        if (arriving) gripper.last_arrival_tick = snapshot->completed_tick();
        if (gripper.last_arrival_tick != 0 && snapshot->completed_tick() <= gripper.last_arrival_tick + 60) {
            // Bounded per-frame arrival evidence uses storage allocated with the identity.
            Vec3 centre{};
            for (const ui::BattleUnit* unit : seen) {
                for (std::size_t axis = 0; axis < 3; ++axis) centre[axis] += unit->position[axis] / static_cast<float>(seen.size());
            }
            if (gripper.arrival_sample_count < gripper.arrival_samples.size()) {
                gripper.arrival_samples[gripper.arrival_sample_count++] = {
                    frame.live->presented_tick(), at, centre, gripper.motion.position, arriving};
                arrival_sample = &gripper.arrival_samples[gripper.arrival_sample_count - 1];
            } else {
                ++gripper.arrival_samples_dropped;
            }
        }
        auto screen = grid_screen ? grid_screen : frame.project(gripper.motion.position);
        if (!screen) continue;
        // WSU-35/36: the sliding world anchor has the screen offset; a joined
        // dogfight icon is placed exactly at its fixed grid slot.
        if (gridded.empty()) (*screen)[1] += ui::squadron_icon_screen_offset(frame.viewport[1]);
        if ((*screen)[0] < -64.0F || (*screen)[0] > frame.viewport[0] + 64.0F
            || (*screen)[1] < -64.0F || (*screen)[1] > frame.viewport[1] + 64.0F) continue;
        if (grid_screen) {
            const double tick = frame.live->presented_tick();
            const auto count = gripper.grid_sample_count;
            if (count == 0 || gripper.grid_samples[(count - 1) % gripper.grid_samples.size()].tick != tick) {
                gripper.grid_samples[count % gripper.grid_samples.size()] = {tick, *cell, *screen};
                ++gripper.grid_sample_count;
            }
        }
        // WU-22: the squadron's health over its craft's (lost craft count as empty).
        float health = 0.0F;
        float max_health = 0.0F;
        for (const sim::EntityId member : members) {
            const auto* craft = instance_of(snapshot.get(), member);
            const auto profile_type = frame.live->unit_frame(member);
            if (craft != nullptr && craft->durability) {
                health += to_float(craft->durability->hull);
                max_health += to_float(craft->durability->max_hull);
            } else if (const sim::tactical::DurabilityTable* table = frame.live->durability()) {
                // A lost craft: its type is the squadron's member type in roster order.
                static_cast<void>(profile_type);
                for (const ui::BattleUnit* unit : seen) {
                    const auto* alive = instance_of(snapshot.get(), unit->part);
                    if (alive == nullptr) continue;
                    if (const auto* profile = table->find(alive->type_id)) max_health += to_float(profile->max_hull);
                    break;
                }
            }
        }
        const float fraction = ui::squadron_health(health, max_health);
        const float x = (*screen)[0];
        const float y = (*screen)[1];
        const sim::tactical::PlayerId owner = container != nullptr ? container->owner : local;
        // WU-43: lower-effect art is behind the frame and its identity image.
        if (container != nullptr) {
            auto ability_type = types_.find(container->type_id);
            // Match the cards: a container without an ability takes its live craft's type.
            if (ability_type != types_.end() && ability_type->second.abilities[0] == ui::ability_none) {
                const auto* craft = instance_of(snapshot.get(), seen.front()->part);
                if (craft != nullptr) ability_type = types_.find(craft->type_id);
            }
            if (ability_type != types_.end()) {
                draw_abilities(frame, canvas_item, squadron, ability_type->second, frame.live->is_ally_of_local(owner),
                               {x, y}, true, 0.0F, ui_scale);
            }
        }
        if (container != nullptr) {
            const auto identity = types_.find(container->type_id);
            if (identity != types_.end()) {
                draw_identity(frame, canvas_item, squadron, identity->second, owner, seen.front()->hostile,
                              {x, y}, fraction, ui_scale);
                icon_rows_.back() += gridded;
                icon_sample.screen = *screen;
                icon_sample.drawn = true;
                icon_sample.grid = grid_screen.has_value();
                if (arrival_sample != nullptr) arrival_sample->drawn = true;
            }
        }
        draw_group(frame, canvas_item, squadron, {x, y}, true, ui_scale);
        // WU-37, WU-38 (#632): a launched squadron's icon carries FoC's small white flag, on an
        // ally's icon only.
        if (atlas_.is_valid() && frame.live->squadron_launched(squadron) && frame.live->is_ally_of_local(owner)) {
            if (const auto region = atlas_region("i_garrison_flag.tga")) {
                const float flag_w = ui::garrison_flag_width * ui_scale;
                const float flag_h = ui::garrison_flag_height * ui_scale;
                const float flag_x = x + ui::garrison_flag_offset_x * ui_scale - flag_w * 0.5F;
                const float flag_y = y + ui::garrison_flag_offset_y * ui_scale - flag_h * 0.5F;
                rendering->canvas_item_add_texture_rect_region(canvas_item, Rect2(flag_x, flag_y, flag_w, flag_h),
                                                               atlas_->get_rid(), *region);
                flag_rows_.push_back(std::to_string(squadron) + ":x=" + std::to_string(std::lround(flag_x))
                                     + ",y=" + std::to_string(std::lround(flag_y)));
            }
        }
    }
    // WU-47/48: visible standalone heroes use the same frame, health and hit rectangle.
    // Team members already belong to their squadron identity; they never add a second icon.
    for (const ui::BattleUnit& unit : *frame.units) {
        if (unit.part != sim::invalid_entity_id || frame.live->squadron_members().contains(unit.entity)) continue;
        const auto type = types_.find(unit.type);
        if (type == types_.end() || !type->second.hero_head) continue;
        const auto* instance = instance_of(snapshot.get(), unit.entity);
        if (instance == nullptr) continue;
        auto screen = frame.project(unit.position);
        if (!screen) continue;
        (*screen)[1] += ui::squadron_icon_screen_offset(frame.viewport[1]);
        if ((*screen)[0] < -64.0F || (*screen)[0] > frame.viewport[0] + 64.0F
            || (*screen)[1] < -64.0F || (*screen)[1] > frame.viewport[1] + 64.0F) continue;
        const float health = instance->durability
            ? ui::squadron_health(to_float(instance->durability->hull), to_float(instance->durability->max_hull)) : 1.0F;
        draw_abilities(frame, canvas_item, unit.entity, type->second, frame.live->is_ally_of_local(instance->owner),
                       *screen, true, 0.0F, ui_scale);
        draw_identity(frame, canvas_item, unit.entity, type->second, instance->owner, unit.hostile,
                      *screen, health, ui_scale);
    }
    max_grid_icons_ = std::max(max_grid_icons_, grid_icons_);
    for (const std::string& row : flag_rows_) flags_seen_.insert(row.substr(0, row.find(':')));
}

} // namespace eawr::presentation::godot_backend
