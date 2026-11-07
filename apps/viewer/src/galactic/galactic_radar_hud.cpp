#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/text_database.hpp"
#include <godot_cpp/variant/utility_functions.hpp>
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
        // Like the command/organization shells, this faceplate has no wheel action.
        // Its radar consumes wheel input itself; bonus/zoom icons and empty art pass it.
        mask->set_force_pass_scroll_events(true);
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
            setup.meshes.push_back({mesh.blend, texture, mesh.triangles, {}});
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
        build_advanced_map_labels();
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
                    // #5508: the authored red flash art (i_radar_galactic_planet_flash) marks a pending battle.
                    blip->set_flash_texture(command_texture(first(*component, data::ui::Field::flash_texture_name)));
                }
                radar_blips[index].emplace(planet.id, blip);
            }
            auto* view = memnew(TextureRect); radar->add_child(view);
            view->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            view->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            radar_view_icons[index] = view;
            if (const auto* component = command_catalog.find("g_radar_view"))
                view->set_texture(radar_view_texture(first(*component, data::ui::Field::icon_texture_name)));
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

// Header and capacity readouts of the advanced planetary map (H037, GHUD-C05). The bones and styles are the retail
// planetary_mode.alo / CommandBarComponents.xml ones; the values come from facts the sim already publishes. Which
// quantity feeds each capacity number and the captions' text keys are name matches, not captured behaviour (see
// docs/behaviour/walks/gc-planetary-view-gap.md).
void GalacticMode::State::build_advanced_map_labels() {
    auto text = data::ui::load_language_text_database(*filesystem, "english");
    const auto caption = [&](const char* name, const char* key) {
        if (!text) return;
        if (const auto* entry = text.value().find(key)) advanced_map_captions[name] = data::ui::to_utf8(entry->value);
    };
    caption("unit_cap_text", "TEXT_UNIT_CAP");
    caption("building_cap_text", "TEXT_BUILDING_CAP");
    for (const auto* name : {"Planet_name_text", "Planetary_Income_number", "Day_Counter_text", "unit_cap_text",
             "unit_cap_number", "building_cap_text", "building_cap_number"}) {
        if (!advanced_map_anchors.find(name) || !command_catalog.find(name)) continue;
        auto* label = memnew(EawrHudText);
        label->set_name(name);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        advanced_map_shell->add_child(label);
        advanced_map_labels.emplace(name, label);
    }
    if (advanced_map_anchors.find("Allegiance_A")) {
        advanced_map_allegiance = memnew(TextureRect);
        advanced_map_allegiance->set_name("Allegiance_A");
        advanced_map_allegiance->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        advanced_map_allegiance->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        advanced_map_shell->add_child(advanced_map_allegiance);
        advanced_map_allegiance->hide();
    }
    for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
        const auto name = galactic_view::land_tray_slot_name(slot);
        const auto* component = command_catalog.find(name);
        if (!component || !advanced_map_anchors.find(name)) continue;
        auto* button = memnew(EawrHudButton);
        button->set_name(String::utf8(name.c_str()));
        button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        // Read-only tray: the button must not swallow map clicks, so it ignores the pointer and the
        // rollover art is a separate non-blocking TextureRect shown while the pointer is over the slot.
        button->set_mouse_filter(galactic_view::land_tray_takes_pointer ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
        button->set_order_icon({}, false);
        button->hide();
        advanced_map_shell->add_child(button);
        land_fleet_buttons[slot] = button;
        auto* hover = memnew(TextureRect);
        hover->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        hover->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        hover->set_stretch_mode(TextureRect::STRETCH_SCALE);
        hover->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
        hover->set_texture(command_texture(first(*component, data::ui::Field::mouse_over_texture_name)));
        hover->hide();
        button->add_child(hover);
        land_fleet_hover[slot] = hover;
        auto* count = memnew(Label);
        count->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
            component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        const int font_size = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
        count->add_theme_font_override("font", hud_fonts->command_font(face, font_size));
        count->add_theme_font_size_override("font_size", font_size);
        const auto colour = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 255, 255, 255});
        count->add_theme_color_override("font_color", Color(colour.r / 255.0F, colour.g / 255.0F,
            colour.b / 255.0F, colour.a / 255.0F));
        count->add_theme_constant_override("outline_size", component->flag(data::ui::Field::text_outline) ? 1 : 0);
        button->add_child(count);
        land_fleet_counts[slot] = count;
    }
    const auto make_pads = [&](auto& pads, const char* prefix) {
        for (std::size_t slot = 0; slot < pads.size(); ++slot) {
            const auto name = std::string(prefix) + (slot < 10 ? "0" : "") + std::to_string(slot);
            const auto* style = command_catalog.find(name);
            if (!style || !advanced_map_anchors.find(name)) continue;
            auto* pad = memnew(EawrHudButton);
            pad->set_name(String::utf8(name.c_str()));
            pad->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            pad->set_texture_normal(command_texture(first(*style, data::ui::Field::blank_texture_name)));
            pad->set_disabled(true);
            pad->hide();
            advanced_map_shell->add_child(pad);
            pads[slot] = pad;
        }
    };
    make_pads(detail_build_pads, "build_pad");
    make_pads(detail_unit_pads, "unit_pad");
    for (auto& ring : detail_shield_rings) {
        ring = memnew(TextureRect);
        ring->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        ring->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        ring->set_stretch_mode(TextureRect::STRETCH_SCALE);
        ring->set_texture(command_texture("shield_range.tga"));
        ring->hide();
        advanced_map_shell->add_child(ring);
    }
    if (advanced_map_anchors.find("weather") && command_catalog.find("weather")) {
        advanced_map_weather = memnew(TextureRect);
        advanced_map_weather->set_name("weather");
        advanced_map_weather->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        advanced_map_weather->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        advanced_map_shell->add_child(advanced_map_weather);
        advanced_map_weather->hide();
    }
}

void GalacticMode::State::update_advanced_map_labels() {
    if (!advanced_map_shell || !advanced_map_shell->is_visible_in_tree()) return;
    const auto placement = advanced_map_shell->placement();
    const auto selected = hud_facts.find(selected_planet);
    const auto* facts = selected == hud_facts.end() ? nullptr : &selected->second;
    for (const auto& [name, label] : advanced_map_labels) {
        const auto* anchor = advanced_map_anchors.find(name);
        const auto* component = command_catalog.find(name);
        if (!anchor || !component) { label->hide(); continue; }
        const auto pixels = presentation::ui::shell_to_screen(anchor->rect, placement);
        label->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
        label->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
        // Same anchoring as the command bar readouts: left text starts at the bone origin plus Text_Offset, right
        // text ends there, unjustified text is centred on it.
        const bool left_justified = component->flag(data::ui::Field::left_justified);
        const bool right_justified = !left_justified && component->flag(data::ui::Field::right_justified);
        const auto origin = anchor->origin.value_or(assets::Vec2f{
            anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
        const auto text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        const auto point = presentation::ui::text_anchor({origin.x, origin.y, 0.0F, 0.0F},
            {text_offset.x, text_offset.y}, placement);
        const float point_x = static_cast<float>(point.x);
        label->set_position({left_justified ? point_x : right_justified ? point_x - static_cast<float>(pixels.width)
            : point_x - static_cast<float>(pixels.width) * 0.5F, static_cast<float>(pixels.y + point.y_shift)});
        std::string value;
        if (name == "Planet_name_text") value = selected_hud.name;
        else if (name == "Planetary_Income_number") value = selected_hud.income;
        else if (name == "Day_Counter_text") value = player_hud.cycle;
        else if (name == "unit_cap_number") value = galactic_view::advanced_map_capacity_text(facts, false);
        else if (name == "building_cap_number") value = galactic_view::advanced_map_capacity_text(facts, true);
        else if (const auto caption = advanced_map_captions.find(name); caption != advanced_map_captions.end())
            value = caption->second;
        const auto upper = String::utf8(value.c_str()).to_upper();
        if (label->get_text() != upper) label->set_text(upper);
        configure_command_bar_text(placement, *label, *component, *anchor);
        const auto tint = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 255, 255, 255});
        label->set_colour(Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, tint.a / 255.0F));
        label->set_horizontal_alignment(left_justified ? HORIZONTAL_ALIGNMENT_LEFT
            : right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_CENTER);
        label->set_visible(!value.empty());
    }
    update_advanced_land_tray(placement);
    update_advanced_map_pads(placement);
    if (advanced_map_weather && galaxy) update_advanced_map_weather(placement);
    if (advanced_map_allegiance) {
        advanced_map_allegiance->hide();
        // Allegiance_A carries no art of its own; the emblem list is the command bar's Text_Allegiance_A one, chosen
        // for the observed owner's faction as there. No emblem while ownership is not visible.
        const auto* anchor = advanced_map_anchors.find("Allegiance_A");
        const auto* style = command_catalog.find("Text_Allegiance_A");
        const auto* own = command_catalog.find("Allegiance_A");
        if (facts && facts->observed_owner && content && anchor && style) {
            const auto* owner = galactic_view::affiliation_for_owner(*content, facts->observed_owner);
            const auto textures = style->list(data::ui::Field::icon_alternate_texture_name);
            if (owner) {
                const auto index = presentation::ui::allegiance_emblem_index(owner->faction, content->side_names);
                if (index < textures.size()) {
                    const auto texture = command_texture(textures[index]);
                    if (texture.is_valid()) {
                        const auto size = texture->get_size() * (own ? own->number(data::ui::Field::scale).value_or(1.0F) : 1.0F);
                        const auto origin = anchor->origin.value_or(assets::Vec2f{
                            anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
                        const auto centre = presentation::ui::shell_to_screen({origin.x, origin.y, 0.0F, 0.0F}, placement);
                        advanced_map_allegiance->set_texture(texture);
                        advanced_map_allegiance->set_size(size);
                        advanced_map_allegiance->set_position(Vector2(static_cast<float>(centre.x), static_cast<float>(centre.y)) - size * 0.5F);
                        advanced_map_allegiance->show();
                    }
                }
            }
        }
    }
}

void GalacticMode::State::update_advanced_map_weather(const presentation::ui::ShellPlacement& placement) {
    advanced_map_weather->hide();
    const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
        [this](const auto& row) { return row.id == selected_planet; });
    const auto* anchor = advanced_map_anchors.find("weather");
    const auto* style = command_catalog.find("weather");
    if (planet == galaxy->planets.end() || !anchor || !style || planet->weather.icon.empty()) return;
    const auto texture = command_texture(planet->weather.icon);
    if (!texture.is_valid()) return;
    // Like planet_bonus: the icon is centred on its bone origin at its authored scale.
    const auto origin = anchor->origin.value_or(assets::Vec2f{
        anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
    const auto size = texture->get_size() * style->number(data::ui::Field::scale).value_or(1.0F);
    const auto pixels = presentation::ui::shell_to_screen({origin.x - size.x * 0.5F, origin.y - size.y * 0.5F, size.x, size.y}, placement);
    advanced_map_weather->set_texture(texture);
    advanced_map_weather->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
    advanced_map_weather->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
    // The planet's weather name and info text (Encyclopedia_Weather_Name/Info, #653).
    const auto tip = galactic_view::weather_tooltip(planet->weather);
    advanced_map_weather->set_tooltip_text(String::utf8(tip.c_str()));
    advanced_map_weather->show();
}

void GalacticMode::State::update_advanced_land_tray(const presentation::ui::ShellPlacement& placement) {
    const auto facts = hud_facts.find(selected_planet);
    const bool owner = facts != hud_facts.end() && galaxy && facts->second.observed_owner == galaxy->local.player;
    // GPD-14 gates landing by the number already on the surface, not the orbiting stack size.
    const auto count = facts == hud_facts.end() ? std::optional<std::size_t>{} : facts->second.landed_companies;
    const auto capacity = facts == hud_facts.end() ? std::optional<std::int64_t>{} : facts->second.unit_capacity;
    const bool enabled = galactic_view::land_tray_enabled(owner, count, capacity);
    const auto entries = galactic_view::land_tray_entries(fleet_intelligence.cards, selected_planet, enabled);
    const auto access = detail_access();
    for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
        auto* button = land_fleet_buttons[slot];
        if (!button) continue;
        const auto* company = galactic_view::land_tray_entry(entries, slot);
        if (!company) { button->hide(); land_fleet_shown[slot].reset(); continue; }
        const auto name = galactic_view::land_tray_slot_name(slot);
        const auto* anchor = advanced_map_anchors.find(name);
        if (!anchor) { button->hide(); continue; }
        const auto pixels = presentation::ui::shell_to_screen(anchor->rect, placement);
        button->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
        button->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
        button->set_hit_rect({{}, button->get_size()});
        // Capacity and view-only access disable the tray.
        button->set_disabled(!company->enabled || !galactic_view::detail_can_edit(access));
        if (land_fleet_shown[slot] != std::optional<sim::galactic::TypeId>(company->type)) {
            land_fleet_shown[slot] = company->type;
            Ref<Texture2D> icon;
            if (const auto type = production_names.find(company->type); type != production_names.end())
                if (const auto art = production_art.find(type->second); art != production_art.end())
                    icon = command_texture(art->second.icon);
            button->set_icon_texture(icon);
        }
        if (auto* count_label = land_fleet_counts[slot]) {
            const auto* style = command_catalog.find(name);
            const auto offset = style ? style->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{}) : data::ui::Vec2{};
            count_label->set_position(button->get_size() * 0.5F + Vector2(offset.x, offset.y) * static_cast<float>(placement.scale));
            count_label->set_text(company->count > 1 ? String("x") + String::num_uint64(company->count) : String{});
        }
        button->show();
        if (auto* hover = land_fleet_hover[slot]; hover && button->get_viewport()) {
            const auto pointer = button->get_viewport()->get_mouse_position();
            hover->set_visible(company->enabled && access == galactic_view::PlanetDetailAccess::edit
                && button->get_global_rect().has_point(pointer));
        }
    }
}

void GalacticMode::State::update_advanced_open_button(const presentation::ui::ShellPlacement& placement) {
    if (!advanced_open_button) return;
    const auto* anchor = fleet_anchors.find("advanced_planetary");
    if (!anchor) { advanced_open_button->hide(); return; }
    const auto pixels = presentation::ui::shell_to_screen(anchor->rect, placement);
    advanced_open_button->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
    advanced_open_button->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
    advanced_open_button->set_hit_rect({{}, advanced_open_button->get_size()});
    advanced_open_button->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked);
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

void GalacticMode::State::update_radar_blips(std::size_t index, Vector2 size, double placement_scale) {
    // Loop invariants are hoisted; a blip is rewritten only when its key changed,
    // and a radar resize or scale change repaints every blip.
    auto& cache = radar_blip_cache[index];
    const bool repaint = cache.size != size || cache.scale != placement_scale;
    cache.size = size; cache.scale = placement_scale;
    const auto* component = command_catalog.find("g_radar_blip");
    const auto* neutral = content ? galactic_view::affiliation_for_owner(*content, std::nullopt) : nullptr;
    const auto neutral_tint = neutral ? neutral->primary : galactic_view::Rgb{100.0F / 255, 100.0F / 255, 100.0F / 255};
    const auto& blips = radar_blips[index];
    Vector2 glyph_size, selected_size, flash_size;
    if (component) {
        const auto scale = component->number(data::ui::Field::scale).value_or(1.0F) * static_cast<float>(placement_scale);
        if (!blips.empty()) {
            const auto texture = static_cast<EawrHudButton*>(blips.begin()->second)->get_texture_normal();
            if (texture.is_valid()) glyph_size = texture->get_size() * scale;
        }
        const auto selected_texture = command_texture(first(*component, data::ui::Field::selected_texture_name));
        if (selected_texture.is_valid()) selected_size = selected_texture->get_size() * scale;
        const auto flash_texture = command_texture(first(*component, data::ui::Field::flash_texture_name));
        if (flash_texture.is_valid()) flash_size = flash_texture->get_size() * scale;
    }
    for (const auto& planet : galaxy->planets) {
        auto* blip = static_cast<EawrHudButton*>(blips.at(planet.id));
        const auto discovery = displayed_snapshot ? galactic_view::observer_discovery(*displayed_snapshot,
            galaxy->local.player, planet.id) : std::nullopt;
        const auto fact = hud_facts.find(planet.id);
        const bool discovered = fact != hud_facts.end() ? fact->second.discovered : discovery.value_or(false);
        const auto local = galactic_view::world_to_radar(planet.position, size.x, size.y);
        auto tint = neutral_tint;
        // Owner RGB colours the icon, while selected art keeps its
        // authored RGB. WGP-119's discovery alpha applies to both.
        if (discovered && fact != hud_facts.end() && fact->second.observed_owner)
            tint = fact->second.name_colour;
        const RadarBlipKey key{discovered, planet.id == selected_planet, local.has_value(), planet.id == pending_battle_planet, tint.r, tint.g, tint.b,
            local ? local->x : 0.0F, local ? local->y : 0.0F};
        auto [slot, inserted] = cache.keys.try_emplace(planet.id, key);
        if (!repaint && !inserted && slot->second == key) continue;
        slot->second = key;
        // WGP-119..121: discovery dims the glyph, independently of
        // ownership permission and background radar positioning.
        blip->set_visible(local.has_value());
        if (!local) continue;
        blip->set_self_modulate(Color(tint.r, tint.g, tint.b, 1.0F));
        blip->set_modulate(Color(1.0F, 1.0F, 1.0F, discovered ? 1.0F : 128.0F / 255));
        if (!component) { blip->hide(); continue; }
        blip->set_order_selected(key.selected);
        blip->set_size(glyph_size);
        blip->set_position(Vector2(size.x * .5F + local->x, size.y * .5F - local->y) - glyph_size * .5F);
        blip->set_special_overlay_box(Rect2((glyph_size - selected_size) * .5F, selected_size));
        blip->set_flash_box(Rect2((glyph_size - flash_size) * .5F, flash_size));
        blip->set_flashing(key.pending);
    }
}

galactic_view::PlanetDetailAccess GalacticMode::State::detail_access() const {
    const auto fact = hud_facts.find(selected_planet);
    if (fact == hud_facts.end() || !fact->second.discovered || !displayed_snapshot || !displayed_snapshot->observer())
        return galactic_view::PlanetDetailAccess::closed;
    const auto roster = ground_roster_find(selected_planet);
    const bool intel = roster != ground_hud.rosters.end() && roster->second.contents_known;
    const auto& planets = displayed_snapshot->observer()->planets;
    const auto planet = std::find_if(planets.begin(), planets.end(), [this](const auto& row) { return row.planet == selected_planet; });
    const bool corrupted = planet != planets.end() && planet->corrupted;
    return galactic_view::planet_detail_access(galaxy && fact->second.observed_owner == galaxy->local.player, intel, corrupted,
        fact->second.local_benefits_from_corruption, roster != ground_hud.rosters.end() && roster->second.accessible);
}

void GalacticMode::State::update_advanced_map_pads(const presentation::ui::ShellPlacement& placement) {
    const auto hide = [&] {
        for (auto* pad : detail_build_pads) if (pad) pad->hide();
        for (auto* pad : detail_unit_pads) if (pad) pad->hide();
        for (auto* ring : detail_shield_rings) if (ring) ring->hide();
    };
    hide();
    if (!content) return;
    const auto art = content->art.find(selected_planet);
    const auto facts = hud_facts.find(selected_planet);
    const auto roster = ground_roster_find(selected_planet);
    const auto* window = advanced_map_anchors.find("map_window");
    if (art == content->art.end() || facts == hud_facts.end() || roster == ground_hud.rosters.end() || !window) return;
    const auto access = detail_access();
    if (!galactic_view::detail_pads_visible(access)) return;
    const auto observations = displayed_snapshot ? displayed_snapshot->observations() : decltype(displayed_snapshot->observations()){};
    const auto observed = std::find_if(observations.begin(), observations.end(), [&](const auto& row) {
        return galaxy && row.player == galaxy->local.player && row.planet == selected_planet;
    });
    if (detail_layout_publication != displayed_snapshot || detail_layout_planet != selected_planet) {
        auto path = art->second.land_tactical_map;
        std::replace(path.begin(), path.end(), '\\', '/');
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!lower.starts_with("data/art/maps/")) path = "data/art/maps/" + path;
        const auto canonical = vfs::canonicalize(path);
        if (!canonical) return;
        const auto markers = marker_cache.find(canonical.value());
        if (markers == marker_cache.end()) return;
        const auto remembered = [&](sim::EntityId object) -> std::optional<std::uint32_t> {
            if (observed == observations.end()) return {};
            const auto found = std::find_if(observed->ground_layout.begin(), observed->ground_layout.end(),
                [&](const auto& row) { return row.object == object; });
            return found == observed->ground_layout.end() ? std::nullopt : std::optional<std::uint32_t>(found->marker);
        };
        std::vector<galactic_view::DetailStructure> structures;
        for (const auto& row : roster->second.structures) {
            const auto name = production_names.find(row.type);
            if (name == production_names.end()) continue;
            const auto type = production_art.find(name->second);
            if (type == production_art.end()) continue;
            structures.push_back({row.id, row.type, type->second.name, type->second.base_position,
                type->second.every_marker_type, type->second.additional_structure, type->second.base_shield_radius, remembered(row.id)});
        }
        std::vector<galactic_view::DetailUnit> units;
        if (facts->second.observed_owner) for (const auto& row : roster->second.companies)
            if (row.owner == *facts->second.observed_owner) units.push_back({row.id, row.type, remembered(row.id)});
        detail_layout = galactic_view::planet_detail_layout(markers->second, structures, units);
        detail_layout_publication = displayed_snapshot;
        detail_layout_planet = selected_planet;
    }
    const auto& layout = detail_layout;
    if (detail_open_planet != selected_planet) { detail_open_planet = selected_planet; detail_defaults_sent = false; }
    if (!detail_defaults_sent && galactic_view::detail_can_edit(access) && observed != observations.end()
        && observed->knowledge.ground_contents && game && galaxy) {
        auto entries = galactic_view::detail_defaults(layout, observed->ground_layout);
        std::erase_if(entries, [&](const auto& entry) {
            const auto structure = std::find_if(roster->second.structures.begin(), roster->second.structures.end(),
                [&](const auto& row) { return row.id == entry.object; });
            return structure != roster->second.structures.end() && structure->owner != galaxy->local.player;
        });
        detail_defaults_sent = true;
        if (!entries.empty()) {
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::SetGroundLayout{selected_planet, std::move(entries)}, {}});
            ++submitted_orders;
        }
    }
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto draw = [&](const auto& pads, const auto& models, const char* prefix, bool building) {
        for (std::size_t slot = 0; slot < std::min(pads.size(), models.size()); ++slot) {
            auto* pad = pads[slot]; if (!pad) continue;
            const auto name = std::string(prefix) + (slot < 10 ? "0" : "") + std::to_string(slot);
            const auto* anchor = advanced_map_anchors.find(name); if (!anchor) continue;
            const auto& model = models[slot];
            const auto x = window->rect.x + model.marker.x * window->rect.width;
            const auto y = window->rect.y + (1.0F - model.marker.y) * window->rect.height;
            const auto pixels = presentation::ui::shell_to_screen({x - anchor->rect.width * 0.5F,
                y - anchor->rect.height * 0.5F, anchor->rect.width, anchor->rect.height}, placement);
            pad->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
            pad->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
            Ref<Texture2D> icon;
            auto type = production_names.find(model.type);
            if (!model.specific_type.empty()) {
                const auto found = production_art.find(model.specific_type);
                if (found != production_art.end()) icon = command_texture(found->second.icon);
            } else if (type != production_names.end()) {
                const auto found = production_art.find(type->second);
                if (found != production_art.end()) icon = command_texture(found->second.icon);
            }
            pad->set_icon_texture(icon);
            const auto* style = command_catalog.find(name);
            if (style) {
                const bool pulse = detail_moving && detail_drag && detail_drag->first == building && !model.object && !model.fixed;
                const auto frames = style->list(data::ui::Field::icon_alternate_texture_name);
                const auto fps = style->number(data::ui::Field::anim_fps).value_or(0.0F);
                const auto frame_index = fps > 0 ? static_cast<std::size_t>(static_cast<double>(ticks) * fps / 1000.0) : 0U;
                const auto frame = frames.empty() ? std::string{} : frames[frame_index % frames.size()];
                pad->set_texture_normal(command_texture(pulse && !frame.empty() ? frame : first(*style, data::ui::Field::blank_texture_name)));
            }
            pad->set_disabled(access != galactic_view::PlanetDetailAccess::edit || model.fixed
                || (detail_moving && detail_drag && detail_drag->first != building));
            pad->set_modulate(Color(1, 1, 1, model.fixed && model.object ? 192.0F / 255.0F : 1.0F));
            pad->show();
            if (building && model.object && model.shield_radius > 0 && model.marker.map_width > 0 && model.marker.map_height > 0) {
                auto* ring = detail_shield_rings[slot];
                const auto w = 2 * model.shield_radius / model.marker.map_width * window->rect.width;
                const auto h = 2 * model.shield_radius / model.marker.map_height * window->rect.height;
                const auto box = presentation::ui::shell_to_screen({x - w * .5F, y - h * .5F, w, h}, placement);
                ring->set_position({static_cast<float>(box.x), static_cast<float>(box.y)});
                ring->set_size({static_cast<float>(box.width), static_cast<float>(box.height)});
                ring->show();
            }
        }
    };
    draw(detail_build_pads, layout.builds, "build_pad", true);
    draw(detail_unit_pads, layout.units, "unit_pad", false);
}

bool GalacticMode::State::detail_hit(Vector2 point) const {
    if (!advanced_map_shell || !advanced_map_shell->is_visible_in_tree()) return false;
    if (detail_drag) return true;
    for (auto* pad : detail_build_pads) if (pad && pad->is_visible_in_tree() && pad->get_global_rect().has_point(point)) return true;
    for (auto* pad : detail_unit_pads) if (pad && pad->is_visible_in_tree() && pad->get_global_rect().has_point(point)) return true;
    return false;
}

void GalacticMode::State::detail_pointer(int kind, Vector2 point) {
    const auto hit = [&](const auto& buttons, const auto& models, bool building) -> std::optional<std::pair<bool, std::size_t>> {
        for (std::size_t i = 0; i < std::min(buttons.size(), models.size()); ++i)
            if (buttons[i] && buttons[i]->is_visible_in_tree() && buttons[i]->get_global_rect().has_point(point))
                return std::pair{building, i};
        return {};
    };
    auto target = hit(detail_build_pads, detail_layout.builds, true);
    if (!target) target = hit(detail_unit_pads, detail_layout.units, false);
    if (ground_hud.input && kind == 1) {
        std::string tooltip;
        if (target) {
            const auto& pads = target->first ? detail_layout.builds : detail_layout.units;
            const auto& pad = pads[target->second];
            auto name = production_names.find(pad.type);
            const auto art = !pad.specific_type.empty() ? production_art.find(pad.specific_type)
                : name != production_names.end() ? production_art.find(name->second) : production_art.end();
            if (pad.object && art != production_art.end()) tooltip = art->second.text;
        }
        ground_hud.input->set_tooltip_text(String::utf8(tooltip.c_str()));
    }
    if (!galactic_view::detail_can_edit(detail_access()) || !game || !galaxy) {
        detail_drag.reset(); detail_moving = false; return;
    }
    if (kind == 0) {
        detail_drag.reset(); detail_moving = false;
        if (!target) return;
        const auto& pads = target->first ? detail_layout.builds : detail_layout.units;
        if (pads[target->second].object && !pads[target->second].fixed) {
            const auto roster = ground_roster_find(selected_planet);
            if (roster == ground_hud.rosters.end()) return;
            const auto object = *pads[target->second].object;
            const auto structure = std::find_if(roster->second.structures.begin(), roster->second.structures.end(),
                [object](const auto& row) { return row.id == object; });
            if (structure == roster->second.structures.end() || structure->owner == galaxy->local.player) detail_drag = target;
        }
    } else if (kind == 1 && detail_drag) detail_moving = true;
    else if (kind == 2 && detail_drag) {
        if (detail_moving && target && target->first == detail_drag->first) {
            const auto& pads = target->first ? detail_layout.builds : detail_layout.units;
            const auto roster = ground_roster_find(selected_planet);
            if (roster == ground_hud.rosters.end()) { detail_drag.reset(); detail_moving = false; return; }
            if (pads[target->second].object) {
                const auto object = *pads[target->second].object;
                const auto structure = std::find_if(roster->second.structures.begin(), roster->second.structures.end(),
                    [object](const auto& row) { return row.id == object; });
                if (structure != roster->second.structures.end() && structure->owner != galaxy->local.player) {
                    detail_drag.reset(); detail_moving = false; return;
                }
            }
            auto entries = galactic_view::detail_move(detail_layout, target->first, detail_drag->second, target->second, true);
            if (!entries.empty()) {
                game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                    sim::galactic::SetGroundLayout{selected_planet, std::move(entries)}, {}});
                ++submitted_orders;
            }
        }
        detail_drag.reset(); detail_moving = false;
    }
}

void GalacticMode::State::update_advanced_map_markers() {
    using namespace std::chrono_literals;
    if (marker_load.valid() && marker_load.wait_for(0s) == std::future_status::ready) {
        auto loaded = marker_load.get();
        if (!loaded.error.empty())
            UtilityFunctions::push_warning(String::utf8(("advanced map markers " + loaded.path + ": " + loaded.error).c_str()));
        // A failed load is cached until this viewer session ends; reopening does not retry or spam warnings.
        marker_cache.emplace(std::move(loaded.path), std::move(loaded.markers));
    }
    if (!seat_flight().advanced_map() || !content || !filesystem || !marker_catalog || marker_load.valid()) return;
    const auto found = content->art.find(selected_planet);
    if (found == content->art.end() || found->second.land_tactical_map.empty()) return;
    auto path = found->second.land_tactical_map;
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string folded = path;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!folded.starts_with("data/art/maps/")) path = "data/art/maps/" + path;
    const auto canonical = vfs::canonicalize(path);
    if (!canonical || marker_cache.contains(canonical.value())) return;
    path = canonical.value();
    const auto fs = filesystem;
    const auto catalog = marker_catalog;
    marker_load = std::async(std::launch::async, [fs, catalog, path] {
        const auto types = assets::object_type_catalog(*catalog);
        const auto map = assets::load_map(*fs, path, types);
        if (!map) return MarkerLoadResult{path, {}, core::format_diagnostic(map.error())};
        return MarkerLoadResult{path, assets::ui_map_markers(map.value(), *catalog), {}};
    });
}

void GalacticMode::State::update_radar_hud() {
    if (!galaxy || !command_bar) return;
    if (seat_flight().advanced_map() && displayed_snapshot
        && detail_access() == galactic_view::PlanetDetailAccess::closed) {
        static_cast<void>(seat_flight().zoom_out());
    }
    const bool advanced = seat_flight().advanced_map();
    if (!advanced) {
        detail_open_planet = 0; detail_defaults_sent = false; detail_drag.reset(); detail_moving = false;
        if (ground_hud.input) ground_hud.input->set_tooltip_text({});
    }
    update_advanced_map_markers();
    if (advanced_map_shell) advanced_map_shell->set_visible(advanced);
    update_advanced_map_labels();
    const bool changed = radar_publication != displayed_snapshot || radar_selection != selected_planet
        || radar_pending_planet != pending_battle_planet
        || radar_viewport != Vector2(static_cast<float>(camera.width), static_cast<float>(camera.height));
    if (changed) {
        radar_publication = displayed_snapshot; radar_selection = selected_planet; radar_pending_planet = pending_battle_planet;
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
            if (changed) update_radar_blips(index, size, placement.scale);
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
