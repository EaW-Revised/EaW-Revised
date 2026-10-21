#include "gpu_debug_labels.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <ostream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto list = component.list(field);
    return list.empty() ? std::string{} : list.front();
}
}

bool GalacticMode::State::build_hero_map_bar() {
    const auto* component = command_catalog.find("g_hero_icon");
    if (!component || !hud) return true; // A mod may omit this optional surface.
    hero_map_bar = memnew(Control);
    hud->add_child(hero_map_bar);
    hero_map_bar->set_name("galactic_hero_bar");
    hero_map_bar->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    hero_map_bar->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    hero_map_bar->set_z_index(3);
    // WGUI-89 capture: circular portraits. The friendly slot supplies authored
    // ring art; this GC component-to-ring binding is explicitly unverified.
    const auto* ring = command_catalog.find("hero_slot_good00");
    Ref<Shader> circle; circle.instantiate();
    name_canvas_shader(*circle.ptr(), "EAWR/UI/HeroPortraitCircle");
    circle->set_code(R"(shader_type canvas_item;
uniform sampler2D build_texture : source_color;
uniform bool has_build_texture = false;
uniform float completion = 1.0;
void fragment() {
    // PU-64: project the ray to the eight-segment square rim, clockwise from noon.
    vec2 ray = vec2(UV.x - 0.5, 0.5 - UV.y);
    vec2 rim = ray / max(2.0 * max(abs(ray.x), abs(ray.y)), 0.00001);
    float step;
    if (rim.y >= abs(rim.x)) step = rim.x >= 0.0 ? 2.0 * rim.x : 8.0 + 2.0 * rim.x;
    else if (rim.x >= abs(rim.y)) step = 2.0 - 2.0 * rim.y;
    else if (-rim.y >= abs(rim.x)) step = 4.0 - 2.0 * rim.x;
    else step = 6.0 + 2.0 * rim.y;
    if (has_build_texture && completion < 1.0 && step >= completion * 8.0)
        COLOR.rgb += texture(build_texture, UV).rgb;
    COLOR.a *= 1.0 - smoothstep(0.47, 0.49, distance(UV, vec2(0.5)));
})");
    const auto build = command_texture(first(*component, data::ui::Field::build_texture_name));
    for (std::size_t index = 0; index < hero_map_buttons.size(); ++index) {
        auto* button = memnew(EawrHudButton);
        hero_map_buttons[index] = button;
        hero_map_bar->add_child(button);
        button->set_name(String::utf8(("galactic_hero_" + std::to_string(index)).c_str()));
        button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        if (ring) {
            button->set_texture_normal(command_texture(first(*ring, data::ui::Field::blank_texture_name)));
            button->set_texture_hover(command_texture(first(*ring, data::ui::Field::mouse_over_texture_name)));
        }
        // The authored ring is a filled backplate; the circular portrait draws over it.
        button->set_order_icon({}, false);
        auto* portrait = Object::cast_to<TextureRect>(button->get_child(0));
        Ref<ShaderMaterial> material; material.instantiate(); material->set_shader(circle);
        material->set_shader_parameter("has_build_texture", build.is_valid());
        if (build.is_valid()) material->set_shader_parameter("build_texture", build);
        portrait->set_material(material);
        button->set_action([this, index] {
            if (index >= hero_map_entries.size() || !map_input.hud_event() || !map_input.gates.world_allowed()) return;
            const auto entry = hero_map_entries[index];
            if (entry.scheduled) return; // No fabricated live actor or unqualified scheduled click action.
            // Share the existing select_hero API path and re-resolve live containment.
            select_hero(entry.type, false, false);
            if (map_input.selected == entry.planet) {
                ++hero_map_clicks; hero_map_selected_type = entry.type;
            }
        });
        button->hide();
    }
    hero_map_bar->hide();
    return true;
}

void GalacticMode::State::update_hero_map_bar() {
    if (!hero_map_bar || !galaxy) return;
    // Ownership uses the session publication even when omniscient drawing is on.
    const auto publication = published_snapshot;
    if (publication != hero_map_publication || published_hero_returns != hero_map_returns) {
        hero_map_publication = publication;
        hero_map_returns = published_hero_returns;
        hero_map_entries = publication
            ? galactic_view::collect_map_heroes(*publication, galaxy->local.player, production_names, production_art,
                hero_map_returns ? std::span<const sim::galactic::HeroReturnView>(*hero_map_returns)
                    : std::span<const sim::galactic::HeroReturnView>{})
            : std::vector<galactic_view::HeroMapEntry>{};
        for (std::size_t index = 0; index < hero_map_buttons.size(); ++index) {
            auto* button = hero_map_buttons[index];
            button->set_visible(index < hero_map_entries.size());
            if (index >= hero_map_entries.size()) continue;
            const auto& look = production_art.at(production_names.at(hero_map_entries[index].type));
            button->set_icon_texture(command_texture(look.icon));
            auto* portrait = Object::cast_to<TextureRect>(button->get_child(0));
            const Ref<ShaderMaterial> material = portrait->get_material();
            material->set_shader_parameter("completion", hero_map_entries[index].completion);
            auto tooltip = look.text;
            if (!look.encyclopedia_looks.unit_class.empty()) tooltip += "\n" + look.encyclopedia_looks.unit_class;
            button->set_tooltip_text(String::utf8(tooltip.c_str()));
        }
    }
    hero_map_bar->set_visible(!hero_map_entries.empty() && !seat_flight().advanced_map() && !tactical && !loading);
    const auto* component = command_catalog.find("g_hero_icon");
    if (!component) return;
    // Capture qualification at 1280x720: roughly 38px diameter, 40px pitch,
    // 8px top and 24px right. Other resolutions use this presentation adapter.
    const float scale = static_cast<float>(camera.height) / 720.0F;
    const float diameter = 48.0F * static_cast<float>(component->number(data::ui::Field::scale).value_or(1.0)) * scale;
    const float pitch = diameter + 2.0F * scale;
    const float width = hero_map_entries.empty() ? 0.0F : static_cast<float>(hero_map_entries.size() - 1) * pitch + diameter;
    const float left = static_cast<float>(camera.width) - 24.0F * scale - width;
    for (std::size_t index = 0; index < hero_map_entries.size(); ++index) {
        auto* button = hero_map_buttons[index];
        button->set_position(Vector2(left + static_cast<float>(index) * pitch, 8.0F * scale));
        button->set_size(Vector2(diameter, diameter));
        button->set_hit_rect(Rect2(0, 0, diameter, diameter));
        const float inset = 2.0F * scale;
        button->set_cell_portrait_box(Rect2(inset, inset, diameter - 2.0F * inset, diameter - 2.0F * inset));
        button->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked
            || map_input.gates.selection_locked);
    }
}

void GalacticMode::State::write_hero_map_report(std::ostream& output) const {
    using galactic_detail::json;
    const auto* component = command_catalog.find("g_hero_icon");
    output << "{\"component\": \"g_hero_icon\", \"source\": \"published-local-heroes\", \"visible\": "
        << (hero_map_bar && hero_map_bar->is_visible_in_tree() ? "true" : "false")
        << ", \"clicks\": " << hero_map_clicks << ", \"selected_type\": " << hero_map_selected_type
        << ", \"local_player\": " << (galaxy ? galaxy->local.player : 0)
        << ", \"current_frame\": " << (hero_map_publication ? hero_map_publication->clock().galactic_frame : 0)
        << ", \"build_texture\": " << json(component ? first(*component, data::ui::Field::build_texture_name) : "")
        << ", \"entries\": [";
    for (std::size_t index = 0; index < hero_map_entries.size(); ++index) {
        const auto& entry = hero_map_entries[index];
        const auto& look = production_art.at(production_names.at(entry.type));
        const auto rect = hero_map_buttons[index]->get_global_rect();
        output << (index ? ", " : "") << "{\"type\": " << entry.type << ", \"actor\": " << entry.actor
            << ", \"planet\": " << entry.planet << ", \"rank\": " << entry.rank
            << ", \"scheduled\": " << (entry.scheduled ? "true" : "false")
            << ", \"start_frame\": " << entry.start_frame << ", \"deadline\": " << entry.deadline
            << ", \"completion\": " << entry.completion << ", \"name\": " << json(look.name)
            << ", \"icon\": " << json(look.icon) << ", \"tooltip\": " << json(galactic_detail::utf8(hero_map_buttons[index]->get_tooltip_text()))
            << ", \"presses\": " << hero_map_buttons[index]->presses()
            << ", \"portrait_shown\": " << (hero_map_buttons[index]->icon_shown() ? "true" : "false")
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "], \"foreign_named_actors\": [";
    bool first_actor = true;
    if (hero_map_publication && galaxy) for (const auto& actor : hero_map_publication->actor_records()) {
        if (actor.owner == galaxy->local.player || actor.dead || actor.delete_pending) continue;
        const auto name = production_names.find(actor.current_type);
        if (name == production_names.end()) continue;
        const auto look = production_art.find(name->second);
        if (look == production_art.end() || !look->second.named_hero) continue;
        output << (first_actor ? "" : ", ") << "{\"actor\": " << actor.id << ", \"type\": " << actor.current_type
            << ", \"owner\": " << actor.owner << ", \"name\": " << json(name->second) << "}";
        first_actor = false;
    }
    output << "]}";
}
} // namespace eawr::presentation::godot_backend
