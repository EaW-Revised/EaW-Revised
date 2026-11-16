#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/superlaser_switch.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <iostream>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;
using test::ui::expect;

class States final : public ui::AbilityState {
public:
    ui::UnitAbilityState value;
    std::optional<ui::UnitAbilityState> state(sim::EntityId, std::uint32_t ability) const override {
        return ability == ui::ability_index("SUPER_LASER") ? std::optional{value} : std::nullopt;
    }
};

void visibility_and_requests() {
    std::array<tactical::TacticalInstance, 3> units{};
    units[0].entity_id = 9;
    units[0].owner = 2;
    tactical::AbilityStatus laser;
    laser.kind = tactical::AbilityKind::super_laser;
    laser.ready = true;
    units[0].abilities.push_back(laser);
    units[1].entity_id = 12;
    units[1].owner = 1;
    // No selection or type identity is supplied: any local countdown holder works.
    units[2].entity_id = 17;
    units[2].owner = 1;
    units[2].abilities.push_back(laser);
    States states;
    auto view = ui::superlaser_switch_view(units, 1, states, true, false, false);
    expect(view.visible && view.enabled && view.unit == 17 && !view.card_slot && view.recharge == 1.0,
        "HCB-63: GC space discovers a local holder past enemy and unrelated rows without selection");
    const auto click = ui::superlaser_switch_click(view);
    expect(click && click->kind == ui::AbilityRequest::Kind::activate
        && click->ability == ui::ability_index("SUPER_LASER") && click->targeted
        && click->target == sim::invalid_entity_id && click->units == std::vector<sim::EntityId>{17},
        "HCB-63: the switch arms a targeted activation for the holder, awaiting a world target");
    auto card = ui::superlaser_switch_view(units, 1, states, true, false, true);
    expect(card.visible && card.card_slot && card.unit == view.unit && card.enabled,
        "HCB-63: the planet-destruction lever relocates the laser action without changing its holder");
    for (const auto status : {ui::AbilityStatus::active, ui::AbilityStatus::recharging, ui::AbilityStatus::disabled}) {
        states.value = {status, 0.4, false};
        view = ui::superlaser_switch_view(units, 1, states, true, false, false);
        expect(view.visible && !view.enabled && !ui::superlaser_switch_click(view),
            "HCB-63: active, recharging and blocked holders remain visible but cannot arm");
        expect(view.recharge == (status == ui::AbilityStatus::active ? 0.0 : 0.4),
            "HCB-63: active forces zero, other states retain the ability countdown progress");
    }
    states.value = {};
    expect(!ui::superlaser_switch_view(units, 1, states, true, true, true).visible,
        "HCB-46: tactical production hides the switch in skirmish even with both super weapons");
    expect(!ui::superlaser_switch_view(units, 1, states, false, false, false).visible,
        "HCB-46: land has no ship-targeted switch");
    expect(!ui::superlaser_switch_view(units, 0, states, true, false, false).visible,
        "HCB-63: an observer without a player has no switch");
    units[2].owner = 2;
    expect(!ui::superlaser_switch_view(units, 1, states, true, false, false).visible,
        "HCB-63: losing the local holder clears the binding, ignoring remote holders");
    units[2].owner = 1;
    units[2].abilities.clear();
    expect(!ui::superlaser_switch_view(units, 1, states, true, false, false).visible,
        "HCB-63: removing SUPER_LASER clears the binding");
    expect(!ui::superlaser_switch_click({}), "HCB-63: a hidden control cannot issue a request");
}

void authored_shell() {
    const std::string xml = R"(<CommandBarComponents>
<CommandBarComponent Name="sl_switch"><Type>TextButton</Type>
<Icon_Texture_Name>base.tga</Icon_Texture_Name><Icon_Alternate_Texture_Name>ready.tga</Icon_Alternate_Texture_Name>
<Mouse_Over_Texture_Name>hover.tga</Mouse_Over_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="sl_switch_recharge"><Type>Bar</Type>
<Bar_Texture_Name>back.tga</Bar_Texture_Name><Bar_Overlay_Name>charge.tga</Bar_Overlay_Name>
<Max_Bar_Level>0</Max_Bar_Level><Scale>2</Scale><Offset>3 4</Offset><Smooth_Bar>True</Smooth_Bar>
</CommandBarComponent></CommandBarComponents>)";
    data::ui::CommandBarCatalog catalog;
    std::vector<core::Diagnostic> diagnostics;
    vfs::AssetRecord record;
    record.canonical_path = "data/xml/commandbarcomponents.xml";
    expect(data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(), xml.size())), record,
        catalog, diagnostics).has_value(), "HCB-63: synthetic components load");
    data::ui::ShellAnchors shell;
    data::ui::ShellAnchor anchor;
    anchor.name = "sl_switch";
    anchor.rect = {10, 20, 30, 40};
    shell.add(anchor);
    shell.add_bone({"sl_switch_recharge", {80, 90}});
    const auto built = ui::hud_shell(shell, catalog, ui::HudFaction::empire);
    expect(built.superlaser_switch && built.superlaser_switch->button.normal == "base.tga"
        && built.superlaser_switch->button.alternates == std::vector<std::string>{"ready.tga"}
        && built.superlaser_switch->button.mouse_over == "hover.tga"
        && built.superlaser_switch->button.rect.x == 10,
        "HCB-63: base, ready and hover art stay in the independent authored slot");
    expect(built.superlaser_recharge && built.superlaser_recharge->origin.x == 80
        && built.superlaser_recharge->origin.y == 90 && built.superlaser_recharge->scale == 2
        && built.superlaser_recharge->offset.x == 3 && built.superlaser_recharge->offset.y == 4
        && built.superlaser_recharge->back == std::vector<std::string>{"back.tga"}
        && built.superlaser_recharge->overlay == std::vector<std::string>{"charge.tga"}
        && built.superlaser_recharge->smooth && built.superlaser_recharge->max_level == 0,
        "HCB-63: bone-only recharge placement retains the authored bar properties");
    expect(!built.deathstar_switch, "HCB-63: the super-laser never creates a planet-destruction lever");
    const auto absent = ui::hud_shell({}, catalog, ui::HudFaction::rebel);
    expect(!absent.superlaser_switch && !absent.superlaser_recharge,
        "HCB-63: a shell without the slots does not invent controls");
}
} // namespace

int main() {
    visibility_and_requests();
    authored_shell();
    if (test::ui::failures()) return 1;
    std::cout << "super-laser switch contracts passed\n";
    return 0;
}
