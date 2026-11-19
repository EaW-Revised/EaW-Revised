#include "galactic_mode_internal.hpp"
#include <charconv>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <algorithm>
#include <cmath>
#include <godot_cpp/classes/polygon2d.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

using namespace godot;

namespace eawr::presentation::godot_backend {
// WGUI-188: the observed planet encyclopedia, retained by admitted facts and UI placement.
void GalacticMode::State::service_planet_hover() {
    if (!planet_hover_tooltip || !hud || !content || !galaxy || !command_bar) return;
    const auto hide = [this] {
        if (planet_hover_tooltip->is_visible()) planet_hover_tooltip->hide();
    };
    const auto now = Time::get_singleton()->get_ticks_msec();
    const auto pointer = hud->get_viewport()->get_mouse_position();
    const auto* control = hud->get_viewport()->gui_get_hovered_control();
    galactic_view::ObjectId hovered{};
    if (encyclopedia_admitted() && !dragged_fleet) for (const auto& [id, label] : planet_labels) {
        if (label == control && label->is_visible_in_tree()) { hovered = id; break; }
    }
    if (hovered != planet_hover_id || pointer != planet_hover_position) {
        planet_hover_id = hovered; planet_hover_position = pointer; planet_hover_since = now;
        hide();
    }
    if (!hovered || now - planet_hover_since < production_tooltip_delay) { hide(); return; }
    const auto found = hud_facts.find(hovered);
    const auto record = planet_label_records.find(hovered);
    const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
        [hovered](const auto& row) { return row.id == hovered; });
    if (found == hud_facts.end() || record == planet_label_records.end() || planet == galaxy->planets.end()
        || !found->second.discovered || found->second.display_name.empty()) { hide(); return; }
    const auto& facts = found->second;
    const auto placement = command_bar->placement();
    const float scale = static_cast<float>(placement.scale);
    if (scale <= 0.0F) { hide(); return; }
    const auto anchor = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
    const PlanetHoverKey key{hovered, record->second.key.facts, camera.width, camera.height, placement.scale,
        Vector2(static_cast<float>(anchor.x), static_cast<float>(anchor.y))};
    // Camera movement changes only the leader. No strings, texture lookups, font measurement or child changes
    // occur while the admitted facts, planet and UI placement remain unchanged.
    const auto move_leader = [&] {
        const auto target = hud_planet_anchors.find(hovered);
        if (!planet_hover_leader || target == hud_planet_anchors.end() || target->second == planet_hover_target) return;
        planet_hover_target = target->second;
        planet_hover_leader->set_point_position(1, planet_hover_target);
    };
    if (planet_hover_key && *planet_hover_key == key) {
        move_leader();
        if (!planet_hover_tooltip->is_visible()) planet_hover_tooltip->show();
        return;
    }
    namespace ui = data::ui;
    const auto* back = command_catalog.find("encyclopedia_back");
    const auto* header = command_catalog.find("encyclopedia_header_text");
    const auto* body = command_catalog.find("encyclopedia_text");
    const auto* cost = command_catalog.find("encyclopedia_cost_text");
    if (!back || !header || !body || !hud_fonts) { hide(); return; }
    const auto size = back->vec2(ui::Field::size).value_or(ui::Vec2{});
    const auto inset = back->vec2(ui::Field::offset).value_or(ui::Vec2{});
    const float width = size.x * scale, padding = inset.x * scale;
    if (width <= padding * 2.0F) { hide(); return; }
    const auto texture = [&](const std::string& name, const std::string& object, const std::string& tag) {
        if (name.empty()) return Ref<Texture2D>{}; // An unauthored name is not a missing asset.
        const auto art = command_texture_optional(name);
        if (art.is_null() && planet_hover_missing.emplace(object, tag).second) {
            auto note = galactic_view::unresolved_ui_texture_note(name);
            note.object = object; note.tag = tag; pending_texture_notes.push_back(std::move(note));
        }
        return art;
    };
    const auto component_texture = [&](const ui::CommandBarComponent* component, const ui::Field field) {
        if (!component) return Ref<Texture2D>{};
        const auto names = component->list(field);
        return names.empty() ? Ref<Texture2D>{} : texture(names.front(), component->name,
            field == ui::Field::blank_texture_name ? "Blank_Texture_Name" : "Icon_Alternate_Texture_Name");
    };
    const auto pixel_size = [&](const ui::CommandBarComponent* component) {
        return std::max(1, presentation::ui::font_pixel_height(
            static_cast<int>(component->integer(ui::Field::font_point_size).value_or(0)), camera.height));
    };
    const auto font_of = [&](const ui::CommandBarComponent* component) {
        const auto face = hud_fonts->resolve({std::string(component->text(ui::Field::font_name)),
            component->integer(ui::Field::font_point_size).value_or(0)}, "english");
        return hud_fonts->font(face, true);
    };
    const auto body_font = font_of(body);
    const int body_pixels = pixel_size(body);
    if (body_font.is_null()) { hide(); return; }
    const auto art = content->art.find(hovered);
    planet_hover_levels = galactic_view::planet_hover_levels(facts,
        art == content->art.end() ? std::nullopt : art->second.space_capacity,
        art == content->art.end() ? std::nullopt : art->second.ground_capacity);
    const auto* space_style = command_catalog.find("g_space_level_pips");
    const auto* ground_style = command_catalog.find("g_ground_level_pips");
    const std::array<const ui::CommandBarComponent*, 2> pip_styles{space_style, ground_style};
    std::array<Ref<Texture2D>, 2> pip_art;
    for (std::size_t row = 0; row < pip_styles.size(); ++row) {
        const auto names = pip_styles[row] ? pip_styles[row]->list(ui::Field::icon_alternate_texture_name) : std::span<const std::string>{};
        planet_hover_pip_names[row] = names.empty() ? std::string{} : names.front();
        pip_art[row] = component_texture(pip_styles[row], ui::Field::icon_alternate_texture_name);
    }
    // The rich panel uses the overview emblem table, not the absent legacy faction Icon_Name textures.
    const auto emblem = facts.apparent_allegiance_owner ? fleet_faction_texture(*facts.apparent_allegiance_owner, true)
        : Ref<Texture2D>{};
    // WGUI-188: the ordinary planet header explicitly requests icon scale one.
    const Vector2 emblem_size = emblem.is_valid() ? emblem->get_size() * scale : Vector2{};
    const auto weather = command_atlas_texture(planet->weather.icon);
    const float band_height = header->vec2(ui::Field::size).value_or(ui::Vec2{}).y * scale;
    const auto header_separator = texture("e_line.tga", "planet encyclopedia", "header separator");
    // The emblem hangs below the band but is excluded from row sizing. Weather and the shared header
    // separator occupy the following row, before the planet's description.
    const float detail_height = std::max(header_separator.is_valid() ? header_separator->get_height() * scale : 0.0F,
        weather.is_valid() ? weather->get_height() * scale : 0.0F);
    const float header_height = band_height + detail_height;
    String description;
    for (const auto& paragraph : planet->info.encyclopedia) {
        if (!description.is_empty()) description += "\n";
        description += String::utf8(paragraph.c_str());
    }
    const float body_height = description.is_empty() ? 0.0F : std::max(size.y * scale,
        static_cast<float>(body_font->get_multiline_string_size(description, HORIZONTAL_ALIGNMENT_LEFT,
            width - padding * 2.0F, body_pixels).y));
    const auto separator = texture("e_line2.tga", "planet encyclopedia", "separator");
    const float separator_height = separator.is_valid() ? separator->get_height() * scale : 0.0F;
    const std::array<std::int64_t, 2> maxima{planet_hover_levels.space_max, planet_hover_levels.ground_max};
    const std::array<std::int64_t, 2> filled{planet_hover_levels.space_filled, planet_hover_levels.ground_filled};
    std::array<Vector2, 2> pip_sizes;
    float height = header_height + body_height + separator_height + inset.y * scale;
    for (std::size_t row = 0; row < pip_art.size(); ++row) {
        if (pip_art[row].is_valid() && maxima[row] > 0) {
            pip_sizes[row] = pip_art[row]->get_size() * (1.5F * scale);
            height += pip_sizes[row].y;
        }
    }
    const auto offset = back->vec2(ui::Field::default_offset).value_or(ui::Vec2{});
    const Rect2 panel(key.anchor + Vector2(offset.x, -offset.y) * scale - Vector2(0, height), Vector2(width, height));
    planet_hover_rect = panel;
    while (planet_hover_tooltip->get_child_count()) {
        auto* child = planet_hover_tooltip->get_child(0);
        planet_hover_tooltip->remove_child(child); child->queue_free();
    }
    planet_hover_pips.clear();
    planet_hover_title = nullptr; planet_hover_description = nullptr; planet_hover_income = nullptr;
    planet_hover_badge = nullptr; planet_hover_leader = nullptr;
    const auto image = [&](const Ref<Texture2D>& resource, const Rect2& rect, const Color tint) -> TextureRect* {
        if (resource.is_null()) return nullptr;
        auto* node = memnew(TextureRect);
        node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        node->set_stretch_mode(TextureRect::STRETCH_SCALE);
        node->set_texture(resource); node->set_position(rect.position); node->set_size(rect.size); node->set_modulate(tint);
        planet_hover_tooltip->add_child(node);
        return node;
    };
    const auto text = [&](const String& value, const ui::CommandBarComponent* style, const Rect2& rect,
        const HorizontalAlignment align, const bool wrap) -> Label* {
        if (!style || value.is_empty()) return nullptr;
        auto* node = memnew(Label);
        node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        node->set_text(value); node->add_theme_font_override("font", font_of(style));
        node->add_theme_font_size_override("font_size", pixel_size(style));
        const auto ink = style->color(ui::Field::text_color).value_or(ui::Rgba8{255,255,255,255});
        node->add_theme_color_override("font_color", Color(ink.r / 255.0F, ink.g / 255.0F, ink.b / 255.0F, 1));
        node->set_horizontal_alignment(align);
        if (wrap) node->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
        node->set_position(rect.position); node->set_size(rect.size); planet_hover_tooltip->add_child(node);
        return node;
    };
    const auto alternates = back->list(ui::Field::icon_alternate_texture_name);
    if (alternates.size() > 1) image(texture(alternates[1], back->name, "Icon_Alternate_Texture_Name"),
        panel.grow(8 * scale), Color(0.5F, 0.5F, 0.5F, 0.5F));
    if (!alternates.empty()) image(texture(alternates.front(), back->name, "Icon_Alternate_Texture_Name"),
        panel.grow(scale), Color(1,1,1,1));
    image(component_texture(back, ui::Field::blank_texture_name), panel, Color(1,1,1,1));
    auto* connector = memnew(Polygon2D);
    PackedVector2Array connector_points; PackedColorArray connector_colours;
    popup_connector(panel, scale, 1.0F, connector_points, connector_colours);
    connector->set_polygon(connector_points); connector->set_vertex_colors(connector_colours);
    planet_hover_tooltip->add_child(connector);
    const bool population = planet->info.population_capacity != 0;
    const auto band = texture(population ? "e_topbar.tga" : "e_topbar2.tga", "planet encyclopedia", "header");
    image(band, Rect2(panel.position, Vector2(width, band_height)), Color(1,1,1,1));
    const float population_shift = population ? 0.0F : encyclopedia_globals.population_offset;
    const float name_units = emblem.is_valid() ? encyclopedia_globals.name_offset - population_shift
        : population ? encyclopedia_globals.name_offset - (encyclopedia_globals.icon_x_offset - encyclopedia_globals.population_offset)
        : encyclopedia_globals.population_offset;
    const float name_left = name_units * scale;
    const String income = facts.visible_income ? String("$ ") + String::num_int64(static_cast<std::int64_t>(std::llround(*facts.visible_income))) : String();
    const auto cost_font = cost ? font_of(cost) : Ref<Font>{};
    const float cost_width = cost_font.is_valid() ? cost_font->get_string_size(income, HORIZONTAL_ALIGNMENT_LEFT, -1, pixel_size(cost)).x : 0.0F;
    const float cost_right = encyclopedia_globals.cost_offset * scale;
    planet_hover_title = text(String::utf8(facts.display_name.c_str()), header,
        Rect2(panel.position + Vector2(name_left,0), Vector2(std::max(0.0F,cost_right - name_left - cost_width),band_height)), HORIZONTAL_ALIGNMENT_LEFT, false);
    if (population) {
        auto* number = text(String::num_int64(std::abs(planet->info.population_capacity)), header,
            Rect2(panel.position, Vector2(encyclopedia_globals.population_offset * 2 * scale,band_height)), HORIZONTAL_ALIGNMENT_CENTER, false);
        if (number) number->add_theme_color_override("font_color", planet->info.population_capacity > 0 ? Color(0,1,0,1) : Color(1,0.84F,0.22F,1));
    }
    if (facts.visible_income) planet_hover_income = text(income, cost,
        Rect2(panel.position + Vector2(cost_right - cost_width,0), Vector2(cost_width,band_height)), HORIZONTAL_ALIGNMENT_RIGHT, false);
    const Vector2 emblem_centre = panel.position + Vector2((encyclopedia_globals.icon_x_offset - population_shift) * scale,
        band_height * 0.5F - encyclopedia_globals.icon_y_offset * scale);
    planet_hover_badge = image(emblem, Rect2(emblem_centre - emblem_size * 0.5F,emblem_size), Color(1,1,1,1));
    if (weather.is_valid()) image(weather, Rect2(panel.position + Vector2(width - weather->get_width() * scale,band_height),
        weather->get_size() * scale), Color(1,1,1,1));
    if (header_separator.is_valid()) image(header_separator,
        Rect2(panel.position + Vector2((width - header_separator->get_width() * scale) * 0.5F,band_height),
            header_separator->get_size() * scale), Color(1,1,1,1));
    Vector2 at = panel.position + Vector2(padding,header_height);
    planet_hover_description = text(description, body, Rect2(at,Vector2(width - padding * 2,body_height)), HORIZONTAL_ALIGNMENT_LEFT, true);
    at.y += body_height;
    if (separator.is_valid()) image(separator,
        Rect2(Vector2(panel.get_center().x - separator->get_width() * scale * 0.5F,at.y),separator->get_size() * scale), Color(1,1,1,1));
    at.y += separator_height;
    const Color owner(facts.name_colour.r,facts.name_colour.g,facts.name_colour.b,1);
    for (unsigned row = 0; row < pip_art.size(); ++row) {
        if (pip_art[row].is_null()) continue;
        for (std::int64_t pip = 0; pip < maxima[row]; ++pip) {
            const bool full = pip < filled[row];
            auto* node = image(pip_art[row], Rect2(at + Vector2(static_cast<float>(pip) * pip_sizes[row].x,0),pip_sizes[row]),
                full ? owner : Color(owner.r * 0.5F,owner.g * 0.5F,owner.b * 0.5F,1));
            planet_hover_pips.push_back({node,row,full});
        }
        at.y += pip_sizes[row].y;
    }
    planet_hover_leader = memnew(Line2D);
    planet_hover_leader->set_width(std::max(1.0F,scale));
    planet_hover_leader->set_default_color(Color(0.65F,0.85F,1.0F,0.5F));
    planet_hover_leader->add_point(Vector2((panel.position.x + panel.size.x),panel.get_center().y));
    planet_hover_leader->add_point(planet_hover_target);
    planet_hover_tooltip->add_child(planet_hover_leader);
    move_leader();
    planet_hover_key = key; ++planet_hover_builds;
    planet_hover_tooltip->set_meta("builds", static_cast<int64_t>(planet_hover_builds));
    planet_hover_tooltip->show();
}

// #7177 / WGSI-8/9: the pointer tooltip of the planet's hero status icons lists the heroes the observer may see there as a
// grid of unit-frame cells, after the construction tooltip's delay. The grid, cell size and padding follow the debug build's
// icon tooltip (WGSI-8): 44-unit cells, six to a row, filled in reverse listing order.
void GalacticMode::State::service_hero_tooltip() {
    if (!hero_tooltip || !hud || !command_bar) return;
    const auto now = Time::get_singleton()->get_ticks_msec();
    const auto pointer = hud->get_viewport()->get_mouse_position();
    const auto* control = hud->get_viewport()->gui_get_hovered_control();
    std::uint64_t key{};
    galactic_view::ObjectId planet{};
    galactic_view::PlanetStatusIcon kind{};
    if (encyclopedia_admitted() && !dragged_fleet) for (const auto& [id_kind, icon] : status_effect_icons) {
        if (!icon || control != icon || !icon->is_visible_in_tree()) continue;
        const auto candidate = static_cast<galactic_view::PlanetStatusIcon>(id_kind.second);
        if (!galactic_view::is_hero_status_icon(candidate)) continue;
        planet = id_kind.first; kind = candidate;
        key = ((static_cast<std::uint64_t>(planet) << 8U) | id_kind.second) + 1U;
        break;
    }
    if (key != hero_hover_key || pointer != hero_hover_position) {
        hero_hover_key = key; hero_hover_position = pointer; hero_hover_since = now;
        hero_tooltip->hide();
    }
    hero_hover_planet = planet; hero_hover_kind = kind;
    for (auto& cell : hero_tooltip_cells) { cell.frame->hide(); cell.icon->hide(); cell.type = 0; }
    if (!key || now - hero_hover_since < production_tooltip_delay) { hero_tooltip->hide(); return; }
    const auto facts = hud_facts.find(planet);
    const auto* component = command_catalog.find("tooltip_icon");
    if (facts == hud_facts.end() || !component) { hero_tooltip->hide(); return; }
    const auto side_of = [this](const galactic_view::PlayerId owner) {
        const auto* appearance = content ? galactic_view::affiliation_for_owner(*content, owner) : nullptr;
        if (!appearance || !route_styles) return galactic_view::HeroSide::none;
        return galactic_view::hero_side(appearance->faction, route_styles->good_faction, route_styles->evil_faction,
            route_styles->corrupt_faction);
    };
    const auto types = galactic_view::hero_tooltip_types(facts->second.hero_tips, kind, side_of);
    const auto grid = galactic_view::tooltip_icon_slots(types.size());
    if (grid.slots.empty()) { hero_tooltip->hide(); return; }
    const float scale = static_cast<float>(command_bar->placement().scale);
    constexpr float cell_units = 44.0F;
    const float cell_size = cell_units * scale;
    const float authored_scale = static_cast<float>(component->number(data::ui::Field::scale).value_or(1.0));
    const auto blanks = component->list(data::ui::Field::blank_texture_name);
    const auto blank = blanks.empty() ? Ref<Texture2D>{} : command_texture(blanks.front());
    std::size_t shown{};
    for (std::size_t item = 0; item < grid.slots.size(); ++item) {
        const auto name = production_names.find(types[item]);
        const auto art = name == production_names.end() ? production_art.end() : production_art.find(name->second);
        const auto texture = art == production_art.end() ? Ref<Texture2D>{} : command_texture(art->second.icon);
        if (texture.is_null()) continue;
        auto& cell = hero_tooltip_cells[item];
        cell.type = types[item];
        cell.icon->set_texture(texture);
        const Vector2 icon_size = texture->get_size() * scale * authored_scale;
        cell.icon->set_size(icon_size);
        cell.frame->set_texture(blank);
        const Vector2 frame_size = blank.is_valid() ? blank->get_size() * scale * authored_scale : Vector2{};
        cell.frame->set_size(frame_size);
        const Vector2 centre(3.0F * scale + (static_cast<float>(grid.slots[item].column) + 0.5F) * cell_size,
            3.0F * scale + (static_cast<float>(grid.slots[item].row) + 0.5F) * cell_size);
        cell.icon->set_position(centre - icon_size * 0.5F);
        cell.frame->set_position(centre - frame_size * 0.5F);
        cell.frame->show(); cell.icon->show();
        ++shown;
    }
    if (!shown) { hero_tooltip->hide(); return; }
    // The backdrop is the occupied grid plus four units, its frame two units outside (the construction tooltip's metrics).
    const Vector2 size(static_cast<float>(grid.columns) * cell_size + 6.0F * scale,
        static_cast<float>(grid.rows) * cell_size + 6.0F * scale);
    hero_tooltip->set_size(size);
    hero_tooltip_frame->set_size(size);
    hero_tooltip_back->set_position(Vector2(scale, scale));
    hero_tooltip_back->set_size(size - Vector2(2.0F * scale, 2.0F * scale));
    const Vector2 gap(8192.0F / static_cast<float>(camera.width) * scale,
        6144.0F / static_cast<float>(camera.height) * 3.0F * scale);
    Vector2 position = hero_hover_position + gap;
    if (position.x + size.x > static_cast<float>(camera.width)) position.x = hero_hover_position.x - size.x - gap.x;
    if (position.y + size.y > static_cast<float>(camera.height)) position.y = hero_hover_position.y - size.y;
    position.x = std::clamp(position.x, 0.0F, std::max(0.0F, static_cast<float>(camera.width) - size.x));
    position.y = std::clamp(position.y, 0.0F, std::max(0.0F, static_cast<float>(camera.height) - size.y));
    hero_tooltip->set_position(position);
    hero_tooltip->show();
}

// GHUD-S82..S83: focused cells supply observer-admitted display types to the
// ordinary encyclopedia. The ground input surface covers the pads, while
// orbit drag controls are children of their linked cell's native button.
sim::galactic::TypeId GalacticMode::State::focused_planet_hover_type() const {
    if (!fleet_organize || !fleet_organize->is_visible_in_tree() || !hud || !selected_planet
        || !displayed_snapshot || dragged_fleet || ground_hud.pressed
        || hud->get_viewport()->gui_is_dragging()) return 0;
    auto* pointer = hud->get_viewport()->gui_get_hovered_control();
    if (!pointer) return 0;
    const auto point = hud->get_global_mouse_position();
    const auto roster = ground_roster_find(selected_planet);
    if (roster != ground_hud.rosters.end() && ground_hud.publication == displayed_snapshot) {
        for (const auto& [name, button] : ground_hud.buttons) {
            if (!button->is_visible_in_tree() || !button->get_global_rect().has_point(point)) continue;
            if (pointer != ground_hud.input && pointer != button && !button->is_ancestor_of(pointer)) continue;
            const std::string_view prefix = name.starts_with("Pad_") ? "Pad_" : "Planet_structure_";
            if (!name.starts_with(prefix)) continue;
            std::size_t index{};
            const auto digits = std::string_view(name).substr(prefix.size());
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) continue;
            if (prefix == "Pad_") {
                // The focused organizer can move a company to a different pad.
                // Use that pad's displayed identity, never its roster index.
                if (!roster->second.contents_known) continue;
                const auto widget = std::find_if(ground_hud.widgets.begin(), ground_hud.widgets.end(),
                    [index](const auto& row) { return row.id == index; });
                if (widget == ground_hud.widgets.end() || !widget->object) continue;
                const auto company = std::find_if(roster->second.companies.begin(), roster->second.companies.end(),
                    [&](const auto& row) { return row.id == widget->object; });
                if (company != roster->second.companies.end()) return company->type;
            } else if (roster->second.structures_known && index < roster->second.structures.size())
                return roster->second.structures[index].type;
        }
    }
    for (std::size_t slot = 0; slot < fleet_group_buttons.size(); ++slot) {
        const auto* card = shown_panel_cards[slot];
        if (!card || fleet_publication != displayed_snapshot) continue;
        for (std::size_t cell = 0; cell < fleet_group_buttons[slot].size(); ++cell) {
            const auto button = fleet_buttons.find(fleet_group_buttons[slot][cell]);
            if (button == fleet_buttons.end() || !button->second->is_visible_in_tree()) continue;
            if (pointer != button->second && !button->second->is_ancestor_of(pointer)) continue;
            const auto& owners = fleet_cell_layouts[slot].owners;
            if (cell >= owners.size() || !owners[cell] || *owners[cell] >= card->groups.size()) return 0;
            return card->groups[*owners[cell]].type.value_or(0);
        }
    }
    return 0;
}
} // namespace eawr::presentation::godot_backend
