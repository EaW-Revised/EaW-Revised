#include "galaxy_view.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <unordered_map>
#include <utility>

namespace eawr::viewer::galactic {
namespace {

[[nodiscard]] float to_float(const sim::math::Fixed value) noexcept {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

[[nodiscard]] Vec3 to_vec(const std::array<sim::math::Fixed, 3>& value) noexcept {
    return {to_float(value[0]), to_float(value[1]), to_float(value[2])};
}

[[nodiscard]] Rgb to_rgb(const std::array<std::uint8_t, 4>& colour) noexcept {
    return {static_cast<float>(colour[0]) / 255.0F, static_cast<float>(colour[1]) / 255.0F,
            static_cast<float>(colour[2]) / 255.0F};
}

[[nodiscard]] Vec3 lerp(const Vec3& a, const Vec3& b, const float t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

[[nodiscard]] float radians(const float degrees) noexcept {
    return degrees * std::numbers::pi_v<float> / 180.0F;
}

template <class Item>
[[nodiscard]] bool unique_ids(const std::vector<Item>& items) {
    std::vector<decltype(Item{}.id)> ids;
    ids.reserve(items.size());
    for (const Item& item : items) {
        if (item.id == 0) return false;
        ids.push_back(item.id);
    }
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

// WGUI-60: debug-build engine factor; the authored per-type scale multiplies it.
constexpr float focus_radius_factor = 4.92F;

} // namespace

sim::galactic::GalacticSetup synthetic_live_setup() {
    const auto metadata = synthetic_galaxy();
    sim::galactic::GalacticSetup setup;
    setup.seed = 1;
    for (const auto& player : metadata.players) setup.players.push_back({player.id, player.id, 0});
    setup.players.push_back({synthetic_neutral_owner, synthetic_neutral_owner, 0});
    for (const auto& seat : metadata.seats) setup.seats.push_back({sim::SeatId{seat.id}, seat.player,
        seat.human ? sim::SeatKind::human : sim::SeatKind::ai, seat.id, seat.active,
        seat.human ? seat.player : 0});
    ObjectId highest = 0;
    for (const auto& planet : metadata.planets) {
        setup.planets.push_back({planet.id, {planet.position[0], planet.position[1], planet.position[2]},
            planet.owner.value_or(synthetic_neutral_owner)});
        highest = std::max(highest, planet.id);
    }
    for (const auto& route : metadata.routes) {
        setup.routes.push_back({route.id, route.from, route.to});
        highest = std::max(highest, route.id);
    }
    for (const auto& fleet : metadata.fleets) highest = std::max(highest, fleet.id);
    for (const auto& fleet : metadata.fleets) {
        sim::galactic::Fleet row{fleet.id, fleet.owner, fleet.orbit.value_or(fleet.from), {}};
        for (std::uint32_t member = 0; member < fleet.members; ++member) row.members.push_back({++highest, 1});
        setup.fleets.push_back(std::move(row));
    }
    return setup;
}

ViewResult build_live_view(const sim::galactic::GalacticSnapshot& snapshot,
    const GalaxySnapshotStub& metadata, const SeatId seat, const PlayerId neutral_owner,
    const std::span<const ObjectId> scene_objects, const FleetMapVisibility visibility) {
    const auto local_seat = std::find_if(metadata.seats.begin(), metadata.seats.end(),
        [seat](const auto& row) { return row.id == seat; });
    if (local_seat == metadata.seats.end() || !local_seat->active)
        return {{}, "live view requires an active local seat"};
    const auto local = local_seat->player;
    const auto intelligence = sim::galactic::observed_fleets(snapshot, local, {}, 0, visibility.planets);
    std::set<std::pair<ObjectId, PlayerId>> presence;
    for (const auto& row : intelligence.presence) presence.emplace(row.planet, row.owner);
    std::set<ObjectId> counted;
    for (const auto& row : intelligence.cards) if (row.fleet) counted.insert(*row.fleet);
    std::unordered_map<ObjectId, PlayerId> planet_owners;
    planet_owners.reserve(snapshot.planets().size());
    for (const auto& row : snapshot.planets()) planet_owners.emplace(row.id, row.owner);
    const auto* observer = snapshot.observer() ? &*snapshot.observer() : nullptr;
    const bool local_observer = observer && snapshot.view_player() == local;
    std::map<sim::galactic::TypeId, const sim::galactic::ObserverType*> types;
    std::map<ObjectId, const sim::galactic::MovementContents*> contents;
    if (observer) for (const auto& row : observer->types) types.emplace(row.type, &row);
    if (snapshot.movement()) for (const auto& row : snapshot.movement()->contents) contents.emplace(row.container, &row);
    struct Traits { bool resolved{true}, nonstealth{}, superweapon{}; };
    const auto traits_for = [&](const auto& fleet) {
        Traits traits;
        const auto visit = [&](auto&& self, const sim::galactic::FleetMember& member, std::size_t depth) -> void {
            if (depth > 32) { traits.resolved = false; return; }
            const auto children = contents.find(member.id);
            if (children != contents.end()) {
                for (const auto& child : children->second->children) self(self, child, depth + 1);
                return;
            }
            const auto type = types.find(member.type);
            if (type == types.end()) { traits.resolved = false; return; }
            traits.nonstealth |= !type->second->stealth_object && !type->second->stealth_hero;
            traits.superweapon |= type->second->superweapon;
        };
        for (const auto& member : fleet.members) visit(visit, member, 0);
        return traits;
    };
    std::set<PlayerId> allies{local};
    if (local_observer) for (const auto& player : observer->players) if (player.player == local)
        allies.insert(player.allies.begin(), player.allies.end());
    std::map<ObjectId, bool> transit_sight;
    for (const auto& row : visibility.transit) transit_sight.emplace(row.fleet, row.range_path_visible.value_or(false));
    GalaxySnapshotStub display;
    display.completed_tick = snapshot.completed_tick();
    display.players = metadata.players;
    display.seats = metadata.seats;
    // One id lookup per planet instead of a scan of the metadata (#4313); emplace keeps the first row of a repeated id, as the scans did.
    std::unordered_map<ObjectId, std::size_t> art_slots;
    art_slots.reserve(metadata.planets.size());
    for (std::size_t slot = 0; slot < metadata.planets.size(); ++slot) art_slots.emplace(metadata.planets[slot].id, slot);
    display.planets.reserve(snapshot.planets().size() + scene_objects.size());
    for (const auto& planet : snapshot.planets()) {
        const auto art = art_slots.find(planet.id);
        if (art == art_slots.end()) return {{}, "live planet has no presentation metadata"};
        auto item = metadata.planets[art->second];
        item.position = {planet.position.x, planet.position.y, planet.position.z};
        item.owner = planet.owner == neutral_owner ? std::nullopt : std::optional(planet.owner);
        display.planets.push_back(std::move(item));
    }
    for (const auto id : scene_objects) {
        const auto art = art_slots.find(id);
        if (art == art_slots.end()) return {{}, "scene object has no presentation metadata"};
        auto item = metadata.planets[art->second];
        item.owner.reset();
        display.planets.push_back(std::move(item));
    }
    for (const auto& route : snapshot.routes()) display.routes.push_back({route.id, route.from, route.to});
    for (const auto& fleet : snapshot.fleets()) {
        if (!fleet.id) continue;
        const auto traits = traits_for(fleet);
        if (!fleet.motion.transit) {
            // WGUI-28: the same presence admission as the cards, then choose
            // a visible representative so an all-stealth sibling cannot leak.
            if (!visibility.reveal_orbit && (!presence.contains({fleet.orbit, fleet.owner})
                || (!allies.contains(fleet.owner) && (!traits.resolved || !traits.nonstealth)))) continue;
        } else {
            // WGUI-30/31: own/view-all, immediate own destination, superweapon,
            // then stealth rejection and the explicit WGUI-31/32 range/path seam.
            if (!local_observer) continue;
            bool visible = fleet.owner == local || visibility.view_everything;
            const auto& transit = *fleet.motion.transit;
            if (!visible && transit.path.size() >= 2 && transit.hop < transit.path.size() - 1) {
                const auto destination = planet_owners.find(transit.path[transit.hop + 1]);
                visible = traits.resolved && traits.nonstealth && destination != planet_owners.end()
                    && destination->second == local;
            }
            visible |= traits.superweapon;
            const auto supplied = transit_sight.find(fleet.id);
            visible |= traits.resolved && traits.nonstealth && supplied != transit_sight.end() && supplied->second;
            if (!visible) continue;
        }
        StubFleet row;
        row.id = fleet.id; row.owner = fleet.owner;
        // Presence alone must not expose member counts in map reports.
        row.members = fleet.owner == local || visibility.reveal_orbit || counted.contains(fleet.id)
            ? static_cast<std::uint32_t>(fleet.members.size()) : 0;
        if (fleet.motion.transit) {
            const auto& transit = *fleet.motion.transit;
            if (transit.path.size() < 2 || transit.hop >= transit.path.size() - 1)
                return {{}, "live fleet transit has no current route hop"};
            row.from = transit.path[transit.hop]; row.to = transit.path[transit.hop + 1];
            const auto& position = fleet.motion.position;
            const auto& facing = fleet.motion.facing;
            row.position = std::array{position.x, position.y, position.z};
            row.facing = std::array{facing.x, facing.y, facing.z};
        } else {
            row.orbit = fleet.orbit; row.from = row.to = fleet.orbit;
        }
        display.fleets.push_back(std::move(row));
    }
    return build_view(display, seat);
}

std::string_view to_string(const Relation relation) noexcept {
    switch (relation) {
    case Relation::own: return "own";
    case Relation::other: return "other";
    case Relation::neutral: return "neutral";
    }
    return "neutral";
}

ViewResult build_view(const GalaxySnapshotStub& snapshot, const SeatId local_seat) {
    ViewResult result;
    const auto fail = [&](std::string message) {
        result.failure = std::move(message);
        return result;
    };
    if (!unique_ids(snapshot.players) || !unique_ids(snapshot.seats) || !unique_ids(snapshot.planets)
        || !unique_ids(snapshot.routes) || !unique_ids(snapshot.fleets)) {
        return fail("the snapshot repeats an ID or uses ID 0");
    }
    std::map<PlayerId, const StubPlayer*> players;
    for (const StubPlayer& player : snapshot.players) players.emplace(player.id, &player);
    for (const StubSeat& seat : snapshot.seats) {
        if (!players.contains(seat.player)) return fail("seat " + std::to_string(seat.id) + " sits in no player");
    }
    const auto seat = std::find_if(snapshot.seats.begin(), snapshot.seats.end(),
                                   [&](const StubSeat& candidate) { return candidate.id == local_seat; });
    if (seat == snapshot.seats.end()) return fail("local seat " + std::to_string(local_seat) + " is not in the setup");
    if (!seat->active) return fail("local seat " + std::to_string(local_seat) + " is not active");

    GalaxyView view;
    view.tick = snapshot.completed_tick;
    view.local.seat = seat->id;
    view.local.player = seat->player;
    view.local.faction = players.at(seat->player)->faction;
    for (const StubSeat& other : snapshot.seats) {
        if (other.active && other.player == seat->player) view.local.faction_seats.push_back(other.id);
    }
    std::sort(view.local.faction_seats.begin(), view.local.faction_seats.end());

    const auto relation = [&](const std::optional<PlayerId> owner) {
        if (!owner) return Relation::neutral;
        return *owner == view.local.player ? Relation::own : Relation::other;
    };

    std::unordered_map<ObjectId, std::size_t> planet_index;
    planet_index.reserve(snapshot.planets.size());
    for (const StubPlanet& planet : snapshot.planets) {
        if (planet.owner && !players.contains(*planet.owner)) {
            return fail("planet " + std::to_string(planet.id) + " is owned by an unknown player");
        }
        PlanetView item;
        item.ability = planet.ability;
        item.id = planet.id;
        item.name = planet.name;
        item.model = planet.model;
        item.position = to_vec(planet.position);
        item.scale = to_float(planet.scale);
        if (!(item.scale > 0.0F) || !std::isfinite(item.scale)) {
            return fail("planet " + std::to_string(planet.id) + " has no positive scale");
        }
        item.owner = planet.owner;
        // WGP-05: geometry colour follows the owning faction for the local
        // observer. The stub carries no intelligence, so the apparent owner is
        // the owner until the snapshot builder filters for the observer.
        if (planet.owner) {
            const StubPlayer& owner = *players.at(*planet.owner);
            item.owner_faction = owner.faction;
            item.colour = to_rgb(owner.colour);
        } else {
            item.colour = to_rgb(neutral_colour);
        }
        item.relation = relation(planet.owner);
        view.planets.push_back(std::move(item));
    }
    std::sort(view.planets.begin(), view.planets.end(),
              [](const PlanetView& left, const PlanetView& right) { return left.id < right.id; });
    for (std::size_t index = 0; index < view.planets.size(); ++index) planet_index.emplace(view.planets[index].id, index);

    for (const StubRoute& route : snapshot.routes) {
        const auto from = planet_index.find(route.from);
        const auto to = planet_index.find(route.to);
        if (from == planet_index.end() || to == planet_index.end() || route.from == route.to) {
            return fail("route " + std::to_string(route.id) + " does not join two planets");
        }
        const PlanetView& a = view.planets[from->second];
        const PlanetView& b = view.planets[to->second];
        RouteView item;
        item.id = route.id;
        item.from = route.from;
        item.to = route.to;
        item.a = a.position;
        item.b = b.position;
        item.typed = a.owner && b.owner && *a.owner == *b.owner;
        item.colour = item.typed ? a.colour : to_rgb(neutral_colour);
        view.routes.push_back(std::move(item));
    }
    std::sort(view.routes.begin(), view.routes.end(),
              [](const RouteView& left, const RouteView& right) { return left.id < right.id; });

    // Orbit markers group by (planet, owner); transit fleets get one each.
    std::map<std::pair<ObjectId, PlayerId>, FleetMarker> orbit;
    std::vector<FleetMarker> transit;
    for (const StubFleet& fleet : snapshot.fleets) {
        const auto owner = players.find(fleet.owner);
        if (owner == players.end()) return fail("fleet " + std::to_string(fleet.id) + " is owned by an unknown player");
        FleetMarker marker;
        marker.fleet = fleet.id;
        marker.owner = fleet.owner;
        marker.faction = owner->second->faction;
        marker.colour = to_rgb(owner->second->colour);
        marker.relation = relation(fleet.owner);
        marker.fleets = 1;
        marker.members = fleet.members;
        if (fleet.orbit) {
            if (!planet_index.contains(*fleet.orbit)) {
                return fail("fleet " + std::to_string(fleet.id) + " orbits an unknown planet");
            }
            marker.planet = *fleet.orbit;
            auto [entry, inserted] = orbit.try_emplace({*fleet.orbit, fleet.owner}, marker);
            if (!inserted) {
                entry->second.fleet = std::min(entry->second.fleet, fleet.id);
                ++entry->second.fleets;
                entry->second.members += fleet.members;
            }
            continue;
        }
        const auto from = planet_index.find(fleet.from);
        const auto to = planet_index.find(fleet.to);
        const float progress = to_float(fleet.progress);
        if (from == planet_index.end() || to == planet_index.end() || !(progress >= 0.0F && progress <= 1.0F)) {
            return fail("fleet " + std::to_string(fleet.id) + " is not on a hop between two planets");
        }
        marker.transit = true;
        marker.destination = fleet.to;
        marker.position = fleet.position ? to_vec(*fleet.position)
            : lerp(view.planets[from->second].position, view.planets[to->second].position, progress);
        if (fleet.facing) marker.facing = to_vec(*fleet.facing);
        transit.push_back(std::move(marker));
    }
    // Orbit slots around the planet: one per present faction, in owner order.
    std::map<ObjectId, std::uint32_t> slots;
    for (const auto& entry : orbit) ++slots[entry.first.first];
    std::map<ObjectId, std::uint32_t> next;
    for (auto& [key, marker] : orbit) {
        const PlanetView& planet = view.planets[planet_index.at(key.first)];
        const std::uint32_t slot = next[key.first]++;
        const float angle = radians(90.0F + 360.0F * static_cast<float>(slot) / static_cast<float>(slots[key.first]));
        const float offset = skeleton_orbit_offset * planet.scale;
        marker.position = {planet.position.x + offset * std::cos(angle), planet.position.y + offset * std::sin(angle),
                           planet.position.z};
        view.fleets.push_back(std::move(marker));
    }
    std::sort(transit.begin(), transit.end(),
              [](const FleetMarker& left, const FleetMarker& right) { return left.fleet < right.fleet; });
    for (FleetMarker& marker : transit) view.fleets.push_back(std::move(marker));

    if (!view.planets.empty()) {
        constexpr float huge = 3.0e38F;
        view.bounds = {{huge, huge, huge}, {-huge, -huge, -huge}};
        for (const PlanetView& planet : view.planets) {
            const float radius = skeleton_planet_radius * planet.scale;
            view.bounds.min = {std::min(view.bounds.min.x, planet.position.x - radius),
                               std::min(view.bounds.min.y, planet.position.y - radius),
                               std::min(view.bounds.min.z, planet.position.z - radius)};
            view.bounds.max = {std::max(view.bounds.max.x, planet.position.x + radius),
                               std::max(view.bounds.max.y, planet.position.y + radius),
                               std::max(view.bounds.max.z, planet.position.z + radius)};
        }
    }
    result.view = std::move(view);
    return result;
}

Bounds pan_bounds(const GalaxyView& view) {
    Bounds box = view.bounds;
    const float pad_x = 0.15F * (box.max.x - box.min.x) * 0.5F;
    const float pad_y = 0.15F * (box.max.y - box.min.y) * 0.5F;
    box.min.x -= pad_x;
    box.max.x += pad_x;
    box.min.y -= pad_y;
    box.max.y += pad_y;
    return box;
}

OrbitCamera overview_camera(const GalaxyView& view, const float aspect, const float vertical_fov_degrees) {
    OrbitCamera camera;
    const Bounds& box = view.bounds;
    camera.target = {(box.min.x + box.max.x) * 0.5F, (box.min.y + box.max.y) * 0.5F, (box.min.z + box.max.z) * 0.5F};
    const float half_x = (box.max.x - box.min.x) * 0.5F;
    const float half_y = (box.max.y - box.min.y) * 0.5F;
    const float radius = std::sqrt(half_x * half_x + half_y * half_y);
    const float half_vertical = radians(vertical_fov_degrees) * 0.5F;
    const float half_horizontal = std::atan(std::tan(half_vertical) * std::max(aspect, 0.01F));
    const float half_angle = std::min(half_vertical, half_horizontal);
    camera.distance = std::clamp(radius / std::sin(half_angle) * 1.05F, orbit_min_distance, orbit_max_distance);
    return camera;
}

void pan(OrbitCamera& camera, const GalaxyView& view, const float right, const float forward) {
    const float yaw = radians(camera.yaw_degrees);
    camera.target.x += right * std::cos(yaw) + forward * std::sin(yaw);
    camera.target.y += -right * std::sin(yaw) + forward * std::cos(yaw);
    const Bounds box = pan_bounds(view);
    camera.target.x = std::clamp(camera.target.x, box.min.x, box.max.x);
    camera.target.y = std::clamp(camera.target.y, box.min.y, box.max.y);
}

void orbit(OrbitCamera& camera, const float yaw_degrees, const float pitch_degrees) {
    camera.yaw_degrees = std::fmod(camera.yaw_degrees + yaw_degrees, 360.0F);
    camera.pitch_degrees = std::clamp(camera.pitch_degrees + pitch_degrees, orbit_min_pitch, orbit_max_pitch);
}

void zoom(OrbitCamera& camera, const float factor) {
    if (!(factor > 0.0F) || !std::isfinite(factor)) return;
    camera.distance = std::clamp(camera.distance * factor, orbit_min_distance, orbit_max_distance);
}

std::array<float, 3> render_point(const Vec3& source) noexcept { return {source.x, source.z, -source.y}; }

float galactic_vertical_fov(const std::uint32_t width, const std::uint32_t height) noexcept {
    // WGP-43: the perspective argument atan(.5) is the full horizontal angle.
    const float horizontal = std::atan(0.5F);
    const float aspect = static_cast<float>(std::max(1U, width)) / static_cast<float>(std::max(1U, height));
    return 2.0F * std::atan(std::tan(horizontal * 0.5F) / aspect) * 180.0F / std::numbers::pi_v<float>;
}

presentation::FixedCamera fixed_camera(const OrbitCamera& camera, const std::uint32_t width,
                                       const std::uint32_t height, const float vertical_fov_degrees) {
    const float yaw = radians(camera.yaw_degrees);
    const float pitch = radians(camera.pitch_degrees);
    // yaw 0 looks along +Y; the eye sits behind and above the target.
    const Vec3 eye{camera.target.x - std::sin(yaw) * std::cos(pitch) * camera.distance,
                   camera.target.y - std::cos(yaw) * std::cos(pitch) * camera.distance,
                   camera.target.z + std::sin(pitch) * camera.distance};
    presentation::FixedCamera result;
    result.width = width;
    result.height = height;
    result.vertical_fov_degrees = vertical_fov_degrees;
    result.near_plane = 10.0F; // WGP-43: stock perspective depth range.
    result.far_plane = 10000.0F;
    result.eye = render_point(eye);
    result.target = render_point(camera.target);
    result.up = {0.0F, 1.0F, 0.0F};
    return result;
}

std::string_view to_string(const CameraState state) noexcept {
    switch (state) {
    case CameraState::overview: return "overview";
    case CameraState::inward: return "inward";
    case CameraState::focused: return "focused";
    case CameraState::outward: return "outward";
    }
    return "overview";
}

PlanetCamera::PlanetCamera(CameraConstants constants) : constants_(constants) {
    scroll_.z = constants_.scroll_plane;
}

void PlanetCamera::position(const Vec3 point) {
    scroll_ = {point.x, point.y, constants_.scroll_plane};
}

void PlanetCamera::pan(const GalaxyView& view, const float x, const float y, const bool forced_scroll) {
    if (forced_scroll || state_ != CameraState::overview || !std::isfinite(x) || !std::isfinite(y)) return;
    const Bounds box = pan_bounds(view);
    scroll_.x = std::clamp(scroll_.x + x, box.min.x, box.max.x);
    scroll_.y = std::clamp(scroll_.y - y, box.min.y, box.max.y);
    scroll_.z = constants_.scroll_plane;
}

bool PlanetCamera::right_drag(const GalaxyView& view, const float x, const float y,
                             const bool held, const bool control) {
    if (!held) return false;
    const float multiplier = 1000.0F * constants_.right_scroll_factor * (control ? 1.0F : 4.0F);
    pan(view, x * multiplier, -y * multiplier);
    return true;
}

bool PlanetCamera::zoom_in(const PlanetView& planet, const bool renderable, const ZoomGates gates) {
    if (!gates.tutorial_allowed) return false;
    if (state_ == CameraState::focused) {
        if (advanced_map_) return false;
        advanced_map_ = true;
        radar_rate_ = 1.5F; // WGUI-65: explicit commands use the fixed radar animation rate.
        return true;
    }
    if (state_ != CameraState::overview || gates.dialog_open) return false;
    target_ = planet.position;
    radius_ = renderable ? focus_radius_factor * planet.scale : 0.0F; // WGUI-60: fixed radius factor times Scale_Factor.
    planet_ = planet.id;
    state_ = CameraState::inward;
    alpha_ = 0.0F;
    start_.reset();
    radar_rate_ = 1.5F; // WGUI-65: explicit commands use the fixed radar animation rate.
    return true;
}

bool PlanetCamera::zoom_out(const ZoomGates gates) {
    if (!gates.tutorial_allowed) return false;
    if (state_ == CameraState::focused && advanced_map_) {
        advanced_map_ = false;
        radar_rate_ = 1.5F; // WGUI-65: explicit commands use the fixed radar animation rate.
        return true;
    }
    if (gates.dialog_open) return false;
    if (state_ != CameraState::inward && state_ != CameraState::focused) {
        state_ = CameraState::overview; // WGUI-61: reset does not clear target or alpha.
        return false;
    }
    state_ = CameraState::outward;
    alpha_ = 0.0F;
    start_.reset();
    radar_rate_ = 1.5F; // WGUI-65: explicit commands use the fixed radar animation rate.
    return true;
}

bool PlanetCamera::toggle(const PlanetView& planet, const bool renderable, const ZoomGates gates) {
    if (!gates.tutorial_allowed) return false;
    bool accepted = false;
    if (state_ == CameraState::focused) {
        advanced_map_ = false;
        accepted = zoom_out({gates.tutorial_allowed, gates.selection_locked, false});
    } else if (state_ == CameraState::overview) {
        accepted = zoom_in(planet, renderable, gates);
    }
    if (accepted) radar_rate_ = 1.0F; // WGUI-65: toggle uses the fixed default radar animation rate.
    return accepted;
}

bool PlanetCamera::refocus(const PlanetView& planet, const bool renderable) {
    // WGUI-48/60: selecting during an inward flight refocuses immediately.
    if (state_ != CameraState::focused && state_ != CameraState::inward) return false;
    target_ = planet.position;
    radius_ = renderable ? focus_radius_factor * planet.scale : 0.0F; // WGUI-60: refocus uses the same Scale_Factor radius.
    planet_ = planet.id;
    state_ = CameraState::focused;
    alpha_ = 1.0F;
    return true;
}

void PlanetCamera::service(const std::uint64_t wall_milliseconds) {
    if (state_ != CameraState::inward && state_ != CameraState::outward) return;
    if (!start_) start_ = wall_milliseconds;
    const auto elapsed = wall_milliseconds >= *start_ ? wall_milliseconds - *start_ : 0;
    alpha_ = constants_.zoom_seconds > 0.0F
        ? std::clamp(static_cast<float>(elapsed) / (1000.0F * constants_.zoom_seconds), 0.0F, 1.0F) : 1.0F;
    if (alpha_ == 1.0F) {
        if (state_ == CameraState::inward) state_ = CameraState::focused;
        else { state_ = CameraState::overview; planet_ = 0; }
    }
}

OrbitCamera PlanetCamera::pose() const {
    float blend = 0.0F;
    if (state_ == CameraState::focused) blend = 1.0F;
    else if (state_ == CameraState::inward || state_ == CameraState::outward) {
        blend = alpha_ * alpha_ * (3.0F - 2.0F * alpha_);
        if (state_ == CameraState::outward) blend = 1.0F - blend;
    }
    const Vec3 focus{target_.x + radius_ * constants_.focused_offset.x,
                     target_.y + radius_ * constants_.focused_offset.y,
                     target_.z + radius_ * constants_.focused_offset.z};
    return {lerp(scroll_, focus, blend), 0.0F,
            // WGP-41/WGUI-63: X(90 + authored pitch), translated along local +Z.
            // Orbit elevation is the negative authored pitch in the source XY plane.
            -constants_.initial_pitch - (constants_.focused_pitch - constants_.initial_pitch) * blend,
            constants_.initial_pullback + (radius_ * constants_.focused_pullback - constants_.initial_pullback) * blend};
}

Vec3 source_light_direction(const Vec3 angles) noexcept {
    const float heading = radians(angles.y);
    const float elevation = radians(angles.z);
    return {std::sin(heading) * std::cos(elevation), -std::cos(heading) * std::cos(elevation),
            std::sin(elevation)};
}

CameraLighting PlanetCamera::lighting() const {
    float blend = state_ == CameraState::focused ? 1.0F : 0.0F;
    if (state_ == CameraState::inward) blend = alpha_;
    if (state_ == CameraState::outward) blend = 1.0F - alpha_;
    return {0.5F + (constants_.focused_light - 0.5F) * blend,
            lerp(constants_.out_angles, constants_.in_angles, blend)};
}

std::optional<Vec3> radar_to_world(const Vec3 local, const float width, const float height) {
    if (!(width > 0.0F && height > 0.0F) || !std::isfinite(width) || !std::isfinite(height)
        || !std::isfinite(local.x) || !std::isfinite(local.y)) return std::nullopt;
    // WGUI-90: fixed radar half-extents 380 X / 460 Y and world Z 10.
    return Vec3{local.x / (width * 0.5F) * 380.0F, local.y / (height * 0.5F) * 460.0F, 10.0F};
}

std::optional<Vec3> world_to_radar(const Vec3 world, const float width, const float height) {
    if (!(width > 0.0F && height > 0.0F) || !std::isfinite(width) || !std::isfinite(height)
        || !std::isfinite(world.x) || !std::isfinite(world.y)) return std::nullopt;
    // WGUI-90: inverse uses the same fixed 380 X / 460 Y half-extents and radar Z 0.
    return Vec3{world.x / 380.0F * width * 0.5F, world.y / 460.0F * height * 0.5F, 0.0F};
}

} // namespace eawr::viewer::galactic
