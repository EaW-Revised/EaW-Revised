#include "galactic_mode_internal.hpp"
#include "ground_stack.hpp"
#include "ui/input_routing.hpp"
#include "fleet_drag.hpp"
#include "fleet_member_placement.hpp"
#include "eawr/presentation/ui/fleet_pathfinder.hpp"
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
        && (state.pause_allows(presentation::GalacticPausedAction::fleet_management)
            || state.pause_allows(presentation::GalacticPausedAction::fleet_move)
            || state.pause_allows(presentation::GalacticPausedAction::surface_deploy)
            || state.pause_allows(presentation::GalacticPausedAction::ability));
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
    drag->right_action([this, identity, component](Vector2 point) {
        const auto picked = identity();
        if (component.ends_with("_0000")) {
            if (picked.member && !request_pathfinder(picked.fleet, 0, true) && audio) audio->negative_feedback();
            return;
        }
        // GHUD-S93: reuse the exact cell representative, never its type stack or whole fleet header.
        static_cast<void>(open_object_sale(picked.member, point));
    });
    drag->source([this, control, component, preview, should_ghost, base_only, drag_back, identity = std::move(identity)](Vector2 start) -> std::optional<std::uint64_t> {
        auto* input = Input::get_singleton();
        if (!allowed(*this) || !input) return std::nullopt;
        const auto picked = identity();
        const bool pathfinder = component.ends_with("_0000");
        const bool ctrl = !pathfinder && input->is_key_pressed(KEY_CTRL);
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
                    if (!pathfinder && picked.cell_slot && pathfinder_source(*picked.cell_slot).member == member.id) continue;
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
        dragged_pathfinder = pathfinder;
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

void GalacticMode::State::attach_ground_stack_drag(Control* control, const galactic_view::ObjectId planet) {
    auto* drag = memnew(EawrFleetDrag); control->add_child(drag);
    drag->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    drag->source([this, control, planet](Vector2 start) -> std::optional<std::uint64_t> {
        if (!allowed(*this) || !fleet_drag_receiver || !control->is_visible_in_tree()
            || static_cast<EawrHudButton*>(control)->is_disabled()
            || !pause_allows(presentation::GalacticPausedAction::fleet_management)) return {};
        const auto stack = galactic_view::ground_stack(*published_snapshot, galaxy->local.player, planet, 0);
        if (!stack) return {};
        auto units = galactic_view::ground_stack_units(*stack);
        if (units.empty()) return {};
        dragged_ground_planet = planet; dragged_ground_units = std::move(units); ground_stack_accepted = false;
        map_input.dragged = planet; map_input.hyperspace = true; ++fleet_drag_begins;
        const auto* style = command_catalog.find("g_planet_land_forces");
        if (!style || style->flag(data::ui::Field::should_render_at_drag_pos, true)) {
            fleet_drag_receiver->start(EawrFleetDrag::ghost_of(*control, start - control->get_global_position(), {}, false,
                style && style->flag(data::ui::Field::drag_back)));
            fleet_drag_receiver->lift_source(*control);
        } else fleet_drag_receiver->start();
        if (audio) audio->ui_sound("GUI_Drag_Fleet");
        return planet;
    }, [control] {
        if (control->is_visible_in_tree() && !static_cast<EawrHudButton*>(control)->is_disabled())
            static_cast<EawrHudButton*>(control)->_pressed();
    });
}

bool GalacticMode::State::ground_stack_drop(const Vector2 point, const bool submit) {
    if (!allowed(*this) || !dragged_ground_planet || dragged_ground_units.empty()
        || !pause_allows(presentation::GalacticPausedAction::fleet_management)) return false;
    const auto origin = dragged_ground_planet;
    galactic_view::ObjectId destination{}, target{};
    std::uint8_t source_slot{};
    std::optional<std::uint8_t> arrival_slot;
    bool hit{}, land_on_arrival{};
    for (const auto& [key, badge] : fleet_badges) {
        if (!badge.shown || !badge.button->is_visible_in_tree() || !badge.button->get_global_rect().has_point(point)) continue;
        if (badge.ground_planet) {
            if (key.first == origin || !pause_allows(presentation::GalacticPausedAction::surface_deploy)) return false;
            destination = key.first; hit = true; land_on_arrival = true; break;
        }
        destination = key.first; hit = true;
        const auto slot = badge.owner == galaxy->local.player ? stack_icon_slot(destination, key.second) : std::nullopt;
        if (destination == origin) {
            if (badge.owner != galaxy->local.player || !slot) return false;
            target = badge.fleet.value_or(0); source_slot = target ? 0 : *slot;
        } else arrival_slot = slot;
        break;
    }
    for (std::size_t icon = 0; !hit && icon < fleet_empty_slots.size(); ++icon) {
        if (!fleet_empty_shown[icon] || !fleet_empty_slots[icon]->is_visible_in_tree()
            || !fleet_empty_slots[icon]->get_global_rect().has_point(point)) continue;
        const auto slot = stack_icon_slot(fleet_drag_planet, static_cast<std::uint8_t>(icon));
        if (!slot) return false;
        destination = fleet_drag_planet; hit = true;
        if (destination == origin) source_slot = *slot; else arrival_slot = slot;
    }
    if (!hit && selected_planet == origin && fleet_organize && fleet_organize->is_visible_in_tree()) {
        for (std::size_t panel = 0; panel < fleet_drop_rects.size(); ++panel) {
            if (!fleet_drop_rects[panel].has_point(point)) continue;
            if (!fleet_panels.drop_slot[panel]) return false;
            destination = origin; hit = true;
            const auto* card = shown_panel_cards[panel];
            target = card ? card->fleet.value_or(0) : 0;
            source_slot = target ? 0 : *fleet_panels.drop_slot[panel];
            break;
        }
    }
    if (!hit) {
        if (hud && pointer_over_panel(*hud, point)) return false;
        const auto* picked = picked_planet(point.x, point.y);
        if (!picked || picked->id == origin) return false;
        destination = picked->id;
    }
    std::vector<sim::galactic::PlanetId> path;
    if (destination != origin) {
        if (!pause_allows(presentation::GalacticPausedAction::fleet_move)
            || galactic_view::observer_discovery(*published_snapshot, galaxy->local.player, destination) != true) return false;
        const auto planets = input_planets();
        auto planned = galactic_view::destination_path(*published_snapshot, galaxy->local.player, origin, destination, planets);
        if (!planned) return false;
        path = std::move(*planned); source_slot = 2; // GGS-04 / WGM-199.
    }
    const auto intent = galactic_view::ground_stack_launch(*published_snapshot, galaxy->local.player,
        origin, dragged_ground_units, target, source_slot, std::move(path), arrival_slot, land_on_arrival);
    if (!intent) return false;
    const auto planets = input_planets();
    const auto command = map_input.command(*intent, planets);
    if (!command) return false;
    if (!submit) return true;
    game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, *command, {}});
    ++submitted_orders; ++ground_hud.launch_orders; ground_stack_accepted = true;
    ground_hud.last_launch_units = dragged_ground_units; ground_hud.last_launch_target = target;
    ground_hud.last_launch_slot = source_slot; ground_hud.last_launch_path = intent->path;
    ground_hud.last_launch_land = land_on_arrival;
    if (audio) audio->ui_sound("GUI_Drop_Fleet");
    return true;
}

void GalacticMode::State::build_fleet_drag_receiver() {
    fleet_drag_receiver = memnew(EawrFleetDrag); hud->add_child(fleet_drag_receiver);
    fleet_drag_receiver->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    fleet_drag_receiver->receiver([this](auto id, auto point, bool submit) {
        return dragged_ground_planet ? ground_stack_drop(point, submit) : fleet_drop(id, point, submit);
    },
        [this] { cancel_fleet_drag(); }, [this](Vector2 point) { fleet_drag_hover(point); });
}

void GalacticMode::State::cancel_fleet_drag() {
    if (dragged_ground_planet) {
        if (!ground_stack_accepted) { ++ground_hud.rejected; if (audio) audio->negative_feedback(); }
        dragged_ground_planet = 0; dragged_ground_units.clear(); ground_stack_accepted = false;
    }
    if (dragged_fleet) ++fleet_drag_ends;
    dragged_fleet = 0; map_input.dragged = 0; map_input.hyperspace = false;
    dragged_stack.clear(); fleet_drag_planet = 0; hide_empty_fleet_slots(*this);
    dragged_cell_slot.reset(); dragged_cell_type.reset();
    dragged_members.clear();
    dragged_type_stack = false;
    dragged_pathfinder = false;
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
    const auto change_planet = [this](const galactic_view::ObjectId planet) {
        if (planet == fleet_drag_planet) return;
        hide_empty_fleet_slots(*this);
        // Hover receivers change without a simulation publication (especially paused).
        // Re-place both ends so the shared empty-slot controls appear at the new planet.
        hud_label_key.reset();
        for (const auto id : {fleet_drag_planet, planet})
            if (const auto label = planet_label_records.find(id); label != planet_label_records.end())
                label->second.placed = false;
        fleet_drag_planet = planet;
    };
    if (!dragged_fleet && !dragged_ground_planet && !(ground_hud.pressed_object && map_input.box_end)) {
        hide_empty_fleet_slots(*this); change_planet(0); return;
    }
    if (hud && pointer_over_panel(*hud, point)) {
        hide_empty_fleet_slots(*this);
        change_planet(0);
        return;
    }
    // The pointer's planet: one of its stack icons (empty ones included, so the
    // shown targets stay while the pointer moves onto them) or its pick area.
    galactic_view::ObjectId planet{};
    if (destruction_hud.slot && destruction_hud.slot->is_visible_in_tree()
        && destruction_hud.slot->get_global_rect().has_point(point) && destruction_hud.drop)
        planet = destruction_hud.drop->planet;
    for (std::size_t icon = 0; icon < fleet_empty_slots.size() && !planet; ++icon)
        if (fleet_empty_shown[icon] && fleet_empty_slots[icon]->is_visible_in_tree()
            && fleet_empty_slots[icon]->get_global_rect().has_point(point)) planet = fleet_drag_planet;
    for (const auto& [key, badge] : fleet_badges) {
        if (planet) break;
        if (badge.shown && badge.button->is_visible_in_tree() && badge.button->get_global_rect().has_point(point))
            planet = key.first;
    }
    if (!planet) if (const auto* picked = picked_planet(point.x, point.y)) planet = picked->id;
    change_planet(planet);
}

bool GalacticMode::State::submit_fleet_stack(const galactic_view::DropIntent& intent,
    const std::span<const galactic_view::ObjectId> stack) {
    const auto planets = input_planets();
    const auto order = map_input.command(intent, planets);
    if (!order) return false;
    auto commands = galactic_view::fleet_stack_orders(*published_snapshot, galaxy->local.player, intent, stack, *order,
        [this](const auto fleet, const auto planet) { return hero_ability_drop(fleet, planet); });
    if (!commands) return false;
    for (auto& command : *commands) {
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(command), {}});
        ++submitted_orders;
    }
    return true;
}

bool GalacticMode::State::request_pathfinder(const sim::galactic::FleetId fleet, const sim::EntityId member, const bool submit) {
    if (!allowed(*this)) return false;
    const auto frame = game->frame(sim::SeatId{galaxy->local.seat});
    if (!frame.latest) return false;
    const bool pending = frame.pending_conflict != nullptr;
    const bool hold = pending || frame.summary_hold != 0 || frame.conflict != nullptr || frame.auto_resolve_progress != nullptr;
    const auto order = presentation::ui::fleet_pathfinder_order(*frame.latest, sim::SeatId{galaxy->local.seat}, fleet, member, hold, pending);
    if (!order) return false;
    if (submit) {
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, *order, {}});
        ++submitted_orders; ++fleet_drag_orders;
        if (audio) audio->ui_sound("GUI_Drop_Fleet");
    }
    return true;
}

bool GalacticMode::State::fleet_drop(std::uint64_t id, Vector2 point, bool submit) {
    if (!allowed(*this) || id != dragged_fleet) return false;
    const auto* source = local_fleet(*this, id);
    if (!source) return false; // Stable identity is revalidated at the current publication.
    if (fleet_organize && fleet_organize->is_visible_in_tree()) {
        for (const auto& [name, control] : fleet_buttons) {
            if (!name.ends_with("_0000") || !control->is_visible_in_tree() || !control->get_global_rect().has_point(point)) continue;
            const auto panel = static_cast<std::size_t>(name[1] - '1');
            if (source->orbit != selected_planet || galactic_view::fleet_panel_for_slot(fleet_panels.mode, source->orbit_slot) != panel
                || dragged_members.size() != 1) return false;
            if (!request_pathfinder(id, dragged_members.front().member, false)) return false;
            // A panel can aggregate several native fleets. Replace its old selector
            // through commands too; presentation never clears a published flag itself.
            std::vector<sim::galactic::FleetId> clear;
            for (const auto& fleet : published_snapshot->fleets()) {
                if (fleet.id == id || fleet.owner != source->owner || fleet.orbit != source->orbit || fleet.motion.transit
                    || galactic_view::fleet_panel_for_slot(fleet_panels.mode, fleet.orbit_slot) != panel) continue;
                if (std::any_of(fleet.members.begin(), fleet.members.end(), [](const auto& member) { return member.pathfinder; })) {
                    if (!request_pathfinder(fleet.id, 0, false)) return false;
                    clear.push_back(fleet.id);
                }
            }
            if (submit) for (const auto fleet : clear) static_cast<void>(request_pathfinder(fleet, 0, true));
            return request_pathfinder(id, dragged_members.front().member, submit);
        }
        // GHUD-R44: returning the selector to its fleet's ordinary area clears it;
        // it never transfers the whole fleet or changes membership.
        if (dragged_pathfinder) {
            const auto panel = galactic_view::fleet_panel_for_slot(fleet_panels.mode, source->orbit_slot);
            return source->orbit == selected_planet && fleet_drop_rects[panel].has_point(point)
                && request_pathfinder(id, 0, submit);
        }
    }
    if (dragged_pathfinder) return false;
    for (const auto& [planet, slot] : destruction_hud.raid_slots)
        if (slot.button && slot.button->is_visible_in_tree() && slot.button->get_global_rect().has_point(point))
            return drop_raid(id, planet, submit);
    for (const auto& [planet, slot] : destruction_hud.ability_slots)
        if (slot.button && slot.button->is_visible_in_tree() && slot.button->get_global_rect().has_point(point))
            return drop_hero_ability(id, submit, planet);
    // WGCH-154: the first galaxy special slot consumes its own release before
    // ordinary fleet movement. A rejection cannot fall through into the world.
    if (destruction_hud.slot && destruction_hud.slot->is_visible_in_tree()
        && destruction_hud.slot->get_global_rect().has_point(point))
        return destruction_source(id)
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
        for (std::size_t slot = 0; slot < fleet_drop_rects.size(); ++slot) {
            if (!fleet_drop_rects[slot].has_point(point)) continue;
            if (source->orbit != selected_planet
                || !pause_allows(presentation::GalacticPausedAction::fleet_management)) return false;
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
                if (badge.ground_planet) {
                    // GGS-05: only the existing same-planet landing command is expressible here.
                    if (key.first != source->orbit || !pause_allows(presentation::GalacticPausedAction::surface_deploy)) return false;
                    if (dragged_stack.size() > 1 && !pause_allows(presentation::GalacticPausedAction::fleet_management)) return false;
                    const auto* roster = command_roster(key.first);
                    if (!roster || !roster->accessible || !roster->allied
                        || !galactic_view::fleet_can_land(*published_snapshot, galaxy->local.player, id, dragged_stack)) return false;
                    intent = galactic_view::DropIntent{galactic_view::DropKind::land, id, 0, key.first, {}};
                    break;
                }
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
            if (!pause_allows(presentation::GalacticPausedAction::fleet_move)
                || (dragged_stack.size() > 1 && !pause_allows(presentation::GalacticPausedAction::fleet_management))) return false;
            if (!submit) return published_snapshot->movement() != nullptr;
            // Search only on release, never in the GUI's per-frame drop predicate.
            const auto planets = input_planets();
            intent = galactic_view::fleet_destination_intent(*published_snapshot, galaxy->local.player, id, destination, planets, dragged_stack);
            if (intent) intent->destination_slot = destination_slot;
        }
    }
    if (!intent) return false;
    if (intent->kind == galactic_view::DropKind::land
        && !galactic_view::fleet_can_land(*published_snapshot, galaxy->local.player, id, dragged_stack)) return false;
    if (!pause_allows(intent->kind == galactic_view::DropKind::move ? presentation::GalacticPausedAction::fleet_move
            : intent->kind == galactic_view::DropKind::land ? presentation::GalacticPausedAction::surface_deploy
            : presentation::GalacticPausedAction::fleet_management)) return false;
    if (!submit) return true;
    if (!submit_fleet_stack(*intent, dragged_stack)) return false;
    // WGP-184: a same-owner merge drop (only local fleets are merge targets here).
    if (intent->kind == galactic_view::DropKind::merge && audio) audio->ui_sound("GUI_Drop_Fleet");
    ++fleet_drag_orders; last_fleet_drag_kind = intent->kind;
    last_fleet_drag_source = id; last_fleet_drag_target = intent->target; last_fleet_drag_destination = intent->planet;
    return true;
}
} // namespace eawr::presentation::godot_backend
