#include "battle_input.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

using Vec3 = std::array<float, 3>;

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

// #665: the centre of the unit's collision-mesh triangle nearest its box centre, in world space.
[[nodiscard]] ui::Vec3f mesh_aim_point(const ui::BattleUnit& unit) {
    const auto& box = unit.box;
    const ui::Vec3f middle{0.5F * (box.low[0] + box.high[0]), 0.5F * (box.low[1] + box.high[1]),
                           0.5F * (box.low[2] + box.high[2])};
    ui::Vec3f best{};
    float best_distance = std::numeric_limits<float>::infinity();
    for (const auto& triangle : unit.mesh->triangles) {
        ui::Vec3f centre{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            centre[axis] = (triangle[0][axis] + triangle[1][axis] + triangle[2][axis]) / 3.0F;
        }
        const float dx = centre[0] - middle[0];
        const float dy = centre[1] - middle[1];
        const float dz = centre[2] - middle[2];
        const float distance = dx * dx + dy * dy + dz * dz;
        if (distance < best_distance) {
            best_distance = distance;
            best = centre;
        }
    }
    const auto& m = box.model_to_world;
    return {m[0] * best[0] + m[1] * best[1] + m[2] * best[2] + m[3], m[4] * best[0] + m[5] * best[1] + m[6] * best[2] + m[7],
            m[8] * best[0] + m[9] * best[1] + m[10] * best[2] + m[11]};
}

[[nodiscard]] Vec3 subtract(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] std::optional<Vec3> normalized(const Vec3& value) {
    const float length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length < 1.0e-6F) return std::nullopt;
    return Vec3{value[0] / length, value[1] / length, value[2] / length};
}

// The render basis is source (x, z, -y).
[[nodiscard]] Vec3 render_from_source(const Vec3& source) { return {source[0], source[2], -source[1]}; }
[[nodiscard]] Vec3 source_from_render(const Vec3& render) { return {render[0], -render[2], render[1]}; }

// The camera's view basis: forward, right and up, render basis.
struct ViewBasis final {
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    float tan_half{};
};

[[nodiscard]] std::optional<ViewBasis> basis(const camera::TacticalFrame& frame) {
    const auto forward = normalized(subtract(frame.target, frame.eye));
    if (!forward) return std::nullopt;
    const auto right = normalized(cross(*forward, frame.up));
    if (!right) return std::nullopt;
    constexpr float degrees = 3.14159265358979323846F / 180.0F;
    const float tan_half = std::tan(frame.vertical_fov_degrees * 0.5F * degrees);
    if (!std::isfinite(tan_half) || tan_half <= 0.0F) return std::nullopt;
    return ViewBasis{*forward, *right, cross(*right, *forward), tan_half};
}

[[nodiscard]] ui::Modifiers modifiers_of(const InputEventWithModifiers& event) {
    return ui::Modifiers{event.is_shift_pressed(), event.is_ctrl_pressed(), event.is_alt_pressed()};
}

[[nodiscard]] std::string json(const std::string& text) {
    std::string result{"\""};
    for (const char character : text) {
        if (character == '"' || character == '\\') result += '\\';
        result += static_cast<unsigned char>(character) < 0x20U ? ' ' : character;
    }
    return result + "\"";
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
}

void BattleInput::note(std::string text) {
    if (log_.size() < log_limit) log_.push_back(std::move(text));
}

ui::ScreenRect BattleInput::viewport_rect() const { return {0.0F, 0.0F, viewport_[0], viewport_[1]}; }

double BattleInput::now(const LiveSessionView& live) const {
    if (live.options().real_time) {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<double>(clock::now() - start).count();
    }
    // A driven run times the control-group double tap on the presented tick, so it repeats.
    return live.presented_tick() / static_cast<double>(sim::tactical::logical_frames_per_second);
}

std::optional<ui::PickRay> BattleInput::ray(const float x, const float y) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const auto view = basis(*frame_);
    if (!view) return std::nullopt;
    const float aspect = viewport_[0] / viewport_[1];
    const float ndc_x = 2.0F * x / viewport_[0] - 1.0F;
    const float ndc_y = 1.0F - 2.0F * y / viewport_[1];
    Vec3 direction{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        direction[axis] = view->forward[axis] + view->right[axis] * ndc_x * view->tan_half * aspect
            + view->up[axis] * ndc_y * view->tan_half;
    }
    return ui::PickRay{source_from_render(frame_->eye), source_from_render(direction)};
}

std::optional<std::array<float, 2>> BattleInput::project(const ui::Vec3f& source) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const auto view = basis(*frame_);
    if (!view) return std::nullopt;
    const Vec3 offset = subtract(render_from_source(source), frame_->eye);
    const float depth = dot(offset, view->forward);
    if (!(depth > frame_->near_plane)) return std::nullopt;
    const float aspect = viewport_[0] / viewport_[1];
    const float x = dot(offset, view->right) / (depth * view->tan_half * aspect);
    const float y = dot(offset, view->up) / (depth * view->tan_half);
    return std::array<float, 2>{(x + 1.0F) * 0.5F * viewport_[0], (1.0F - y) * 0.5F * viewport_[1]};
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
            pick_shapes_.insert_or_assign(skirmish::type_id(type.id), std::move(shape));
        }
        pick_shapes_loaded_ = true;
    }
    units_.clear();
    const auto& squadron_of = live.squadron_of();
    const auto latest = live.battle_frame().latest;
    for (const LiveSessionView::VisibleUnit& visible : live.visible_units()) {
        const auto box = population.live_ship_box(visible.ship);
        if (!box) continue;  // not drawn: nothing to point at
        ui::BattleUnit unit;
        unit.entity = visible.entity;
        unit.type = visible.type;
        if (const auto shape = pick_shapes_.find(visible.type); shape != pick_shapes_.end()) {
            unit.mesh = &shape->second.mesh;
            unit.sphere_radius = shape->second.sphere_radius;
        }
        // #424 (S-7): a craft is a pick volume of its squadron, the team container.
        if (const auto squadron = squadron_of.find(visible.entity); squadron != squadron_of.end()) {
            const auto instances = latest ? latest->instances() : std::span<const sim::tactical::TacticalInstance>{};
            const auto container = std::find_if(instances.begin(), instances.end(),
                [&](const sim::tactical::TacticalInstance& instance) { return instance.entity_id == squadron->second; });
            if (container == instances.end()) continue;  // the squadron left the session
            unit.entity = squadron->second;
            unit.type = container->type_id;
            unit.part = visible.entity;
            unit.part_type = visible.type;
        }
        unit.own = visible.own;
        unit.hostile = visible.hostile;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                unit.box.model_to_world[row * 4 + column] = to_float(box->model_to_world.rows[row][column]);
            }
        }
        unit.box.low = box->low;
        unit.box.high = box->high;
        unit.position = {static_cast<float>(visible.position[0]), static_cast<float>(visible.position[1]),
                         static_cast<float>(visible.position[2])};
        Vec3 centre{};
        const auto& m = unit.box.model_to_world;
        for (std::size_t row = 0; row < 3; ++row) {
            centre[row] = m[row * 4] * 0.5F * (box->low[0] + box->high[0])
                + m[row * 4 + 1] * 0.5F * (box->low[1] + box->high[1])
                + m[row * 4 + 2] * 0.5F * (box->low[2] + box->high[2]) + m[row * 4 + 3];
        }
        unit.screen = project(centre);
        units_.push_back(unit);
    }
    selection_.retain(live.alive_units());
}

bool BattleInput::input(const Ref<InputEvent>& event, LiveSessionView& live, const SpacePopulation& population,
                        SpaceEnvironment& space) {
    if (event.is_null()) return false;
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        pointer_ = {static_cast<float>(motion->get_position().x), static_cast<float>(motion->get_position().y)};
        if (left_) {
            left_->end = {static_cast<float>(motion->get_position().x), static_cast<float>(motion->get_position().y)};
            left_->moved = true;
        }
        return false;  // the camera keeps its edge scroll and drags
    }
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        const auto index = button->get_button_index();
        if (index != MOUSE_BUTTON_LEFT && index != MOUSE_BUTTON_RIGHT) return false;
        ++events_;
        refresh(live, population, space);
        const std::array<float, 2> at{static_cast<float>(button->get_position().x),
                                      static_cast<float>(button->get_position().y)};
        const ui::Modifiers modifiers = modifiers_of(*button);
        if (index == MOUSE_BUTTON_LEFT) {
            if (button->is_pressed()) {
                if (button->is_double_click()) {
                    // FoC: the double click selects the type on screen and its release is spent.
                    // #550 (WU-23a, WSU-38): an own squadron's icon takes it before the world under
                    // it and selects every own squadron with a craft of its leader's type on screen.
                    if (const auto icon = world_ui_->icon_at(at)) {
                        const auto leader = own_squadron_leader_type(*icon);
                        if (leader && selection_.craft_type_on_screen(*leader, units_, viewport_rect())) {
                            note("double click: craft type on screen");
                            acknowledge(Acknowledgement::Kind::select);
                        } else {
                            note(leader ? "double click: no new squadron on screen" : "double click: not an own squadron");
                        }
                    } else {
                        const auto picked = ray(at[0], at[1]) ? ui::pick_unit(*ray(at[0], at[1]), units_) : std::nullopt;
                        if (const auto pick_ray = ray(at[0], at[1])) record_pick_candidates(*pick_ray);
                        if (selection_.double_click(picked, units_, viewport_rect())) {
                            note("double click: type on screen");
                            acknowledge(Acknowledgement::Kind::select);
                        }
                    }
                    ignore_left_release_ = true;
                    left_.reset();
                } else {
                    left_ = Drag{at, at, false};
                }
            } else {
                left_release(at, modifiers, live);
            }
        } else if (button->is_pressed()) {
            right_start_ = at;
        } else {
            right_release(at, modifiers, live);
        }
        refresh_cards(live);
        draw();
        return true;
    }
    if (const auto* pressed_key = Object::cast_to<InputEventKey>(event.ptr())) {
        if (!pressed_key->is_pressed() || pressed_key->is_echo()) return false;
        const Key code = pressed_key->get_physical_keycode() != KEY_NONE ? pressed_key->get_physical_keycode()
                                                                          : pressed_key->get_keycode();
        refresh(live, population, space);
        const bool taken = key(static_cast<std::int64_t>(code), modifiers_of(*pressed_key), live, space);
        if (taken) {
            refresh_cards(live);
            ++events_;
            draw();
        }
        return taken;
    }
    return false;
}

void BattleInput::record_pick_candidates(const ui::PickRay& pick_ray) {
    std::vector<PickCandidate> candidates;
    for (const ui::BattleUnit& unit : units_) {
        const auto contact = ui::pick_contact(pick_ray, unit);
        const auto box = ui::ray_box_contact(pick_ray, unit.box);
        if (!contact && !box) continue;
        PickCandidate candidate{unit.entity, unit.part};
        if (contact) {
            ui::BattleUnit geometry = unit;
            geometry.sphere_radius = 0.0F;
            const bool mesh = unit.mesh != nullptr && !unit.mesh->triangles.empty();
            candidate.contact_z = (*contact)[2];
            candidate.volume = ui::pick_contact(pick_ray, geometry) ? (mesh ? "mesh" : "box") : "sphere";
        }
        if (box) candidate.box_z = (*box)[2];
        candidates.push_back(candidate);
    }
    double_click_candidates_.push_back(std::move(candidates));
}

std::optional<sim::tactical::TypeId> BattleInput::own_squadron_leader_type(const sim::EntityId squadron) const {
    const auto icons = world_ui_->icon_units();
    const auto icon = std::find_if(icons.begin(), icons.end(), [&](const ui::BattleUnit& unit) { return unit.entity == squadron; });
    if (icon == icons.end() || !icon->own) return std::nullopt;
    if (live_ != nullptr) {
        const auto latest = live_->battle_frame().latest;
        const auto members = live_->squadron_members().find(squadron);
        if (latest && members != live_->squadron_members().end()) {
            const auto instances = latest->instances();
            for (const sim::EntityId member : members->second) {
                const auto craft = std::find_if(instances.begin(), instances.end(),
                    [&](const sim::tactical::TacticalInstance& instance) { return instance.entity_id == member; });
                if (craft != instances.end()) return craft->type_id;
            }
        }
    }
    return icon->type;
}

void BattleInput::left_release(const std::array<float, 2> at, const ui::Modifiers modifiers, LiveSessionView& live) {
    if (ignore_left_release_) {
        ignore_left_release_ = false;
        left_.reset();
        return;
    }
    if (!left_) return;
    const Drag drag = *left_;
    left_.reset();
    if (drag.moved && ui::drag_extent(drag.start, at) > ui::minimum_drag_select_distance) {
        ++boxes_;
        if (selection_.box(ui::drag_rect(drag.start, at), modifiers.shift, units_)) {
            note("box select");
            acknowledge(Acknowledgement::Kind::select);
        }
        return;
    }
    const auto pick_ray = ray(at[0], at[1]);
    auto picked = pick_ray ? ui::pick_unit(*pick_ray, units_) : std::nullopt;
    // WU-23: a squadron icon takes the click before the world under it.
    if (const auto icon = world_ui_->icon_at(at)) picked = *icon;
    if (ability_target_) {
        // #561 (AB-11): the click aims the waiting targeted ability; the selection stays.
        const auto unit = std::find_if(units_.begin(), units_.end(),
            [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
        if (unit == units_.end() || !unit->hostile) {
            cancel_ability_target(unit == units_.end() ? "empty space" : "not an enemy");
            return;
        }
        auto request = *ability_target_;
        ability_target_.reset();
        // A squadron is aimed at through the craft clicked (space-abilities AB-62).
        request.target = unit->part != sim::invalid_entity_id ? unit->part : unit->entity;
        ++ability_targeted_;
        note("ability " + std::string(ui::ability_name(request.ability)) + " at " + std::to_string(request.target));
        if (ability_commands_ != nullptr) ability_commands_->request(request);
        acknowledge(Acknowledgement::Kind::attack);
        return;
    }
    ui::OrderInput* input = live.order_input();
    if (input != nullptr && input->mode() != ui::OrderMode::none) {
        // An armed attack or move mode: a click on empty space or an own unit disarms it.
        const auto unit = std::find_if(units_.begin(), units_.end(),
            [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
        if (!picked || (unit != units_.end() && unit->own)) input->cancel_mode();
        if (!picked) return;
    }
    if (selection_.click(picked, modifiers, units_, viewport_rect())) {
        note(picked ? "click select " + std::to_string(*picked) : std::string("click: deselect"));
        acknowledge(Acknowledgement::Kind::select);
    }
}

void BattleInput::right_release(const std::array<float, 2> at, const ui::Modifiers modifiers, LiveSessionView& live) {
    const auto start = right_start_;
    right_start_.reset();
    // #561 (AB-11): a right click cancels a waiting targeted ability and orders nothing.
    if (ability_target_) {
        cancel_ability_target("right click");
        return;
    }
    switch (ui::right_release(start, at)) {
    case ui::RightRelease::ignored:
        return;  // the press was cancelled (focus loss, pointer exit) or never seen
    case ui::RightRelease::compass:
        // FoC's right drag sets a facing (the compass); the rules have no move-with-facing command.
        note("right drag: compass facing not modelled");
        return;
    case ui::RightRelease::click:
        break;
    }
    ui::OrderInput* input = live.order_input();
    if (input == nullptr || selection_.empty()) return;
    const auto pick_ray = ray(at[0], at[1]);
    if (!pick_ray) return;
    auto picked = ui::pick_unit(*pick_ray, units_);
    // #553 (WU-23b): a squadron icon takes the right click before the world under it, as it takes
    // the left one; the order point stays the battle plane point under the cursor (P-3).
    if (const auto icon = world_ui_->icon_at(at)) picked = *icon;
    const auto unit = std::find_if(units_.begin(), units_.end(),
        [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
    const ui::BattleUnit* over = unit == units_.end() ? nullptr : &*unit;
    const ui::OrderMode mode = input->mode();
    const bool over_selected = over != nullptr && selection_.contains(over->entity);
    // OR-01: Ctrl attack-moves and Ctrl with Alt guards, even over a unit a plain click ignores.
    const bool guarding = mode == ui::OrderMode::guard || (mode == ui::OrderMode::none && modifiers.ctrl && modifiers.alt);
    const bool attack_moving = mode == ui::OrderMode::attack_move || (mode == ui::OrderMode::none && modifiers.ctrl && !modifiers.alt);
    const auto click = guarding || attack_moving ? ui::RightClick::order : ui::right_click(mode, over, over_selected);
    switch (click) {
    case ui::RightClick::nothing:
        return;
    case ui::RightClick::disarm:
        input->cancel_mode();
        note("attack mode cancelled");
        return;
    case ui::RightClick::order:
        break;
    }
    const auto point = ui::battle_plane_point(*pick_ray);
    if (!point) return;
    auto x = scene::fixed_from_binary32((*point)[0]);
    auto y = scene::fixed_from_binary32((*point)[1]);
    if (!x || !y) return;
    ui::WorldPick pick{{x.value(), y.value(), sim::math::Fixed{}}, over ? over->entity : sim::invalid_entity_id,
                       over != nullptr && over->hostile, over != nullptr && over->own, over_selected};
    input->set_selection(selection_.units());
    const std::uint64_t tick = live.order_tick();
    auto issued = input->world_command(pick, ui::CommandOrigin::world_click, ui::OrderModifiers{modifiers.ctrl, modifiers.alt});
    if (issued && issued.value()) {
        ++orders_;
        std::string units;
        for (const sim::EntityId entity : selection_.units()) units += (units.empty() ? "" : ",") + std::to_string(entity);
        char where[96];
        std::snprintf(where, sizeof(where), "%.9g,%.9g,0", static_cast<double>((*point)[0]), static_cast<double>((*point)[1]));
        // Move mode moves even onto an enemy (O-3).
        const bool attacked = pick.hostile && mode != ui::OrderMode::move;
        std::string what = "move @" + std::string(where);
        if (attacked) {
            what = "attack " + std::to_string(pick.entity);
        } else if (guarding) {
            what = pick.entity != sim::invalid_entity_id && pick.own && !over_selected
                ? "guard " + std::to_string(pick.entity) : "guard @" + std::string(where);
        } else if (attack_moving) {
            what = "attack-move @" + std::string(where);
        }
        note(what + " units " + units + " tick " + std::to_string(tick));
        // BA-20 to BA-23: an attack-move or guard is a move with a flag (OR-10, OR-16) and speaks as
        // a move (unverified for FoC's audio).
        acknowledge(attacked ? Acknowledgement::Kind::attack : Acknowledgement::Kind::move);
    } else if (!issued) {
        ++refused_;
        note("refused: " + issued.error().message);
    }
}

std::optional<std::array<std::array<double, 2>, 4>> BattleInput::ground_corners(const double height) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const std::array<std::array<float, 2>, 4> corners{
        {{0.0F, 0.0F}, {viewport_[0], 0.0F}, {viewport_[0], viewport_[1]}, {0.0F, viewport_[1]}}};
    std::array<std::array<double, 2>, 4> out{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto corner_ray = ray(corners[index][0], corners[index][1]);
        if (!corner_ray) return std::nullopt;
        const auto& origin = corner_ray->origin;
        const auto& direction = corner_ray->direction;
        const double length = std::sqrt(static_cast<double>(direction[0]) * direction[0]
                                        + static_cast<double>(direction[1]) * direction[1]
                                        + static_cast<double>(direction[2]) * direction[2]);
        if (!(length > 0.0)) return std::nullopt;
        const double t = std::abs(direction[2]) > 1.0e-9F ? (height - origin[2]) / direction[2] : -1.0;
        // MM-09: a ray that never reaches the plane gives the far end of the view instead.
        const double reach = t > 0.0 ? t : static_cast<double>(frame_->far_plane) / length;
        out[index] = {origin[0] + reach * direction[0], origin[1] + reach * direction[1]};
    }
    return out;
}

bool BattleInput::minimap_move(const double x, const double y, LiveSessionView& live) {
    ui::OrderInput* input = live.order_input();
    if (input == nullptr || selection_.empty()) return false;
    auto fixed_x = scene::fixed_from_binary32(static_cast<float>(x));
    auto fixed_y = scene::fixed_from_binary32(static_cast<float>(y));
    if (!fixed_x || !fixed_y) return false;
    const ui::WorldPick pick{{fixed_x.value(), fixed_y.value(), sim::math::Fixed{}}, sim::invalid_entity_id, false};
    input->set_selection(selection_.units());
    const std::uint64_t tick = live.order_tick();
    auto issued = input->world_command(pick, ui::CommandOrigin::minimap);
    if (!issued) {
        ++refused_;
        note("minimap refused: " + issued.error().message);
        return false;
    }
    if (!issued.value()) return false;
    ++orders_;
    char where[96];
    std::snprintf(where, sizeof(where), "%.9g,%.9g,0", x, y);
    note("minimap move @" + std::string(where) + " tick " + std::to_string(tick));
    acknowledge(Acknowledgement::Kind::move);
    return true;
}

bool BattleInput::key(const std::int64_t code, const ui::Modifiers modifiers, LiveSessionView& live,
                      SpaceEnvironment& space) {
    ui::OrderInput* input = live.order_input();
    // #561 (AB-11): Esc cancels a waiting targeted ability.
    if (ability_target_ && code == static_cast<std::int64_t>(KEY_ESCAPE)) {
        cancel_ability_target("escape");
        return true;
    }
    if (code >= static_cast<std::int64_t>(KEY_0) && code <= static_cast<std::int64_t>(KEY_9)) {
        // G-1 (foc-battle-selection): n selects group n, Ctrl assigns, Shift adds the
        // group to the selection, Alt adds the selection to the group.
        const auto group = static_cast<std::size_t>(code - static_cast<std::int64_t>(KEY_0));
        std::optional<ui::Vec3f> focus;
        if (modifiers.ctrl) {
            selection_.assign_group(group);
            note("assign group " + std::to_string(group));
        } else if (modifiers.alt) {
            focus = selection_.add_to_group(group, now(live), units_);
            note("add to group " + std::to_string(group));
            acknowledge(Acknowledgement::Kind::select);
        } else {
            focus = selection_.recall_group(group, modifiers.shift, now(live), units_);
            note((modifiers.shift ? "add group " : "select group ") + std::to_string(group));
            acknowledge(Acknowledgement::Kind::select);
        }
        if (focus) {
            ++focuses_;
            space.live_camera_focus((*focus)[0], (*focus)[1]);
            note("focus group " + std::to_string(group));
        }
        return true;
    }
    // #454 AB-10: FoC's default ability keys press the selection's buttons of that ability; a key no
    // shown button takes goes on to the other bindings.
    const auto key_char = [&]() -> std::optional<char> {
        if (code >= static_cast<std::int64_t>(KEY_A) && code <= static_cast<std::int64_t>(KEY_Z)) {
            return static_cast<char>('A' + (code - static_cast<std::int64_t>(KEY_A)));
        }
        switch (code) {
        case KEY_BRACKETLEFT: return '[';
        case KEY_BRACKETRIGHT: return ']';
        case KEY_SEMICOLON: return ';';
        case KEY_COMMA: return ',';
        case KEY_PERIOD: return '.';
        case KEY_SLASH: return '/';
        default: return std::nullopt;
        }
    };
    if (const auto letter = key_char()) {
        if (const auto ability = ui::ability_hotkey(*letter, modifiers)) {
            const bool shown = std::any_of(ability_bar_.buttons.begin(), ability_bar_.buttons.end(),
                [&](const ui::AbilityButton& button) { return button.ability == *ability; });
            if (shown) {
                ++ability_hotkeys_;
                note("ability key " + std::string(ui::ability_name(*ability)));
                static_cast<void>(press_ability(*ability, live));
                return true;
            }
        }
    }
    if (modifiers.ctrl || modifiers.alt || modifiers.shift) return false;
    if (code == static_cast<std::int64_t>(KEY_S)) {
        if (input == nullptr) return true;
        input->set_selection(selection_.units());
        if (selection_.empty()) return true;
        if (auto stopped = input->stop(ui::CommandOrigin::hotkey); stopped) {
            ++orders_;
            note("stop");
        } else {
            ++refused_;
        }
        return true;
    }
    // OR-01: the default keys are A attack, M move, T attack-move and G guard; the key
    // of the armed mode disarms it, another key replaces it (OR-01).
    if (code == static_cast<std::int64_t>(KEY_A) || code == static_cast<std::int64_t>(KEY_M)
        || code == static_cast<std::int64_t>(KEY_T) || code == static_cast<std::int64_t>(KEY_G)) {
        if (input == nullptr) return true;
        ui::OrderMode mode = ui::OrderMode::move;
        const char* name = "move mode";
        if (code == static_cast<std::int64_t>(KEY_A)) {
            mode = ui::OrderMode::attack;
            name = "attack mode";
        } else if (code == static_cast<std::int64_t>(KEY_T)) {
            mode = ui::OrderMode::attack_move;
            name = "attack-move mode";
        } else if (code == static_cast<std::int64_t>(KEY_G)) {
            mode = ui::OrderMode::guard;
            name = "guard mode";
        }
        if (input->mode() == mode) input->cancel_mode();
        else input->arm(mode);
        note(name);
        return true;
    }
    if (code == static_cast<std::int64_t>(KEY_INSERT)) {
        space.live_camera_overview_key();
        note("overview " + space.live_camera_overview());
        return true;
    }
    return false;
}

void BattleInput::update_hover() {
    hovered_.reset();
    hovered_icon_.reset();
    if (!pointer_) return;
    if (const auto icon = world_ui_->icon_at(*pointer_)) {
        hovered_icon_ = icon;
        return;
    }
    if (const auto pick_ray = ray((*pointer_)[0], (*pointer_)[1])) hovered_ = ui::pick_index(*pick_ray, units_);
}

void BattleInput::acknowledge(const Acknowledgement::Kind kind) {
    // FoC speaks only for a selection that holds units (Set_Selected_Objects_List, the group
    // acknowledgements' highest ranking object).
    if (selection_.empty()) return;
    acknowledgements_.push_back({kind, selection_.units()});
}

std::vector<BattleInput::Acknowledgement> BattleInput::take_acknowledgements() {
    return std::exchange(acknowledgements_, {});
}

void BattleInput::refresh_cards(const LiveSessionView& live) {
    card_units_.clear();
    card_layout_ = {};
    const auto& latest = live.battle_frame().latest;
    const units::UnitTables* tables = live.tables();
    // #534: the ability bar mirrors the cards; an empty layout (deselect, the selection's last unit
    // dying or leaving a group) must clear it too, or its last button lingers over an empty panel.
    if (card_slots_ == 0 || selection_.empty() || !latest || tables == nullptr) {
        ability_bar_ = {};
        return;
    }
    std::map<sim::tactical::TypeId, const units::UnitType*> types;
    for (const units::UnitType& type : tables->units) types.emplace(skirmish::type_id(type.id), &type);
    std::map<sim::EntityId, const sim::tactical::TacticalInstance*> instances;
    for (const sim::tactical::TacticalInstance& instance : latest->instances()) instances.emplace(instance.entity_id, &instance);
    const auto type_of = [&](const sim::tactical::TacticalInstance& instance) -> const units::UnitType* {
        const auto found = types.find(instance.type_id);
        return found == types.end() ? nullptr : found->second;
    };
    const auto ability_of = [](const units::UnitType* type) {
        return type == nullptr || type->abilities.empty() ? ui::ability_none : ui::ability_index(type->abilities.front().type);
    };
    // #454: the type's second unit ability, which gets a second button.
    const auto second_ability_of = [](const units::UnitType* type) {
        return type == nullptr || type->abilities.size() < 2 ? ui::ability_none : ui::ability_index(type->abilities[1].type);
    };
    // Get_Health_Percent: the hull over its maximum (FoC's hardpoint-weighted display health is on
    // the fidelity list).
    const auto health_of = [](const sim::tactical::TacticalInstance& instance) {
        if (!instance.durability || instance.durability->max_hull.raw() <= 0) return 1.0;
        return std::clamp(static_cast<double>(instance.durability->hull.raw())
                              / static_cast<double>(instance.durability->max_hull.raw()), 0.0, 1.0);
    };
    // #435: the selection holds a squadron as its team container (#424). The card model reads a
    // squadron through its craft, so a container stands for its live craft here, and every card's
    // members map back to the container below: one card per homogeneous squadron, whose click
    // selects the squadron as one unit.
    std::vector<sim::EntityId> shown;
    for (const sim::EntityId entity : selection_.units()) {
        const auto squadron = live.squadron_members().find(entity);
        if (squadron == live.squadron_members().end()) {
            shown.push_back(entity);
            continue;
        }
        for (const sim::EntityId craft : squadron->second) {
            if (instances.contains(craft)) shown.push_back(craft);
        }
    }
    std::vector<ui::SelectedUnit> selected;
    for (const sim::EntityId entity : shown) {
        const auto instance = instances.find(entity);
        if (instance == instances.end()) continue;
        const units::UnitType* type = type_of(*instance->second);
        ui::SelectedUnit unit;
        unit.entity = entity;
        unit.type = type != nullptr ? type->id : std::to_string(instance->second->type_id);
        unit.ability = ability_of(type);
        unit.second_ability = second_ability_of(type);
        unit.health = health_of(*instance->second);
        const auto& durability = instance->second->durability;
        if (type != nullptr && type->shielded && durability && durability->shields && durability->max_shields
            && durability->max_shields->raw() > 0) {
            unit.shield = std::clamp(static_cast<double>(durability->shields->raw())
                                         / static_cast<double>(durability->max_shields->raw()), 0.0, 1.0);
        }
        if (const sim::tactical::Squadron* squadron = live.squadron_of(entity)) {
            const auto container = instances.find(squadron->container);
            const units::UnitType* squadron_type = container == instances.end() ? nullptr : type_of(*container->second);
            if (squadron_type != nullptr) {
                ui::SquadronOf team;
                team.container = squadron->container;
                team.type = squadron_type->id;
                team.ability = ability_of(squadron_type);
                team.second_ability = second_ability_of(squadron_type);
                if (team.ability == ui::ability_none) {
                    team.ability = unit.ability;
                    team.second_ability = unit.second_ability;
                }
                team.homogeneous = std::all_of(squadron_type->members.begin(), squadron_type->members.end(),
                    [&](const units::SquadronMember& member) { return member.craft == squadron_type->members.front().craft; });
                // L-9 (foc-unit-cards): a squadron's health is the mean health of the craft still standing.
                double sum = 0.0;
                for (const sim::EntityId craft : squadron->members) {
                    const auto live_craft = instances.find(craft);
                    if (live_craft == instances.end()) continue;
                    team.members.push_back(craft);
                    sum += health_of(*live_craft->second);
                }
                team.health = team.members.empty() ? 0.0 : sum / static_cast<double>(team.members.size());
                unit.squadron = std::move(team);
            }
        }
        selected.push_back(std::move(unit));
    }
    card_units_ = ui::card_units(selected);
    const auto& container_of = live.squadron_of();
    for (ui::CardUnit& unit : card_units_) {
        std::vector<sim::EntityId> members;
        for (const sim::EntityId member : unit.members) {
            const auto squadron = container_of.find(member);
            const sim::EntityId selectable = squadron != container_of.end() ? squadron->second : member;
            if (std::find(members.begin(), members.end(), selectable) == members.end()) members.push_back(selectable);
        }
        unit.members = std::move(members);
    }
    card_layout_ = ui::layout_unit_cards(card_units_, card_slots_);
    refresh_abilities(live);
}

void BattleInput::refresh_abilities(const LiveSessionView& live) {
    ready_abilities_.set_units(card_units_);
    ability_demo_ = live.options().ability_demo;
    std::vector<ui::ReadyAbilities::Staged> staged;
    if (live.options().ability_demo) {
        // --eawr-live-ability-demo: the first four ability groups in their demo states.
        const std::array<ui::UnitAbilityState, 4> demo{{{ui::AbilityStatus::active, 0.6, false},
                                                        {ui::AbilityStatus::recharging, 0.35, true},
                                                        {ui::AbilityStatus::disabled, 1.0, false},
                                                        {ui::AbilityStatus::ready, 1.0, true}}};
        std::size_t rank = 0;
        for (std::uint32_t ability = 1; ability < ui::ability_count && rank < demo.size(); ++ability) {
            if (card_layout_.by_ability[ability].empty()) continue;
            for (const std::size_t index : card_layout_.by_ability[ability]) {
                staged.push_back({card_units_[index].id, ability, demo[rank]});
            }
            ++rank;
        }
    }
    ready_abilities_.stage(std::move(staged));
    const ui::AbilityState& state = ability_state_ != nullptr ? *ability_state_ : ready_abilities_;
    ability_bar_ = card_slots_ == 0 ? ui::AbilityBar{} : ui::ability_bar(card_layout_, card_units_, state, nullptr);
}

void BattleInput::NoteCommands::request(const ui::AbilityRequest& request) {
    std::string units;
    for (const sim::EntityId unit : request.units) units += (units.empty() ? "" : ",") + std::to_string(unit);
    owner->note("ability " + std::string(ui::to_string(request.kind)) + " " + std::string(ui::ability_name(request.ability))
                + (request.targeted ? " (targeted)" : "") + " units " + units + ": no-op until the simulation has abilities (#76)");
}

bool BattleInput::ability_click(const std::size_t index, const bool right, const LiveSessionView& live) {
    refresh_cards(live);
    if (index >= ability_bar_.buttons.size()) return false;
    const ui::AbilityState& state = ability_state_ != nullptr ? *ability_state_ : ready_abilities_;
    const auto request = ui::ability_click(ability_bar_.buttons[index], right, state);
    if (!request) {
        note("ability " + std::string(ui::ability_name(ability_bar_.buttons[index].ability)) + ": nothing can take it");
        return false;
    }
    ++ability_requests_;
    note_commands_.owner = this;
    // #561 (AB-11): a targeted activation first takes its target from the next world click.
    if (request->targeted && request->kind == ui::AbilityRequest::Kind::activate && ability_commands_ != nullptr) {
        ability_target_ = *request;
        note("ability " + std::string(ui::ability_name(request->ability)) + ": pick a target");
        return true;
    }
    (ability_commands_ != nullptr ? *ability_commands_ : static_cast<ui::AbilityCommands&>(note_commands_)).request(*request);
    return true;
}

void BattleInput::cancel_ability_target(const char* why) {
    if (!ability_target_) return;
    ++ability_target_cancels_;
    note("ability " + std::string(ui::ability_name(ability_target_->ability)) + " targeting cancelled: " + why);
    ability_target_.reset();
}

bool BattleInput::press_ability(const std::uint32_t ability, const LiveSessionView& live) {
    // AB-10: the key presses every shown button of the ability.
    refresh_cards(live);
    bool pressed = false;
    for (std::size_t index = 0; index < ability_bar_.buttons.size(); ++index) {
        if (ability_bar_.buttons[index].ability != ability) continue;
        pressed = ability_click(index, false, live) || pressed;
    }
    return pressed;
}

bool BattleInput::card_click(const std::size_t slot, const bool shift, const LiveSessionView& live) {
    // A world gesture or another card click may have changed selection since the last frame.
    refresh_cards(live);
    const auto next = ui::card_click(card_layout_, card_units_, slot, shift, selection_.units());
    if (!next) return false;
    ++card_clicks_;
    const bool changed = selection_.replace(*next);
    refresh_cards(live);
    note((shift ? "card deselect " : "card select ") + std::to_string(slot) + ": " + std::to_string(next->size())
         + " selected");
    draw();
    return changed;
}

void BattleInput::cancel() noexcept {
    left_.reset();
    right_start_.reset();
    ignore_left_release_ = false;
}

void BattleInput::frame(LiveSessionView& live, const SpacePopulation& population, SpaceEnvironment& space) {
    refresh(live, population, space);
    refresh_cards(live);
    if (overview_sample_pending_) {
        overview_samples_.push_back({*overview_sample_pending_, space.live_camera_overview()});
        overview_sample_pending_.reset();
    }
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
    draw();
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

void BattleInput::replay(const LiveSessionView::ScriptedInput& scripted) {
    Input* engine = Input::get_singleton();
    if (engine == nullptr) return;
    const auto modifiers = [&](InputEventWithModifiers& event) {
        event.set_shift_pressed(scripted.shift);
        event.set_ctrl_pressed(scripted.ctrl);
        event.set_alt_pressed(scripted.alt);
    };
    const std::string name = "scripted " + scripted.kind
        + (scripted.frame ? " at frame " + std::to_string(*scripted.frame) : " at tick " + std::to_string(scripted.tick));
    if (scripted.kind == "wheel") {
        for (const bool pressed : {true, false}) {
            Ref<InputEventMouseButton> event;
            event.instantiate();
            event->set_button_index(MOUSE_BUTTON_WHEEL_DOWN);
            event->set_pressed(pressed);
            event->set_factor(1.0F);
            event->set_position(Vector2(viewport_[0] * 0.5F, viewport_[1] * 0.5F));
            event->set_global_position(event->get_position());
            engine->parse_input_event(event);
        }
        overview_sample_pending_ = scripted.tick;
        ++scripted_fired_;
        return;
    }
    if (scripted.kind == "mdrag" || scripted.kind == "mclick") {
        replay_middle(scripted, name);
        return;
    }
    if (scripted.kind == "key") {
        const Key code = OS::get_singleton()->find_keycode_from_string(String(scripted.key.c_str()));
        if (code == KEY_NONE) {
            note(name + ": unknown key " + scripted.key);
            return;
        }
        for (const bool pressed : {true, false}) {
            Ref<InputEventKey> event;
            event.instantiate();
            event->set_keycode(code);
            event->set_physical_keycode(code);
            event->set_pressed(pressed);
            modifiers(**event);
            engine->parse_input_event(event);
        }
        ++scripted_fired_;
        return;
    }
    const auto screen_of = [&](const std::size_t index) -> std::optional<std::array<float, 2>> {
        if (scripted.unit && scripted.icon) return world_ui_->icon_centre(*scripted.unit);
        if (scripted.card) return card_point_ ? card_point_(*scripted.card) : std::nullopt;
        if (scripted.ability) return ability_point_ ? ability_point_(*scripted.ability) : std::nullopt;
        if (!scripted.hud.empty()) return hud_point_ ? hud_point_(scripted.hud) : std::nullopt;
        if (scripted.unit) {
            // A craft's own pick volume, or the unit's (a squadron's first craft).
            const auto unit = std::find_if(units_.begin(), units_.end(), [&](const ui::BattleUnit& candidate) {
                return candidate.part == *scripted.unit || candidate.entity == *scripted.unit;
            });
            if (unit == units_.end()) return std::nullopt;
            if (scripted.offset && !scripted.points.empty()) {
                const auto& offset = scripted.points.front();
                return project({unit->position[0] + static_cast<float>(offset[0]),
                                unit->position[1] + static_cast<float>(offset[1]),
                                unit->position[2] + static_cast<float>(offset[2])});
            }
            // #665: a hand clicks on the model, and a box centre can fall in the open space of a
            // hull like the Nebulon-B's, so a unit with a collision mesh is aimed at on it.
            if (unit->mesh != nullptr && !unit->mesh->triangles.empty()) return project(mesh_aim_point(*unit));
            return unit->screen;
        }
        if (index >= scripted.points.size()) return std::nullopt;
        const auto& point = scripted.points[index];
        if (scripted.minimap) return minimap_point_ ? minimap_point_(point[0], point[1]) : std::nullopt;
        if (scripted.screen) return std::array<float, 2>{static_cast<float>(point[0]), static_cast<float>(point[1])};
        return project({static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])});
    };
    const auto at = screen_of(0);
    if (!at) {
        note(name + ": its target is not on screen");
        return;
    }
    scripted_points_.push_back({scripted.kind, scripted.tick, *at});
    const auto button = [&](const MouseButton index, const bool pressed, const std::array<float, 2>& where,
                            const bool double_click) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(index);
        event->set_pressed(pressed);
        event->set_double_click(double_click);
        event->set_position(Vector2(where[0], where[1]));
        event->set_global_position(Vector2(where[0], where[1]));
        modifiers(**event);
        engine->parse_input_event(event);
    };
    // #459, #453: a hand reaches a HUD button before it clicks; Godot's GUI only takes the click
    // once the pointer has moved over the window.
    if (!scripted.hud.empty() && scripted.kind != "hover") {
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(Vector2((*at)[0], (*at)[1]));
        motion->set_global_position(Vector2((*at)[0], (*at)[1]));
        engine->parse_input_event(motion);
    }
    if (scripted.kind == "hover") {
        // The pointer moves there and stays: a motion event, no button.
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(Vector2((*at)[0], (*at)[1]));
        motion->set_global_position(Vector2((*at)[0], (*at)[1]));
        modifiers(**motion);
        engine->parse_input_event(motion);
    } else if (scripted.kind == "click" || scripted.kind == "rclick") {
        const MouseButton index = scripted.kind == "click" ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_RIGHT;
        button(index, true, *at, false);
        button(index, false, *at, false);
    } else if (scripted.kind == "dclick") {
        button(MOUSE_BUTTON_LEFT, true, *at, false);
        button(MOUSE_BUTTON_LEFT, false, *at, false);
        button(MOUSE_BUTTON_LEFT, true, *at, true);
        button(MOUSE_BUTTON_LEFT, false, *at, false);
    } else if (scripted.kind == "box") {
        const auto to = screen_of(1);
        if (!to) {
            note(name + ": its corner is not on screen");
            return;
        }
        scripted_points_.back().to = *to;
        button(MOUSE_BUTTON_LEFT, true, *at, false);
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(Vector2((*to)[0], (*to)[1]));
        motion->set_global_position(Vector2((*to)[0], (*to)[1]));
        motion->set_relative(Vector2((*to)[0] - (*at)[0], (*to)[1] - (*at)[1]));
        motion->set_button_mask(MOUSE_BUTTON_MASK_LEFT);
        modifiers(**motion);
        engine->parse_input_event(motion);
        button(MOUSE_BUTTON_LEFT, false, *to, false);
    }
    ++scripted_fired_;
}

void BattleInput::replay_middle(const LiveSessionView::ScriptedInput& scripted, const std::string& name) {
    Input* engine = Input::get_singleton();
    if (engine == nullptr) return;
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) {
        note(name + ": no drawn camera yet");
        return;
    }
    // The events a hand makes, in the order the platform sends them: Ctrl's own key press and an
    // auto-repeat before the button, the button, the motion in steps (button held, Ctrl held),
    // the button's release and then Ctrl's release, which no longer carries the Ctrl bit.
    const auto ctrl_key = [&](const bool pressed, const bool echo) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(KEY_CTRL);
        event->set_physical_keycode(KEY_CTRL);
        event->set_pressed(pressed);
        event->set_echo(echo);
        event->set_ctrl_pressed(pressed);
        engine->parse_input_event(event);
    };
    Vector2 at(viewport_[0] * 0.5F, viewport_[1] * 0.5F);
    const auto button = [&](const bool pressed) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(MOUSE_BUTTON_MIDDLE);
        event->set_pressed(pressed);
        event->set_button_mask(BitField<MouseButtonMask>(pressed ? MOUSE_BUTTON_MASK_MIDDLE : 0));
        event->set_position(at);
        event->set_global_position(at);
        event->set_ctrl_pressed(scripted.ctrl);
        engine->parse_input_event(event);
    };
    camera_samples_.push_back({name + " before", *frame_});
    if (scripted.ctrl) {
        ctrl_key(true, false);
        ctrl_key(true, true);
    }
    button(true);
    constexpr int steps = 8;
    const Vector2 step(static_cast<float>(scripted.drag[0]) / steps, static_cast<float>(scripted.drag[1]) / steps);
    for (int index = 0; scripted.kind == "mdrag" && index < steps; ++index) {
        at += step;
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(at);
        motion->set_global_position(at);
        motion->set_relative(step);
        motion->set_screen_relative(step);
        motion->set_button_mask(MOUSE_BUTTON_MASK_MIDDLE);
        motion->set_ctrl_pressed(scripted.ctrl);
        engine->parse_input_event(motion);
        if (scripted.ctrl && index == steps / 2) ctrl_key(true, true);
    }
    button(false);
    if (scripted.ctrl) ctrl_key(false, false);
    camera_sample_pending_ = name;
    ++scripted_fired_;
}

void BattleInput::draw() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !canvas_item_.is_valid()) return;
    rendering->canvas_item_clear(canvas_item_);
    // #424: FoC's selection circles, bars, squadron icons and hardpoint reticles.
    if (live_ != nullptr) {
        WorldUiView::Frame view;
        view.units = &units_;
        view.selection = &selection_;
        view.hovered = hovered_;
        view.hovered_icon = hovered_icon_;
        view.pointer = pointer_;
        view.camera = frame_;
        view.viewport = viewport_;
        view.project = [this](const ui::Vec3f& source) { return project(source); };
        view.live = live_;
        world_ui_->draw(view, canvas_item_);
    }
    // FoC's drag box (SelectBoxBorderColor / SelectBoxFillColor): once the drag passes the select
    // distance, or at once while nothing is selected.
    if (left_ && left_->moved
        && (ui::drag_extent(left_->start, left_->end) > ui::minimum_drag_select_distance || selection_.empty())) {
        const ui::ScreenRect rect = ui::drag_rect(left_->start, left_->end);
        const float width = std::max(3.0F, rect.max_x - rect.min_x);
        const float height = std::max(3.0F, rect.max_y - rect.min_y);
        const Color fill(200.0F / 255.0F, 50.0F / 255.0F, 50.0F / 255.0F, 50.0F / 255.0F);
        const Color border(200.0F / 255.0F, 50.0F / 255.0F, 50.0F / 255.0F, 1.0F);
        rendering->canvas_item_add_rect(canvas_item_, Rect2(rect.min_x + 1.0F, rect.min_y + 1.0F, width - 1.0F, height - 1.0F), fill);
        const Vector2 a(rect.min_x, rect.min_y), b(rect.min_x + width, rect.min_y), c(rect.min_x + width, rect.min_y + height),
            d(rect.min_x, rect.min_y + height);
        rendering->canvas_item_add_line(canvas_item_, a, b, border, 1.0F);
        rendering->canvas_item_add_line(canvas_item_, b, c, border, 1.0F);
        rendering->canvas_item_add_line(canvas_item_, c, d, border, 1.0F);
        rendering->canvas_item_add_line(canvas_item_, d, a, border, 1.0F);
    }
}

void BattleInput::write_report(std::ostream& output, const SpaceEnvironment& space) const {
    output << "  \"battle_input\": {\"events\": " << events_ << ", \"pickable_units\": " << units_.size();
    // #665: how many pick volumes carry a collision mesh and an override sphere.
    std::size_t meshes = 0;
    std::size_t spheres = 0;
    for (const ui::BattleUnit& unit : units_) {
        meshes += unit.mesh != nullptr && !unit.mesh->triangles.empty() ? 1U : 0U;
        spheres += unit.sphere_radius > 0.0F ? 1U : 0U;
    }
    output << ", \"pick_meshes\": " << meshes << ", \"pick_spheres\": " << spheres << ", \"double_click_candidates\": [";
    for (std::size_t click = 0; click < double_click_candidates_.size(); ++click) {
        output << (click ? ", " : "") << "[";
        const auto& candidates = double_click_candidates_[click];
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const PickCandidate& candidate = candidates[index];
            const auto number = [](const std::optional<float> value) {
                return value ? std::to_string(*value) : std::string("null");
            };
            output << (index ? ", " : "") << "{\"entity\": " << candidate.entity << ", \"part\": "
                   << (candidate.part == sim::invalid_entity_id ? std::string("null") : std::to_string(candidate.part))
                   << ", \"contact_z\": " << number(candidate.contact_z) << ", \"volume\": "
                   << (candidate.volume != nullptr ? json(candidate.volume) : std::string("null"))
                   << ", \"box_z\": " << number(candidate.box_z) << "}";
        }
        output << "]";
    }
    output << "], \"selected\": [";
    for (std::size_t index = 0; index < selection_.units().size(); ++index) {
        output << (index ? ", " : "") << selection_.units()[index];
    }
    output << "], \"groups\": {";
    bool first = true;
    for (std::size_t group = 0; group < ui::control_group_count; ++group) {
        if (selection_.group(group).empty()) continue;
        output << (first ? "" : ", ") << "\"" << group << "\": [";
        first = false;
        for (std::size_t index = 0; index < selection_.group(group).size(); ++index) {
            output << (index ? ", " : "") << selection_.group(group)[index];
        }
        output << "]";
    }
    output << "}, \"orders\": " << orders_ << ", \"refused\": " << refused_ << ", \"boxes\": " << boxes_
           << ", \"camera_focuses\": " << focuses_ << ", \"camera_follow_moves\": " << follow_moves_
           << ", \"scripted_fired\": " << scripted_fired_
           << ", \"overview\": " << json(space.live_camera_overview()) << ", \"scripted_points\": [";
    for (std::size_t index = 0; index < scripted_points_.size(); ++index) {
        const ScriptedPoint& point = scripted_points_[index];
        output << (index ? ", " : "") << "{\"kind\": " << json(point.kind) << ", \"tick\": " << point.tick
               << ", \"at\": [" << point.at[0] << ", " << point.at[1] << "]";
        if (point.to) output << ", \"to\": [" << (*point.to)[0] << ", " << (*point.to)[1] << "]";
        output << "}";
    }
    output << "], \"overview_samples\": [";
    for (std::size_t index = 0; index < overview_samples_.size(); ++index) {
        const OverviewSample& sample = overview_samples_[index];
        output << (index ? ", " : "") << "{\"tick\": " << sample.tick
               << ", \"level\": " << json(sample.level) << '}';
    }
    output << "], \"log\": [";
    for (std::size_t index = 0; index < log_.size(); ++index) output << (index ? ", " : "") << json(log_[index]);
    output << "], \"camera_samples\": [";
    const auto vector = [&output](const std::array<float, 3>& value) {
        output << "[" << value[0] << ", " << value[1] << ", " << value[2] << "]";
    };
    for (std::size_t index = 0; index < camera_samples_.size(); ++index) {
        const CameraSample& sample = camera_samples_[index];
        output << (index ? ", " : "") << "{\"label\": " << json(sample.label) << ", \"eye\": ";
        vector(sample.frame.eye);
        output << ", \"target\": ";
        vector(sample.frame.target);
        output << "}";
    }
    // #425: the cards the HUD draws for the selection, and the card clicks taken.
    output << "], \"unit_cards\": {\"slots\": " << card_slots_ << ", \"clicks\": " << card_clicks_
           << ", \"groups\": " << card_layout_.groups << ", \"cards\": [";
    for (std::size_t index = 0; index < card_layout_.cards.size(); ++index) {
        const ui::UnitCard& card = card_layout_.cards[index];
        const ui::CardUnit& unit = card_units_[card.unit];
        output << (index ? ", " : "") << "{\"slot\": " << card.slot << ", \"unit\": " << unit.id
               << ", \"type\": " << json(unit.type) << ", \"squadron\": " << (unit.squadron ? "true" : "false")
               << ", \"ability\": " << card.ability << ", \"count\": " << card.count
               << ", \"stacked\": " << (card.stacked ? "true" : "false") << ", \"health_level\": " << card.health_level
               << ", \"shield\": ";
        if (card.shield) output << *card.shield;
        else output << "null";
        output << ", \"members\": [";
        for (std::size_t member = 0; member < unit.members.size(); ++member) {
            output << (member ? ", " : "") << unit.members[member];
        }
        output << "]}";
    }
    output << "], \"borders\": [";
    constexpr std::array<const char*, 4> pieces{"full", "left", "centre", "right"};
    for (std::size_t index = 0; index < card_layout_.borders.size(); ++index) {
        const ui::CardBorder& border = card_layout_.borders[index];
        output << (index ? ", " : "") << "{\"column\": " << border.column << ", \"piece\": \""
               << pieces[static_cast<std::size_t>(border.piece)] << "\"}";
    }
    output << "]}";
    // #454: the ability buttons the HUD draws, and the requests the buttons and keys made.
    output << ", \"ability_bar\": {\"requests\": " << ability_requests_ << ", \"hotkeys\": " << ability_hotkeys_
           << ", \"targeted\": " << ability_targeted_ << ", \"target_cancels\": " << ability_target_cancels_
           << ", \"targeting\": " << (ability_target_ ? "true" : "false")
           << ", \"demo\": " << (ability_demo_ ? "true" : "false") << ", \"buttons\": [";
    for (std::size_t index = 0; index < ability_bar_.buttons.size(); ++index) {
        const ui::AbilityButton& button = ability_bar_.buttons[index];
        output << (index ? ", " : "") << "{\"component\": " << button.component << ", \"ability\": "
               << json(std::string(ui::ability_name(button.ability))) << ", \"units\": " << button.units.size() << "}";
    }
    output << "]}";
    output << ", \"hovered\": ";
    if (hovered_ && *hovered_ < units_.size()) {
        const ui::BattleUnit& unit = units_[*hovered_];
        output << (unit.part != sim::invalid_entity_id ? unit.part : unit.entity);
    } else {
        output << "null";
    }
    output << ", \"hovered_icon\": ";
    if (hovered_icon_) output << *hovered_icon_;
    else output << "null";
    output << "},\n";
    world_ui_->write_report(output);
}

} // namespace eawr::presentation::godot_backend
