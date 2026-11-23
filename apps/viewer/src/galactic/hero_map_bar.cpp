#include "gpu_debug_labels.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
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
    // The first heroes sit on the good hero frame's authored slot bones (hero_slot_good00..), so a data set with more
    // or fewer slots, or its own rows, lays out as authored; heroes past the slots fill the rows below.
    hero_map_frame = {};
    if (const auto anchors = data::ui::load_shell_anchors(*filesystem, presentation::ui::hero_frame_model(
            command_catalog, presentation::ui::HeroSide::good)); anchors) {
        hero_map_frame = presentation::ui::hero_frame(anchors.value().shell, command_catalog, presentation::ui::HeroSide::good);
    }
    if (hero_map_frame.slots.empty()) {
        // A data set without the frame: one row of the old pitch, the same extra-row rule behind it.
        for (int slot = 0; slot < 2; ++slot) {
            presentation::ui::HudHeroSlot synthetic;
            synthetic.button.scale = 0.8F;
            synthetic.button.origin = {-52.0F * static_cast<float>(slot), 0.0F};
            hero_map_frame.slots.push_back(std::move(synthetic));
        }
    }
    hero_map_buttons.clear();
    // WGUI-89 capture: circular portraits. The friendly slot supplies authored
    // ring art; this GC component-to-ring binding is explicitly unverified.
    hero_map_circle.instantiate();
    name_canvas_shader(*hero_map_circle.ptr(), "EAWR/UI/HeroPortraitCircle");
    hero_map_circle->set_code(R"(shader_type canvas_item;
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
    hero_map_build = command_texture(first(*component, data::ui::Field::build_texture_name));
    hero_map_bar->hide();
    return true;
}

void GalacticMode::State::ensure_hero_map_buttons(const std::size_t count) {
    const auto* component = command_catalog.find("g_hero_icon");
    const auto* ring = command_catalog.find("hero_slot_good00");
    if (!component || !hero_map_bar) return;
    while (hero_map_buttons.size() < count) {
        const std::size_t index = hero_map_buttons.size();
        auto* button = memnew(EawrHudButton);
        hero_map_buttons.push_back(button);
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
        Ref<ShaderMaterial> material; material.instantiate(); material->set_shader(hero_map_circle);
        material->set_shader_parameter("has_build_texture", hero_map_build.is_valid());
        if (hero_map_build.is_valid()) material->set_shader_parameter("build_texture", hero_map_build);
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
        ensure_hero_map_buttons(hero_map_entries.size());
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
    // Retail pins the good frame at the upper right of the screen; the first slot's bone sits 44 px from the right edge
    // and 28 px from the top at 1280x720 (capture 2026-10-20, 46.9 and 29.9 reference units), the other slots follow
    // their own bones. Heroes past the authored slots fill rows below them; the strip stays in the right half of the
    // safe area (clear of the top status bar) and above the planet card, shrinking the rows' spacing before it would
    // leave them.
    if (!command_bar) return;
    const auto space = command_bar->space();
    const auto placement = presentation::ui::hero_frame_placement(space, hero_map_frame, 46.9, 29.9, false);
    const presentation::ui::HeroStripBounds bounds{(space.safe_left + space.safe_width / 2.0) * space.scale,
                                                  space.height * 0.45 * space.scale};
    const auto layout = presentation::ui::hero_strip_layout(hero_map_frame, placement, hero_map_entries.size(), bounds,
                                                           50.0 * placement.scale);
    const auto& authored = hero_map_frame.slots.front().button;
    for (std::size_t index = 0; index < hero_map_entries.size(); ++index) {
        auto* button = hero_map_buttons[index];
        // The ring texture sized by the slot's authored scale; the portrait is the circle inside it.
        const auto ring = command_texture(hero_map_frame.slots[std::min(index, hero_map_frame.slots.size() - 1)].button.blank);
        const float texels = ring.is_valid() ? static_cast<float>(ring->get_width()) : 48.0F;
        const float diameter = texels * authored.scale * static_cast<float>(placement.scale);
        const float scale = static_cast<float>(space.scale);
        const auto centre = layout.centres[index];
        button->set_position(Vector2(static_cast<float>(centre.x) - diameter / 2.0F, static_cast<float>(centre.y) - diameter / 2.0F));
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
