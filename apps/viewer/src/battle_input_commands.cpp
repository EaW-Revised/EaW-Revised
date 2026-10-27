#include "battle_input.hpp"
#include "settings_store.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;

namespace {
[[nodiscard]] ui::Modifiers modifiers_of(const InputEventWithModifiers& event) {
    return ui::Modifiers{event.is_shift_pressed(), event.is_ctrl_pressed(), event.is_alt_pressed()};
}

} // namespace

bool BattleInput::input(const Ref<InputEvent>& event, LiveSessionView& live, const BattleScene& population,
                        BattleCamera& space) {
    scene_ = &population;
    if (event.is_null()) return false;
    // Capture before refreshing/picking; decoding an input may span simulation
    // frames. LAN reserves this epoch until its intent reaches the core.
    const auto input_epoch = live.input_epoch();
    if (input_epoch) live.note_native_input(input_epoch.epoch());
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        pointer_ = {static_cast<float>(motion->get_position().x), static_cast<float>(motion->get_position().y)};
        if (left_) {
            left_->end = {static_cast<float>(motion->get_position().x), static_cast<float>(motion->get_position().y)};
            left_->moved = true;
        }
        return false;  // the camera keeps its edge scroll and drags
    }
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        const auto index = button->get_button_index();
        if (placing_ && (index == MOUSE_BUTTON_WHEEL_UP || index == MOUSE_BUTTON_WHEEL_DOWN)) {
            // WR-X01: claim both press and release so the camera cannot zoom during the drag.
            if (button->is_pressed()) {
                static_cast<void>(live.rotate_reinforcement(button->get_factor()
                    * (index == MOUSE_BUTTON_WHEEL_UP ? 1.0 : -1.0)));
            }
            return true;
        }
        if (index != MOUSE_BUTTON_LEFT && index != MOUSE_BUTTON_RIGHT) return false;
        ++events_;
        refresh(live, population, space);
        const std::array<float, 2> at{static_cast<float>(button->get_position().x),
                                      static_cast<float>(button->get_position().y)};
        const ui::Modifiers modifiers = modifiers_of(*button);
        if (index == MOUSE_BUTTON_LEFT) {
            if (button->is_pressed()) {
                if (button->is_double_click() && !beacon_targeting_
                    && (!live.order_input() || live.order_input()->mode() != ui::OrderMode::waypoint)) {
                    // FoC: the double click selects the type on screen and its release is spent.
                    // #550 (WU-23a, WSU-38): an own squadron's icon takes it before the world under
                    // it and selects every own squadron with a craft of its leader's type on screen.
                    // WSU-18 (#531): a hardpoint reticle under the cursor stands for its ship; it is
                    // tested before the icon, as for a single click (WU-41).
                    const auto reticle = world_ui_->reticle_at(at);
                    const auto icon = reticle ? std::nullopt : world_ui_->icon_at(at);
                    if (icon) {
                        const auto leader = own_squadron_leader_type(*icon);
                        if (leader && selection_.craft_type_on_screen(*leader, units_, viewport_rect())) {
                            note("double click: craft type on screen");
                            acknowledge(Acknowledgement::Kind::select);
                        } else {
                            note(leader ? "double click: no new squadron on screen" : "double click: not an own squadron");
                        }
                    } else {
                        auto picked = ray(at[0], at[1]) ? ui::pick_unit(*ray(at[0], at[1]), units_) : std::nullopt;
                        if (reticle) picked = reticle->entity;
                        if (const auto pick_ray = ray(at[0], at[1])) record_pick_candidates(*pick_ray);
                        if (selection_.double_click(picked, units_, viewport_rect())) {
                            note("double click: type on screen");
                            acknowledge(Acknowledgement::Kind::select);
                        }
                    }
                    ignore_left_release_ = true;
                    left_.reset();
                } else {
                    left_ = Drag{at, at, false};
                }
            } else {
                left_release(at, modifiers, live);
            }
        } else if (button->is_pressed()) {
            right_start_ = at;
            right_double_click_ = button->is_double_click();
        } else {
            right_release(at, modifiers, live);
        }
        refresh_cards(live);
        draw();
        return true;
    }
    if (const auto* pressed_key = Object::cast_to<InputEventKey>(event.ptr())) {
        if (!pressed_key->is_pressed() || pressed_key->is_echo()) return false;
        const Key code = pressed_key->get_physical_keycode() != KEY_NONE ? pressed_key->get_physical_keycode()
                                                                          : pressed_key->get_keycode();
        std::int64_t command_code = static_cast<std::int64_t>(code);
        const auto modifiers = modifiers_of(*pressed_key);
        if (const auto* saved = ActiveSettings::get(); saved && !modifiers.ctrl && !modifiers.shift
            && !modifiers.alt && !pressed_key->is_meta_pressed() && code >= KEY_A && code <= KEY_Z) {
            const char letter = ui::camera_command_key(static_cast<char>('A' + (code - KEY_A)), saved->camera_wasd);
            if (letter == '\0') return false; // The bound camera position owns this bare chord.
            command_code = static_cast<std::int64_t>(KEY_A) + letter - 'A';
        }
        refresh(live, population, space);
        const bool taken = key(command_code, modifiers, live, space);
        if (taken) {
            refresh_cards(live);
            ++events_;
            draw();
        }
        return taken;
    }
    return false;
}

void BattleInput::left_release(const std::array<float, 2> at, const ui::Modifiers modifiers, LiveSessionView& live) {
    if (beacon_targeting_) {
        left_.reset();
        const auto pick = ray(at[0], at[1]);
        const auto point = pick ? ground_point(*pick) : std::nullopt;
        if (!point) return;
        const auto x = scene::fixed_from_binary32((*point)[0]);
        const auto y = scene::fixed_from_binary32((*point)[1]);
        if (!x || !y) return;
        beacon_targeting_ = false;
        if (live.place_beacon({x.value(), y.value(), {}})) {
            ++orders_;
            note("beacon placement tick " + std::to_string(live.order_tick()));
        } else { ++refused_; negative_feedback(); }
        return;
    }
    if (ignore_left_release_) {
        ignore_left_release_ = false;
        left_.reset();
        return;
    }
    // HCB-57: waypoint placement owns a left release before selection gestures.
    if (auto* input = live.order_input(); input && input->mode() == ui::OrderMode::waypoint && !placing_ && !ability_target_) {
        left_.reset();
        const auto pick_ray = ray(at[0], at[1]);
        const auto point = pick_ray ? ground_point(*pick_ray) : std::nullopt;
        if (!point) return;
        const auto x = scene::fixed_from_binary32((*point)[0]);
        const auto y = scene::fixed_from_binary32((*point)[1]);
        if (!x || !y) return;
        const sim::math::Vec3 destination{x.value(), y.value(), {}};
        input->set_selection(controlled_selection(live));
        const auto issued = input->world_command(ui::WorldPick{destination}, ui::CommandOrigin::world_click,
            ui::OrderModifiers{modifiers.ctrl, modifiers.alt, false});
        if (issued && issued.value()) {
            ++orders_;
            note("waypoint placement tick " + std::to_string(live.order_tick()));
            if (move_feedback_) move_feedback_(destination, ui::OrderMode::move, false, live.order_tick());
            acknowledge(Acknowledgement::Kind::move, sim::invalid_entity_id, sim::tactical::attack_hull, destination);
        } else if (!issued) {
            ++refused_;
            note("refused: " + issued.error().message);
        }
        return;
    }
    // WR-15: the original press belonged to the pool's GUI, so a drag drop needs no world press.
    if (placing_) {
        const auto type = *placing_;
        placing_.reset();
        left_.reset();
        const auto placing_ray = ray(at[0], at[1]);
        const auto point = placing_ray ? ground_point(*placing_ray) : std::nullopt;
        if (!point) {
            live.reinforcement_feedback(LiveSessionView::ReinforcementFeedback::Kind::cancelled, type);
            note("reinforce " + std::to_string(type) + ": no plane point");
            return;
        }
        auto x = scene::fixed_from_binary32((*point)[0]);
        auto y = scene::fixed_from_binary32((*point)[1]);
        if (!x || !y) {
            live.reinforcement_feedback(LiveSessionView::ReinforcementFeedback::Kind::cancelled, type);
            note("reinforce " + std::to_string(type) + ": no plane point");
            return;
        }
        const bool issued = live.reinforce(type, sim::math::Vec3{x.value(), y.value(), sim::math::Fixed{}});
        if (issued) ++placements_;
        else live.reinforcement_feedback(LiveSessionView::ReinforcementFeedback::Kind::cancelled, type);
        char where[96];
        std::snprintf(where, sizeof(where), "%.9g,%.9g,0", static_cast<double>((*point)[0]), static_cast<double>((*point)[1]));
        note("reinforce " + std::to_string(type) + (issued ? " @" : " refused @") + std::string(where));
        return;
    }
    if (!left_) return;
    const Drag drag = *left_;
    left_.reset();
    if (drag.moved && ui::drag_extent(drag.start, at) > ui::minimum_drag_select_distance) {
        ++boxes_;
        if (!modifiers.shift) inspect_.reset(); // #4919: a box drag leaves the inspect card
        auto icons = world_ui_->box_icons();
        if (live.controller_policy_enabled()) std::erase_if(icons, [&](const auto& icon) { return !live.can_control(icon.entity); });
        if (selection_.box(ui::drag_rect(drag.start, at), modifiers.shift, units_, icons)) {
            note("box select");
            acknowledge(Acknowledgement::Kind::select);
        }
        return;
    }
    const auto pick_ray = ray(at[0], at[1]);
    const auto picked_index = pick_ray ? ui::pick_index(*pick_ray, units_) : std::nullopt;
    auto picked = picked_index ? std::optional(special_target_source_
        ? ui::special_weapon_pick_target(units_[*picked_index].entity, units_[*picked_index].part)
        : units_[*picked_index].entity) : std::nullopt;
    // WU-23: a squadron icon takes the click before the world under it; WU-41: a hardpoint reticle
    // replaces the picked object and is tested before the icon.
    if (const auto reticle = world_ui_->reticle_at(at)) {
        picked = reticle->entity;
    } else if (const auto icon = world_ui_->icon_at(at)) {
        // MC-11: the icon is not a special-weapon target, even over a valid world pick.
        picked = special_target_source_
            ? ui::special_weapon_pick_target(*icon, sim::invalid_entity_id, true) : *icon;
    }
    if (special_target_source_) {
        const auto target = picked.value_or(sim::invalid_entity_id);
        if (!special_target_valid(target, live)) {
            ++special_invalid_;
            negative_feedback();
            note("special weapon: invalid target");
            return;
        }
        const auto source = *special_target_source_;
        if (live.special_fire(source, target)) {
            const auto* instance = live.snapshot_index().instance(source);
            if (instance && !instance->planetary_weapon) {
                Acknowledgement ack;
                ack.cue = audio::CommandCue::attack;
                acknowledgements_.push_back(std::move(ack));
            }
            special_target_source_.reset();
            ++special_fires_;
            ++orders_;
            note("special fire " + std::to_string(source) + " target " + std::to_string(target));
        } else negative_feedback();
        return;
    }
    if (ability_target_) {
        const auto kind = sim::tactical::ability_kind(ui::ability_name(ability_target_->ability));
        // AB-09 / WAD-38: area abilities accept a world point, including empty space.
        if (kind == sim::tactical::AbilityKind::weaken_enemy || kind == sim::tactical::AbilityKind::fow_reveal_ping || kind == sim::tactical::AbilityKind::barrage
            || kind == sim::tactical::AbilityKind::buzz_droids || kind == sim::tactical::AbilityKind::corrupt_systems) {
            const auto point = ability_point(at, live);
            if (!point) {
                negative_feedback();
                cancel_ability_target("invalid ability point");
                return;
            }
            auto request = *ability_target_;
            request.position = *point;
            ability_target_.reset();
            ++ability_targeted_;
            note("ability " + std::string(ui::ability_name(request.ability)) + " at point");
            if (ability_commands_) ability_commands_->request(request);
            acknowledge(Acknowledgement::Kind::attack);
            return;
        }
        // #561 (AB-11): the click aims the waiting targeted ability; the selection stays.
        const auto unit = std::find_if(units_.begin(), units_.end(),
            [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
        if (unit == units_.end() || !unit->hostile) {
            // BA-28: invalid targeted ability actions request refusal feedback; cancellation keys do not.
            negative_feedback();
            cancel_ability_target(unit == units_.end() ? "empty space" : "not an enemy");
            return;
        }
        auto request = *ability_target_;
        ability_target_.reset();
        // A squadron is aimed at through the craft clicked (space-abilities AB-62).
        request.target = unit->part != sim::invalid_entity_id ? unit->part : unit->entity;
        ++ability_targeted_;
        note("ability " + std::string(ui::ability_name(request.ability)) + " at " + std::to_string(request.target));
        if (ability_commands_ != nullptr) ability_commands_->request(request);
        acknowledge(Acknowledgement::Kind::attack);
        return;
    }
    ui::OrderInput* input = live.order_input();
    const bool armed = input != nullptr && input->mode() != ui::OrderMode::none;
    if (armed) {
        // An armed attack or move mode: a click on empty space or an own unit disarms it.
        const auto unit = std::find_if(units_.begin(), units_.end(),
            [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
        if (!picked || (unit != units_.end() && unit->own)) input->cancel_mode();
        if (!picked) return;
    }
    if (!modifiers.ctrl && !modifiers.alt && !armed) {
        if (const auto reticle = world_ui_->reticle_at(at);
            reticle && live.repair_hardpoint(reticle->entity, reticle->hardpoint)) {
            ++orders_;
            world_ui_->flash_reticle(reticle->entity, reticle->hardpoint);
            note("repair " + std::to_string(reticle->entity) + " hardpoint " + std::to_string(reticle->hardpoint));
            return;
        }
    }
    if (picked && live.pad_action_allowed(*picked)) {
        pad_palette_.open(*picked, true);
        const std::array<sim::EntityId, 1> selected{*picked};
        selection_.replace(selected);
        cards_snapshot_.reset();
        refresh_cards(live);
        note("pad palette " + std::to_string(*picked));
        return;
    }
    pad_palette_.close();
    cards_snapshot_.reset();
    // #4919 (deliberate deviation): a plain click on an allied structure inspects it. The inspect target
    // stays out of selection_, so no order, build menu or ability can reach it; nothing goes to the sim.
    if (const ui::BattleUnit* inspected = ui::inspect_click(units_, picked, armed, modifiers)) {
        selection_.clear();
        inspect_ = inspected->entity;
        note("inspect allied " + std::to_string(*inspect_));
        return;
    }
    inspect_.reset();
    if (selection_.click(picked, modifiers, units_, viewport_rect())) {
        note(picked ? "click select " + std::to_string(*picked) : std::string("click: deselect"));
        acknowledge(Acknowledgement::Kind::select);
    }
}

void BattleInput::right_release(const std::array<float, 2> at, const ui::Modifiers modifiers, LiveSessionView& live) {
    const auto start = right_start_;
    right_start_.reset();
    if (beacon_targeting_) { beacon_targeting_ = false; return; }
    if (special_target_source_) { cancel_special_target("right click"); return; }
    if (placing_) {
        // #530 PU-68: a right click cancels the placement.
        cancel_placement(live);
        return;
    }
    // #561 (AB-11): a right click cancels a waiting targeted ability and orders nothing.
    if (ability_target_) {
        cancel_ability_target("right click");
        return;
    }
    switch (ui::right_release(start, at)) {
    case ui::RightRelease::ignored:
        return;  // the press was cancelled (focus loss, pointer exit) or never seen
    case ui::RightRelease::compass:
        // FoC's right drag sets a facing (the compass); the rules have no move-with-facing command.
        note("right drag: compass facing not modelled");
        return;
    case ui::RightRelease::click:
        break;
    }
    ui::OrderInput* input = live.order_input();
    if (input != nullptr && input->mode() == ui::OrderMode::none && !modifiers.ctrl && !modifiers.alt) {
        if (const auto reticle = world_ui_->reticle_at(at);
            reticle && live.repair_hardpoint(reticle->entity, reticle->hardpoint)) {
            ++orders_;
            world_ui_->flash_reticle(reticle->entity, reticle->hardpoint);
            note("repair " + std::to_string(reticle->entity) + " hardpoint " + std::to_string(reticle->hardpoint));
            return;
        }
    }
    if (input == nullptr || selection_.empty()) return;
    const auto pick_ray = ray(at[0], at[1]);
    if (!pick_ray) return;
    auto picked = ui::pick_unit(*pick_ray, units_);
    // WU-41 (OR-20): a hovered hardpoint reticle under the pointer replaces the picked object: the
    // order's target is the reticle's unit and it names the hardpoint. #553 (WU-23b): otherwise a
    // squadron icon takes the right click before the world under it, as it takes the left one; the
    // order point stays the battle plane point under the cursor (P-3).
    const auto reticle = world_ui_->reticle_at(at);
    if (reticle) {
        picked = reticle->entity;
    } else if (const auto icon = world_ui_->icon_at(at)) {
        picked = *icon;
    }
    const auto unit = std::find_if(units_.begin(), units_.end(),
        [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
    const ui::BattleUnit* over = unit == units_.end() ? nullptr : &*unit;
    const ui::OrderMode mode = input->mode();
    const bool over_selected = over != nullptr && selection_.contains(over->entity);
    // OR-01: Ctrl attack-moves and Ctrl with Alt guards, even over a unit a plain click ignores.
    const bool guarding = mode == ui::OrderMode::guard || (mode == ui::OrderMode::none && modifiers.ctrl && modifiers.alt);
    // WBP-30: the guard override on a selected sale-capable child requests sale.
    if (guarding && over_selected && live.pad_sale_allowed(over->entity)) {
        if (live.sell_pad_structure(over->entity)) {
            ++orders_;
            input->cancel_mode();
            note("sell " + std::to_string(over->entity) + " tick " + std::to_string(live.order_tick()));
        }
        return;
    }
    const bool attack_moving = mode == ui::OrderMode::attack_move || (mode == ui::OrderMode::none && modifiers.ctrl && !modifiers.alt);
    // HCB-57: only Waypoint needs an Alt override; OR-01 ordinary point clicks already order.
    const bool queueing = modifiers.alt && mode == ui::OrderMode::waypoint;
    const auto click = guarding || attack_moving || queueing ? ui::RightClick::order : ui::right_click(mode, over, over_selected);
    switch (click) {
    case ui::RightClick::nothing:
        return;
    case ui::RightClick::disarm:
        input->cancel_mode();
        note(mode == ui::OrderMode::waypoint ? "waypoint mode cancelled" : "attack mode cancelled");
        return;
    case ui::RightClick::order:
        break;
    }
    const auto point = ground_point(*pick_ray);
    if (!point) return;
    auto x = scene::fixed_from_binary32((*point)[0]);
    auto y = scene::fixed_from_binary32((*point)[1]);
    if (!x || !y) return;
    ui::WorldPick pick{{x.value(), y.value(), sim::math::Fixed{}}, over ? over->entity : sim::invalid_entity_id,
                       over != nullptr && over->hostile, over != nullptr && over->own, over_selected};
    if (reticle && over != nullptr && over->hostile) pick.hardpoint = reticle->hardpoint;
    input->set_selection(controlled_selection(live));
    const std::uint64_t tick = live.order_tick();
    auto issued = input->world_command(pick, ui::CommandOrigin::world_click, ui::OrderModifiers{modifiers.ctrl, modifiers.alt, right_double_click_});
    if (issued && issued.value()) {
        ++orders_;
        std::string units;
        for (const sim::EntityId entity : selection_.units()) units += (units.empty() ? "" : ",") + std::to_string(entity);
        char where[96];
        std::snprintf(where, sizeof(where), "%.9g,%.9g,0", static_cast<double>((*point)[0]), static_cast<double>((*point)[1]));
        // Move mode moves even onto an enemy (O-3).
        const bool attacked = pick.hostile && mode != ui::OrderMode::move && mode != ui::OrderMode::waypoint;
        std::string what = "move @" + std::string(where);
        if (attacked) {
            what = "attack " + std::to_string(pick.entity);
            if (pick.hardpoint != sim::tactical::attack_hull) {
                what += " hardpoint " + std::to_string(pick.hardpoint);
                world_ui_->flash_reticle(pick.entity, pick.hardpoint);  // WU-42
                ++hardpoint_orders_;
            }
        } else if (guarding) {
            what = pick.entity != sim::invalid_entity_id && pick.own && !over_selected
                ? "guard " + std::to_string(pick.entity) : "guard @" + std::string(where);
        } else if (attack_moving) {
            what = "attack-move @" + std::string(where);
        }
        note(what + " units " + units + " tick " + std::to_string(tick));
        if (!attacked && move_feedback_ && local_order_selection(live)) {
            move_feedback_(pick.point, guarding ? ui::OrderMode::guard
                : attack_moving ? ui::OrderMode::attack_move : ui::OrderMode::move, right_double_click_, tick);
        }
        // BA-25/26 (SND-20/23): retain the aimed hardpoint; guard has its own response.
        acknowledge(attacked ? Acknowledgement::Kind::attack
                             : guarding ? Acknowledgement::Kind::guard : Acknowledgement::Kind::move,
                    pick.entity, pick.hardpoint, pick.point);
    } else if (!issued) {
        ++refused_;
        note("refused: " + issued.error().message);
    }
}

bool BattleInput::minimap_move(const double x, const double y, const bool double_click, LiveSessionView& live) {
    ui::OrderInput* input = live.order_input();
    if (input == nullptr || selection_.empty()) return false;
    auto fixed_x = scene::fixed_from_binary32(static_cast<float>(x));
    auto fixed_y = scene::fixed_from_binary32(static_cast<float>(y));
    if (!fixed_x || !fixed_y) return false;
    const ui::WorldPick pick{{fixed_x.value(), fixed_y.value(), sim::math::Fixed{}}, sim::invalid_entity_id, false};
    input->set_selection(controlled_selection(live));
    const std::uint64_t tick = live.order_tick();
    const ui::OrderMode mode = input->mode();
    auto issued = input->world_command(pick, ui::CommandOrigin::minimap);
    if (!issued) {
        ++refused_;
        note("minimap refused: " + issued.error().message);
        return false;
    }
    if (!issued.value()) return false;
    ++orders_;
    char where[96];
    std::snprintf(where, sizeof(where), "%.9g,%.9g,0", x, y);
    note("minimap move @" + std::string(where) + " tick " + std::to_string(tick));
    acknowledge(Acknowledgement::Kind::move, sim::invalid_entity_id, sim::tactical::attack_hull, pick.point);
    if (move_feedback_ && local_order_selection(live)) move_feedback_(pick.point, mode, double_click, tick);
    return true;
}

std::vector<sim::EntityId> BattleInput::controlled_selection(const LiveSessionView& live) const {
    auto selected = selection_.units();
    if (live.controller_policy_enabled()) std::erase_if(selected, [&](const auto id) { return !live.can_control(id); });
    return selected;
}

bool BattleInput::local_order_selection(const LiveSessionView& live) const {
    return std::any_of(selection_.units().begin(), selection_.units().end(), [&](const auto entity) {
        const auto* instance = live.snapshot_index().instance(entity);
        return instance != nullptr && instance->owner == live.local_player() && live.can_control(*instance);
    });
}

bool BattleInput::key(const std::int64_t code, const ui::Modifiers modifiers, LiveSessionView& live,
                      BattleCamera& space) {
    ui::OrderInput* input = live.order_input();
    // MP-06: requests enter the shared future-tick options stream, never local pacing.
    // The keyboard binding is a remake control; MP-U1 allows either human to request.
    if (live.multiplayer() && !modifiers.ctrl && !modifiers.alt && !modifiers.shift
        && (code == static_cast<std::int64_t>(KEY_EQUAL) || code == static_cast<std::int64_t>(KEY_MINUS))) {
        const auto step = live.lan_view().options.tactical_speed;
        const auto requested = code == static_cast<std::int64_t>(KEY_EQUAL)
            ? std::min<std::uint32_t>(step + 1, static_cast<std::uint32_t>(ui::speed_step_rates.size() - 1))
            : step == 0 ? 0 : step - 1;
        if (requested != step) static_cast<void>(live.request_speed(requested));
        return true;
    }
    if (inspect_ && code == static_cast<std::int64_t>(KEY_ESCAPE)) { // #4919: Esc drops the inspect card
        inspect_.reset();
        return true;
    }
    if (beacon_targeting_ && code == static_cast<std::int64_t>(KEY_ESCAPE)) {
        beacon_targeting_ = false;
        return true;
    }
    if (special_target_source_ && code == static_cast<std::int64_t>(KEY_ESCAPE)) {
        cancel_special_target("escape");
        return true;
    }
    // #561 (AB-11): Esc cancels a waiting targeted ability.
    if (ability_target_ && code == static_cast<std::int64_t>(KEY_ESCAPE)) {
        cancel_ability_target("escape");
        return true;
    }
    if (code == static_cast<std::int64_t>(KEY_A) && modifiers.ctrl && !modifiers.shift && !modifiers.alt) {
        // S-5a: use the selection path, without issuing an order or focusing the camera.
        auto own_units = units_;
        for (auto& unit : own_units) {
            // S-5a: allied community property is click-selectable, but select-all is owner-only.
            const auto* instance = live.snapshot_index().instance(unit.entity);
            unit.own = instance != nullptr && instance->owner == live.local_player() && live.can_control(*instance);
        }
        inspect_.reset();
        selection_.all(own_units);
        note("select all");
        acknowledge(Acknowledgement::Kind::select);
        return true;
    }
    if (code >= static_cast<std::int64_t>(KEY_0) && code <= static_cast<std::int64_t>(KEY_9)) {
        // G-1 (foc-battle-selection): n selects group n, Ctrl assigns, Shift adds the
        // group to the selection, Alt adds the selection to the group.
        const auto group = static_cast<std::size_t>(code - static_cast<std::int64_t>(KEY_0));
        std::optional<ui::Vec3f> focus;
        if (modifiers.ctrl) {
            selection_.assign_group(group);
            note("assign group " + std::to_string(group));
        } else if (modifiers.alt) {
            focus = selection_.add_to_group(group, logical_frame(live), units_);
            note("add to group " + std::to_string(group));
            acknowledge(Acknowledgement::Kind::select);
        } else {
            focus = selection_.recall_group(group, modifiers.shift, logical_frame(live), units_);
            note((modifiers.shift ? "add group " : "select group ") + std::to_string(group));
            acknowledge(Acknowledgement::Kind::select);
        }
        if (focus) {
            ++focuses_;
            space.live_camera_focus((*focus)[0], (*focus)[1]);
            note("focus group " + std::to_string(group));
        }
        return true;
    }
    // #454 AB-10: FoC's default ability keys press the selection's buttons of that ability; a key no
    // shown button takes goes on to the other bindings.
    const auto key_char = [&]() -> std::optional<char> {
        if (code >= static_cast<std::int64_t>(KEY_A) && code <= static_cast<std::int64_t>(KEY_Z)) {
            return static_cast<char>('A' + (code - static_cast<std::int64_t>(KEY_A)));
        }
        switch (code) {
        case KEY_BRACKETLEFT: return '[';
        case KEY_BRACKETRIGHT: return ']';
        case KEY_SEMICOLON: return ';';
        case KEY_COMMA: return ',';
        case KEY_PERIOD: return '.';
        case KEY_SLASH: return '/';
        default: return std::nullopt;
        }
    };
    if (const auto letter = key_char()) {
        if (const auto ability = ui::ability_hotkey(*letter, modifiers)) {
            const bool shown = std::any_of(ability_bar_.buttons.begin(), ability_bar_.buttons.end(),
                [&](const ui::AbilityButton& button) { return button.ability == *ability; });
            if (shown) {
                ++ability_hotkeys_;
                note("ability key " + std::string(ui::ability_name(*ability)));
                static_cast<void>(press_ability(*ability, live));
                return true;
            }
        }
    }
    if (modifiers.ctrl || modifiers.alt || modifiers.shift) return false;
    if (code == static_cast<std::int64_t>(KEY_S)) {
        command_click(ui::OrderMode::none, true, live, ui::CommandOrigin::hotkey);
        return true;
    }
    // OR-01: the default keys are A attack, M move, T attack-move and G guard; the key
    // of the armed mode disarms it, another key replaces it (OR-01).
    if (code == static_cast<std::int64_t>(KEY_A) || code == static_cast<std::int64_t>(KEY_M)
        || code == static_cast<std::int64_t>(KEY_T) || code == static_cast<std::int64_t>(KEY_G)
        || code == static_cast<std::int64_t>(KEY_W)) {
        if (input == nullptr) return true;
        ui::OrderMode mode = ui::OrderMode::move;
        const char* name = "move mode";
        if (code == static_cast<std::int64_t>(KEY_A)) {
            mode = ui::OrderMode::attack;
            name = "attack mode";
        } else if (code == static_cast<std::int64_t>(KEY_T)) {
            mode = ui::OrderMode::attack_move;
            name = "attack-move mode";
        } else if (code == static_cast<std::int64_t>(KEY_G)) {
            mode = ui::OrderMode::guard;
            name = "guard mode";
        } else if (code == static_cast<std::int64_t>(KEY_W)) {
            mode = ui::OrderMode::waypoint;
            name = "waypoint mode";
        }
        command_click(mode, false, live, ui::CommandOrigin::hotkey);
        note(name);
        return true;
    }
    if (code == static_cast<std::int64_t>(KEY_INSERT)) {
        space.live_camera_overview_key();
        note("overview " + space.live_camera_overview());
        return true;
    }
    return false;
}

void BattleInput::acknowledge(const Acknowledgement::Kind kind, const sim::EntityId target,
                              const std::uint32_t hardpoint, const std::optional<sim::math::Vec3> destination) {
    // BA-20: a unit response needs a nonempty selection with a ranked speaker.
    if (selection_.empty()) return;
    Acknowledgement acknowledgement;
    acknowledgement.kind = kind;
    acknowledgement.units = selection_.units();
    acknowledgement.target = target;
    acknowledgement.hardpoint = hardpoint;
    acknowledgement.destination = destination;
    acknowledgements_.push_back(std::move(acknowledgement));
}

bool BattleInput::command_click(const ui::OrderMode mode, const bool stop, LiveSessionView& live,
                                const ui::CommandOrigin origin) {
    auto* input = live.order_input();
    if (!input) return false;
    beacon_targeting_ = false;
    // OR-01: an ordinary command owns the next gesture, including Stop.
    cancel_special_target("ordinary order");
    cancel_ability_target("ordinary order");
    if (placing_) cancel_placement(live);
    audio::CommandCue cue = audio::CommandCue::none;
    if (stop) {
        input->set_selection(controlled_selection(live));
        if (!selection_.empty()) {
            if (auto stopped = input->stop(origin); stopped) {
                ++orders_;
                note("stop");
                acknowledge(Acknowledgement::Kind::stop);
            } else ++refused_;
        }
        cue = audio::CommandCue::stop;
    } else {
        if (input->mode() == mode) input->cancel_mode();
        else input->arm(mode);
        switch (mode) {
        case ui::OrderMode::attack: cue = audio::CommandCue::attack; break;
        case ui::OrderMode::attack_move: cue = audio::CommandCue::attack_move; break;
        case ui::OrderMode::guard: cue = audio::CommandCue::guard; break;
        case ui::OrderMode::move: cue = audio::CommandCue::move; break;
        case ui::OrderMode::waypoint: cue = audio::CommandCue::waypoint; break;
        case ui::OrderMode::none: break;
        }
    }
    if (audio::command_cue_on_press(cue, input->mode() == mode)) {
        Acknowledgement feedback;
        feedback.cue = cue;
        acknowledgements_.push_back(std::move(feedback));
    }
    return true;
}

void BattleInput::negative_feedback() {
    Acknowledgement feedback;
    feedback.cue = audio::CommandCue::negative;
    acknowledgements_.push_back(std::move(feedback));
}

std::vector<BattleInput::Acknowledgement> BattleInput::take_acknowledgements() {
    return std::exchange(acknowledgements_, {});
}

} // namespace eawr::presentation::godot_backend
