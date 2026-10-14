#include "galactic_mode_internal.hpp"
#include <charconv>
#include <godot_cpp/classes/viewport.hpp>

namespace eawr::presentation::godot_backend {
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
    const auto roster = ground_hud.rosters.find(selected_planet);
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
        const auto* card = shown_fleet_cards[slot];
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
