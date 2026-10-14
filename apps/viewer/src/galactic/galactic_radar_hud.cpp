#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/core/diagnostic.hpp"
#include <ostream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
}

bool GalacticMode::State::build_radar_hud() {
    if (const auto* shell = command_catalog.find("map_shell")) {
        auto loaded = data::ui::load_shell_anchors(*filesystem, shell->text(data::ui::Field::model_name));
        if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
        advanced_map_anchors = std::move(loaded).value().shell;
        auto* mask = memnew(EawrUiHitMask); hud->add_child(mask);
        mask->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
        advanced_map_shell = memnew(EawrTacticalHud); mask->add_child(advanced_map_shell);
        EawrTacticalHud::Setup setup;
        setup.view = presentation::ui::hud_view_model(advanced_map_anchors, command_catalog,
            presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem),
            presentation::ui::ShellOrigin::centre);
        const auto look = presentation::ui::hud_shell(advanced_map_anchors, command_catalog, command_faction);
        for (const auto& mesh : look.meshes) {
            // Authored shell meshes may name cropped command-atlas entries
            // (including i_button_blank.tga), as well as standalone textures.
            const auto texture = command_texture(mesh.texture);
            if (texture.is_null()) { failure = "advanced map shell texture is absent or undecodable: " + mesh.texture; return false; }
            setup.meshes.push_back({mesh.blend, texture, mesh.triangles});
        }
        advanced_map_shell->setup(std::move(setup), nullptr);
        advanced_map_shell->hide();
        if (advanced_map_anchors.find("planet_bonus") && command_catalog.find("planet_bonus")) {
            planet_bonus_icon = memnew(TextureRect);
            planet_bonus_icon->set_name("planet_bonus");
            planet_bonus_icon->set_mouse_filter(Control::MOUSE_FILTER_STOP);
            planet_bonus_icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            advanced_map_shell->add_child(planet_bonus_icon);
            planet_bonus_icon->hide();
        }
        mask->set_hit_test([this](double x, double y) {
            return advanced_map_shell->is_visible_in_tree() && advanced_map_shell->hit(x, y);
        });
    }
    for (std::size_t index = 0; index < radar_controls.size(); ++index) {
        auto* parent = index ? advanced_map_shell : command_bar;
        const auto& anchors = index ? advanced_map_anchors : command_anchors;
        const auto name = index ? "radar_galactic_adv_map" : "radar_galactic";
        const auto zoom_name = index ? "b_zoom_adv_map" : "b_zoom";
        if (!parent) continue;
        if (anchors.find(name) && command_catalog.find(name)) {
            auto* radar = memnew(EawrGalacticRadar); parent->add_child(radar); radar->set_name(name);
            radar_controls[index] = radar;
            radar->set_actions([this](auto point, float width, float height) {
                return request_radar_position(point, width, height);
            }, [this](bool inward) {
                if (map_input.wheel_zoom()) static_cast<void>(request_hud_zoom(inward));
            }, [this](bool right) {
                if (right) { scrolling = false; map_input.held_scroll = false; pointer_warp.reset(); }
                else { orbiting = false; map_input.held_rotate = false; }
            });
            for (const auto& planet : galaxy->planets) {
                auto* blip = memnew(EawrHudButton); radar->add_child(blip);
                blip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                // HCB-12g/PU-70: selected art overlays the retained icon;
                // Selected_Alpha chooses blending, otherwise it adds.
                if (const auto* component = command_catalog.find("g_radar_blip")) {
                    blip->set_texture_normal(command_texture(first(*component, data::ui::Field::icon_texture_name)));
                    blip->set_special_overlays({},
                        command_texture(first(*component, data::ui::Field::selected_texture_name)), {},
                        !component->flag(data::ui::Field::selected_alpha));
                }
                radar_blips[index].emplace(planet.id, blip);
            }
            auto* view = memnew(TextureRect); radar->add_child(view);
            view->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            view->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            radar_view_icons[index] = view;
            if (const auto* component = command_catalog.find("g_radar_view"))
                view->set_texture(command_texture(first(*component, data::ui::Field::icon_texture_name)));
        }
        if (const auto* component = command_catalog.find(zoom_name); component && anchors.find(zoom_name)) {
            auto* button = memnew(EawrHudButton);
            button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
            parent->add_child(button);
            button->set_name(zoom_name);
            button->set_texture_normal(command_texture(first(*component, data::ui::Field::blank_texture_name)));
            button->set_texture_hover(command_texture(first(*component, data::ui::Field::mouse_over_texture_name)));
            button->set_texture_pressed(command_texture(first(*component, data::ui::Field::selected_texture_name)));
            button->set_order_icon(command_texture(first(*component, data::ui::Field::icon_texture_name)), false);
            if (index == 0) {
                // COMMANDBARCOMPONENTS.XML b_zoom uses its camera-state table,
                // not the faction ALT table: zoom_out first, then zoom_in.
                const auto icons = component->list(data::ui::Field::icon_alternate_texture_name);
                for (std::size_t variant = 0; variant < std::min(icons.size(), radar_zoom_icons.size()); ++variant)
                    radar_zoom_icons[variant] = command_texture(icons[variant]);
            }
            button->set_action([this, index] { static_cast<void>(request_hud_zoom(false, index == 0)); });
            radar_zoom_buttons[index] = button;
        }
    }
    if (fleet_organize) {
        if (auto* mask = Object::cast_to<EawrUiHitMask>(fleet_organize->get_parent())) {
            // The organize faceplate overlaps the command bar. Its display-only
            // mask must defer to visible native radar/zoom controls (WGUI-51);
            // those controls still consume events before world input.
            mask->set_hit_test([this, mask](double x, double y) {
                if (!fleet_organize->is_visible_in_tree() || !fleet_organize->hit(x, y)) return false;
                const auto point = mask->get_global_transform().xform(Vector2(static_cast<float>(x), static_cast<float>(y)));
                for (const auto* control : {static_cast<const Control*>(radar_controls[0]),
                         static_cast<const Control*>(radar_zoom_buttons[0])}) {
                    if (control && control->is_visible_in_tree() && control->get_global_rect().has_point(point)) return false;
                }
                return true;
            });
        }
    }
    return true;
}

bool GalacticMode::State::request_hud_zoom(bool inward, bool toggle) {
    if (!galaxy || tactical || loading || !map_input.hud_event() || !map_input.gates.world_allowed()) return false;
    const auto& gates = map_input.gates;
    const galactic_view::ZoomGates zoom_gates{gates.tutorial_scroll && !gates.tutorial_locked,
        gates.selection_locked, gates.action_menu};
    auto& flight = seat_flight();
    bool accepted{};
    if (inward || toggle) {
        const auto selected = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [this](const auto& planet) { return planet.id == selected_planet; });
        if (selected == galaxy->planets.end()) return false;
        const auto model = planet_models.find(selected->id);
        const bool renderable = model != planet_models.end() && model->second;
        accepted = toggle ? flight.toggle(*selected, renderable, zoom_gates)
            : flight.zoom_in(*selected, renderable, zoom_gates);
    } else if (flight.state() == galactic_view::CameraState::focused) {
        accepted = flight.zoom_out(zoom_gates); // WGUI-65: explicit outward command requires focus.
    }
    if (!accepted) return false;
    map_input.multiselection.clear(); map_input.dragged = 0; map_input.hyperspace = false;
    map_input.box_start.reset(); map_input.box_end.reset();
    ++hud_zoom_requests;
    apply_camera();
    return update_routes();
}

bool GalacticMode::State::request_radar_position(galactic_view::PointerPoint point, float width, float height) {
    if (!galaxy || tactical || loading || !map_input.hud_event()) return false;
    const auto position = map_input.radar(point, width, height);
    if (!position) return false;
    seat_flight().position(*position); last_radar_position = position; ++radar_positions;
    apply_camera();
    return update_routes(); // WGUI-56: position without changing advanced-map state.
}

void GalacticMode::State::update_radar_hud() {
    if (!galaxy || !command_bar) return;
    const bool advanced = seat_flight().advanced_map();
    if (advanced_map_shell) advanced_map_shell->set_visible(advanced);
    const bool changed = radar_publication != displayed_snapshot || radar_selection != selected_planet
        || radar_viewport != Vector2(static_cast<float>(camera.width), static_cast<float>(camera.height));
    if (changed) {
        radar_publication = displayed_snapshot; radar_selection = selected_planet;
        radar_viewport = Vector2(static_cast<float>(camera.width), static_cast<float>(camera.height)); ++radar_rebuilds;
        if (planet_bonus_icon) {
            planet_bonus_icon->hide();
            const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
                [this](const auto& row) { return row.id == selected_planet; });
            const auto* anchor = advanced_map_anchors.find("planet_bonus");
            const auto* component = command_catalog.find("planet_bonus");
            if (planet != galaxy->planets.end() && anchor && component && !planet->ability.icon.empty()) {
                const auto texture = command_texture(planet->ability.icon);
                if (texture.is_valid()) {
                    // GHUD-S96: selected planet bonus at its mounted map-shell bone.
                    const auto placement = advanced_map_shell->placement();
                    const auto origin = anchor->origin.value_or(assets::Vec2f{
                        anchor->rect.x + anchor->rect.width * 0.5F,
                        anchor->rect.y + anchor->rect.height * 0.5F});
                    const auto offset = component->vec2(data::ui::Field::icon_offset).value_or(data::ui::Vec2{});
                    const auto size = texture->get_size() * component->number(data::ui::Field::scale).value_or(1.0F);
                    const auto pixels = presentation::ui::shell_to_screen({origin.x + offset.x - size.x * 0.5F,
                        origin.y + offset.y - size.y * 0.5F, size.x, size.y}, placement);
                    planet_bonus_icon->set_texture(texture);
                    planet_bonus_icon->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
                    planet_bonus_icon->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
                    planet_bonus_icon->show();
                }
            }
        }
    }
    for (std::size_t index = 0; index < radar_controls.size(); ++index) {
        auto* parent = index ? advanced_map_shell : command_bar;
        if (!parent) continue;
        const auto& anchors = index ? advanced_map_anchors : command_anchors;
        const auto placement = parent->placement();
        const auto place = [&](Control* control, const char* name) {
            // GHUD-C01/C05: authored shell anchors are the placement input;
            // live zoom-button/radar placement awaits the paired retail captures.
            // Next/previous anchors exist only in the unused test shell, so
            // PageUp/PageDown retain their accepted map-input routing (#1156).
            if (!control) return;
            const auto* anchor = anchors.find(name);
            if (!anchor) return;
            const auto pixels = presentation::ui::shell_to_screen(anchor->rect, placement);
            control->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
            control->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
        };
        auto* radar = radar_controls[index];
        place(radar, index ? "radar_galactic_adv_map" : "radar_galactic");
        if (radar) {
            radar->set_visible(index != 0 || !advanced);
            radar->set_enabled(map_input.gates.hud_enabled && !map_input.gates.tutorial_locked);
            const auto size = radar->get_size();
            if (changed) for (const auto& planet : galaxy->planets) {
                auto* blip = static_cast<EawrHudButton*>(radar_blips[index].at(planet.id));
                const auto discovery = displayed_snapshot ? galactic_view::observer_discovery(*displayed_snapshot,
                    galaxy->local.player, planet.id) : std::nullopt;
                const auto fact = hud_facts.find(planet.id);
                const bool discovered = fact != hud_facts.end() ? fact->second.discovered : discovery.value_or(false);
                const auto local = galactic_view::world_to_radar(planet.position, size.x, size.y);
                // WGP-119..121: discovery dims the glyph, independently of
                // ownership permission and background radar positioning.
                blip->set_visible(local.has_value());
                if (!local) continue;
                const auto* neutral = content ? galactic_view::affiliation_for_owner(*content, std::nullopt) : nullptr;
                auto tint = neutral ? neutral->primary : galactic_view::Rgb{100.0F / 255, 100.0F / 255, 100.0F / 255};
                if (discovered && fact != hud_facts.end() && fact->second.observed_owner)
                    tint = fact->second.name_colour;
                // Owner RGB colours the icon, while selected art keeps its
                // authored RGB. WGP-119's discovery alpha applies to both.
                blip->set_self_modulate(Color(tint.r, tint.g, tint.b, 1.0F));
                blip->set_modulate(Color(1.0F, 1.0F, 1.0F, discovered ? 1.0F : 128.0F / 255));
                const auto* component = command_catalog.find("g_radar_blip");
                if (!component) { blip->hide(); continue; }
                blip->set_order_selected(planet.id == selected_planet);
                const auto texture = blip->get_texture_normal();
                const auto scale = component->number(data::ui::Field::scale).value_or(1.0F);
                const auto glyph_size = texture.is_valid() ? texture->get_size() * (scale * static_cast<float>(placement.scale)) : Vector2{};
                blip->set_size(glyph_size);
                blip->set_position(Vector2(size.x * .5F + local->x, size.y * .5F - local->y) - glyph_size * .5F);
                const auto selected_texture = command_texture(first(*component, data::ui::Field::selected_texture_name));
                const auto selected_size = selected_texture.is_valid()
                    ? selected_texture->get_size() * (scale * static_cast<float>(placement.scale)) : Vector2{};
                blip->set_special_overlay_box(Rect2((glyph_size - selected_size) * .5F, selected_size));
            }
            if (auto* view = radar_view_icons[index]) {
                const auto local = galactic_view::world_to_radar(seat_flight().pose().target, size.x, size.y);
                const auto texture = view->get_texture();
                const auto* style = command_catalog.find("g_radar_view");
                const auto authored = style ? style->vec2(data::ui::Field::size) : std::nullopt;
                const auto glyph_size = authored ? Vector2(authored->x, authored->y) * static_cast<float>(placement.scale)
                    : texture.is_valid() ? texture->get_size() * static_cast<float>(placement.scale) : Vector2{};
                view->set_size(glyph_size);
                view->set_visible(local && texture.is_valid() && seat_flight().state() != galactic_view::CameraState::focused);
                if (local) view->set_position(Vector2(size.x * .5F + local->x, size.y * .5F - local->y) - glyph_size * .5F);
                // WGUI-U07: camera view extent calibration remains a named seam;
                // the authored icon follows camera position, without a fabricated frustum.
            }
        }
        auto* zoom = radar_zoom_buttons[index];
        place(zoom, index ? "b_zoom_adv_map" : "b_zoom");
        if (zoom) {
            static_cast<EawrHudButton*>(zoom)->set_hit_rect({{}, zoom->get_size()});
            if (index == 0) {
                // WGUI-36 requires the icon to reflect camera state. Focused
                // -> zoom_out (0), otherwise zoom_in (1), is an inference from
                // b_zoom's authored texture names, pending GHUD-C01 pairing.
                // Tutorial permission currently disables input only; its exact
                // icon variant is an uncaptured WGUI-36/GHUD-C01 seam.
                const std::size_t variant = seat_flight().state() == galactic_view::CameraState::focused ? 0 : 1;
                if (changed || radar_zoom_variant != variant) {
                    static_cast<EawrHudButton*>(zoom)->set_icon_texture(radar_zoom_icons[variant]);
                    radar_zoom_variant = variant;
                }
            }
            zoom->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked);
            zoom->set_visible(index != 0 || !advanced);
        }
    }
}

void GalacticMode::State::write_radar_report(std::ostream& output) const {
    using galactic_detail::json;
    const auto flight = flights.find(galaxy ? galaxy->local.seat : 0);
    const bool advanced = flight != flights.end() && flight->second.advanced_map();
    output << "{\"positions\": " << radar_positions << ", \"zoom_requests\": " << hud_zoom_requests
        << ", \"rebuilds\": " << radar_rebuilds << ", \"advanced\": " << (advanced ? "true" : "false")
        << ", \"last_position\": ";
    if (last_radar_position) output << '[' << last_radar_position->x << ", " << last_radar_position->y << ", " << last_radar_position->z << ']';
    else output << "null";
    output << ", \"controls\": [";
    std::size_t written{};
    for (std::size_t index = 0; index < radar_controls.size(); ++index) {
        for (const auto* control : {static_cast<const Control*>(radar_controls[index]), static_cast<const Control*>(radar_zoom_buttons[index])}) {
            if (!control) continue;
            const auto rect = control->get_global_rect();
            output << (written++ ? ", " : "") << "{\"name\": " << json(galactic_detail::utf8(control->get_name()))
                << ", \"visible\": " << (control->is_visible_in_tree() ? "true" : "false") << ", \"rect\": ["
                << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        }
    }
    output << "], \"blips\": ["; written = 0;
    for (std::size_t index = 0; index < radar_blips.size(); ++index) for (const auto& [planet, blip] : radar_blips[index]) {
        const auto* button = static_cast<const EawrHudButton*>(blip);
        const auto tint = blip->get_modulate();
        const auto icon_tint = blip->get_self_modulate();
        const auto rect = blip->get_global_rect();
        const auto selected_rect = button->special_overlay_rect();
        output << (written++ ? ", " : "") << "{\"planet\": " << planet << ", \"visible\": "
            << (blip->is_visible() ? "true" : "false") << ", \"radar\": " << index
            << ", \"alpha\": " << std::lround(tint.a * 255)
            << ", \"base_icon\": " << (button->get_texture_normal().is_valid() ? "true" : "false")
            << ", \"overlay\": " << json(button->overlay_name())
            << ", \"selected_rect\": [" << selected_rect.position.x << ", " << selected_rect.position.y
            << ", " << selected_rect.size.x << ", " << selected_rect.size.y << ']'
            << ", \"colour\": [" << std::lround(icon_tint.r * 255) << ", " << std::lround(icon_tint.g * 255)
            << ", " << std::lround(icon_tint.b * 255) << "], \"rect\": [" << rect.position.x << ", "
            << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "]}";
}
} // namespace eawr::presentation::godot_backend
