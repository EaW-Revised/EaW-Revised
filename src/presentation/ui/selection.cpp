#include "eawr/presentation/ui/selection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace eawr::presentation::ui {

bool can_control(const sim::SeatId local, const sim::tactical::TacticalInstance& instance,
    const std::optional<sim::tactical::BattleControllerPolicy>& policy, const bool shared_authorized) noexcept {
    if (!policy) return true;
    const auto seat = std::lower_bound(policy->seats.begin(), policy->seats.end(), local,
        [](const auto& row, const auto id) { return row.seat < id; });
    if (seat == policy->seats.end() || seat->seat != local || !seat->active || seat->kind != sim::SeatKind::human) return false;
    if (instance.control == sim::tactical::UnitControl::shared_structure)
        return seat->player == instance.owner || shared_authorized;
    if (instance.control == sim::tactical::UnitControl::none && shared_authorized
        && std::none_of(policy->factions.begin(), policy->factions.end(),
            [&](const auto& row) { return row.player == instance.owner; })) return true;
    return seat->player == instance.owner && instance.control == sim::tactical::UnitControl::mobile
        && instance.controller_seat == local;
}

std::vector<sim::SeatId> gift_recipients(const sim::SeatId local,
    const std::optional<sim::tactical::BattleControllerPolicy>& policy) {
    std::vector<sim::SeatId> result;
    if (!policy) return result;
    const auto sender = std::find_if(policy->seats.begin(), policy->seats.end(),
        [local](const auto& row) { return row.seat == local; });
    if (sender == policy->seats.end() || !sender->active || sender->kind != sim::SeatKind::human) return result;
    for (const auto& seat : policy->seats)
        if (seat.active && seat.kind == sim::SeatKind::human && seat.player == sender->player && seat.seat != local)
            result.push_back(seat.seat);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::vector<sim::EntityId>> gift_chunks(const sim::SeatId local,
    const std::span<const sim::EntityId> selected, const std::span<const sim::tactical::TacticalInstance> instances,
    const std::optional<sim::tactical::BattleControllerPolicy>& policy) {
    std::vector<std::vector<sim::EntityId>> result;
    if (!policy) return result;
    std::vector<sim::EntityId> roots;
    for (const auto id : selected) {
        const auto unit = std::lower_bound(instances.begin(), instances.end(), id,
            [](const auto& row, const auto entity) { return row.entity_id < entity; });
        if (unit != instances.end() && unit->entity_id == id && unit->control == sim::tactical::UnitControl::mobile
            && (!unit->durability || unit->durability->hull.raw() > 0)
            && can_control(local, *unit, policy)) roots.push_back(id);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    for (std::size_t start = 0; start < roots.size(); start += sim::tactical::max_units_per_command) {
        const auto end = std::min(roots.size(), start + sim::tactical::max_units_per_command);
        result.emplace_back(roots.begin() + static_cast<std::ptrdiff_t>(start), roots.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

namespace {

[[nodiscard]] bool finite(const Vec3f& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

// Solves R d = v for the 3x3 rotation-and-scale block of `m` (row-major 3x4).
[[nodiscard]] std::optional<Vec3f> solve(const std::array<float, 12>& m, const Vec3f& v) noexcept {
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1.0e-12) return std::nullopt;
    const double x = (v[0] * (e * i - f * h) - b * (v[1] * i - f * v[2]) + c * (v[1] * h - e * v[2])) / det;
    const double y = (a * (v[1] * i - f * v[2]) - v[0] * (d * i - f * g) + c * (d * v[2] - v[1] * g)) / det;
    const double z = (a * (e * v[2] - v[1] * h) - b * (d * v[2] - v[1] * g) + v[0] * (d * h - e * g)) / det;
    return Vec3f{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
}

[[nodiscard]] const BattleUnit* find(const std::span<const BattleUnit> units, const sim::EntityId entity) noexcept {
    const auto found = std::find_if(units.begin(), units.end(),
        [entity](const BattleUnit& unit) { return unit.entity == entity; });
    return found == units.end() ? nullptr : &*found;
}

} // namespace

ScreenRect drag_rect(const std::array<float, 2> start, const std::array<float, 2> end) noexcept {
    return {std::min(start[0], end[0]), std::min(start[1], end[1]), std::max(start[0], end[0]),
            std::max(start[1], end[1])};
}

float drag_extent(const std::array<float, 2> start, const std::array<float, 2> end) noexcept {
    return std::max(std::abs(start[0] - end[0]), std::abs(start[1] - end[1]));
}

namespace {

// The ray in a frame's model space: the frame may carry a uniform scale, so directions are solved
// through the same block and the ray parameter stays the world one.
struct ModelRay {
    Vec3f origin{};
    Vec3f direction{};
};

[[nodiscard]] std::optional<ModelRay> model_ray(const PickRay& ray, const std::array<float, 12>& m) noexcept {
    if (!finite(ray.origin) || !finite(ray.direction)) return std::nullopt;
    const Vec3f offset{ray.origin[0] - m[3], ray.origin[1] - m[7], ray.origin[2] - m[11]};
    const auto origin = solve(m, offset);
    const auto direction = solve(m, ray.direction);
    if (!origin || !direction) return std::nullopt;
    return ModelRay{*origin, *direction};
}

// The ray parameter where it enters the box [low, high] (0 when it starts inside), or nothing.
[[nodiscard]] std::optional<double> box_entry(const ModelRay& ray, const Vec3f& low, const Vec3f& high) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!(low[axis] <= high[axis])) return std::nullopt;
    }
    double entry = 0.0;
    double leave = std::numeric_limits<double>::infinity();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double o = ray.origin[axis];
        const double d = ray.direction[axis];
        if (std::abs(d) < 1.0e-12) {
            if (o < low[axis] || o > high[axis]) return std::nullopt;
            continue;
        }
        double t0 = (low[axis] - o) / d;
        double t1 = (high[axis] - o) / d;
        if (t0 > t1) std::swap(t0, t1);
        entry = std::max(entry, t0);
        leave = std::min(leave, t1);
        if (entry > leave) return std::nullopt;
    }
    return entry;
}

// The ray parameter where it crosses the triangle, from either side, or nothing.
[[nodiscard]] std::optional<double> triangle_entry(const ModelRay& ray, const std::array<Vec3f, 3>& triangle) noexcept {
    const auto sub = [](const Vec3f& a, const Vec3f& b) {
        return std::array<double, 3>{static_cast<double>(a[0]) - b[0], static_cast<double>(a[1]) - b[1],
                                     static_cast<double>(a[2]) - b[2]};
    };
    const auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const std::array<double, 3> direction{ray.direction[0], ray.direction[1], ray.direction[2]};
    const auto edge1 = sub(triangle[1], triangle[0]);
    const auto edge2 = sub(triangle[2], triangle[0]);
    const auto p = cross(direction, edge2);
    const double det = dot(edge1, p);
    if (std::abs(det) < 1.0e-12) return std::nullopt;
    const auto s = sub(ray.origin, triangle[0]);
    const double u = dot(s, p) / det;
    if (u < 0.0 || u > 1.0) return std::nullopt;
    const auto q = cross(s, edge1);
    const double v = dot(direction, q) / det;
    if (v < 0.0 || u + v > 1.0) return std::nullopt;
    const double t = dot(edge2, q) / det;
    if (!(t >= 0.0)) return std::nullopt;
    return t;
}

// The ray parameter where it enters the world-space sphere, or nothing.
[[nodiscard]] std::optional<double> sphere_entry(const PickRay& ray, const Vec3f& centre, const float radius) noexcept {
    const double ox = static_cast<double>(ray.origin[0]) - centre[0];
    const double oy = static_cast<double>(ray.origin[1]) - centre[1];
    const double oz = static_cast<double>(ray.origin[2]) - centre[2];
    const double a = static_cast<double>(ray.direction[0]) * ray.direction[0]
        + static_cast<double>(ray.direction[1]) * ray.direction[1] + static_cast<double>(ray.direction[2]) * ray.direction[2];
    if (a < 1.0e-18) return std::nullopt;
    const double b = ox * ray.direction[0] + oy * ray.direction[1] + oz * ray.direction[2];
    const double c = ox * ox + oy * oy + oz * oz - static_cast<double>(radius) * radius;
    const double discriminant = b * b - a * c;
    if (discriminant < 0.0) return std::nullopt;
    const double root = std::sqrt(discriminant);
    const double closest = (-b - root) / a;
    const double farthest = (-b + root) / a;
    if (farthest < 0.0) return std::nullopt;
    return std::max(closest, 0.0);
}

[[nodiscard]] Vec3f along(const PickRay& ray, const double t) noexcept {
    return Vec3f{static_cast<float>(ray.origin[0] + t * ray.direction[0]),
                 static_cast<float>(ray.origin[1] + t * ray.direction[1]),
                 static_cast<float>(ray.origin[2] + t * ray.direction[2])};
}

} // namespace

PickMesh make_pick_mesh(std::vector<std::array<Vec3f, 3>> triangles) {
    PickMesh mesh;
    mesh.triangles = std::move(triangles);
    if (mesh.triangles.empty()) return mesh;
    mesh.low = mesh.triangles.front()[0];
    mesh.high = mesh.low;
    for (const auto& triangle : mesh.triangles) {
        for (const Vec3f& corner : triangle) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                mesh.low[axis] = std::min(mesh.low[axis], corner[axis]);
                mesh.high[axis] = std::max(mesh.high[axis], corner[axis]);
            }
        }
    }
    return mesh;
}

std::optional<Vec3f> ray_box_contact(const PickRay& ray, const UnitBox& box) noexcept {
    const auto local = model_ray(ray, box.model_to_world);
    if (!local) return std::nullopt;
    const auto entry = box_entry(*local, box.low, box.high);
    if (!entry) return std::nullopt;
    return along(ray, *entry);
}

std::optional<Vec3f> pick_contact(const PickRay& ray, const BattleUnit& unit) noexcept {
    std::optional<double> hit;
    if (unit.mesh != nullptr && !unit.mesh->triangles.empty()) {
        // WSU-11: the collidable meshes, triangle by triangle after a bounds check.
        const auto local = model_ray(ray, unit.box.model_to_world);
        if (local && box_entry(*local, unit.mesh->low, unit.mesh->high)) {
            for (const auto& triangle : unit.mesh->triangles) {
                const auto t = triangle_entry(*local, triangle);
                if (t && (!hit || *t < *hit)) hit = t;
            }
        }
    } else if (const auto local = model_ray(ray, unit.box.model_to_world)) {
        hit = box_entry(*local, unit.box.low, unit.box.high);
    }
    // WSU-10, WSU-12: the override sphere only where the collision geometry is missed.
    if (!hit && unit.sphere_radius > 0.0F && finite(ray.origin) && finite(ray.direction) && finite(unit.position)) {
        hit = sphere_entry(ray, unit.position, unit.sphere_radius);
    }
    if (!hit) return std::nullopt;
    return along(ray, *hit);
}

std::optional<std::size_t> pick_index(const PickRay& ray, const std::span<const BattleUnit> units) noexcept {
    std::optional<std::size_t> best;
    float best_z = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < units.size(); ++index) {
        const BattleUnit& unit = units[index];
        if (unit.entity == sim::invalid_entity_id || !unit.mouse_sensitive) continue;
        const auto contact = pick_contact(ray, unit);
        if (!contact) continue;
        const bool lower = best && (unit.entity < units[*best].entity
            || (unit.entity == units[*best].entity && unit.part < units[*best].part));
        if ((*contact)[2] > best_z || ((*contact)[2] == best_z && lower)) {
            best_z = (*contact)[2];
            best = index;
        }
    }
    return best;
}

std::optional<sim::EntityId> pick_unit(const PickRay& ray, const std::span<const BattleUnit> units) noexcept {
    const auto index = pick_index(ray, units);
    if (!index) return std::nullopt;
    return units[*index].entity;
}

std::optional<Vec3f> battle_plane_point(const PickRay& ray) noexcept {
    if (!finite(ray.origin) || !finite(ray.direction) || std::abs(ray.direction[2]) < 1.0e-9F) return std::nullopt;
    const float t = -ray.origin[2] / ray.direction[2];
    if (!std::isfinite(t)) return std::nullopt;
    return Vec3f{ray.origin[0] + t * ray.direction[0], ray.origin[1] + t * ray.direction[1], 0.0F};
}

RightRelease right_release(const std::optional<std::array<float, 2>> press, const std::array<float, 2> release) noexcept {
    if (!press) return RightRelease::ignored;
    return drag_extent(*press, release) > minimum_drag_select_distance ? RightRelease::compass : RightRelease::click;
}

bool allied_inspectable(const BattleUnit& unit, const bool ally_of_local) noexcept {
    return ally_of_local && !unit.neutral && !unit.own && !unit.hostile && unit.part == sim::invalid_entity_id
        && !unit.locomotion && !unit.decoration;
}

const BattleUnit* inspect_click(const std::span<const BattleUnit> units, const std::optional<sim::EntityId> picked,
                                const bool armed, const Modifiers modifiers) noexcept {
    if (!picked || armed || modifiers.shift || modifiers.ctrl || modifiers.alt) return nullptr;
    const auto found = std::find_if(units.begin(), units.end(), [&](const BattleUnit& unit) { return unit.entity == *picked; });
    return found != units.end() && found->allied_inspectable ? &*found : nullptr;
}

RightClick right_click(const OrderMode mode, const BattleUnit* over, const bool over_selected) noexcept {
    switch (mode) {
    case OrderMode::waypoint: return RightClick::disarm;
    case OrderMode::attack:
        return over != nullptr && over->hostile ? RightClick::order : RightClick::disarm;
    case OrderMode::move:
    case OrderMode::attack_move:
    case OrderMode::guard:
        return RightClick::order;
    case OrderMode::none:
        break;
    }
    if (over != nullptr && ((over->own && over_selected) || (!over->own && !over->hostile))) return RightClick::nothing;
    return RightClick::order;
}

void Selection::add(const sim::EntityId entity) { append(entity); }

bool Selection::append(const sim::EntityId entity) {
    ++membership_operations_;
    if (!members_.insert(entity).second) return false;
    selected_.push_back(entity);
    return true;
}

void Selection::adopt(std::vector<sim::EntityId> next) {
    selected_ = std::move(next);
    members_.clear();
    members_.reserve(selected_.size());
    members_.insert(selected_.begin(), selected_.end());
    membership_operations_ += selected_.size();
}

void Selection::unique_in_order(std::vector<sim::EntityId>& members) {
    std::unordered_set<sim::EntityId> seen;
    seen.reserve(members.size());
    std::erase_if(members, [&](const sim::EntityId entity) {
        ++membership_operations_;
        return !seen.insert(entity).second;
    });
}

bool Selection::click(const std::optional<sim::EntityId> picked, const Modifiers modifiers,
                      const std::span<const BattleUnit> units, const ScreenRect& viewport) {
    const BattleUnit* unit = picked ? find(units, *picked) : nullptr;
    if (unit == nullptr) {
        if (picked || selected_.empty()) return false;
        clear();
        return true;
    }
    if (!unit->own || !unit->controlled || !unit->selectable || unit->neutral) {
        // FoC: another player's unit gives no select action; with nothing selected the click
        // falls through to the deselect, which has nothing to clear.
        return false;
    }
    if (modifiers.shift) {
        ++membership_operations_;
        if (members_.erase(unit->entity) != 0) std::erase(selected_, unit->entity);
        else append(unit->entity);
        return true;
    }
    if (modifiers.ctrl) return type_on_screen(unit->type, units, viewport);
    if (selected_.size() == 1 && selected_.front() == unit->entity) return false;
    adopt({unit->entity});
    return true;
}

bool Selection::double_click(const std::optional<sim::EntityId> picked, const std::span<const BattleUnit> units,
                             const ScreenRect& viewport) {
    const BattleUnit* unit = picked ? find(units, *picked) : nullptr;
    if (unit == nullptr || !unit->own || !unit->controlled || !unit->selectable || unit->neutral) return false;
    // WSU-18: the picked object's type; for a craft, the craft type, whose objects select as their
    // squadrons (the squadron's first craft stands for the one under the pointer; M2 squadrons are
    // of one craft type).
    if (unit->part != sim::invalid_entity_id) return craft_type_on_screen(unit->part_type, units, viewport);
    return type_on_screen(unit->type, units, viewport);
}

bool Selection::box(const ScreenRect& rect, const bool shift, const std::span<const BattleUnit> units,
                    const std::span<const SquadronIcon> icons) {
    bool icon_found = false;
    for (const SquadronIcon& icon : icons) {
        if (!icon.own || !icon.selectable || !rect.contains_quad(icon.rect)) continue;
        add(icon.entity);
        icon_found = true;
    }
    bool found = false;
    for (const BattleUnit& unit : units) {
        if (!unit.own || !unit.controlled || !unit.selectable || unit.neutral || !unit.locomotion || unit.decoration
            || !unit.screen || !rect.contains_origin(*unit.screen)) continue;
        if (!shift && !found) clear();
        found = true;
        add(unit.entity);
    }
    return found || icon_found;
}

bool Selection::all(const std::span<const BattleUnit> units) {
    std::vector<sim::EntityId> next;
    for (const BattleUnit& unit : units) {
        // S-5a: global admission has no projection or pointer-sensitivity test.
        if (unit.entity == sim::invalid_entity_id || !unit.own || !unit.controlled || !unit.selectable || unit.neutral
            || !unit.locomotion || unit.decoration) continue;
        next.push_back(unit.entity);
    }
    return replace(next);
}

bool Selection::type_on_screen(const sim::tactical::TypeId type, const std::span<const BattleUnit> units,
                               const ScreenRect& viewport) {
    bool added = false;
    for (const BattleUnit& unit : units) {
        if (!unit.own || !unit.controlled || !unit.selectable || unit.neutral || unit.decoration || unit.type != type
            || !unit.screen || !viewport.contains_origin(*unit.screen)) continue;
        if (append(unit.entity)) added = true;
    }
    return added;
}

bool Selection::craft_type_on_screen(const sim::tactical::TypeId craft_type, const std::span<const BattleUnit> units,
                                     const ScreenRect& viewport) {
    bool added = false;
    for (const BattleUnit& unit : units) {
        if (!unit.own || !unit.controlled || !unit.selectable || unit.neutral || unit.decoration
            || unit.part == sim::invalid_entity_id || unit.part_type != craft_type) continue;
        if (!unit.screen || !viewport.contains_origin(*unit.screen)) continue;
        if (append(unit.entity)) added = true;
    }
    return added;
}

void Selection::retain_sorted(const std::span<const sim::EntityId> alive) {
    if (selected_.empty() && std::all_of(groups_.begin(), groups_.end(),
            [](const auto& group) { return group.empty(); })) return;
    const auto gone = [this, alive](const sim::EntityId entity) {
        const auto found = std::lower_bound(alive.begin(), alive.end(), entity,
            [this](const sim::EntityId candidate, const sim::EntityId target) {
                ++membership_operations_;
                return candidate < target;
            });
        ++membership_operations_;
        return found == alive.end() || *found != entity;
    };
    std::erase_if(selected_, [this, &gone](const sim::EntityId entity) {
        if (!gone(entity)) return false;
        members_.erase(entity);
        return true;
    });
    for (auto& group : groups_) {
        std::erase_if(group, [this, &gone](const sim::EntityId entity) {
            if (!gone(entity)) return false;
            group_numbers_.erase(entity);
            return true;
        });
    }
}

bool Selection::replace(const std::span<const sim::EntityId> units) {
    std::vector<sim::EntityId> next(units.begin(), units.end());
    unique_in_order(next);
    if (next == selected_) return false;
    adopt(std::move(next));
    return true;
}

void Selection::replace_entity(const sim::EntityId previous, const sim::EntityId replacement) {
    const auto transfer = [=, this](std::vector<sim::EntityId>& members) {
        std::replace(members.begin(), members.end(), previous, replacement);
        unique_in_order(members);
    };
    transfer(selected_);
    adopt(std::move(selected_));
    for (auto& members : groups_) transfer(members);
}

void Selection::owner_changed(const sim::EntityId entity, const sim::tactical::PlayerId previous,
    const sim::tactical::PlayerId current, const sim::tactical::PlayerId observer) {
    if (previous == current || previous != observer) return;
    ++membership_operations_;
    if (members_.erase(entity) != 0) std::erase(selected_, entity);
    for (auto& members : groups_) std::erase(members, entity);
    group_numbers_.erase(entity);
}

void Selection::controller_changed(const sim::EntityId entity, const sim::SeatId previous,
    const sim::SeatId current, const sim::SeatId observer) {
    owner_changed(entity, previous.value, current.value, observer.value);
}

void Selection::assign_group(const std::size_t group) {
    for (const auto entity : groups_.at(group)) group_numbers_.erase(entity);
    for (auto& members : groups_) {
        std::erase_if(members, [this](const sim::EntityId entity) {
            ++membership_operations_;
            return members_.contains(entity);
        });
    }
    groups_.at(group) = selected_;
    for (const auto entity : selected_) group_numbers_[entity] = group;
}

std::optional<std::size_t> Selection::group_of(const sim::EntityId entity) const noexcept {
    const auto found = group_numbers_.find(entity);
    return found == group_numbers_.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

std::optional<Vec3f> Selection::recall_group(const std::size_t group, const bool add, const double logical_frame,
                                             const std::span<const BattleUnit> alive) {
    if (!add) clear();
    return selected_group(group, logical_frame, alive);
}

std::optional<Vec3f> Selection::add_to_group(const std::size_t group, const double logical_frame,
                                             const std::span<const BattleUnit> alive) {
    auto& members = groups_.at(group);
    for (std::size_t other = 0; other < groups_.size(); ++other) {
        if (other == group) continue;
        std::erase_if(groups_[other], [this](const sim::EntityId entity) {
            ++membership_operations_;
            return members_.contains(entity);
        });
    }
    std::unordered_set<sim::EntityId> present(members.begin(), members.end());
    membership_operations_ += members.size();
    for (const sim::EntityId entity : selected_) {
        ++membership_operations_;
        if (present.insert(entity).second) members.push_back(entity);
        group_numbers_[entity] = group;
    }
    return selected_group(group, logical_frame, alive);
}

std::optional<Vec3f> Selection::selected_group(const std::size_t group, const double logical_frame,
                                               const std::span<const BattleUnit> alive) {
    double sum_x = 0.0;
    double sum_y = 0.0;
    std::size_t count = 0;
    // The first standing unit of an ID answers for it, as a scan of `alive` would.
    std::unordered_map<sim::EntityId, const BattleUnit*> standing;
    if (!groups_.at(group).empty()) {
        standing.reserve(alive.size());
        for (const BattleUnit& unit : alive) standing.try_emplace(unit.entity, &unit);
        membership_operations_ += alive.size();
    }
    for (const sim::EntityId entity : groups_.at(group)) {
        ++membership_operations_;
        const auto found = standing.find(entity);
        if (found == standing.end()) continue;
        const BattleUnit* unit = found->second;
        if (!unit->controlled) continue;
        add(entity);
        sum_x += unit->position[0];
        sum_y += unit->position[1];
        ++count;
    }
    std::optional<Vec3f> focus;
    if (count > 0 && last_group_ == group && logical_frame >= last_group_frame_
        && logical_frame - last_group_frame_ < control_group_double_tap_frames) {
        focus = Vec3f{static_cast<float>(sum_x / static_cast<double>(count)),
                      static_cast<float>(sum_y / static_cast<double>(count)), 0.0F};
    }
    last_group_ = group;
    last_group_frame_ = logical_frame;
    return focus;
}

} // namespace eawr::presentation::ui
