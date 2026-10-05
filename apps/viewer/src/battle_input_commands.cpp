#include "battle_input.hpp"

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

bool BattleInput::input(const Ref<InputEvent>& event, LiveSessionView& live, const SpacePopulation& population,
                        SpaceEnvironment& space) {
    if (event.is_null()) return false;
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
        if (index != MOUSE_BUTTON_LEFT && index != MOUSE_BUTTON_RIGHT) return false;
        ++events_;
        refresh(live, population, space);
        const std::array<float, 2> at{static_cast<float>(button->get_position().x),
                                      static_cast<float>(button->get_position().y)};
        const ui::Modifiers modifiers = modifiers_of(*button);
        if (index == MOUSE_BUTTON_LEFT) {
            if (button->is_pressed()) {
                if (button->is_double_click()) {
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
        refresh(live, population, space);
        const bool taken = key(static_cast<std::int64_t>(code), modifiers_of(*pressed_key), live, space);
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
    if (ignore_left_release_) {
        ignore_left_release_ = false;
        left_.reset();
        return;
    }
    // WR-15: the original press belonged to the pool's GUI, so a drag drop needs no world press.
    if (placing_) {
        const auto type = *placing_;
        placing_.reset();
        left_.reset();
        const auto placing_ray = ray(at[0], at[1]);
        const auto point = placing_ray ? ui::battle_plane_point(*placing_ray) : std::nullopt;
        if (!point) {
            note("reinforce " + std::to_string(type) + ": no plane point");
            return;
        }
        auto x = scene::fixed_from_binary32((*point)[0]);
        auto y = scene::fixed_from_binary32((*point)[1]);
        if (!x || !y) {
            note("reinforce " + std::to_string(type) + ": no plane point");
            return;
        }
        const bool issued = live.reinforce(type, sim::math::Vec3{x.value(), y.value(), sim::math::Fixed{}});
        if (issued) ++placements_;
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
        if (selection_.box(ui::drag_rect(drag.start, at), modifiers.shift, units_, world_ui_->box_icons())) {
            note("box select");
            acknowledge(Acknowledgement::Kind::select);
        }
        return;
    }
    const auto pick_ray = ray(at[0], at[1]);
    auto picked = pick_ray ? ui::pick_unit(*pick_ray, units_) : std::nullopt;
    // WU-23: a squadron icon takes the click before the world under it; WU-41: a hardpoint reticle
    // replaces the picked object and is tested before the icon.
    if (const auto reticle = world_ui_->reticle_at(at)) {
        picked = reticle->entity;
    } else if (const auto icon = world_ui_->icon_at(at)) {
        picked = *icon;
    }
    if (ability_target_) {
        if (sim::tactical::ability_kind(ui::ability_name(ability_target_->ability)) == sim::tactical::AbilityKind::weaken_enemy) {
            const auto aim_ray = ray(at[0], at[1]);
            const auto point = aim_ray ? ui::battle_plane_point(*aim_ray) : std::nullopt;
            if (!point) { cancel_ability_target("no plane point"); return; }
            auto x = scene::fixed_from_binary32((*point)[0]);
            auto y = scene::fixed_from_binary32((*point)[1]);
            if (!x || !y) { cancel_ability_target("no plane point"); return; }
            auto request = *ability_target_;
            request.position = sim::math::Vec3{x.value(), y.value(), sim::math::Fixed{}};
            ability_target_.reset();
            ++ability_targeted_;
            note("ability WEAKEN_ENEMY at point");
            if (ability_commands_) ability_commands_->request(request);
            acknowledge(Acknowledgement::Kind::attack);
            return;
        }
        // #561 (AB-11): the click aims the waiting targeted ability; the selection stays.
        const auto unit = std::find_if(units_.begin(), units_.end(),
            [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
        if (unit == units_.end() || !unit->hostile) {
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
    if (input != nullptr && input->mode() != ui::OrderMode::none) {
        // An armed attack or move mode: a click on empty space or an own unit disarms it.
        const auto unit = std::find_if(units_.begin(), units_.end(),
            [&](const ui::BattleUnit& candidate) { return picked && candidate.entity == *picked; });
        if (!picked || (unit != units_.end() && unit->own)) input->cancel_mode();
        if (!picked) return;
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
    if (selection_.click(picked, modifiers, units_, viewport_rect())) {
        note(picked ? "click select " + std::to_string(*picked) : std::string("click: deselect"));
        acknowledge(Acknowledgement::Kind::select);
    }
}

void BattleInput::right_release(const std::array<float, 2> at, const ui::Modifiers modifiers, LiveSessionView& live) {
    const auto start = right_start_;
    right_start_.reset();
    if (placing_) {
        // #530 PU-68: a right click cancels the placement.
        placing_.reset();
        ++placements_cancelled_;
        note("reinforce placement cancelled");
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
    const auto click = guarding || attack_moving ? ui::RightClick::order : ui::right_click(mode, over, over_selected);
    switch (click) {
    case ui::RightClick::nothing:
        return;
    case ui::RightClick::disarm:
        input->cancel_mode();
        note("attack mode cancelled");
        return;
    case ui::RightClick::order:
        break;
    }
    const auto point = ui::battle_plane_point(*pick_ray);
    if (!point) return;
    auto x = scene::fixed_from_binary32((*point)[0]);
    auto y = scene::fixed_from_binary32((*point)[1]);
    if (!x || !y) return;
    ui::WorldPick pick{{x.value(), y.value(), sim::math::Fixed{}}, over ? over->entity : sim::invalid_entity_id,
                       over != nullptr && over->hostile, over != nullptr && over->own, over_selected};
    if (reticle && over != nullptr && over->hostile) pick.hardpoint = reticle->hardpoint;
    input->set_selection(selection_.units());
    const std::uint64_t tick = live.order_tick();
    auto issued = input->world_command(pick, ui::CommandOrigin::world_click, ui::OrderModifiers{modifiers.ctrl, modifiers.alt, right_double_click_});
    if (issued && issued.value()) {
        ++orders_;
        std::string units;
        for (const sim::EntityId entity : selection_.units()) units += (units.empty() ? "" : ",") + std::to_string(entity);
        char where[96];
        std::snprintf(where, sizeof(where), "%.9g,%.9g,0", static_cast<double>((*point)[0]), static_cast<double>((*point)[1]));
        // Move mode moves even onto an enemy (O-3).
        const bool attacked = pick.hostile && mode != ui::OrderMode::move;
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
        // BA-25/26 (SND-20/23): retain the aimed hardpoint; guard has its own response.
        acknowledge(attacked ? Acknowledgement::Kind::attack
                             : guarding ? Acknowledgement::Kind::guard : Acknowledgement::Kind::move,
                    pick.entity, pick.hardpoint);
    } else if (!issued) {
        ++refused_;
        note("refused: " + issued.error().message);
    }
}

bool BattleInput::minimap_move(const double x, const double y, LiveSessionView& live) {
    ui::OrderInput* input = live.order_input();
    if (input == nullptr || selection_.empty()) return false;
    auto fixed_x = scene::fixed_from_binary32(static_cast<float>(x));
    auto fixed_y = scene::fixed_from_binary32(static_cast<float>(y));
    if (!fixed_x || !fixed_y) return false;
    const ui::WorldPick pick{{fixed_x.value(), fixed_y.value(), sim::math::Fixed{}}, sim::invalid_entity_id, false};
    input->set_selection(selection_.units());
    const std::uint64_t tick = live.order_tick();
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
    acknowledge(Acknowledgement::Kind::move);
    return true;
}

bool BattleInput::key(const std::int64_t code, const ui::Modifiers modifiers, LiveSessionView& live,
                      SpaceEnvironment& space) {
    ui::OrderInput* input = live.order_input();
    // #561 (AB-11): Esc cancels a waiting targeted ability.
    if (ability_target_ && code == static_cast<std::int64_t>(KEY_ESCAPE)) {
        cancel_ability_target("escape");
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
            focus = selection_.add_to_group(group, now(live), units_);
            note("add to group " + std::to_string(group));
            acknowledge(Acknowledgement::Kind::select);
        } else {
            focus = selection_.recall_group(group, modifiers.shift, now(live), units_);
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
        if (input == nullptr) return true;
        input->set_selection(selection_.units());
        if (selection_.empty()) return true;
        if (auto stopped = input->stop(ui::CommandOrigin::hotkey); stopped) {
            ++orders_;
            note("stop");
            acknowledge(Acknowledgement::Kind::stop);
        } else {
            ++refused_;
        }
        return true;
    }
    // OR-01: the default keys are A attack, M move, T attack-move and G guard; the key
    // of the armed mode disarms it, another key replaces it (OR-01).
    if (code == static_cast<std::int64_t>(KEY_A) || code == static_cast<std::int64_t>(KEY_M)
        || code == static_cast<std::int64_t>(KEY_T) || code == static_cast<std::int64_t>(KEY_G)) {
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
        }
        if (input->mode() == mode) input->cancel_mode();
        else input->arm(mode);
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
                              const std::uint32_t hardpoint) {
    // FoC speaks only for a selection that holds units (Set_Selected_Objects_List, the group
    // acknowledgements' highest ranking object).
    if (selection_.empty()) return;
    acknowledgements_.push_back({kind, selection_.units(), target, hardpoint});
}

std::vector<BattleInput::Acknowledgement> BattleInput::take_acknowledgements() {
    return std::exchange(acknowledgements_, {});
}

} // namespace eawr::presentation::godot_backend
