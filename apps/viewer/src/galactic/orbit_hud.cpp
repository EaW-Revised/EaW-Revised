#include "galactic_mode_internal.hpp"
#include "model_preview.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <algorithm>
#include <charconv>
#include <ostream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::optional<std::size_t> slot_index(std::string_view name) {
    constexpr std::string_view prefix = "space_Pad_";
    if (!name.starts_with(prefix)) return {};
    std::size_t index{};
    const auto suffix = name.substr(prefix.size());
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || !index) return {};
    return index - 1;
}
Ref<Texture2D> first_texture(GalacticMode::State& state, const data::ui::CommandBarComponent& component,
    data::ui::Field field) {
    const auto list = component.list(field);
    return list.empty() ? Ref<Texture2D>{} : state.command_texture(list.front());
}
}

bool GalacticMode::State::build_orbit_hud() {
    if (!fleet_organize) return true;
    if (options.hud_provider && !options.hud_provider->orbit(*this)) return false;
    // GHUD-S79: enumerate the authored components, never a fixed two-slot table.
    for (const auto& anchor : fleet_anchors.anchors()) {
        const auto index = slot_index(anchor.name);
        if (!index || orbit_hud.slots.contains(*index)) continue;
        const auto* component = command_catalog.find(anchor.name);
        if (!component) continue;
        auto* button = memnew(EawrHudButton);
        fleet_organize->add_child(button);
        button->set_name(String::utf8(anchor.name.c_str()));
        button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        button->setup_art({.normal = first_texture(*this, *component, data::ui::Field::blank_texture_name),
            .hover = first_texture(*this, *component, data::ui::Field::mouse_over_texture_name),
            .pressed = first_texture(*this, *component, data::ui::Field::selected_texture_name),
            .portrait = HudButtonArt::Portrait{{}, false, false}});
        button->set_action([] {});
        orbit_hud.slots.emplace(*index, OrbitHudState::Slot{anchor.name, button});
    }
    return true;
}

void GalacticMode::State::update_orbit_hud() {
    if (!galaxy || !displayed_snapshot || !fleet_organize) return;
    const auto observations = displayed_snapshot->observations();
    const auto seen = std::find_if(observations.begin(), observations.end(), [this](const auto& row) {
        return row.player == galaxy->local.player && row.planet == selected_planet;
    });
    const bool discovered = seen != observations.end() && seen->discovered;
    const bool structures_known = discovered && seen->knowledge.structures;
    const auto* production = displayed_snapshot->production() ? &*displayed_snapshot->production() : nullptr;
    const sim::galactic::PlanetProduction* producer = nullptr;
    if (production) {
        const auto found = std::find_if(production->planets.begin(), production->planets.end(),
            [this](const auto& row) { return row.planet == selected_planet; });
        if (found != production->planets.end()) producer = &*found;
    }
    orbit_hud.structures_known = structures_known;
    orbit_hud.capacity = discovered && producer ? static_cast<std::size_t>(std::max<std::int64_t>(producer->space_slots, 0)) : 0;
    std::vector<const sim::galactic::ProductionObject*> structures;
    if (structures_known && producer) for (const auto& object : producer->objects)
        if (object.domain == sim::galactic::ProductionDomain::space && object.kind == sim::galactic::ProductionKind::structure)
            structures.push_back(&object);
    const auto placement = fleet_organize->placement();
    for (auto& [index, slot] : orbit_hud.slots) {
        auto* button = static_cast<EawrHudButton*>(slot.button);
        const auto* anchor = fleet_anchors.find(slot.name);
        if (!anchor) continue;
        const auto rect = presentation::ui::shell_to_screen(anchor->rect, placement);
        button->set_position({static_cast<float>(rect.x), static_cast<float>(rect.y)});
        button->set_size({static_cast<float>(rect.width), static_cast<float>(rect.height)});
        button->set_hit_rect({{}, button->get_size()});
        const auto* object = index < structures.size() ? structures[index] : nullptr;
        Ref<Texture2D> icon;
        if (object) if (const auto name = production_names.find(object->type); name != production_names.end())
            if (const auto art = production_art.find(name->second); art != production_art.end()) icon = command_texture(art->second.icon);
        button->set_icon_texture(icon);
        // Unlike the land row, retail leaves every authored orbital pad drawn,
        // including empty pads; knowledge filters the icon, not the pad's alpha.
        button->set_visible(discovered);
        button->set_disabled(false);
        button->set_modulate(Color(1, 1, 1, 1));
        slot.object = object ? object->id : 0; slot.type = object ? object->type : 0;
    }

    bool shown = fleet_organize->is_visible_in_tree() && !tactical && !loading && discovered
        && seen->knowledge.space_level && producer && producer->space_level > 0 && producer->starbase_type;
    std::string model;
    galactic_view::Vec3 position;
    if (shown) {
        const auto name = production_names.find(producer->starbase_type);
        if (name != production_names.end()) if (const auto art = production_art.find(name->second); art != production_art.end())
            model = art->second.galactic_model;
        const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [this](const auto& row) { return row.id == selected_planet; });
        const auto upload = planet_models.find(selected_planet);
        shown = !model.empty() && planet != galaxy->planets.end() && upload != planet_models.end()
            && upload->second && models[*upload->second].station_anchor.has_value();
        if (shown) {
            const auto anchor = *models[*upload->second].station_anchor;
            // GHUD-S78 / R-ROT-01: the attachment inherits the planet's
            // fixed +Z quarter turn and world scale. The station stays unit scale.
            position = {planet->position.x - anchor.y * planet->scale,
                planet->position.y + anchor.x * planet->scale, planet->position.z + anchor.z * planet->scale};
        }
    }
    if (shown) shown = std::any_of(models.begin(), models.end(), [&](const auto& upload) {
        return upload.layer == 0 && upload.logical_path == model && upload.status == "drawn";
    });
    const bool changed = shown != orbit_hud.shown || model != orbit_hud.model || selected_planet != orbit_hud.planet;
    orbit_hud.shown = shown; orbit_hud.model = model; orbit_hud.position = position; orbit_hud.planet = selected_planet;
    orbit_hud.station_type = shown ? producer->starbase_type : 0;
    if (changed) {
        // The display ID is stable; remap its cached asset when a station upgrade changes the model.
        planet_models.erase(orbit_station_id);
        if (!refresh_model_instances()) return;
        // GHUD-S78: focus/upgrade topology changes must publish even at a paused tick.
        submit();
    }
}

void GalacticMode::State::write_orbit_report(std::ostream& out) const {
    std::size_t drawn{};
    for (const auto& instance : scenes[0].instances)
        if (instance_planets.at(instance.entity_id) == orbit_station_id) ++drawn;
    out << "{\"planet\":" << orbit_hud.planet << ",\"station_type\":" << orbit_hud.station_type
        << ",\"station_visible\":" << (orbit_hud.shown ? "true" : "false")
        << ",\"model\":" << galactic_detail::json(orbit_hud.model) << ",\"station_position\":["
        << orbit_hud.position.x << ',' << orbit_hud.position.y << ',' << orbit_hud.position.z
        << "],\"station_rotation\":" << orbit_hud.rotation
        << ",\"rotation_rate\":" << orbit_hud.rotation_rate
        << ",\"rotation_updates\":" << orbit_hud.rotation_updates
        << ",\"display_delta\":" << orbit_hud.display_delta
        << ",\"rotation_probe_advances\":[" << orbit_hud.rotation_probe_advances[0] << ','
        << orbit_hud.rotation_probe_advances[1] << ',' << orbit_hud.rotation_probe_advances[2] << ','
        << orbit_hud.rotation_probe_advances[3] << ',' << orbit_hud.rotation_probe_advances[4] << ']'
        << ",\"station_surfaces\":" << drawn
        << ",\"capacity\":" << orbit_hud.capacity << ",\"structures_known\":"
        << (orbit_hud.structures_known ? "true" : "false") << ",\"station_rect\":";
    bool rect_written = false;
    if (orbit_hud.shown) {
        const auto found = planet_models.find(orbit_station_id);
        if (found != planet_models.end() && found->second && models[*found->second].model_bounds) {
            const auto& bounds = *models[*found->second].model_bounds;
            auto transform = animation::Player::identity_matrix();
            // Source +Z rotation is render +Y rotation (GFC-47).
            transform[0] = transform[10] = std::cos(orbit_hud.rotation);
            transform[2] = -std::sin(orbit_hud.rotation);
            transform[8] = std::sin(orbit_hud.rotation);
            const auto point = galactic_view::render_point(orbit_hud.position);
            std::copy(point.begin(), point.end(), transform.begin() + 12);
            const auto world = viewer::model_preview::world_bounds({{bounds[0], bounds[1], bounds[2]},
                {bounds[3], bounds[4], bounds[5]}}, transform);
            const auto projection = viewer::model_preview::project_corners(world, camera);
            if (projection.finite) {
                out << '[' << (projection.ndc_min_x + 1) * static_cast<float>(camera.width) * 0.5F
                    << ',' << (1 - projection.ndc_max_y) * static_cast<float>(camera.height) * 0.5F
                    << ',' << (projection.ndc_max_x - projection.ndc_min_x) * static_cast<float>(camera.width) * 0.5F
                    << ',' << (projection.ndc_max_y - projection.ndc_min_y) * static_cast<float>(camera.height) * 0.5F << ']';
                rect_written = true;
            }
        }
    }
    if (!rect_written) out << "null";
    out << ",\"slots\":[";
    bool comma = false;
    for (const auto& [index, slot] : orbit_hud.slots) {
        if (comma) out << ',';
        comma = true;
        const auto rect = slot.button->get_global_rect();
        out << "{\"index\":" << index << ",\"name\":" << galactic_detail::json(slot.name)
            << ",\"visible\":" << (slot.button->is_visible_in_tree() ? "true" : "false")
            << ",\"object\":" << slot.object << ",\"type\":" << slot.type
            << ",\"alpha\":" << slot.button->get_modulate().a << ",\"rect\":["
            << rect.position.x << ',' << rect.position.y << ',' << rect.size.x << ',' << rect.size.y << "]}";
    }
    out << "]}";
}
} // namespace eawr::presentation::godot_backend
