#include "eawr/core/json_text.hpp"
#include "tactical_hud_internal.hpp"

#include <sstream>

namespace eawr::presentation::godot_backend {

diagnostics::MemoryCapacity TacticalHud::memory_capacity() const {
    return state_->minimap_fog.memory_capacity();
}

using tactical_hud_detail::text;
using tactical_hud_detail::rect2;

namespace {
[[nodiscard]] std::string json(const std::string_view value) {
    return eawr::core::json_quote(value);
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

void TacticalHud::State::run_switch_probe() {
    Viewport* viewport = hud->get_viewport();
    if (viewport == nullptr || ds_button == nullptr || !ds_button->is_visible_in_tree()) { probed = true; return; }
    const Rect2 hit(ds_button->get_global_position() + ds_button->hit_rect().position, ds_button->hit_rect().size);
    const float inset = 1.5F;
    const float rim = 3.0F; // outside the hit rect: the world's
    const std::vector<std::pair<std::string, Vector2>> points{
        {"panel_centre", hit.get_center()},
        {"panel_top_left", hit.position + Vector2(inset, inset)},
        {"panel_bottom_right", hit.position + hit.size - Vector2(inset, inset)},
        {"panel_top_right", hit.position + Vector2(hit.size.x - inset, inset)},
        {"panel_bottom_left", hit.position + Vector2(inset, hit.size.y - inset)},
        {"outside_left", hit.position + Vector2(-rim, hit.size.y * 0.5F)},
        {"outside_above", hit.position + Vector2(hit.size.x * 0.5F, -rim)}};
    for (const auto& [name, at] : points) {
        const int before = ds_button->presses();
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(at);
        motion->set_global_position(at);
        viewport->push_input(motion, true);
        for (const bool pressed : {true, false}) {
            Ref<InputEventMouseButton> click;
            click.instantiate();
            click->set_button_index(MOUSE_BUTTON_LEFT);
            click->set_pressed(pressed);
            click->set_button_mask(pressed ? BitField<MouseButtonMask>(MOUSE_BUTTON_MASK_LEFT) : BitField<MouseButtonMask>(0));
            click->set_position(at);
            click->set_global_position(at);
            viewport->push_input(click, true);
        }
        ds_probes.push_back({name, at, ds_button->presses() - before});
    }
    probed = true;
}

std::string TacticalHud::report_json() const {
    const State& state = *state_;
    std::ostringstream output;
    const auto& special = state.special_view;
    output << "{\"gift\": {\"visible\": " << (state.gift_button && state.gift_button->is_visible_in_tree() ? "true" : "false")
        << ", \"enabled\": " << (state.gift_enabled ? "true" : "false")
        << ", \"open\": " << (state.gift_open ? "true" : "false")
        << ", \"base\": " << (state.gift_button && state.gift_button->get_texture_normal().is_valid() ? "true" : "false")
        << ", \"overlay\": " << json(state.gift_button ? state.gift_button->overlay_name() : "none")
        << ", \"recipient_overlay\": " << json(!state.gift_cells.empty() ? state.gift_cells.front().button->overlay_name() : "none")
        << ", \"layout_changes\": " << state.gift_layout.changes()
        << ", \"state\": " << json(state.gift_button ? state.gift_button->visual_state() : "none")
        << ", \"recipient_state\": " << json(!state.gift_cells.empty() ? state.gift_cells.front().button->visual_state() : "none")
        << ", \"presses\": " << state.gift_presses << ", \"submissions\": " << state.gift_submissions << ", \"recipients\": [";
    for (std::size_t i = 0; i < state.gift_recipients.size(); ++i)
        output << (i ? ", " : "") << "{\"seat\": " << state.gift_recipients[i].seat.value
            << ", \"name\": " << json(state.gift_recipients[i].name) << "}";
    output << "]}, ";
    output << "\"movie_camera\": {\"active\": " << (state.bands.movie() ? "true" : "false")
           << ", \"script_letterbox\": " << (state.bands.script() ? "true" : "false")
           << ", \"bands_progress\": " << state.bands.progress()
           << ", \"gui_hidden\": " << (state.bands.wanted() ? "true" : "false")
           << ", \"button\": " << (state.movie_button ? "true" : "false")
           << ", \"presses\": " << (state.movie_button ? state.movie_button->presses() : 0)
           << ", \"letterbox_height\": ";
    if (state.movie_height) output << *state.movie_height;
    else output << "null";
    output << "}, \"beacon_button\": {\"presses\": "
           << (state.beacon_button ? state.beacon_button->presses() : 0) << ", \"icon\": "
           << (state.beacon_button && state.beacon_button->icon_shown() ? "true" : "false") << ", \"overlay\": "
           << json(state.beacon_button ? state.beacon_button->overlay_name() : "none") << "}, ";
    output << "\"retreat_button\": {\"present\": " << (state.retreat_button ? "true" : "false")
           << ", \"presses\": " << (state.retreat_button ? state.retreat_button->presses() : 0)
           << ", \"icon\": " << (state.retreat_button && state.retreat_button->icon_shown() ? "true" : "false")
           << ", \"overlay\": " << json(state.retreat_button ? state.retreat_button->overlay_name() : "none")
           << ", \"enabled\": " << (state.retreat_view.enabled ? "true" : "false")
           << ", \"flashing\": " << (state.retreat_view.flashing ? "true" : "false")
           << ", \"dial\": " << state.retreat_view.dial;
    if (state.retreat_button != nullptr && state.retreat_button->is_visible_in_tree()) {
        const Rect2 hit(state.retreat_button->get_global_position() + state.retreat_button->hit_rect().position,
            state.retreat_button->hit_rect().size);
        output << ", \"hit_rect\": " << rect_json(hit);
    }
    output << "}, ";
    // HCB-46/63: report the independently bound holder and target switch.
    output << "\"sl_switch\": {\"present\": " << (state.sl_button ? "true" : "false")
           << ", \"shown\": " << (state.sl_view.visible ? "true" : "false")
           << ", \"enabled\": " << (state.sl_view.enabled ? "true" : "false")
           << ", \"unit\": " << state.sl_view.unit
           << ", \"card_slot\": " << (state.sl_view.card_slot ? "true" : "false")
           << ", \"recharge\": " << state.sl_view.recharge << "}, ";
    // HCB-47: the Death Star switch and its readout.
    output << "\"deathstar_switch\": {\"present\": " << (state.ds_button ? "true" : "false")
           << ", \"shown\": " << (state.ds_button && state.ds_button->is_visible_in_tree() ? "true" : "false")
           << ", \"presses\": " << (state.ds_button ? state.ds_button->presses() : 0)
           << ", \"enabled\": " << (state.ds_view.enabled ? "true" : "false")
           << ", \"icon_frame\": " << state.ds_view.icon_frame
           << ", \"tooltip\": " << json(state.ds_button ? state.ds_button->get_tooltip_text().utf8().get_data() : "")
           << ", \"countdown_shown\": " << (state.ds_readout && state.ds_readout->is_visible_in_tree() ? "true" : "false")
           << ", \"countdown_text\": " << json(state.ds_view.countdown_text)
           << ", \"countdown_dial_rect\": " << rect_json(state.ds_dial_rect)
           << ", \"countdown_text_rect\": " << rect_json(state.ds_text_rect)
           << ", \"countdown_pixels\": " << state.ds_pixels
           << ", \"countdown_anchored\": " << (state.shell.deathstar_countdown_anchored ? "true" : "false");
    if (state.ds_label && state.fonts) {
        const auto& look = state.ds_label->style();
        const auto face = state.fonts->resolve({look.text.face, look.text.point_size}, state.options.language);
        const auto& ink = look.text.colour;
        output << ", \"countdown_face\": " << json(face.face)
               << ", \"countdown_face_source\": " << json(model::to_string(face.source))
               << ", \"countdown_colour\": [" << static_cast<unsigned>(ink.r) << ", " << static_cast<unsigned>(ink.g)
               << ", " << static_cast<unsigned>(ink.b) << ", " << static_cast<unsigned>(ink.a) << "]";
    }
    output << ", \"clicks\": {\"none\": " << state.ds_feedback[0] << ", \"press\": " << state.ds_feedback[1]
           << ", \"negative\": " << state.ds_feedback[2] << "}, \"probe\": [";
    for (std::size_t index = 0; index < state.ds_probes.size(); ++index) {
        const auto& probe = state.ds_probes[index];
        output << (index == 0 ? "" : ", ") << "{\"name\": " << json(probe.name) << ", \"point\": [" << probe.point.x << ", "
               << probe.point.y << "], \"presses\": " << probe.presses << "}";
    }
    output << "], \"overlay\": " << json(state.ds_button ? state.ds_button->overlay_name() : "none")
           << ", \"state\": " << json(state.ds_button ? state.ds_button->visual_state() : "none");
    if (state.ds_button != nullptr && state.ds_button->is_visible_in_tree()) {
        const Rect2 hit(state.ds_button->get_global_position() + state.ds_button->hit_rect().position,
            state.ds_button->hit_rect().size);
        output << ", \"hit_rect\": " << rect_json(hit);
    }
    output << "}, ";
    output << "\"mode\": \"tactical\", \"faction\": " << json(model::to_string(state.options.faction))
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
    output << ", \"special_weapon\": {\"source\": " << special.source
        << ", \"availability\": " << json(model::to_string(special.availability))
        << ", \"readiness\": " << static_cast<double>(special.readiness.raw()) / sim::math::Fixed::scale
        << ", \"dial\": " << special.dial() << ", \"pulses_left\": " << special.pulses_left
        << ", \"target_action\": " << special.target_action << ", \"icon\": " << json(state.special_icon)
        << ", \"enabled\": " << (special.enabled() ? "true" : "false")
        << ", \"targeting\": " << (special.targeting ? "true" : "false") << '}';
    if (state.cards != nullptr) output << ", \"unit_cards\": " << state.cards->report_json();
    if (state.heroes != nullptr) output << ", \"hero_tray\": " << state.heroes->report_json();
    if (state.production != nullptr) output << ", \"production\": " << state.production->report_json();
    if (state.abilities != nullptr) output << ", \"ability_buttons\": " << state.abilities->report_json();
    output << ", \"team_dock\": {\"team_game\": " << (state.team_dock.team_game() ? "true" : "false")
           << ", \"revision\": " << state.team_dock.revision() << ", \"rows\": [";
    for (std::size_t index = 0; index < state.team_dock.rows().size(); ++index) {
        const auto& row = state.team_dock.rows()[index];
        const auto* button = index < state.team_dock_cells.size() ? state.team_dock_cells[index].button : nullptr;
        output << (index == 0 ? "" : ", ") << "{\"owner\": " << row.owner << ", \"type\": " << row.type
               << ", \"in_progress\": " << (row.in_progress ? "true" : "false")
               << ", \"starbase\": " << (row.starbase ? "true" : "false") << ", \"percent\": " << row.percent
               << ", \"icon_loaded\": " << (button && button->icon_shown() ? "true" : "false")
               << ", \"shown\": " << (button && button->is_visible_in_tree() ? "true" : "false")
               << ", \"rect\": " << rect_json(button ? button->get_global_rect() : Rect2()) << "}";
    }
    output << "]}";
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
               << ", \"tooltip\": " << json(button->get_tooltip_text().utf8().get_data())
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
