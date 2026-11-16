#include "galactic_mode_internal.hpp"
#include <charconv>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <algorithm>

using namespace godot;

namespace eawr::presentation::godot_backend {
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
