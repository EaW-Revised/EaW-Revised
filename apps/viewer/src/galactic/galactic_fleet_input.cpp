#include "galactic_mode_internal.hpp"
#include "fleet_drag.hpp"
#include "fleet_member_placement.hpp"
#include "ui/tactical_hud_internal.hpp"
#include <godot_cpp/classes/input.hpp>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
const sim::galactic::Fleet* local_fleet(const GalacticMode::State& state, std::uint64_t id) {
    if (!state.published_snapshot || !state.galaxy) return nullptr;
    const auto fleets = state.published_snapshot->fleets();
    const auto row = std::lower_bound(fleets.begin(), fleets.end(), id,
        [](const auto& fleet, auto key) { return fleet.id < key; });
    return row != fleets.end() && row->id == id && row->owner == state.galaxy->local.player
        && row->orbit && !row->motion.transit ? &*row : nullptr;
}
bool allowed(const GalacticMode::State& state) {
    const auto& gates = state.map_input.gates;
    return state.game && state.galaxy && state.published_snapshot && !state.tactical && !state.loading
        && gates.galactic && gates.hud_enabled && gates.world_allowed() && !gates.tutorial_locked
        && !gates.action_menu && !gates.selection_locked && !state.retreat_dialog_open
        && (!state.battle_dialog || !state.battle_dialog->visible())
        && state.published_snapshot->clock().pause_depth == 0;
}
void hide_empty_fleet_slots(GalacticMode::State& state) {
    for (std::size_t icon = 0; icon < state.fleet_empty_slots.size(); ++icon) {
        if (state.fleet_empty_slots[icon]) state.fleet_empty_slots[icon]->hide();
        state.fleet_empty_shown[icon] = false;
    }
}
}

void GalacticMode::State::attach_fleet_drag(Control* control, const std::string& component,
    std::function<FleetDragSource()> identity) {
    auto* drag = memnew(EawrFleetDrag); control->add_child(drag);
    // #3072: the dragged art follows the pointer unless the component sets
    // Should_Render_At_Drag_Pos False (FoC authors False only on reinforcement pads).
    const auto* authored = command_catalog.find(component);
    const bool preview = !authored || authored->flag(data::ui::Field::should_render_at_drag_pos, true);
    drag->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    const bool should_ghost = authored && authored->flag(data::ui::Field::should_ghost);
    const bool base_only = authored && authored->flag(data::ui::Field::ghost_base_only);
    const bool drag_back = authored && authored->flag(data::ui::Field::drag_back);
    drag->right_action([this, identity](Vector2 point) {
        const auto picked = identity();
        // GHUD-S93: reuse the exact cell representative, never its type stack or whole fleet header.
        static_cast<void>(open_object_sale(picked.member, point));
    });
    drag->source([this, control, preview, should_ghost, base_only, drag_back, identity = std::move(identity)](Vector2 start) -> std::optional<std::uint64_t> {
        auto* input = Input::get_singleton();
        if (!allowed(*this) || !input) return std::nullopt;
        const auto picked = identity();
        const bool ctrl = input->is_key_pressed(KEY_CTRL);
        if (ctrl && !picked.member) return std::nullopt; // Galaxy stack Ctrl path remains separate.
        const auto id = picked.fleet;
        const auto* source = local_fleet(*this, id);
        if (!source || !fleet_drag_receiver) return std::nullopt;
        // #3072: a whole card or stack icon carries every local fleet of its slots.
        dragged_stack = picked.slots.empty() ? std::vector<galactic_view::ObjectId>{}
            : galactic_view::stack_fleets(*published_snapshot, galaxy->local.player, source->orbit, picked.slots);
        if (std::find(dragged_stack.begin(), dragged_stack.end(), id) == dragged_stack.end()) dragged_stack = {id};
        dragged_members.clear();
        dragged_type_stack = ctrl;
        if (picked.member) {
            for (const auto fleet_id : dragged_stack) {
                const auto* fleet = local_fleet(*this, fleet_id);
                if (!fleet) continue;
                for (const auto& member : fleet->members) {
                    const auto actors = published_snapshot->actor_records();
                    const auto actor = std::lower_bound(actors.begin(), actors.end(), member.id,
                        [](const auto& row, auto key) { return row.id < key; });
                    const auto type = actor != actors.end() && actor->id == member.id && actor->original_type
                        ? actor->original_type : member.type;
                    if ((!ctrl && member.id == picked.member) || (ctrl && picked.cell_type == type))
                        dragged_members.push_back({fleet_id, member.id});
                }
            }
            if (dragged_members.empty()) return std::nullopt;
        }
        dragged_fleet = id; map_input.dragged = id; map_input.hyperspace = true;
        dragged_cell_slot = picked.cell_slot; dragged_cell_type = picked.cell_type;
        ++fleet_drag_begins;
        // WGP-184: the held icon has left its rest point (GUI activation here, not the >12 px seam).
        if (audio) audio->ui_sound("GUI_Drag_Fleet");
        std::optional<std::size_t> count;
        std::size_t remainder{};
        if (picked.member && picked.cell_slot) {
            if (const auto* card = shown_panel_cards[*picked.cell_slot])
                for (const auto& group : card->groups) if (group.representative == picked.member) {
                    const auto counts = galactic_view::fleet_drag_counts(group.count, !ctrl, should_ghost);
                    count = ctrl ? dragged_members.size() : counts.first; remainder = counts.second;
                    break;
                }
        }
        fleet_drag_receiver->start(preview ? EawrFleetDrag::ghost_of(*control,
            start - control->get_global_position(), count, false, drag_back) : nullptr);
        if (preview) {
            fleet_drag_receiver->lift_source(*control, remainder ? std::optional(remainder) : std::nullopt,
                should_ghost && base_only && !ctrl);
            // Ctrl may take a type displayed at more than one remembered cell.
            if (ctrl && picked.cell_slot) if (const auto* card = shown_panel_cards[*picked.cell_slot]) {
                const auto& names = fleet_group_buttons[*picked.cell_slot];
                const auto& owners = fleet_cell_layouts[*picked.cell_slot].owners;
                for (std::size_t cell = 0; cell < names.size() && cell < owners.size(); ++cell) {
                    if (!owners[cell] || *owners[cell] >= card->groups.size()) continue;
                    const auto& group = card->groups[*owners[cell]];
                    if (!group.representative || !std::any_of(dragged_members.begin(), dragged_members.end(),
                            [&](const auto& ref) { return ref.member == *group.representative; })) continue;
                    const auto found = fleet_buttons.find(names[cell]);
                    if (found != fleet_buttons.end() && found->second != control && found->second->is_visible_in_tree())
                        fleet_drag_receiver->lift_source(*found->second);
                }
            }
        }
        return id;
    }, [control] {
        if (control->is_visible_in_tree() && !static_cast<EawrHudButton*>(control)->is_disabled())
            static_cast<EawrHudButton*>(control)->_pressed();
    });
}

void GalacticMode::State::build_fleet_drag_receiver() {
    fleet_drag_receiver = memnew(EawrFleetDrag); hud->add_child(fleet_drag_receiver);
    fleet_drag_receiver->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    fleet_drag_receiver->receiver([this](auto id, auto point, bool submit) { return fleet_drop(id, point, submit); },
        [this] { cancel_fleet_drag(); }, [this](Vector2 point) { fleet_drag_hover(point); });
}

void GalacticMode::State::cancel_fleet_drag() {
    if (dragged_fleet) ++fleet_drag_ends;
    dragged_fleet = 0; map_input.dragged = 0; map_input.hyperspace = false;
    dragged_stack.clear(); fleet_drag_planet = 0; hide_empty_fleet_slots(*this);
    dragged_cell_slot.reset(); dragged_cell_type.reset();
    dragged_members.clear();
    dragged_type_stack = false;
    if (fleet_drag_receiver) fleet_drag_receiver->cancel();
    if (ground_hud.input && ground_hud.input->drag_art) ground_hud.input->drag_art->cancel();
}

std::optional<std::uint8_t> GalacticMode::State::stack_icon_slot(const galactic_view::ObjectId planet,
    const std::uint8_t icon) const {
    // fleet_stacks.hpp: local-only icons are slots 0/1/2; with another owner
    // present, the centre is slot 1 and the right icon gathers slots 0 and 2.
    if (!mixed_fleet_planets.contains(planet)) return icon <= 2 ? std::optional{icon} : std::nullopt;
    return icon == 1 ? std::optional{std::uint8_t{1}} : std::nullopt;
}

std::vector<std::uint8_t> GalacticMode::State::stack_icon_slots(const galactic_view::ObjectId planet,
    const std::uint8_t icon) const {
    if (const auto slot = stack_icon_slot(planet, icon)) return {*slot};
    if (mixed_fleet_planets.contains(planet) && icon == 2) return {0, 2};
    return {};
}

void GalacticMode::State::fleet_drag_hover(const Vector2 point) {
    if (!dragged_fleet) return;
    // The pointer's planet: one of its stack icons (empty ones included, so the
    // shown targets stay while the pointer moves onto them) or its pick area.
    galactic_view::ObjectId planet{};
    for (std::size_t icon = 0; icon < fleet_empty_slots.size() && !planet; ++icon)
        if (fleet_empty_shown[icon] && fleet_empty_slots[icon]->is_visible_in_tree()
            && fleet_empty_slots[icon]->get_global_rect().has_point(point)) planet = fleet_drag_planet;
    for (const auto& [key, badge] : fleet_badges) {
        if (planet) break;
        if (badge.shown && badge.button->is_visible_in_tree() && badge.button->get_global_rect().has_point(point))
            planet = key.first;
    }
    if (!planet) if (const auto* picked = picked_planet(point.x, point.y)) planet = picked->id;
    if (planet != fleet_drag_planet) hide_empty_fleet_slots(*this);
    fleet_drag_planet = planet;
}

bool GalacticMode::State::submit_fleet_stack(const galactic_view::DropIntent& intent,
    const std::span<const galactic_view::ObjectId> stack) {
    const auto planets = input_planets();
    const auto order = map_input.command(intent, planets);
    if (!order) return false;
    for (auto& command : galactic_view::stack_commands(intent, stack, *order)) {
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(command), {}});
        ++submitted_orders;
    }
    return true;
}

bool GalacticMode::State::fleet_drop(std::uint64_t id, Vector2 point, bool submit) {
    if (!allowed(*this) || id != dragged_fleet) return false;
    const auto* source = local_fleet(*this, id);
    if (!source) return false; // Stable identity is revalidated at the current publication.
    // WGCH-154: the first galaxy special slot consumes its own release before
    // ordinary fleet movement. A rejection cannot fall through into the world.
    if (destruction_hud.slot && destruction_hud.slot->is_visible_in_tree()
        && destruction_hud.slot->get_global_rect().has_point(point))
        return destruction_hud.facts
            ? source->orbit == destruction_hud.facts->planet && drop_destruction(id, submit)
            : drop_hero_ability(id, submit);
    if (fleet_organize && fleet_organize->is_visible_in_tree()) {
        for (const auto& [name, button] : ground_hud.buttons) {
            if (!button->is_visible_in_tree() || !button->get_global_rect().has_point(point)) continue;
            if (name == "b_underground") return false; // Ability/raid provider seam, #2263/#1156.
            // GHUD-S68: the structure slots bind a click only (no receive logic): a drop there is rejected.
            if (name.starts_with("Planet_structure_")) return false;
            const bool accepted = request_ground_land(submit, id);
            if (accepted && submit) {
                ++fleet_drag_orders; last_fleet_drag_kind = galactic_view::DropKind::land;
                last_fleet_drag_source = id; last_fleet_drag_target = 0; last_fleet_drag_destination = selected_planet;
            }
            return accepted;
        }
    }
    std::optional<galactic_view::DropIntent> intent;
    if (fleet_organize && fleet_organize->is_visible_in_tree()) {
        // The reserved leading pathfinder slot is never an ordinary fleet target.
        for (const auto& [name, control] : fleet_buttons)
            if (name.ends_with("_0000") && control->get_global_rect().has_point(point)) return false;
        for (std::size_t slot = 0; slot < fleet_drop_rects.size(); ++slot) {
            if (!fleet_drop_rects[slot].has_point(point)) continue;
            if (source->orbit != selected_planet) return false;
            // WGUI-175: the other owner's panel takes no drop; a local panel names the orbit slot it stands for.
            if (!fleet_panels.drop_slot[slot]) return false;
            const std::uint8_t orbit_slot = *fleet_panels.drop_slot[slot];
            if (!dragged_members.empty()) {
                const auto& cells = fleet_cell_rects[slot];
                const auto cell = std::find_if(cells.begin(), cells.end(), [point](const auto& rect) { return rect.has_point(point); });
                if (cell == cells.end()) return false;
                const auto authored = static_cast<std::size_t>(cell - cells.begin());
                // Linked footprint cells draw one native button. Receiving on
                // that button names its anchor, including its enlarged hit area.
                // A raw empty-cell drop still names the authored cell itself.
                std::vector<presentation::ui::PixelRect> buttons(fleet_group_buttons[slot].size());
                for (std::size_t at = 0; at < buttons.size(); ++at) {
                    const auto found = fleet_buttons.find(fleet_group_buttons[slot][at]);
                    if (found == fleet_buttons.end() || !found->second->is_visible_in_tree()) continue;
                    const auto rect = found->second->get_global_rect();
                    buttons[at] = {rect.position.x, rect.position.y, rect.size.x, rect.size.y};
                }
                const auto index = galactic_view::fleet_member_target_cell(authored, buttons, {point.x, point.y});
                const bool reorder = dragged_cell_slot == slot;
                const auto* target = shown_panel_cards[slot];
                // GHUD-S87: a safe target joins matching destination units;
                // an unlike-type/footprint collision keeps a separate placement.
                const auto selected = [&](const sim::galactic::ObservedFleetGroup& group) {
                    return group.representative && std::any_of(dragged_members.begin(), dragged_members.end(),
                        [&](const auto& ref) { return ref.member == *group.representative; });
                };
                presentation::ui::FleetCellIcon moving;
                if (dragged_cell_slot) if (const auto* card = shown_panel_cards[*dragged_cell_slot])
                    for (std::size_t group = 0; group < card->groups.size(); ++group)
                        if (selected(card->groups[group])) { moving = fleet_cell_looks[*dragged_cell_slot][group].footprint; break; }
                std::vector<presentation::ui::FleetCellIcon> icons;
                std::vector<std::optional<sim::galactic::TypeId>> types;
                if (target) for (std::size_t group = 0; group < target->groups.size(); ++group) {
                    if (reorder && selected(target->groups[group]) && (dragged_type_stack || target->groups[group].count == 1)) continue;
                    auto icon = fleet_cell_looks[slot][group].footprint;
                    if (fleet_cell_layouts[slot].groups[group]) icon.previous = fleet_cell_layouts[slot].groups[group]->cell;
                    icon.nearest = false; icons.push_back(icon); types.push_back(target->groups[group].type);
                }
                std::vector<std::optional<presentation::ui::FleetCellCoordinate>> coordinates;
                for (const auto& name : fleet_group_buttons[slot]) coordinates.push_back(presentation::ui::fleet_cell_coordinate(name));
                if (!dragged_cell_type) return false;
                const auto placement = galactic_view::fleet_member_cell_drop(coordinates, types, icons, moving,
                    *dragged_cell_type, index);
                if (!placement) return false;
                if (!reorder) {
                    const auto fleet = target && target->fleet ? *target->fleet : 0;
                    if (fleet && !local_fleet(*this, fleet)) return false;
                    if (!submit) return true;
                    game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                        sim::galactic::TransferFleetMembers{dragged_members, fleet,
                            fleet ? std::uint8_t{} : orbit_slot}, {}});
                    ++submitted_orders; ++fleet_drag_orders;
                }
                if (!submit) return true;
                std::vector<sim::EntityId> incoming, matching;
                for (const auto& member : dragged_members) incoming.push_back(member.member);
                if (placement->second) {
                    const auto actors = published_snapshot->actor_records();
                    for (const auto& fleet : published_snapshot->fleets()) {
                        if (fleet.owner != galaxy->local.player || fleet.orbit != selected_planet
                            || galactic_view::fleet_panel_for_slot(fleet_panels.mode, fleet.orbit_slot) != slot
                            || fleet.motion.transit) continue;
                        for (const auto& member : fleet.members) {
                            const auto actor = std::lower_bound(actors.begin(), actors.end(), member.id,
                                [](const auto& row, auto key) { return row.id < key; });
                            const auto type = actor != actors.end() && actor->id == member.id && actor->original_type
                                ? actor->original_type : member.type;
                            if (type == *dragged_cell_type) matching.push_back(member.id);
                        }
                    }
                }
                galactic_view::remember_fleet_member_drop(fleet_gui_indices, incoming, matching, *placement);
                fleet_layout_dirty = true;
                if (reorder) ++fleet_cell_reorders;
                last_fleet_drag_kind = target && target->fleet ? galactic_view::DropKind::merge : galactic_view::DropKind::orbit_slot;
                last_fleet_drag_source = id; last_fleet_drag_target = target && target->fleet ? *target->fleet : 0;
                last_fleet_drag_destination = selected_planet;
                if (audio) audio->ui_sound("GUI_Drop_Fleet");
                return true;
            }
            const auto* target = shown_panel_cards[slot];
            if (target && target->fleet) {
                // A source already shown on the slot's card is in that stack.
                if (std::any_of(dragged_stack.begin(), dragged_stack.end(),
                        [target](auto fleet) { return sim::galactic::holds_fleet(*target, fleet); })
                    || sim::galactic::holds_fleet(*target, id) || !local_fleet(*this, *target->fleet)) return false;
                intent = galactic_view::DropIntent{galactic_view::DropKind::merge, id, *target->fleet, selected_planet, {}};
            } else {
                if (source->orbit_slot == orbit_slot) return false;
                intent = galactic_view::DropIntent{galactic_view::DropKind::orbit_slot, id, 0, selected_planet, {},
                    orbit_slot};
            }
            break;
        }
    }
    galactic_view::ObjectId destination{};
    std::optional<std::uint8_t> destination_slot;
    if (!intent) {
        for (std::size_t index = 0; index < radar_controls.size(); ++index) {
            const auto* radar = radar_controls[index];
            if (!radar || !radar->is_visible_in_tree() || !radar->get_global_rect().has_point(point)) continue;
            for (const auto& [planet, blip] : radar_blips[index])
                if (blip->is_visible_in_tree() && blip->get_global_rect().has_point(point)) { destination = planet; break; }
            if (!destination) return false; // Radar empty space is never a planet command.
        }
        if (!destination) {
            // An unrelated HUD surface cannot become a world drop through it.
            for (const auto* shell : {command_bar, fleet_organize, advanced_map_shell})
                if (shell && shell->is_visible_in_tree() && shell->hit(point.x, point.y)) return false;
            // #3072: an empty stack icon of the hovered planet is a slot target.
            for (std::size_t icon = 0; icon < fleet_empty_slots.size(); ++icon) {
                if (!fleet_empty_shown[icon] || !fleet_empty_slots[icon]->is_visible_in_tree()
                    || !fleet_empty_slots[icon]->get_global_rect().has_point(point)) continue;
                const auto slot = stack_icon_slot(fleet_drag_planet, static_cast<std::uint8_t>(icon));
                if (fleet_drag_planet != source->orbit) { destination = fleet_drag_planet; destination_slot = slot; break; }
                if (!slot || source->orbit_slot == *slot) return false;
                intent = galactic_view::DropIntent{galactic_view::DropKind::orbit_slot, id, 0, source->orbit, {}, *slot};
                break;
            }
            for (const auto& [key, badge] : fleet_badges) {
                if (intent || destination) break;
                if (!badge.shown || !badge.button->is_visible_in_tree()
                    || !badge.button->get_global_rect().has_point(point)) continue;
                if (badge.owner != galaxy->local.player) {
                    // GHUD-S77: a foreign stack is its planet, never a merge
                    // target or a request for the foreign fleet's orbit slot.
                    destination = key.first;
                    break;
                }
                if (key.first != source->orbit) {
                    // #3072: another planet's own stack: move there, into that stack's slot.
                    destination = key.first; destination_slot = stack_icon_slot(key.first, key.second);
                    break;
                }
                galactic_view::ObjectId target = badge.fleet ? *badge.fleet : 0;
                if (!target) {
                    const auto row = std::find_if(galaxy->fleets.begin(), galaxy->fleets.end(),
                        [&](const auto& value) { return value.planet == key.first && value.owner == badge.owner; });
                    if (row != galaxy->fleets.end()) target = row->fleet;
                }
                if (!target || target == id || !local_fleet(*this, target)
                    || std::find(dragged_stack.begin(), dragged_stack.end(), target) != dragged_stack.end()) return false;
                intent = galactic_view::DropIntent{galactic_view::DropKind::merge, id, target, source->orbit, {}};
                break;
            }
            if (!intent && !destination) {
                const auto* picked = picked_planet(point.x, point.y);
                if (!picked) return false;
                destination = picked->id;
            }
        }
        if (!intent) {
            if (destination == source->orbit || galactic_view::observer_discovery(*published_snapshot, galaxy->local.player, destination) != true)
                return false;
            if (!submit) return published_snapshot->movement() != nullptr;
            // Search only on release, never in the GUI's per-frame drop predicate.
            const auto planets = input_planets();
            intent = galactic_view::fleet_destination_intent(*published_snapshot, galaxy->local.player, id, destination, planets);
            if (intent) intent->destination_slot = destination_slot;
        }
    }
    if (!intent) return false;
    if (!submit) return true;
    if (!submit_fleet_stack(*intent, dragged_stack)) return false;
    // WGP-184: a same-owner merge drop (only local fleets are merge targets here).
    if (intent->kind == galactic_view::DropKind::merge && audio) audio->ui_sound("GUI_Drop_Fleet");
    ++fleet_drag_orders; last_fleet_drag_kind = intent->kind;
    last_fleet_drag_source = id; last_fleet_drag_target = intent->target; last_fleet_drag_destination = intent->planet;
    return true;
}
} // namespace eawr::presentation::godot_backend
