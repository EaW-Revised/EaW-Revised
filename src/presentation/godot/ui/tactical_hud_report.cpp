#include "tactical_hud_internal.hpp"

namespace eawr::presentation::godot_backend {
using tactical_hud_detail::text;
using tactical_hud_detail::rect2;

namespace {
[[nodiscard]] std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

} // namespace

void TacticalHud::State::run_probe() {
    Viewport* viewport = hud->get_viewport();
    if (viewport == nullptr) return;
    const model::ShellPlacement placement = hud->placement();
    const auto screen = [&](const double x, const double y) {
        const model::ReferencePoint point = model::shell_point_to_screen(x, y, placement);
        return Vector2(static_cast<float>(point.x), static_cast<float>(point.y));
    };
    const Vector2 size = hud->get_size();
    const Vector2 sky(size.x * 0.5F, size.y * 0.25F);
    const auto move = [&](const Vector2& at) {
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(at);
        motion->set_global_position(at);
        viewport->push_input(motion, true);
    };
    std::vector<std::pair<std::string, Vector2>> points;
    if (options_button != nullptr) {
        points.emplace_back("options", options_button->get_position() + options_button->get_size() * 0.5F);
    }
    // The art overhangs the component's mesh; only the mesh takes the click.
    // Probe half a shell unit inside each side edge and 3 units into each overhang.
    if (shell.options) {
        const data::ui::ReferenceRect& rect = shell.options->rect;
        const double y = rect.y + rect.height / 2.0;
        points.emplace_back("options_inside_left", screen(rect.x + 0.5, y));
        points.emplace_back("options_inside_right", screen(rect.x + rect.width - 0.5, y));
        points.emplace_back("options_overhang_left", screen(rect.x - 3.0, y));
        points.emplace_back("options_overhang_right", screen(rect.x + rect.width + 3.0, y));
    }
    points.emplace_back("sky", sky);
    if (shell.minimap) {
        points.emplace_back("minimap", screen(shell.minimap->x + shell.minimap->width / 2.0,
                                              shell.minimap->y + shell.minimap->height / 2.0));
    }
    // Section 1.3 geometry: panel art between the tech and population texts, and
    // the transparent sky over the unit strip inside the HUD's extent.
    points.emplace_back("faceplate_art", screen(150.0, 215.0));
    points.emplace_back("faceplate_gap", screen(700.0, 250.0));
    for (const auto& [name, at] : points) {
        const int world_before = world_presses;
        const int button_before = options_button != nullptr ? options_button->presses() : 0;
        move(at);
        for (const bool pressed : {true, false}) {
            Ref<InputEventMouseButton> click;
            click.instantiate();
            click->set_button_index(MOUSE_BUTTON_LEFT);
            click->set_pressed(pressed);
            click->set_button_mask(pressed ? BitField<MouseButtonMask>(MOUSE_BUTTON_MASK_LEFT)
                                           : BitField<MouseButtonMask>(0));
            click->set_position(at);
            click->set_global_position(at);
            viewport->push_input(click, true);
        }
        const int button_after = options_button != nullptr ? options_button->presses() : 0;
        probes.push_back({name, at, hud->hit(at.x, at.y), world_presses - world_before, button_after - button_before});
    }
    // Leave the pointer over the world, so the capture shows every button unhovered.
    move(sky);
    probed = true;
}


std::string TacticalHud::report_json() const {
    const State& state = *state_;
    std::ostringstream output;
    output << "{\"mode\": \"tactical\", \"faction\": " << json(model::to_string(state.options.faction))
           << ", \"rules\": " << json(state.options.rules == model::LayoutRules::retail ? "retail" : "aspect")
           << ", \"shell_model\": " << json(state.shell_model) << ", \"atlas\": " << json(state.atlas_path)
           << ", \"faceplate_masks\": " << state.faceplates << ", \"meshes\": [";
    for (std::size_t index = 0; index < state.mesh_textures.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(state.mesh_textures[index]);
    }
    output << "], \"options_textures\": [";
    for (std::size_t index = 0; index < state.button_textures.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(state.button_textures[index]);
    }
    output << "], \"planet_name\": {\"text\": " << json(state.planet.text)
           << ", \"source\": " << json(model::to_string(state.planet.source))
           << ", \"context\": " << json(state.planet.context) << ", \"text_id\": " << json(state.planet.text_id)
           << ", \"face\": " << json(state.planet_face.face)
           << ", \"face_source\": " << json(model::to_string(state.planet_face.source)) << "}"
           << ", \"font_cache\": {\"directory\": " << json(state.fonts ? state.fonts->cache().directory : std::string())
           << ", \"source\": " << json(state.options.font_cache_source) << "}";
    if (state.hud != nullptr) {
        const Vector2 size = state.hud->get_size();
        const model::ShellPlacement shell = state.hud->placement();
        output << ", \"viewport\": [" << size.x << ", " << size.y << "]"
               << ", \"placement\": {\"left\": " << shell.left << ", \"bottom\": " << shell.bottom
               << ", \"scale\": " << shell.scale << "}";
        if (state.shell.minimap) {
            output << ", \"minimap_rect\": " << rect_json(rect2(model::shell_to_screen(*state.shell.minimap, shell)));
        }
        output << ", \"panel_buttons\": [";
        for (std::size_t index = 0; index < state.panel.size(); ++index) {
            const State::PanelArt& art = state.panel[index];
            output << (index == 0 ? "" : ", ") << "{\"name\": " << json(art.name) << ", \"texture\": "
                   << json(art.texture) << ", \"origin\": " << json(art.origin) << ", \"rect\": "
                   << rect_json(rect2(model::shell_to_screen(art.quad, shell))) << "}";
        }
        output << "]";
        if (state.options_button != nullptr) {
            output << ", \"options_rect\": "
                   << rect_json(Rect2(state.options_button->get_position(), state.options_button->get_size()))
                   << ", \"options_hit_rect\": "
                   << rect_json(Rect2(state.options_button->get_position() + state.options_button->hit_rect().position,
                                      state.options_button->hit_rect().size));
        }
        output << ", \"planet_rect\": " << rect_json(state.hud->planet_rect())
               << ", \"planet_pixels\": " << state.hud->planet_pixels();
    }
    if (state.options.probe) {
        output << ", \"probe\": {\"ran\": " << (state.probed ? "true" : "false") << ", \"clicks\": [";
        for (std::size_t index = 0; index < state.probes.size(); ++index) {
            const State::Probe& probe = state.probes[index];
            output << (index == 0 ? "" : ", ") << "{\"name\": " << json(probe.name) << ", \"point\": ["
                   << probe.point.x << ", " << probe.point.y << "], \"hud_hit\": " << (probe.hud_hit ? "true" : "false")
                   << ", \"world\": " << probe.world << ", \"button\": " << probe.button << "}";
        }
        output << "]}";
    }
    if (state.cards != nullptr) output << ", \"unit_cards\": " << state.cards->report_json();
    if (state.production != nullptr) output << ", \"production\": " << state.production->report_json();
    if (state.abilities != nullptr) output << ", \"ability_buttons\": " << state.abilities->report_json();
    if (state.minimap != nullptr) {
        output << ", \"minimap\": " << state.minimap->report_json() << ", \"minimap_fog\": {\"fogged\": "
               << state.minimap_fog.fogged() << ", \"next_row\": " << state.minimap_fog.next_row()
               << ", \"rows_per_frame\": " << model::MinimapFog::rows_per_frame << "}";
    }
    // #459: the time panel's buttons; #453/#459: the overlay.
    output << ", \"time_panel\": {\"textures\": [";
    for (std::size_t index = 0; index < state.time_textures.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(state.time_textures[index]);
    }
    output << "]";
    for (const auto& [name, button] : {std::pair<const char*, const EawrHudButton*>{"pause", state.pause_button},
                                       std::pair<const char*, const EawrHudButton*>{"fast_forward", state.fast_forward_button}}) {
        output << ", \"" << name << "\": ";
        if (button == nullptr) {
            output << "null";
            continue;
        }
        output << "{\"rect\": " << rect_json(Rect2(button->get_position(), button->get_size())) << ", \"hit_rect\": "
               << rect_json(Rect2(button->get_position() + button->hit_rect().position, button->hit_rect().size))
               << ", \"pressed\": " << (button->is_pressed() ? "true" : "false")
               << ", \"disabled\": " << (button->is_disabled() ? "true" : "false")
               << ", \"presses\": " << button->presses() << "}";
    }
    output << "}";
    if (state.overlay != nullptr) output << ", \"battle_overlay\": " << state.overlay->report_json();
    output << ", \"overview\": " << (state.overview ? "true" : "false")
           << ", \"shell_shown\": " << (shell_shown() ? "true" : "false");
    output << ", \"options_presses\": " << options_presses() << ", \"diagnostics\": [";
    for (std::size_t index = 0; index < state.diagnostics.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(core::format_diagnostic(state.diagnostics[index]));
    }
    output << "]}";
    return output.str();
}


} // namespace eawr::presentation::godot_backend
