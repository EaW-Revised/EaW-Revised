#include "galactic_mode_internal.hpp"
#include "battle_content.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/core/diagnostic.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace galactic_detail {
namespace {
constexpr std::string_view strip_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;
uniform sampler2D line_texture : filter_linear;
void fragment() {
    vec4 texel = texture(line_texture, UV);
    ALBEDO = texel.rgb * COLOR.rgb;
    ALPHA = texel.a * COLOR.a;
}
)GODOT";

std::string upper(std::string text) {
    for (char& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    return text;
}
Vector3 point(galactic_view::Vec3 source) {
    const auto p = galactic_view::render_point(source);
    return {p[0], p[1], p[2]};
}
RouteGeometry::Quad quad(Vector3 a, Vector3 b, Vector3 side, galactic_view::LineColour colour) {
    RouteGeometry::Quad result;
    const std::array<Vector3, 4> corners{a - side, b - side, b + side, a + side};
    for (std::size_t i = 0; i < corners.size(); ++i) result.points[i] = {corners[i].x, corners[i].y, corners[i].z};
    result.colour = colour;
    return result;
}
}

bool RouteGeometry::initialize(const RID& scenario, const vfs::Vfs& filesystem,
                               const std::string& name, std::string& failure) {
    auto* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario.is_valid()) { failure = "route scene unavailable"; return false; }
    std::string logical = name;
    for (char& c : logical) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c == '\\') c = '/';
    }
    const auto path = resolve_texture(filesystem, logical);
    if (!path) { failure = "route texture is not present in the mounted view: " + name; return false; }
    auto loaded = assets::load_texture(filesystem, *path);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    const auto image = texture_image(loaded.value(), failure);
    if (image.is_null()) return false;
    texture_width_ = static_cast<float>(image->get_width());
    texture_ = rendering->texture_2d_create(image);
    shader_ = rendering->shader_create();
    rendering->shader_set_code(shader_, String(std::string(strip_shader).c_str()));
    material_ = rendering->material_create();
    rendering->material_set_shader(material_, shader_);
    rendering->material_set_param(material_, "line_texture", texture_);
    mesh_ = rendering->mesh_create();
    instance_ = rendering->instance_create2(mesh_, scenario);
    return texture_.is_valid() && instance_.is_valid();
}

void RouteGeometry::update(const std::vector<Quad>& quads) {
    auto* rendering = RenderingServer::get_singleton();
    if (!rendering || !mesh_.is_valid()) return;
    rendering->mesh_clear(mesh_);
    if (quads.empty()) return;
    PackedVector3Array positions;
    PackedVector2Array uvs;
    PackedColorArray colours;
    constexpr std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
    const std::array<Vector2, 4> corners{Vector2(1, 1), Vector2(1, 0), Vector2(0, 0), Vector2(0, 1)};
    for (const Quad& item : quads) for (const int corner : indices) {
        const auto& p = item.points[static_cast<std::size_t>(corner)];
        positions.push_back(Vector3(p[0], p[1], p[2]));
        uvs.push_back(corners[static_cast<std::size_t>(corner)]);
        colours.push_back(Color(item.colour.r, item.colour.g, item.colour.b, item.colour.a));
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = positions;
    arrays[Mesh::ARRAY_TEX_UV] = uvs;
    arrays[Mesh::ARRAY_COLOR] = colours;
    rendering->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
    rendering->mesh_surface_set_material(mesh_, 0, material_);
}

void RouteGeometry::release() {
    auto* rendering = RenderingServer::get_singleton();
    if (!rendering) return;
    for (RID* rid : {&instance_, &mesh_, &material_, &shader_, &texture_}) {
        if (rid->is_valid()) rendering->free_rid(*rid);
        *rid = RID();
    }
}
}

using namespace galactic_detail;

void GalacticMode::State::update_fleet_route_preview() {
    fleet_preview_pointer.reset();
    const auto clear = [this] {
        fleet_preview_path.clear(); fleet_preview_publication.reset();
        fleet_preview_source = fleet_preview_destination = 0;
    };
    if (!dragged_fleet || !fleet_drag_receiver || !published_snapshot || !galaxy || tactical || loading
        || !map_input.gates.galactic || !map_input.gates.hud_enabled || !map_input.gates.world_allowed()
        || map_input.gates.tutorial_locked || map_input.gates.action_menu || map_input.gates.selection_locked
        || retreat_dialog_open || (battle_dialog && battle_dialog->visible())
        || published_snapshot->clock().pause_depth) { clear(); return; }
    const auto fleets = published_snapshot->fleets();
    const auto source = std::find_if(fleets.begin(), fleets.end(), [this](const auto& row) { return row.id == dragged_fleet; });
    if (source == fleets.end() || source->owner != galaxy->local.player || !source->orbit || source->motion.transit) {
        clear(); return;
    }
    const Vector2 cursor = fleet_drag_receiver->get_global_mouse_position();
    galactic_view::ObjectId destination{};
    bool world = true;
    for (std::size_t index = 0; index < radar_controls.size(); ++index) {
        const auto* radar = radar_controls[index];
        if (!radar || !radar->is_visible_in_tree() || !radar->get_global_rect().has_point(cursor)) continue;
        world = false;
        for (const auto& [id, blip] : radar_blips[index])
            if (blip->is_visible_in_tree() && blip->get_global_rect().has_point(cursor)) { destination = id; break; }
        break;
    }
    if (world) {
        for (const auto* shell : {command_bar, fleet_organize, advanced_map_shell})
            if (shell && shell->is_visible_in_tree() && shell->hit(cursor.x, cursor.y)) { world = false; break; }
    }
    if (world) {
        for (std::size_t icon = 0; icon < fleet_empty_slots.size(); ++icon)
            if (fleet_empty_shown[icon] && fleet_empty_slots[icon]->is_visible_in_tree()
                && fleet_empty_slots[icon]->get_global_rect().has_point(cursor)) { destination = fleet_drag_planet; break; }
        for (const auto& [key, badge] : fleet_badges)
            if (!destination && badge.shown && badge.button->is_visible_in_tree()
                && badge.button->get_global_rect().has_point(cursor)) destination = key.first;
        if (!destination) if (const auto* picked = picked_planet(cursor.x, cursor.y)) destination = picked->id;
    }
    if (destination) {
        // Use the release's target admission, then its exact route search. Merge,
        // land and same-planet slot drops have no inter-system route.
        if (destination == source->orbit || !fleet_drop(dragged_fleet, cursor, false)) { clear(); return; }
        bool same_inputs = fleet_preview_publication && fleet_preview_source == source->orbit
            && fleet_preview_destination == destination;
        if (same_inputs && fleet_preview_publication != published_snapshot) {
            const auto& old = *fleet_preview_publication;
            const auto& now = *published_snapshot;
            same_inputs = old.maximum_movement_distance() == now.maximum_movement_distance()
                && std::equal(old.planets().begin(), old.planets().end(), now.planets().begin(), now.planets().end())
                && std::equal(old.routes().begin(), old.routes().end(), now.routes().begin(), now.routes().end())
                && old.movement() && now.movement() && old.movement()->access == now.movement()->access;
            if (same_inputs) {
                // Only discovery affects this search; intelligence timers and
                // fleet motion in other publications do not rebuild its graph.
                const auto observed = [&](const auto& snapshot) {
                    std::vector<galactic_view::ObjectId> result;
                    for (const auto& row : snapshot.observations())
                        if (row.player == galaxy->local.player && row.discovered) result.push_back(row.planet);
                    return result;
                };
                same_inputs = observed(old) == observed(now);
            }
        }
        if (!same_inputs) {
            ++fleet_preview_searches;
            const auto planets = input_planets();
            const auto intent = galactic_view::fleet_destination_intent(*published_snapshot,
                galaxy->local.player, dragged_fleet, destination, planets);
            fleet_preview_path = intent && intent->kind == galactic_view::DropKind::move
                ? intent->path : std::vector<sim::galactic::PlanetId>{};
        }
        fleet_preview_source = source->orbit; fleet_preview_destination = destination;
        fleet_preview_publication = published_snapshot;
        return;
    }
    clear();
    if (!world || !displayed_snapshot->maximum_movement_distance()) return;
    // GHUD-S81: the empty-world guide ends on the galaxy's zero plane. It
    // is visual feedback only and cannot become a movement command.
    const Vector3 eye(camera.eye[0], camera.eye[1], camera.eye[2]);
    const Vector3 target(camera.target[0], camera.target[1], camera.target[2]);
    const Vector3 forward = (target - eye).normalized();
    const Vector3 right = forward.cross(Vector3(0, 1, 0)).normalized();
    const Vector3 up = right.cross(forward);
    const float tangent = std::tan(camera.vertical_fov_degrees * std::numbers::pi_v<float> / 360.0F);
    const Vector3 ray = forward + right * ((cursor.x - static_cast<float>(camera.width) * 0.5F)
        * 2.0F * tangent / static_cast<float>(camera.height)) - up * ((cursor.y - static_cast<float>(camera.height) * 0.5F)
        * 2.0F * tangent / static_cast<float>(camera.height));
    if (std::abs(ray.y) < 0.000001F) return; // Parallel to the guide plane.
    const float distance = -eye.y / ray.y;
    if (distance <= 0.0F) return;
    const Vector3 end = eye + ray * distance;
    const auto origin = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
        [&](const auto& row) { return row.id == source->orbit; });
    if (origin != galaxy->planets.end() && point(origin->position).distance_to(end)
        < static_cast<float>(displayed_snapshot->maximum_movement_distance()->raw()) / static_cast<float>(sim::math::Fixed::scale)) {
        fleet_preview_source = source->orbit; fleet_preview_pointer = end;
    }
}

bool GalacticMode::State::build_routes() {
    if (!filesystem) return true; // Asset-free fixture retains its simple overlay.
    auto loaded = galactic_view::load_route_styles(*filesystem);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    route_styles.emplace(std::move(loaded.value()));
    const auto planet = [&](galactic_view::ObjectId id) -> const galactic_view::PlanetView* {
        const auto found = std::find_if(galaxy->planets.begin(), galaxy->planets.end(), [&](const auto& p) { return p.id == id; });
        return found == galaxy->planets.end() ? nullptr : &*found;
    };
    for (const auto& route : galaxy->routes) {
        const std::string name = content && content->route_styles.contains(route.id)
            ? upper(content->route_styles.at(route.id)) : "DEFAULT";
        const auto style = route_styles->named.find(name);
        if (style == route_styles->named.end()) {
            // U-04: missing style remains an explicit unresolved boundary.
            route_styles->diagnostics.push_back("route " + std::to_string(route.id) + ": missing style " + name);
            continue;
        }
        const auto* a = planet(route.from);
        const auto* b = planet(route.to);
        if (!a || !b) continue;
        const auto owners = route_owners(*a, *b);
        auto [entry, inserted] = route_lines.try_emplace(route.id, style->second);
        static_cast<void>(inserted);
        // Apparent owners (WGUI-100); the adapter keeps null owners neutral.
        entry->second.recreate(galactic_view::route_type(owners, options.red_green_routes, route_styles->good_faction, route_styles->evil_faction));
    }
    return true;
}

galactic_view::RouteOwners GalacticMode::State::route_owners(const galactic_view::PlanetView& a,
    const galactic_view::PlanetView& b) const {
    // WGUI-100: trade-route construction and line animation read each
    // endpoint's apparent owner (debug build): the live owner only when the
    // observer may see ownership, else the cached apparent owner, else none.
    // The synthetic fixtures carry no observer and keep their stub owners.
    const bool observed = displayed_snapshot && displayed_snapshot->observer();
    const bool stub = !observed && options.campaign == galactic_view::synthetic_campaign;
    struct End { std::optional<galactic_view::PlayerId> owner; std::string faction; };
    const auto end = [&](const galactic_view::PlanetView& planet) {
        End result;
        if (stub) return End{planet.owner, planet.owner_faction};
        if (observed) {
            const auto row = std::find_if(displayed_snapshot->observations().begin(), displayed_snapshot->observations().end(),
                [&](const auto& item) { return item.player == galaxy->local.player && item.planet == planet.id; });
            if (row != displayed_snapshot->observations().end() && row->discovered && row->apparent_owner
                && *row->apparent_owner != neutral_owner) result.owner = *row->apparent_owner;
        }
        const auto* appearance = content ? galactic_view::affiliation_for_owner(*content, result.owner) : nullptr;
        if (appearance) result.faction = appearance->faction;
        return result;
    };
    const auto first = end(a), second = end(b);
    return {first.owner, second.owner, upper(first.faction) == "NEUTRAL", upper(second.faction) == "NEUTRAL",
        first.faction != second.faction, first.owner == std::optional(galaxy->local.player),
        first.faction, second.faction};
}

bool GalacticMode::State::route_discovered(const galactic_view::PlanetView& a, const galactic_view::PlanetView& b) const {
    // WGUI-99: a trade line is visible exactly when both endpoints are
    // discovered for the local player; the synthetic stub has no observer.
    if (!displayed_snapshot || !displayed_snapshot->observer())
        return options.campaign == galactic_view::synthetic_campaign;
    const auto discovered = [&](const galactic_view::PlanetView& planet) {
        const auto row = std::find_if(displayed_snapshot->observations().begin(), displayed_snapshot->observations().end(),
            [&](const auto& item) { return item.player == galaxy->local.player && item.planet == planet.id; });
        return row != displayed_snapshot->observations().end() && row->discovered;
    };
    return discovered(a) && discovered(b);
}

bool GalacticMode::State::update_routes() {
    update_fleet_route_preview();
    if (!route_styles) return true;
    std::map<std::string, std::vector<RouteGeometry::Quad>> batches;
    const auto ensure_geometry = [&](const std::string& name) -> RouteGeometry* {
        auto& geometry = route_geometry[name];
        if (!geometry) {
            geometry = std::make_unique<RouteGeometry>();
            // Hand-off shares the mounted VFS with resident battle content.
            // A texture first requested after return still uses that same VFS.
            const vfs::Vfs* mounted = filesystem ? &*filesystem
                : (battle_content ? battle_content->filesystem.get() : nullptr);
            if (!mounted) { failure = "route texture source unavailable"; return nullptr; }
            if (!geometry->initialize(scenes[0].host->get_world_3d()->get_scenario(), *mounted, name, failure)) return nullptr;
        }
        return geometry.get();
    };
    const Vector3 eye(camera.eye[0], camera.eye[1], camera.eye[2]);
    route_quads = 0;
    spark_quads = 0;
    fleet_line_quads = 0;
    fleet_preview_quads = 0;
    const bool service = route_animation_updates != animation_updates;
    const auto& flight = seat_flight();
    // WGP-13 / U-04 research: outward alpha is complemented, focused is exactly 1.
    const float zoom = flight.state() == galactic_view::CameraState::focused ? 1.0F :
        (flight.state() == galactic_view::CameraState::outward ? 1.0F - flight.alpha() :
         flight.state() == galactic_view::CameraState::overview ? 0.0F : flight.alpha());
    for (const auto& route : galaxy->routes) {
        auto found = route_lines.find(route.id);
        if (found == route_lines.end()) continue;
        auto& line = found->second;
        if (line.set_zoom(zoom)) spark_services[route.id] = 0;
        if (line.style().render_mode != "ADDITIVE") {
            failure = "unsupported route render mode " + line.style().render_mode;
            return false;
        }
        const auto from = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [&](const auto& item) { return item.id == route.from; });
        const auto to = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [&](const auto& item) { return item.id == route.to; });
        if (from == galaxy->planets.end() || to == galaxy->planets.end() || !route_discovered(*from, *to)) continue;
        const Vector3 a = point(route.a), b = point(route.b), delta = b - a;
        if (delta.length_squared() == 0.0F) continue;
        const Vector3 along = delta.normalized();
        const Vector3 side = along.cross(eye - (a + b) * 0.5F).normalized();
        auto* geometry = ensure_geometry(line.style().texture);
        if (!geometry) return false;
        const float width = line.style().width > 0.0F ? line.style().width : geometry->texture_width();
        batches[line.style().texture].push_back(quad(a, b, side * (width * 0.5F), line.colour()));
        ++route_quads;
        if (line.sparks()) {
            auto& count = spark_services[route.id];
            if (service) ++count;
            auto colour = line.colour();
            colour.r *= galactic_view::route_spark_colour_scale;
            colour.g *= galactic_view::route_spark_colour_scale;
            colour.b *= galactic_view::route_spark_colour_scale;
            colour.a *= galactic_view::route_spark_colour_scale;
            const float half = galactic_view::route_spark_size * 0.5F;
            for (const float initial : galactic_view::route_spark_positions) {
                const Vector3 centre = a + delta * galactic_view::spark_position(initial, count);
                batches[line.style().spark_texture].push_back(quad(centre - along * half, centre + along * half, side * half, colour));
                ++spark_quads;
            }
        }
    }
    for (const auto& marker : galaxy->fleets) {
        if (!marker.transit) continue; // Locally visible markers are the observer adapter's input.
        const auto destination = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [&](const auto& item) { return item.id == marker.destination; });
        if (destination == galaxy->planets.end()) continue;
        const auto chosen = planet_models.at(destination->id);
        if (!chosen) continue; // A model extent is required; never substitute the pick radius.
        const auto line = galactic_view::fleet_line(marker.position, true, destination->position,
            destination->position, models[*chosen].x_extent * destination->scale, true, marker.colour);
        if (!line) continue;
        const Vector3 a = point(line->a), b = point(line->b);
        const Vector3 side = (b - a).normalized().cross(eye - (a + b) * 0.5F).normalized();
        batches[route_styles->fleet_texture].push_back(quad(a, b, side, {line->colour.r, line->colour.g, line->colour.b, 1.0F}));
        ++fleet_line_quads;
    }
    // GHUD-S80: full planet-centre hops, width 2, neutral opaque additive tint.
    const auto preview_segment = [&](Vector3 a, Vector3 b, float tint) {
        if (route_styles->band_texture.empty() || a == b) return;
        const Vector3 side = (b - a).normalized().cross(eye - (a + b) * 0.5F).normalized();
        batches[route_styles->band_texture].push_back(quad(a, b, side, {tint, tint, tint, 1.0F}));
        ++fleet_preview_quads;
    };
    const auto preview_planet = [this](const auto id) -> const galactic_view::PlanetView* {
        const auto row = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [id](const auto& value) { return value.id == id; });
        return row == galaxy->planets.end() ? nullptr : &*row;
    };
    for (std::size_t hop = 1; hop < fleet_preview_path.size(); ++hop) {
        const auto* a = preview_planet(fleet_preview_path[hop - 1]);
        const auto* b = preview_planet(fleet_preview_path[hop]);
        if (a && b) preview_segment(point(a->position), point(b->position), 128.0F / 255.0F);
    }
    if (fleet_preview_pointer) {
        const auto* source = preview_planet(fleet_preview_source);
        if (source) preview_segment(point(source->position), *fleet_preview_pointer, 50.0F / 255.0F);
    }
    for (auto& [name, batch] : batches) {
        auto* geometry = ensure_geometry(name);
        if (!geometry) return false;
        geometry->update(batch);
    }
    for (auto& [name, geometry] : route_geometry) if (!batches.contains(name)) geometry->update({});
    if (fleet_drag_receiver) {
        // Native GPU input cases inspect the live band before releasing it.
        fleet_drag_receiver->set_name("fleet_route_preview");
        Dictionary preview;
        Array path;
        for (const auto id : fleet_preview_path) path.push_back(static_cast<std::int64_t>(id));
        preview["path"] = path;
        preview["segments"] = static_cast<std::int64_t>(fleet_preview_quads);
        preview["searches"] = static_cast<std::int64_t>(fleet_preview_searches);
        preview["pointer_guide"] = fleet_preview_pointer.has_value();
        fleet_drag_receiver->set_meta("route_preview", preview);
    }
    route_animation_updates = animation_updates;
    return true;
}

} // namespace eawr::presentation::godot_backend
