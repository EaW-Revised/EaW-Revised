#include "battle_input.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"

#include <algorithm>

namespace eawr::presentation::godot_backend {

ui::SpecialWeaponButton BattleInput::special_weapon_button(const LiveSessionView& live) const {
    ui::SpecialWeaponButton view;
    const auto& snapshot = live.battle_frame().latest;
    const auto* economy = live.local_economy();
    if (!snapshot || !live.combat()) return view;
    const bool running = live.phase() == LiveSessionView::Phase::running;
    view.permitted = economy && economy->special_weapons_enabled;
    // WSW-28: setup/permission removal clears the button binding and its readiness.
    if (!running || !view.permitted) return view;
    // WSW-28: own planetary identity, otherwise the first allied station's manual gun.
    for (const auto& instance : snapshot->instances()) {
        if (!instance.planetary_weapon || instance.owner != live.local_player()) continue;
        view.source = instance.entity_id;
        view.target_action = instance.planetary_weapon->target_action;
        view.readiness = instance.planetary_weapon->readiness;
        view.pulses_left = instance.planetary_weapon->state.pulses_left;
        break;
    }
    if (!view.source && ui::hud_faction_from(live.local_faction()) == ui::HudFaction::underworld)
        for (const auto& instance : snapshot->instances()) {
        if (!live.is_ally_of_local(instance.owner)) continue;
        const auto* profile = live.combat()->find(instance.type_id);
        if (!profile || !profile->star_base) continue;
        view.source = instance.entity_id;
        const auto weapon = std::find_if(profile->weapons.begin(), profile->weapons.end(),
            [](const auto& gun) { return gun.requires_manual_target; });
        if (weapon == profile->weapons.end()) break;
        const bool intact = !instance.manual_weapons.empty() && instance.manual_weapons.front().available;
        if (intact) {
            view.readiness = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
            for (const auto& [player, clock] : snapshot->manual_clocks())
                if (player == live.local_player()) view.readiness = sim::tactical::manual_readiness(clock, snapshot->completed_tick());
        }
        break;
    }
    ui::TacticalIntent pending; pending.verb = ui::TacticalVerb::special_fire; pending.units = {view.source};
    if (live.command_pending(pending)) view.readiness = {};
    view.availability = ui::special_weapon_availability(running, view.source != 0, view.permitted, view.readiness);
    view.targeting = special_target_source_ && *special_target_source_ == view.source;
    return view;
}

bool BattleInput::special_weapon_click(LiveSessionView& live) {
    if (special_target_source_) { cancel_special_target("same button"); return true; }
    const auto button = special_weapon_button(live);
    if (!button.enabled()) { negative_feedback(); return false; }
    beacon_targeting_ = false;
    cancel_ability_target("special weapon");
    if (placing_) cancel_placement(live);
    if (auto* orders = live.order_input()) orders->cancel_mode();
    special_target_source_ = button.source;
    note("special weapon: pick a target");
    return true;
}

void BattleInput::cancel_special_target(const char* why) {
    if (!special_target_source_) return;
    special_target_source_.reset();
    ++special_cancels_;
    note(std::string("special weapon targeting cancelled: ") + why);
}

bool BattleInput::special_target_valid(const sim::EntityId target, const LiveSessionView& live) const {
    if (!special_target_source_) return false;
    const auto button = special_weapon_button(live);
    if (!button.enabled() || button.source != *special_target_source_) return false;
    const auto* source = live.snapshot_index().instance(button.source);
    const auto* victim = live.snapshot_index().instance(target);
    if (!source || !victim || !live.combat()) return false;
    const auto& snapshot = live.battle_frame().latest;
    const auto admitted = sim::tactical::manual_special_fire_admission(*snapshot, button.source, target, false);
    return admitted && admitted.value().has_value();
}
} // namespace eawr::presentation::godot_backend
