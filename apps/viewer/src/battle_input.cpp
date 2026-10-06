#include "battle_input.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/space.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

constexpr std::size_t log_limit = 64;

} // namespace

BattleInput::BattleInput(Node3D& host) : host_(&host), world_ui_(std::make_unique<WorldUiView>(host)) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    Viewport* viewport = host.get_viewport();
    if (rendering == nullptr || viewport == nullptr) return;
    const Ref<World2D> world = viewport->find_world_2d();
    if (world.is_null()) return;
    canvas_item_ = rendering->canvas_item_create();
    if (canvas_item_.is_valid()) rendering->canvas_item_set_parent(canvas_item_, world->get_canvas());
}

BattleInput::~BattleInput() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering != nullptr && canvas_item_.is_valid()) rendering->free_rid(canvas_item_);
}

void BattleInput::prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const LiveSessionView& live) {
    world_ui_->prepare(filesystem, catalog, live);
    cursor_.prepare(filesystem);
}

void BattleInput::note(std::string text) {
    if (log_.size() < log_limit) log_.push_back(std::move(text));
}

ui::ScreenRect BattleInput::viewport_rect() const { return {0.0F, 0.0F, viewport_[0], viewport_[1]}; }

double BattleInput::logical_frame(const LiveSessionView& live) const {
    // G-3: pause and speed changes affect the control-group focus window.
    return live.presented_tick();
}

void BattleInput::refresh(LiveSessionView& live, const SpacePopulation& population, const SpaceEnvironment& space) {
    frame_ = space.live_camera_frame();
    if (Viewport* viewport = host_ != nullptr ? host_->get_viewport() : nullptr) {
        const Vector2 size = viewport->get_visible_rect().size;
        viewport_ = {static_cast<float>(size.x), static_cast<float>(size.y)};
    }
    live_ = &live;
    if (!pick_shapes_loaded_ && live.tables() != nullptr) {
        // #665 (WSU-11, WSU-12): the collidable meshes projectiles hit (DG-36), in model space
        // before Scale_Factor like the pick box, and the override sphere.
        for (const units::UnitType& type : live.tables()->units) {
            std::vector<std::array<ui::Vec3f, 3>> triangles;
            // OR-20: include attached hardpoint meshes in the unit pick; only a reticle names
            // a hardpoint in the order. Mesh contacts always identify the owning unit.
            for (const units::CollisionMesh& mesh : type.collision_meshes) {
                for (const auto& triangle : mesh.triangles) {
                    std::array<ui::Vec3f, 3> corners{};
                    for (std::size_t corner = 0; corner < 3; ++corner) {
                        corners[corner] = {to_float(triangle[corner].x), to_float(triangle[corner].y),
                                           to_float(triangle[corner].z)};
                    }
                    triangles.push_back(corners);
                }
            }
            PickShape shape;
            shape.mesh = ui::make_pick_mesh(std::move(triangles));
            shape.sphere_radius = to_float(type.mouse_collide_sphere_radius.value_or(sim::math::Fixed{}));
            shape.movable = type.movement.max_speed && type.movement.max_speed->raw() > 0;
            shape.selectable = type.selectable;
            shape.mouse_sensitive = type.mouse_sensitive;
            shape.locomotion = type.locomotion;
            shape.decoration = type.decoration;
            shape.community_property = type.station_community_property;
            pick_shapes_.insert_or_assign(skirmish::type_id(type.id), std::move(shape));
        }
        for (const units::ObstacleType& type : live.tables()->obstacles) {
            PickShape shape;
            shape.selectable = type.selectable;
            shape.mouse_sensitive = type.mouse_sensitive;
            pick_shapes_.insert_or_assign(skirmish::type_id(type.id), std::move(shape));
        }
        pick_shapes_loaded_ = true;
    }
    // WNO-23/42, WPR-52: consume reached ownership/replacement events before
    // deriving selection flags from the current frame's units.
    for (const auto& tick : live.battle_frame().reached) {
        if (tick.tick <= replacement_tick_) continue;
        for (const auto& event : tick.events) {
            if (event.kind == sim::tactical::EventKind::pad_captured) {
                selection_.owner_changed(event.unit, static_cast<sim::tactical::PlayerId>(event.sequence),
                    event.player, live.local_player());
            }
            if (event.kind == sim::tactical::EventKind::station_replaced)
                selection_.replace_entity(event.unit, event.sequence);
        }
        replacement_tick_ = tick.tick;
    }
    units_.clear();
    own_selection_ = false;
    movable_selection_ = false;
    const auto& squadron_of = live.squadron_of();
    for (const LiveSessionView::VisibleUnit& visible : live.visible_units()) {
        const auto box = population.live_ship_box(visible.ship);
        if (!box) continue;  // not drawn: nothing to point at
        ui::BattleUnit unit;
        unit.entity = visible.entity;
        unit.type = visible.type;
        const auto pick_shape = pick_shapes_.find(visible.type);
        if (pick_shape != pick_shapes_.end()) {
            unit.mesh = &pick_shape->second.mesh;
            unit.sphere_radius = pick_shape->second.sphere_radius;
            unit.selectable = pick_shape->second.selectable;
            unit.mouse_sensitive = pick_shape->second.mouse_sensitive;
            unit.locomotion = pick_shape->second.locomotion;
            unit.decoration = pick_shape->second.decoration;
        }
        // #424 (S-7): a craft is a pick volume of its squadron, the team container.
        if (const auto squadron = squadron_of.find(visible.entity); squadron != squadron_of.end()) {
            const auto* container = live.snapshot_index().instance(squadron->second);
            if (container == nullptr) continue;  // the squadron left the session
            unit.entity = squadron->second;
            unit.type = container->type_id;
            unit.part = visible.entity;
            unit.part_type = visible.type;
        }
        unit.neutral = visible.neutral;
        // WSU-16: authored community stations are selectable by allies and retain their owner.
        unit.own = !unit.neutral && (visible.own || (pick_shape != pick_shapes_.end()
            && pick_shape->second.community_property && live.is_ally_of_local(visible.owner)));
        // WSU-16: community property is selectable by an ally without changing ownership.
        if (const auto* instance = live.snapshot_index().instance(visible.entity)) {
            if (const auto* profile = live.economy().pads.point(instance->type_id)) {
                unit.own = unit.own || (!unit.neutral && profile->community_property && live.is_ally_of_local(visible.owner));
            }
        }
        unit.hostile = visible.hostile;
        if (const auto* point = live.economy().pads.point(visible.type);
            point != nullptr && (point->build_pad || point->community_property) && !unit.neutral
                && live.is_ally_of_local(visible.owner)) {
            unit.own = true; // WBP-08/09: allied pads enter the build UI before ordinary selection.
        }
        if (unit.own && selection_.contains(unit.entity)) {
            own_selection_ = true;
            const auto shape = pick_shapes_.find(visible.type);
            movable_selection_ = movable_selection_ || (shape != pick_shapes_.end() && shape->second.movable);
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                unit.box.model_to_world[row * 4 + column] = to_float(box->model_to_world.rows[row][column]);
            }
        }
        unit.box.low = box->low;
        unit.box.high = box->high;
        unit.position = {static_cast<float>(visible.position[0]), static_cast<float>(visible.position[1]),
                         static_cast<float>(visible.position[2])};
        // WSU-15: box/type selection queries the model origin, not the bounds centre.
        unit.screen = project(unit.position);
        units_.push_back(unit);
    }
    if (std::any_of(selection_.units().begin(), selection_.units().end(), [&](const auto id) {
        return live.pad_selection_target(id) != id;
    })) {
        auto selected = selection_.units();
        for (auto& id : selected) id = live.pad_selection_target(id);
        std::erase(selected, sim::invalid_entity_id);
        selection_.replace(selected);
    }
    selection_.retain(live.alive_units());
}

void BattleInput::cancel() noexcept {
    placing_.reset();
    left_.reset();
    right_start_.reset();
    ignore_left_release_ = false;
}

void BattleInput::observe_pointer(const Ref<InputEvent>& event) {
    if (event.is_null()) return;
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        pointer_ = {static_cast<float>(motion->get_position().x), static_cast<float>(motion->get_position().y)};
    } else if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        pointer_ = {static_cast<float>(button->get_position().x), static_cast<float>(button->get_position().y)};
    }
}

void BattleInput::frame(LiveSessionView& live, const SpacePopulation& population, SpaceEnvironment& space) {
    refresh(live, population, space);
    refresh_cards(live);
    // #848: the frame after a wheel or overview key gesture is sampled once it has drawn (below).
    const std::optional<std::uint64_t> overview_sample = overview_sample_pending_;
    overview_sample_pending_.reset();
    // The frame after a scripted middle gesture: its events reached the camera, which stepped.
    if (camera_sample_pending_ && frame_) {
        camera_samples_.push_back({*camera_sample_pending_ + " after", *frame_});
        camera_sample_pending_.reset();
    }
    follow(live, space);
    const auto& scripted = live.options().inputs;
    // A gesture fires on its tick, or with f<frame> on that shown frame (a paused battle's tick
    // holds, #459); they fire in the order given.
    const auto due = [&](const LiveSessionView::ScriptedInput& input) {
        return input.frame ? live.shown_frames() >= *input.frame
                           : static_cast<double>(input.tick) <= live.presented_tick() + 1.0e-9;
    };
    while (frame_ && next_scripted_ < scripted.size() && due(scripted[next_scripted_])) {
        // #459, #453: a hand reaches a HUD button a frame before it clicks there; Godot's GUI
        // takes a click only over a control it has already seen the pointer on.
        const LiveSessionView::ScriptedInput& input = scripted[next_scripted_];
        if ((!input.hud.empty() || input.minimap) && input.kind != "hover" && approached_ != next_scripted_ + 1U) {
            approached_ = next_scripted_ + 1U;
            const auto approach = [&]() -> std::optional<std::array<float, 2>> {
                if (input.minimap) {
                    if (!minimap_point_ || input.points.empty()) return std::nullopt;
                    return minimap_point_(input.points.front()[0], input.points.front()[1]);
                }
                return hud_point_ ? hud_point_(input.hud) : std::nullopt;
            };
            if (const auto at = approach()) {
                if (Input* engine = Input::get_singleton()) {
                    Ref<InputEventMouseMotion> motion;
                    motion.instantiate();
                    motion->set_position(Vector2((*at)[0], (*at)[1]));
                    motion->set_global_position(Vector2((*at)[0], (*at)[1]));
                    engine->parse_input_event(motion);
                    break;
                }
            }
        }
        replay(input);
        ++next_scripted_;
    }
    update_hover();
    world_ui_->service();  // WU-42: one render service
    draw();
    if (overview_sample) {
        overview_samples_.push_back({*overview_sample, space.live_camera_overview(), world_ui_->drawn(),
                                     overview_probe_ ? overview_probe_() : std::string("null")});
    }
}

void BattleInput::cursor_frame(const LiveSessionView& live, const SpaceEnvironment& space,
    const double delta, const bool capture) {
    ui::CursorInput input;
    input.dragging = left_ && left_->moved
        && (ui::drag_extent(left_->start, left_->end) > ui::minimum_drag_select_distance || selection_.empty());
    if (const auto* viewport = host_->get_viewport()) input.hud = viewport->gui_get_hovered_control() != nullptr;
    input.own_selection = own_selection_;
    input.movable_selection = movable_selection_;
    input.selectable = hovered_selectable_;
    input.hostile = hovered_hostile_;
    // CU-07, PU-31: reuse the hover ray's plane point and the authored playable bounds.
    input.passable = !pointer_ || hover_point_.has_value();
    if (hover_point_ && live.economy().bounds) {
        const auto& bounds = *live.economy().bounds;
        input.passable = (*hover_point_)[0] >= to_float(bounds[0]) && (*hover_point_)[1] >= to_float(bounds[1])
            && (*hover_point_)[0] <= to_float(bounds[2]) && (*hover_point_)[1] <= to_float(bounds[3]);
    }
    if (auto* engine = Input::get_singleton()) {
        input.ctrl = engine->is_physical_key_pressed(KEY_CTRL);
        input.alt = engine->is_physical_key_pressed(KEY_ALT);
    }
    const auto camera = space.live_camera_pointer_mode(input.ctrl);
    input.camera_pan = camera.first;
    input.camera_rotate = camera.second;
    if (const auto* orders = live.order_input()) input.mode = orders->mode();
    if (ability_target_) {
        const auto kind = sim::tactical::ability_kind(ui::ability_name(ability_target_->ability));
        const bool point_target = kind == sim::tactical::AbilityKind::barrage
            || kind == sim::tactical::AbilityKind::weaken_enemy;
        input.ability = point_target ? ui::CursorTarget::space_position : ui::CursorTarget::enemy;
        // CU-09 limitation: object abilities currently classify hostile contacts only; the
        // full ability admission query remains unwired (foc-cursors.md, Scope).
        input.ability_valid = point_target ? pointer_ && ability_point(*pointer_, live).has_value() : hovered_hostile_;
        // CU-12: movement passability must not replace the point ability's invalid pointer.
        if (point_target) input.passable = true;
    }
    if (pointer_ && !ability_target_ && !placing_ && input.mode == ui::OrderMode::none && !input.ctrl && !input.alt) {
        if (const auto reticle = world_ui_->reticle_at(*pointer_))
            input.repair = live.hardpoint_repair_allowed(reticle->entity, reticle->hardpoint);
    }
    input.placing = placing_.has_value();
    input.placement_valid = live.placement_valid();
    const std::string_view id = ui::battle_cursor(input);
    const std::string_view hover = input.hud ? std::string_view("hud") : std::string_view(cursor_hover_);
    cursor_history_.record(live.shown_frames(), id, hover);
    cursor_.update(id, delta);
    if (pointer_) cursor_.capture_overlay(*host_, Vector2((*pointer_)[0], (*pointer_)[1]), capture);
}

std::optional<sim::math::Vec3> BattleInput::ability_point(
    const std::array<float, 2> at, const LiveSessionView& live) const {
    if (!ability_target_) return std::nullopt;
    const auto aim_ray = ray(at[0], at[1]);
    const auto plane = aim_ray ? ui::battle_plane_point(*aim_ray) : std::nullopt;
    if (!plane) return std::nullopt;
    const auto x = scene::fixed_from_binary32((*plane)[0]);
    const auto y = scene::fixed_from_binary32((*plane)[1]);
    if (!x || !y) return std::nullopt;
    const sim::math::Vec3 point{x.value(), y.value(), sim::math::Fixed{}};
    const auto& frame = live.battle_frame();
    if (!frame.latest) return std::nullopt;
    const auto instances = frame.latest->instances();
    const auto kind = sim::tactical::ability_kind(ui::ability_name(ability_target_->ability));
    if (kind == sim::tactical::AbilityKind::barrage) {
        // BARR-01, CU-12: logical visibility, independent of the presentation reveal switch.
        if (frame.fog) {
            const auto& rules = frame.fog->rules;
            const auto as_double = [](const sim::math::Fixed value) {
                return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
            };
            ui::MinimapFogCells cells;
            cells.left = as_double(rules.map_left);
            cells.top = as_double(rules.map_top);
            cells.cell = as_double(rules.cell_size);
            cells.wide = rules.cells_wide;
            cells.tall = rules.cells_tall;
            cells.rows = std::shared_ptr<const std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>>(
                frame.fog, &frame.fog->values);
            return cells.revealed(as_double(point.x), as_double(point.y)) ? std::optional{point} : std::nullopt;
        }
        const auto players = frame.latest->players();
        const auto own = std::find_if(players.begin(), players.end(),
            [&](const auto& player) { return player.player_id == live.local_player(); });
        if (own == players.end()) return std::nullopt;
        for (const auto& observer : instances) {
            if (observer.team != own->team_id || !observer.reveal_range) continue;
            const sim::math::Vec3 position{observer.fixed_transform.rows[0][3],
                observer.fixed_transform.rows[1][3], observer.fixed_transform.rows[2][3]};
            if (sim::tactical::within_range(position, point, *observer.reveal_range, sim::tactical::RangeMetric::planar))
                return point;
        }
        return std::nullopt;
    }
    if (kind != sim::tactical::AbilityKind::weaken_enemy || live.tables() == nullptr) return std::nullopt;
    // U-10: retain the simulation's authored-radius gate and zero-point owner fallback.
    for (const auto entity : ability_target_->units) {
        const auto source = std::find_if(instances.begin(), instances.end(),
            [&](const auto& instance) { return instance.entity_id == entity; });
        if (source == instances.end()) continue;
        const auto& types = live.tables()->units;
        const auto type = std::find_if(types.begin(), types.end(),
            [&](const auto& unit) { return skirmish::type_id(unit.id) == source->type_id; });
        if (type == types.end()) continue;
        const auto ability = std::find_if(type->abilities.begin(), type->abilities.end(),
            [](const auto& entry) { return entry.type == "WEAKEN_ENEMY"; });
        if (ability == type->abilities.end()) continue;
        const auto reach = ability->effective_radius.value_or(sim::math::Fixed{});
        const sim::math::Vec3 position{source->fixed_transform.rows[0][3],
            source->fixed_transform.rows[1][3], source->fixed_transform.rows[2][3]};
        if (point == sim::math::Vec3{} || reach.raw() <= 0
            || sim::tactical::within_range(position, point, reach, sim::tactical::RangeMetric::planar)) return point;
    }
    return std::nullopt;
}

void BattleInput::follow(const LiveSessionView& live, SpaceEnvironment& space) {
    const auto& entries = live.options().follow_groups;
    const double tick = live.presented_tick();
    const LiveSessionView::Options::FollowGroup* active = nullptr;
    for (const auto& entry : entries) {
        if (static_cast<double>(entry.tick) <= tick + 1.0e-9) active = &entry;
    }
    if (active == nullptr) return;
    // The centre of the named units still alive; with none left the camera holds.
    double sum_x = 0.0;
    double sum_y = 0.0;
    std::size_t count = 0;
    for (const sim::EntityId entity : active->entities) {
        const auto unit = live.unit_frame(entity);
        if (!unit) continue;
        sum_x += unit->position[0];
        sum_y += unit->position[1];
        ++count;
    }
    if (count == 0) return;
    const std::array<double, 2> wanted{sum_x / static_cast<double>(count), sum_y / static_cast<double>(count)};
    if (!follow_) {
        follow_ = wanted;
    } else {
        // Eases with a time constant of 20 ticks (2/3 s of game time), so the view glides after
        // the group instead of jumping with each tick.
        const double ease = 1.0 - std::exp(-std::max(0.0, tick - follow_tick_) / 20.0);
        (*follow_)[0] += (wanted[0] - (*follow_)[0]) * ease;
        (*follow_)[1] += (wanted[1] - (*follow_)[1]) * ease;
    }
    follow_tick_ = tick;
    ++follow_moves_;
    space.live_camera_focus(static_cast<float>((*follow_)[0]), static_cast<float>((*follow_)[1]));
}

} // namespace eawr::presentation::godot_backend
