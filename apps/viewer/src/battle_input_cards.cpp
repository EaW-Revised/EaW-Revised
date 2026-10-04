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

std::optional<sim::tactical::TypeId> BattleInput::own_squadron_leader_type(const sim::EntityId squadron) const {
    const auto icons = world_ui_->icon_units();
    const auto icon = std::find_if(icons.begin(), icons.end(), [&](const ui::BattleUnit& unit) { return unit.entity == squadron; });
    if (icon == icons.end() || !icon->own) return std::nullopt;
    if (live_ != nullptr) {
        const auto latest = live_->battle_frame().latest;
        const auto members = live_->squadron_members().find(squadron);
        if (latest && members != live_->squadron_members().end()) {
            for (const sim::EntityId member : members->second) {
                if (const auto* craft = live_->snapshot_index().instance(member)) return craft->type_id;
            }
        }
    }
    return icon->type;
}

void BattleInput::refresh_cards(const LiveSessionView& live) {

    const auto& latest = live.battle_frame().latest;
    const units::UnitTables* tables = live.tables();
    if (cards_snapshot_ == latest && cards_selection_ == selection_.units() && cards_slots_ == card_slots_
        && card_tables_ == tables) {
        refresh_abilities(live);
        return;
    }
    cards_snapshot_ = latest;
    cards_selection_ = selection_.units();
    cards_slots_ = card_slots_;
    card_units_.clear();
    card_layout_ = {};
    production_station_.reset();
    // #534: the ability bar mirrors the cards; an empty layout (deselect, the selection's last unit
    // dying or leaving a group) must clear it too, or its last button lingers over an empty panel.
    if (card_slots_ == 0 || selection_.empty() || !latest || tables == nullptr) {
        build_buttons_.clear();
        pad_palette_.close();
        ability_bar_ = {};
        return;
    }
    if (card_tables_ != tables) {
        card_tables_ = tables;
        card_types_.clear();
        for (const units::UnitType& type : tables->units) card_types_.emplace(skirmish::type_id(type.id), &type);
    }
    const auto& instances = live.snapshot_index();
    if (const auto entity = pad_palette_.entity(); entity && !selection_.contains(*entity)) pad_palette_.close();
    if (const auto entity = pad_palette_.entity()) {
        const auto* pad = live.pad_view(*entity);
        const auto* instance = instances.instance(*entity);
        const auto faction = live.local_faction_id();
        const auto* menu = instance && faction ? live.economy().menu(instance->type_id, *faction) : nullptr;
        if (pad && instance && menu && live.economy().pads.point(instance->type_id)) {
            const auto* economy = live.local_economy();
            std::array<std::uint32_t, ui::pad_slot_count> rows{};
            for (std::size_t index = 0; index < std::min(rows.size(), menu->options.size()); ++index) {
                if (const auto type = card_types_.find(menu->options[index].type); type != card_types_.end()) rows[index] = type->second->gui_row;
            }
            ui::update_pad_buttons(build_buttons_, *menu, economy ? economy->credits : sim::math::Fixed{},
                latest->completed_tick(), pad->state.cooldown_until, card_slots_, rows, pad->state.cooldown_start);
            ability_bar_ = {};
            return;
        }
        pad_palette_.close();
    }
    // WPR-33: a selected allied station offers production to the local buyer.
    if (const auto faction = live.local_faction_id(); faction && !live.economy().empty()) {
        for (const sim::EntityId entity : selection_.units()) {
            const auto* instance = instances.instance(entity);
            if (instance == nullptr) continue;
            const auto* point = live.economy().pads.point(instance->type_id);
            if (point != nullptr && point->build_pad) continue; // WBP-08: explicit opening action above
            if (!live.is_ally_of_local(instance->owner)) continue;
            const sim::tactical::StationMenu* menu = live.economy().menu(instance->type_id, *faction);
            if (menu == nullptr || menu->options.empty()) continue;
            const sim::tactical::EconomyView* economy = live.local_economy();
            std::array<std::size_t, sim::tactical::build_queue_count> sizes{};
            if (economy != nullptr) {
                for (std::size_t queue = 0; queue < sizes.size(); ++queue) sizes[queue] = economy->queues[queue].size();
            }
            production_station_ = entity;
            ui::update_build_buttons(build_buttons_, *menu, economy != nullptr ? economy->credits : sim::math::Fixed{},
                sizes, live.economy().max_queue, card_slots_,
                [&](const auto& option) { return live.build_menu_state(option); });
            ability_bar_ = {};
            return;
        }
    }

    build_buttons_.clear();
    const auto type_of = [&](const sim::tactical::TacticalInstance& instance) -> const units::UnitType* {
        const auto found = card_types_.find(instance.type_id);
        return found == card_types_.end() ? nullptr : found->second;
    };
    const auto ability_of = [](const units::UnitType* type) {
        return type == nullptr || type->abilities.empty() ? ui::ability_none : ui::ability_index(type->abilities.front().type);
    };
    // #454: the type's second unit ability, which gets a second button.
    const auto second_ability_of = [](const units::UnitType* type) {
        return type == nullptr || type->abilities.size() < 2 ? ui::ability_none : ui::ability_index(type->abilities[1].type);
    };
    // Get_Health_Percent: the hull over its maximum (FoC's hardpoint-weighted display health is on
    // the fidelity list).
    const auto health_of = [](const sim::tactical::TacticalInstance& instance) {
        if (!instance.durability || instance.durability->max_hull.raw() <= 0) return 1.0;
        return std::clamp(static_cast<double>(instance.durability->hull.raw())
                              / static_cast<double>(instance.durability->max_hull.raw()), 0.0, 1.0);
    };
    // #435: the selection holds a squadron as its team container (#424). The card model reads a
    // squadron through its craft, so a container stands for its live craft here, and every card's
    // members map back to the container below: one card per homogeneous squadron, whose click
    // selects the squadron as one unit.
    std::vector<sim::EntityId> shown;
    for (const sim::EntityId entity : selection_.units()) {
        const auto squadron = live.squadron_members().find(entity);
        if (squadron == live.squadron_members().end()) {
            shown.push_back(entity);
            continue;
        }
        for (const sim::EntityId craft : squadron->second) {
            if (instances.instance(craft) != nullptr) shown.push_back(craft);
        }
    }
    std::vector<ui::SelectedUnit> selected;
    for (const sim::EntityId entity : shown) {
        const auto* instance = instances.instance(entity);
        if (instance == nullptr) continue;
        const units::UnitType* type = type_of(*instance);
        ui::SelectedUnit unit;
        unit.entity = entity;
        unit.type = type != nullptr ? type->id : std::to_string(instance->type_id);
        unit.ability = ability_of(type);
        unit.second_ability = second_ability_of(type);
        unit.health = health_of(*instance);
        const auto& durability = instance->durability;
        if (type != nullptr && type->shielded && durability && durability->shields && durability->max_shields
            && durability->max_shields->raw() > 0) {
            unit.shield = std::clamp(static_cast<double>(durability->shields->raw())
                                         / static_cast<double>(durability->max_shields->raw()), 0.0, 1.0);
        }
        if (const sim::tactical::Squadron* squadron = live.squadron_of(entity)) {
            const auto* container = instances.instance(squadron->container);
            const units::UnitType* squadron_type = container == nullptr ? nullptr : type_of(*container);
            if (squadron_type != nullptr) {
                ui::SquadronOf team;
                team.container = squadron->container;
                team.type = squadron_type->id;
                team.ability = ability_of(squadron_type);
                team.second_ability = second_ability_of(squadron_type);
                // WHE-63 / AB-01: the authored team container supplies the bar's
                // ability pair; the purchase squadron's default is not its display type.
                if (!squadron_type->team_abilities.empty()) {
                    team.ability = ui::ability_index(squadron_type->team_abilities.front().type);
                    team.second_ability = squadron_type->team_abilities.size() < 2 ? ui::ability_none
                        : ui::ability_index(squadron_type->team_abilities[1].type);
                }
                if (team.ability == ui::ability_none) {
                    team.ability = unit.ability;
                    team.second_ability = unit.second_ability;
                }
                team.homogeneous = squadron_type->homogeneous;
                // L-9 (foc-unit-cards): a squadron's health is the mean health of the craft still standing.
                double sum = 0.0;
                for (const sim::EntityId craft : squadron->members) {
                    const auto* live_craft = instances.instance(craft);
                    if (live_craft == nullptr) continue;
                    team.members.push_back(craft);
                    sum += health_of(*live_craft);
                }
                team.health = team.members.empty() ? 0.0 : sum / static_cast<double>(team.members.size());
                unit.squadron = std::move(team);
            }
        }
        selected.push_back(std::move(unit));
    }
    card_units_ = ui::card_units(selected);
    const auto& container_of = live.squadron_of();
    for (ui::CardUnit& unit : card_units_) {
        std::vector<sim::EntityId> members;
        for (const sim::EntityId member : unit.members) {
            const auto squadron = container_of.find(member);
            const sim::EntityId selectable = squadron != container_of.end() ? squadron->second : member;
            if (std::find(members.begin(), members.end(), selectable) == members.end()) members.push_back(selectable);
        }
        unit.members = std::move(members);
    }
    card_layout_ = ui::layout_unit_cards(card_units_, card_slots_);
    refresh_abilities(live);
}

void BattleInput::refresh_abilities(const LiveSessionView& live) {
    ready_abilities_.set_units(card_units_);
    ability_demo_ = live.options().ability_demo;
    std::vector<ui::ReadyAbilities::Staged> staged;
    if (live.options().ability_demo) {
        // --eawr-live-ability-demo: the first four ability groups in their demo states.
        const std::array<ui::UnitAbilityState, 4> demo{{{ui::AbilityStatus::active, 0.6, false},
                                                        {ui::AbilityStatus::recharging, 0.35, true},
                                                        {ui::AbilityStatus::disabled, 1.0, false},
                                                        {ui::AbilityStatus::ready, 1.0, true}}};
        std::size_t rank = 0;
        for (std::uint32_t ability = 1; ability < ui::ability_count && rank < demo.size(); ++ability) {
            if (card_layout_.by_ability[ability].empty()) continue;
            for (const std::size_t index : card_layout_.by_ability[ability]) {
                staged.push_back({card_units_[index].id, ability, demo[rank]});
            }
            ++rank;
        }
    }
    ready_abilities_.stage(std::move(staged));
    const ui::AbilityState& state = ability_state_ != nullptr ? *ability_state_ : ready_abilities_;
    ability_bar_ = card_slots_ == 0 ? ui::AbilityBar{} : ui::ability_bar(card_layout_, card_units_, state, nullptr);
    // RG-03: unsupported authored abilities retain their disabled button and a reason.
    for (auto& button : ability_bar_.buttons) {
        for (const auto& unit : card_units_) {
            if (std::find(button.units.begin(), button.units.end(), unit.id) == button.units.end()) continue;
            const auto reason = skirmish::roster_ability_reason(unit.type, ui::ability_name(button.ability));
            if (!reason.empty()) button.disabled_reason = reason;
        }
    }
}

void BattleInput::NoteCommands::request(const ui::AbilityRequest& request) {
    std::string units;
    for (const sim::EntityId unit : request.units) units += (units.empty() ? "" : ",") + std::to_string(unit);
    owner->note("ability " + std::string(ui::to_string(request.kind)) + " " + std::string(ui::ability_name(request.ability))
                + (request.targeted ? " (targeted)" : "") + " units " + units + ": no-op until the simulation has abilities (#76)");
}

bool BattleInput::ability_click(const std::size_t index, const bool right, const LiveSessionView& live) {
    refresh_cards(live);
    if (index >= ability_bar_.buttons.size()) return false;
    const ui::AbilityState& state = ability_state_ != nullptr ? *ability_state_ : ready_abilities_;
    const auto request = ui::ability_click(ability_bar_.buttons[index], right, state);
    if (!request) {
        note("ability " + std::string(ui::ability_name(ability_bar_.buttons[index].ability)) + ": nothing can take it");
        return false;
    }
    ++ability_requests_;
    note_commands_.owner = this;
    // #561 (AB-11): a targeted activation first takes its target from the next world click.
    if (request->targeted && request->kind == ui::AbilityRequest::Kind::activate && ability_commands_ != nullptr) {
        ability_target_ = *request;
        note("ability " + std::string(ui::ability_name(request->ability)) + ": pick a target");
        return true;
    }
    (ability_commands_ != nullptr ? *ability_commands_ : static_cast<ui::AbilityCommands&>(note_commands_)).request(*request);
    return true;
}

void BattleInput::cancel_ability_target(const char* why) {
    if (!ability_target_) return;
    ++ability_target_cancels_;
    note("ability " + std::string(ui::ability_name(ability_target_->ability)) + " targeting cancelled: " + why);
    ability_target_.reset();
}

bool BattleInput::press_ability(const std::uint32_t ability, const LiveSessionView& live) {
    // AB-10: the key presses every shown button of the ability.
    refresh_cards(live);
    bool pressed = false;
    for (std::size_t index = 0; index < ability_bar_.buttons.size(); ++index) {
        if (ability_bar_.buttons[index].ability != ability) continue;
        pressed = ability_click(index, false, live) || pressed;
    }
    return pressed;
}

bool BattleInput::card_click(const std::size_t slot, const bool shift, const LiveSessionView& live) {
    // A world gesture or another card click may have changed selection since the last frame.
    refresh_cards(live);
    const auto next = ui::card_click(card_layout_, card_units_, slot, shift, selection_.units());
    if (!next) return false;
    ++card_clicks_;
    const bool changed = selection_.replace(*next);
    refresh_cards(live);
    note((shift ? "card deselect " : "card select ") + std::to_string(slot) + ": " + std::to_string(next->size())
         + " selected");
    draw();
    return changed;
}

} // namespace eawr::presentation::godot_backend
