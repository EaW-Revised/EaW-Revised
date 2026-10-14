// P2-20a (#83): the tactical HUD shell model. Synthetic shells pin which meshes
// and parts are drawn and where they land at 1280x720 and 1920x1080 under both
// rule sets; with EAWR_EAW_GAME_ROOT set, the FoC shell and Coruscant's planet
// name are read from the game.

#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/hud_flytext.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"
#include "eawr/presentation/ui/world_encyclopedia.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/presentation/ui/retreat_button.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

bool near(const double a, const double b, const double tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

data::ui::ShellAnchor anchor(std::string name, const data::ui::ReferenceRect rect, std::string shader = "alDefault.fx",
                             std::string texture = {}, const bool visible = false, const float z = 0) {
    data::ui::ShellAnchor out;
    out.alt = data::ui::split_alt(name);
    out.name = std::move(name);
    out.rect = rect;
    out.z_min = out.z_max = z;
    out.visible = visible;
    out.shader = std::move(shader);
    out.base_texture = texture;
    const auto corner = [&](const float x, const float y, const float u, const float v) {
        return data::ui::ShellVertex{{x, y, z}, {u, v}};
    };
    out.triangles.push_back({{corner(rect.x, rect.y, 0, 1), corner(rect.right(), rect.y, 1, 1),
                              corner(rect.right(), rect.top(), 1, 0)}, texture});
    out.triangles.push_back({{corner(rect.x, rect.y, 0, 1), corner(rect.right(), rect.top(), 1, 0),
                              corner(rect.x, rect.top(), 0, 0)}, texture});
    return out;
}

// The FoC tactical shell's parts that P2-20a reads (section 1.3), plus a card
// slot and a faceplate for each faction.
data::ui::ShellAnchors synthetic_shell() {
    data::ui::ShellAnchors shell;
    shell.set_model_path("data/art/models/i_tactical_controls.alo");
    auto card = anchor("s_select_00", {355, 68, 50, 50}, "alDefault.fx", {}, false, 4);
    card.origin = assets::Vec2f{380, 93};
    shell.add(std::move(card));
    auto health = anchor("s_health_00", {355, 62, 50, 6}, "alDefault.fx", {}, false, 5);
    health.origin = assets::Vec2f{380, 65};
    shell.add(std::move(health));
    shell.add(anchor("s_shield_00", {355, 56, 50, 6}, "alDefault.fx", {}, false, 5));
    shell.add(anchor("special_border_00", {353, 4, 54, 119}, "alDefault.fx", {}, false, 2));
    shell.add(anchor("radar", {14.5F, 10.5F, 175, 175}, "MeshAdditive.fx", "i_scan_lines.tga", false, 0));
    auto option = anchor("b_option_t", {205, 7, 24, 24});
    option.origin = assets::Vec2f{217, 19};
    shell.add(std::move(option));
    shell.add(anchor("b_droid_help_tactical", {-2.6F, 243.5F, 52.8F, 68.3F}));
    shell.add(anchor("b_story_arc_t", {36, 243, 24, 24}));
    shell.add(anchor("b_play_pause_t", {2, 206.5F, 34, 35}));
    auto fast = anchor("b_fast_forward_t", {36.5F, 206.5F, 23, 35});
    fast.origin = assets::Vec2f{48, 224};
    shell.add(std::move(fast));
    shell.add(anchor("Text_Planet_tactical", {84.1643F, 228.005F, 189.671F, 21.9909F}));
    shell.add(anchor("Rebel_Faceplate_ALT1", {-1, -1, 1078, 284}, "MeshAlpha.fx", "face_rebel.tga", true, -1));
    shell.add(anchor("b_Help_Droid_Rebel_ALT1", {-12, 252, 92, 67}, "MeshAlpha.fx", "face_rebel.tga", true, -1.25F));
    shell.add(anchor("Empire_Faceplate_ALT0", {-1, -1, 1078, 284}, "MeshAlpha.fx", "face_empire.tga", true, -1));
    shell.add(anchor("Underworld_Faceplate_ALT2", {-1, -8, 1078, 291}, "MeshAlpha.fx", "face_under.tga", true, -1));
    shell.add(anchor("Hidden_Art", {0, 0, 10, 10}, "MeshAlpha.fx", "hidden.tga", false, 0));
    // #530: the build queue's ten slots, the credits text and the reinforcements button.
    for (int index = 0; index < 10; ++index) {
        auto slot = anchor("tqueue0" + std::to_string(index), {300.0F + 40.0F * static_cast<float>(index), 130, 36, 36});
        slot.z_min = slot.z_max = 4;
        shell.add(std::move(slot));
    }
    shell.add(anchor("Text_Credits_tactical", {190, 228, 90, 20}));
    shell.add(anchor("Text_Planetary_Pop", {130, 228, 40, 20}));
    shell.add(anchor("b_reinforcement", {240, 125, 30, 30}));
    shell.add(anchor("b_special_weapon", {210, 125, 30, 30}));
    shell.add(anchor("b_retreat", {200, 160, 36, 36}));
    auto tech = anchor("Text_Tactical_Tech", {64, 214, 50, 12});
    tech.origin = assets::Vec2f{64, 220};
    shell.add(std::move(tech));
    std::size_t order = 0;
    for (const auto name : ui::tactical_order_buttons) {
        shell.add(anchor(std::string(name), {260.0F + 30.0F * static_cast<float>(order++), 205, 24, 24}));
    }
    return shell;
}

data::ui::CommandBarCatalog synthetic_catalog() {
    std::string xml = R"(<CommandBarComponents>
<CommandBarComponent Name="i_main_commandbar"><Type>Shell</Type><Model_Name>i_galactic_controls.alo</Model_Name>
  <Mega_Texture_Name>MT_CommandBar</Mega_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="i_main_skirmish"><Type>Shell</Type><Model_Name>i_tactical_controls.alo</Model_Name></CommandBarComponent>
<CommandBarComponent Name="radar"><Type>Button</Type><Size>171 175</Size></CommandBarComponent>
<CommandBarComponent Name="generic_flytext"><Type>TextButton</Type><No_Shell>True</No_Shell>
  <Font_Name>Custom-Flytext</Font_Name><Font_Point_Size>11</Font_Point_Size><Text_Color>20 40 60 80</Text_Color>
  <Text_Emboss>True</Text_Emboss><Text_Outline>True</Text_Outline><Text_Offset>3 -5</Text_Offset>
  <Left_Justified>True</Left_Justified></CommandBarComponent>
<CommandBarComponent Name="b_special_weapon"><Type>Button</Type><Blank_Texture_Name>back.tga</Blank_Texture_Name>
  <Icon_Alternate_Texture_Name>ion.tga hyper.tga plasma.tga</Icon_Alternate_Texture_Name>
  <Build_Texture_Name>dial.tga</Build_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="b_retreat"><Type>Button</Type><Blank_Texture_Name>retreat-back.tga</Blank_Texture_Name>
  <Icon_Alternate_Texture_Name>retreat-a.tga retreat-b.tga</Icon_Alternate_Texture_Name>
  <Selected_Texture_Name>retreat-over.tga</Selected_Texture_Name><Disabled_Texture_Name>retreat-off.tga</Disabled_Texture_Name>
  <Build_Texture_Name>retreat-dial.tga</Build_Texture_Name><Click_Shift>True</Click_Shift></CommandBarComponent>
<CommandBarComponent Name="s_select_00"><Type>TextButton</Type><Font_Name>EmpireAtWar-Medium</Font_Name>
  <Font_Point_Size>6</Font_Point_Size><Text_Outline>True</Text_Outline><Text_Offset2>0 -16</Text_Offset2></CommandBarComponent>
<CommandBarComponent Name="s_health_00"><Type>Bar</Type><Bar_Texture_Name>back.tga back.tga back.tga</Bar_Texture_Name>
  <Bar_Overlay_Name>h0.tga h1.tga h2.tga</Bar_Overlay_Name><Max_Bar_Level>2</Max_Bar_Level><Smooth_Bar>True</Smooth_Bar>
  <Offset>1 -2</Offset></CommandBarComponent>
<CommandBarComponent Name="s_shield_00"><Type>Bar</Type><Bar_Overlay_Name>s.tga s.tga s.tga</Bar_Overlay_Name>
  <Max_Bar_Level>2</Max_Bar_Level></CommandBarComponent>
<CommandBarComponent Name="special_border_00"><Type>TextButton</Type>
  <Icon_Alternate_Texture_Name>full.tga left.tga centre.tga right.tga</Icon_Alternate_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="b_option_t"><Type>TextButton</Type>
  <Icon_Texture_Name>i_button_command_bar_options.tga</Icon_Texture_Name>
  <Mouse_Over_Texture_Name>i_button_command_bar_options_over.tga</Mouse_Over_Texture_Name>
  <Selected_Texture_Name>i_button_command_bar_options_press.tga</Selected_Texture_Name>
  <Disabled_Texture_Name>i_button_command_bar_options_disabled.tga</Disabled_Texture_Name>
  <Tooltip_Text>TEXT_BUTTON_MAIN_MENU</Tooltip_Text></CommandBarComponent>
<CommandBarComponent Name="b_droid_help_tactical"><Type>Button</Type>
  <Blank_Texture_Name>i_button_command_bar_help_disabled.tga</Blank_Texture_Name>
  <Icon_Texture_Name>i_button_command_bar_help.tga</Icon_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="b_story_arc_t"><Type>Button</Type>
  <Icon_Texture_Name>i_button_command_bar_holocron.tga</Icon_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="b_play_pause_t"><Type>Button</Type>
  <Icon_Texture_Name>i_button_command_bar_play.tga</Icon_Texture_Name><Scale>1.0</Scale></CommandBarComponent>
<CommandBarComponent Name="b_fast_forward_t"><Type>Button</Type>
  <Icon_Texture_Name>i_button_command_bar_ff.tga</Icon_Texture_Name><Scale>0.5</Scale></CommandBarComponent>
<CommandBarComponent Name="Text_Planet_tactical"><Type>TextButton</Type><Font_Name>EmpireAtWar-Bold</Font_Name>
  <Font_Point_Size>10</Font_Point_Size><Text_Outline>True</Text_Outline><Text_Color>255 212 33 255</Text_Color>
  <Max_Text_Width>110</Max_Text_Width></CommandBarComponent>
<CommandBarComponent Name="tqueue00"><Type>TextButton</Type><Build_Texture_Name>i_button_build.tga</Build_Texture_Name>
  <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>6</Font_Point_Size><Text_Offset>-3 -20</Text_Offset>
  <Text_Color>192 192 255 180</Text_Color><Text_Outline>True</Text_Outline></CommandBarComponent>
<CommandBarComponent Name="tqueue01"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue02"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue03"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue04"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue05"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue06"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue07"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue08"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="tqueue09"><Type>Button</Type></CommandBarComponent>
<CommandBarComponent Name="Text_Credits_tactical"><Type>TextButton</Type><Icon_Texture_Name>i_icon_money_big.tga</Icon_Texture_Name>
  <Font_Point_Size>7</Font_Point_Size><Text_Color>255 212 33 255</Text_Color><Right_Justified>True</Right_Justified>
  <Blink_Duration>0.5</Blink_Duration><Blink_Rate>0.25</Blink_Rate></CommandBarComponent>
<CommandBarComponent Name="Text_Planetary_Pop"><Type>TextButton</Type><Icon_Texture_Name>population.tga</Icon_Texture_Name>
  <Font_Name>Arial Bold</Font_Name><Font_Point_Size>6</Font_Point_Size><Text_Color>255 212 33 255</Text_Color>
  <Right_Justified>True</Right_Justified><Text_Offset>-10 0</Text_Offset></CommandBarComponent>
<CommandBarComponent Name="b_reinforcement"><Type>Button</Type>
  <Icon_Alternate_Texture_Name>i_button_skirmish_reinforcements.tga i_button_skirmish_reinforcements.tga special.tga</Icon_Alternate_Texture_Name>
  <Blank_Texture_Name>pad.tga</Blank_Texture_Name><Flash_Texture_Name>flash.tga</Flash_Texture_Name>
  <Click_Shift>Yes</Click_Shift><Selected_Alpha>Yes</Selected_Alpha></CommandBarComponent>
<CommandBarComponent Name="Text_Tactical_Tech"><Type>TextButton</Type>
  <Font_Name>Arial Bold</Font_Name><Font_Point_Size>8</Font_Point_Size><Text_Color>255 212 33 255</Text_Color>
  <Left_Justified>True</Left_Justified><Text_Offset>3 -2</Text_Offset></CommandBarComponent>
<CommandBarComponent Name="reinforcement_counter"><Type>TextButton</Type>
  <No_Shell>True</No_Shell><Icon_Texture_Name>reinforce.tga</Icon_Texture_Name>
  <Blank_Texture_Name>dark.tga</Blank_Texture_Name><Build_Texture_Name>timer.tga</Build_Texture_Name>
  <Scale>0.6</Scale><Color>10 20 30 40</Color><Blink_Duration>10</Blink_Duration>
  <Blink_Rate>0.2</Blink_Rate><Blink_Fade>True</Blink_Fade></CommandBarComponent>
<CommandBarComponent Name="garrison_respawn_counter"><Type>TextButton</Type>
  <No_Shell>True</No_Shell><Icon_Texture_Name>garrison.tga</Icon_Texture_Name>
  <Build_Texture_Name>garrison-dial.tga</Build_Texture_Name><Scale>0.9</Scale></CommandBarComponent>
</CommandBarComponents>)";
    for (const auto name : ui::tactical_order_buttons) {
        const std::string component = std::string(name);
        xml.insert(xml.rfind("</CommandBarComponents>"),
            "<CommandBarComponent Name=\"" + component + "\"><Type>Button</Type>"
            "<Icon_Texture_Name>" + component + ".tga</Icon_Texture_Name>"
            "<Mouse_Over_Texture_Name>over.tga</Mouse_Over_Texture_Name>"
            "<Selected_Texture_Name>selected.tga</Selected_Texture_Name>"
            "<Blank_Texture_Name>blank.tga</Blank_Texture_Name>"
            "<Click_Shift>Yes</Click_Shift><Selected_Alpha>Yes</Selected_Alpha></CommandBarComponent>");
    }
    data::ui::CommandBarCatalog catalog;
    std::vector<core::Diagnostic> diagnostics;
    vfs::AssetRecord record;
    record.canonical_path = "data/xml/commandbarcomponents.xml";
    const auto parsed =
        data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(), xml.size())), record, catalog, diagnostics);
    expect(parsed.has_value() && diagnostics.empty(), "synthetic command bar parses cleanly");
    return catalog;
}

// HCB-15 (#3078): b_retreat keeps its authored slot and art; its state follows the published view.
void retreat_button() {
    const auto retreat = ui::hud_shell(synthetic_shell(), synthetic_catalog(), ui::HudFaction::underworld).retreat;
    expect(retreat && retreat->button.rect.x == 200 && retreat->button.rect.y == 160 && retreat->button.rect.width == 36
        && retreat->button.normal == "retreat-a.tga" && retreat->blank == "retreat-back.tga"
        && retreat->button.pressed == "retreat-over.tga" && retreat->button.disabled == "retreat-off.tga"
        && retreat->build == "retreat-dial.tga" && retreat->button.click_shift,
        "HCB-15: b_retreat keeps its shell slot, first alternate icon, state art and dial");
    data::ui::ShellAnchors without;
    without.add(anchor("radar", {14.5F, 10.5F, 175, 175}, "MeshAdditive.fx", "i_scan_lines.tga", false, 0));
    expect(!ui::hud_shell(without, synthetic_catalog(), ui::HudFaction::rebel).retreat,
        "HCB-15: a shell without the b_retreat anchor draws no retreat control");

    using sim::tactical::RetreatPhase;
    using sim::tactical::RetreatRefusal;
    expect(ui::retreat_button_view(std::nullopt, 100, false) == ui::RetreatButtonView{},
        "HCB-15a: no published view keeps the skirmish glyph disabled, without a dial");
    sim::tactical::RetreatView view;
    view.player = 1; view.lockout_frames = 300; view.refusal = RetreatRefusal::lockout;
    const auto locked = ui::retreat_button_view(view, 75, false);
    expect(!locked.enabled && !locked.flashing && std::abs(locked.dial - 0.25) < 1e-9,
        "HCB-15b: the opening lockout disables the button and fills the dial with the elapsed share");
    view.refusal = RetreatRefusal::none;
    expect(ui::retreat_button_view(view, 400, false) == ui::RetreatButtonView{true, false, 1.0},
        "HCB-15b: an eligible player gets an enabled button without a dial");
    expect(ui::retreat_button_view(view, 400, true) == ui::RetreatButtonView{},
        "HCB-15b: a decided battle disables the button");
    for (const auto refusal : {RetreatRefusal::prevention, RetreatRefusal::superweapon, RetreatRefusal::other_player}) {
        view.refusal = refusal;
        expect(ui::retreat_button_view(view, 400, false).enabled,
            "HCB-15b: prevention, superweapon and another player's retreat refuse the press, not the button");
    }
    for (const auto refusal : {RetreatRefusal::tutorial, RetreatRefusal::faction, RetreatRefusal::invalid_player,
        RetreatRefusal::wrong_mode, RetreatRefusal::battle_decided}) {
        view.refusal = refusal;
        expect(!ui::retreat_button_view(view, 400, false).enabled, "HCB-15b: gated retreat disables the button");
    }
    view.refusal = RetreatRefusal::none;
    view.state.phase = RetreatPhase::countdown; view.state.player = 1;
    expect(ui::retreat_button_view(view, 400, false) == ui::RetreatButtonView{true, true, 1.0},
        "HCB-15b: the local countdown flashes the still-enabled button, whose press cancels");
    view.state.forced = true;
    expect(ui::retreat_button_view(view, 400, false) == ui::RetreatButtonView{true, true, 1.0},
        "HCB-15b: the local forced countdown keeps the enabled blink, with no command on press");
    view.state.forced = false;
    view.state.player = 2; view.refusal = RetreatRefusal::other_player;
    expect(!ui::retreat_button_view(view, 400, false).flashing, "HCB-15b: another player's countdown does not flash");
    view.state.player = 1; view.refusal = RetreatRefusal::none; view.state.phase = RetreatPhase::escaping;
    expect(!ui::retreat_button_view(view, 400, false).enabled, "the escape phase disables the button");
}

void shell_parts() {
    const auto shell = synthetic_shell();
    const auto catalog = synthetic_catalog();
    expect(ui::tactical_shell_model(catalog) == "i_tactical_controls.alo", "i_main_skirmish names the tactical shell");
    expect(ui::command_bar_mega_texture(catalog) == "Data/Art/Textures/MT_CommandBar.mtd",
           "the command bar's Mega_Texture_Name is the atlas");
    expect(ui::tactical_shell_model({}) == "i_tactical_controls.alo", "an empty catalogue falls back to the retail shell");

    const auto rebel = ui::hud_shell(shell, catalog, ui::HudFaction::rebel);
    expect(rebel.diagnostics.empty(), "the synthetic shell has every P2-20a part");
    data::ui::CommandBarCatalog missing_level_catalog;
    for (auto component : catalog.components()) {
        if (component.name == "s_health_00") {
            std::erase_if(component.fields, [](const auto& field) { return field.field == data::ui::Field::max_bar_level; });
        }
        missing_level_catalog.add(std::move(component));
    }
    const auto missing_level = ui::hud_shell(shell, missing_level_catalog, ui::HudFaction::rebel);
    expect(missing_level.diagnostics.size() == 1 && missing_level.diagnostics.front().code == "EAWR-UI-0320"
               && missing_level.diagnostics.front().message.find("Max_Bar_Level") != std::string::npos,
           "a missing bar level reports the existing HUD diagnostic family");
    expect(!missing_level.card_slots.empty() && missing_level.card_slots.front().health
               && missing_level.card_slots.front().health->max_level == 10,
           "a missing bar level keeps the FoC fallback of ten");
    expect(rebel.order_buttons.size() == 6U, "six authored order controls include waypoint mode");
    for (const auto& button : rebel.order_buttons) {
        expect(button.normal == button.name + ".tga"
                   && button.mouse_over == "over.tga" && button.pressed == "selected.tga"
                   && button.blank == "blank.tga" && button.click_shift && button.selected_alpha,
               "order controls retain their icon, state art and authored pressed flags");
    }
    expect(rebel.ability_buttons.empty(), "#454: a shell without special_button_NN draws no ability buttons");
    // #530 PU-63, PU-65, PU-66.
    expect(rebel.queue_slots.size() == 10U, "the build queue has its ten slots");
    if (rebel.queue_slots.size() == 10U) {
        const auto& front = rebel.queue_slots.front();
        expect(front.button.name == "tqueue00" && front.text && front.build == "i_button_build.tga"
                   && front.face == "EmpireAtWar-Medium" && front.point_size == 6
                   && front.text_offset == (data::ui::Vec2{-3, -20})
                   && front.colour == (data::ui::Rgba8{192, 192, 255, 180}) && front.outline,
               "a queue slot takes its build art and its percent text's font, offset and colour");
        expect(!rebel.queue_slots[5].text && rebel.queue_slots[5].button.name == "tqueue05",
               "a plain button slot has no text");
    }
    expect(rebel.credits && rebel.credits->icon == "i_icon_money_big.tga" && rebel.credits->right_justified
               && rebel.credits->text.point_size == 7 && rebel.credits->text.colour == (data::ui::Rgba8{255, 212, 33, 255})
               && near(rebel.credits->blink_duration, 0.5) && near(rebel.credits->blink_rate, 0.25),
           "the credits text takes its money icon, font and blink");
    expect(rebel.reinforcement && rebel.reinforcement->normal == "i_button_skirmish_reinforcements.tga",
           "the reinforcements button takes its icon");
    expect(rebel.reinforcement && rebel.reinforcement->click_shift && rebel.reinforcement->selected_alpha,
           "the reinforcement pressed state takes its shift and blend flags");
    expect(rebel.reinforcement && rebel.reinforcement->blank == "pad.tga" && rebel.reinforcement->flash == "flash.tga",
           "PU-70/71: alternate-only icon, backing and notification art are preserved");
    std::vector<std::string> names;
    for (const auto& mesh : rebel.meshes) names.push_back(mesh.name);
    expect((names == std::vector<std::string>{"b_Help_Droid_Rebel_ALT1", "Rebel_Faceplate_ALT1", "radar"}),
           "the rebel variant draws its droid, faceplate and radar scan lines, farthest first");
    expect(rebel.meshes[1].blend == ui::ShellBlend::alpha && rebel.meshes[2].blend == ui::ShellBlend::additive,
           "MeshAlpha blends by alpha and MeshAdditive adds");
    expect(rebel.meshes[1].texture == "face_rebel.tga" && rebel.meshes[1].triangles.size() == 2U,
           "a mesh keeps its texture and triangles");
    expect(rebel.minimap && *rebel.minimap == (data::ui::ReferenceRect{14.5F, 10.5F, 175, 175}), "the minimap is the radar rect");
    expect(rebel.options && rebel.options->normal == "i_button_command_bar_options.tga"
               && rebel.options->mouse_over == "i_button_command_bar_options_over.tga"
               && rebel.options->pressed == "i_button_command_bar_options_press.tga"
               && rebel.options->disabled == "i_button_command_bar_options_disabled.tga"
               && rebel.options->tooltip == "TEXT_BUTTON_MAIN_MENU",
           "the options button takes its state textures and tooltip from the catalogue");
    // #349: button art keeps its texture's size around the component's bone,
    // so the options icon overhangs its 24 x 24 mesh instead of squeezing into it.
    const auto options_quad = ui::button_quad(*rebel.options, 36, 25);
    expect(options_quad == (data::ui::ReferenceRect{199, 6.5F, 36, 25}),
           "the options art is its 36 x 25 texture centred on the button's bone");
    std::vector<std::string> panel;
    for (const auto& button : rebel.panel_buttons) panel.push_back(button.name + ":" + button.normal);
    expect((panel == std::vector<std::string>{"b_droid_help_tactical:i_button_command_bar_help.tga",
                                              "b_story_arc_t:i_button_command_bar_holocron.tga",
                                              "b_play_pause_t:i_button_command_bar_play.tga",
                                              "b_fast_forward_t:i_button_command_bar_ff.tga"}),
           "the time panel's four buttons are drawn in their normal (icon) state");
    expect(near(rebel.panel_buttons[1].origin.x, 48) && near(rebel.panel_buttons[1].origin.y, 255),
           "a button without a bone centres on its mesh");
    expect(ui::button_quad(rebel.panel_buttons[3], 24, 36) == (data::ui::ReferenceRect{42, 215, 12, 18}),
           "the component's Scale scales its art");
    expect(rebel.planet_name && rebel.planet_name->face == "EmpireAtWar-Bold" && rebel.planet_name->point_size == 10
               && rebel.planet_name->colour == (data::ui::Rgba8{255, 212, 33, 255}) && rebel.planet_name->outline
               && !rebel.planet_name->emboss && rebel.planet_name->max_text_width == 110,
           "the planet name takes its component font");
    // #425: the card slot with its bars and its column's border.
    expect(rebel.card_slots.size() == 1U && rebel.card_borders.size() == 1U, "one card slot and one border");
    if (!rebel.card_slots.empty()) {
        const auto& slot = rebel.card_slots.front();
        expect(slot.card.name == "s_select_00" && near(slot.card.origin.x, 380) && near(slot.card.origin.y, 93)
                   && slot.face == "EmpireAtWar-Medium" && slot.point_size == 6 && slot.outline
                   && slot.count_offset == (data::ui::Vec2{0, -16}),
               "a card slot takes its bone and its count text's font and offset");
        expect(slot.health && slot.health->overlay == std::vector<std::string>{"h0.tga", "h1.tga", "h2.tga"}
                   && slot.health->back.size() == 3U && slot.health->max_level == 2 && slot.health->smooth
                   && slot.health->offset == (data::ui::Vec2{1, -2}) && near(slot.health->origin.y, 65),
               "the health bar takes its levels, offset and bone");
        expect(slot.shield && !slot.shield->smooth && near(slot.shield->origin.x, 380) && near(slot.shield->origin.y, 59),
               "a bar without a bone centres on its mesh");
    }
    if (!rebel.card_borders.empty()) {
        expect(rebel.card_borders.front().alternates
                   == std::vector<std::string>{"full.tga", "left.tga", "centre.tga", "right.tga"},
               "the border keeps its four pieces");
    }

    const auto empire = ui::hud_shell(shell, catalog, ui::HudFaction::empire);
    expect(empire.meshes.size() == 2U && empire.meshes[0].name == "Empire_Faceplate_ALT0",
           "the empire variant draws its own faceplate");
    const auto underworld = ui::hud_shell(shell, catalog, ui::HudFaction::underworld);
    expect(underworld.meshes.size() == 2U && underworld.meshes[0].texture == "face_under.tga",
           "the underworld variant draws its own faceplate");

    data::ui::ShellAnchors bare;
    bare.add(anchor("Rebel_Faceplate_ALT1", {-1, -1, 1078, 284}, "MeshAlpha.fx", "face_rebel.tga", true, -1));
    const auto partial = ui::hud_shell(bare, catalog, ui::HudFaction::empire);
    expect(partial.diagnostics.size() == 18U && !partial.minimap && partial.queue_slots.empty() && !partial.credits && !partial.population
               && !partial.reinforcement && !partial.options && !partial.planet_name
               && partial.panel_buttons.empty() && partial.order_buttons.empty(),
           "a missing faceplate, radar, options button, panel button, planet name, credits and population text and reinforcements "
           "button and six command buttons are reported once each; a shell without queue slots shows no queue");

    for (const auto* text : {"empire", "Rebel", "UNDERWORLD"}) expect(ui::hud_faction_from(text).has_value(), "factions parse");
    expect(!ui::hud_faction_from("pirate"), "an unknown faction is refused");
    expect(ui::alt_variant(ui::HudFaction::underworld) == 2U, "Underworld is ALT2");
}

// GHUD-S63: the emblem index follows the authored side names, not literal faction names.
void allegiance_emblem_indices() {
    const ui::SideNames retail{"Rebel", "Empire", "Underworld"};
    expect(ui::allegiance_emblem_index("Rebel", retail) == 0, "good side is entry 0");
    expect(ui::allegiance_emblem_index("EMPIRE", retail) == 1, "evil side is entry 1, case-insensitively");
    expect(ui::allegiance_emblem_index("underworld", retail) == 2, "corrupt side is entry 2");
    expect(ui::allegiance_emblem_index("Pirates", retail) == 3, "the Pirates literal is entry 3");
    expect(ui::allegiance_emblem_index("Hutts", retail) == 4, "any other faction is entry 4");
    expect(ui::allegiance_emblem_index("Neutral", retail) == 4, "Neutral is entry 4");
    // A mod-shaped table: sides renamed and reordered, plus an extra faction.
    const ui::SideNames mod{"Alliance", "Imperium", "Cartel"};
    expect(ui::allegiance_emblem_index("Alliance", mod) == 0, "a renamed good side keeps entry 0");
    expect(ui::allegiance_emblem_index("Cartel", mod) == 2, "a renamed corrupt side keeps entry 2");
    expect(ui::allegiance_emblem_index("Rebel", mod) == 4, "a retail name that is no longer a side is entry 4");
    expect(ui::allegiance_emblem_index("Mandalore", mod) == 4, "an extra faction is entry 4");
    // A missing side name matches no faction, not the empty string.
    expect(ui::allegiance_emblem_index("", ui::SideNames{}) == 4, "empty names match nothing");
    expect(ui::allegiance_emblem_index("Rebel", ui::SideNames{"", "Empire", ""}) == 4, "an unset good side is not Rebel");
}

// GHUD-S52: the focused organize shell's alternate is the planet's terrain index, not the faction; the
// land_art family is kept with its index for every faction, and nothing is hard-coded to the stock range.
void organize_shell_alternates() {
    data::ui::ShellAnchors organize;
    organize.add(anchor("Planet_structure_00", {0, 0, 4, 4}, "MeshAlpha.fx", "structure.tga", true, 1));
    for (const int index : {0, 1, 2, 3, 12, 40})
        organize.add(anchor("land_art_ALT" + std::to_string(index), {0, 0, 100, 50}, "MeshAlpha.fx",
                            "land.tga", true, 0));
    const auto catalog = synthetic_catalog();
    const auto filtered = ui::hud_shell(organize, catalog, ui::HudFaction::rebel);
    expect(filtered.meshes.size() == 2U, "by faction only the faction's variant panel is kept (the old look)");
    for (const auto faction : {ui::HudFaction::empire, ui::HudFaction::rebel, ui::HudFaction::underworld}) {
        const auto look = ui::hud_shell(organize, catalog, faction, true);
        expect(look.meshes.size() == 7U, "every panel and the plain mesh are kept for any faction");
        std::vector<std::uint32_t> kept;
        std::size_t plain = 0;
        for (const auto& mesh : look.meshes) {
            if (mesh.alternate) kept.push_back(*mesh.alternate); else ++plain;
        }
        std::sort(kept.begin(), kept.end());
        expect(plain == 1U && kept == std::vector<std::uint32_t>{0, 1, 2, 3, 12, 40},
               "panel indexes past the stock range survive (a mod's larger Zoomed_Terrain_Index)");
    }
}

struct Expected {
    ui::Viewport viewport;
    ui::LayoutRules rules;
    ui::PixelRect minimap;
    ui::PixelRect options;
    double planet_centre_y;
    std::int32_t planet_em;
};

// UI-L1/UI-L2 at the eye-check sizes: the shell's origin is the lower-left of
// the (16:9) safe area at scale H/768; both rule sets agree on 16:9 screens.
void shell_layout() {
    const auto shell = ui::hud_shell(synthetic_shell(), synthetic_catalog(), ui::HudFaction::rebel);
    const double k720 = 720.0 / 768.0;
    const double k1080 = 1080.0 / 768.0;
    const std::array cases{
        Expected{{1280, 720}, ui::LayoutRules::aspect_correct,
                 {14.5 * k720, 720 - 185.5 * k720, 175 * k720, 175 * k720}, {205 * k720, 720 - 31 * k720, 24 * k720, 24 * k720},
                 495.94, 15},
        Expected{{1280, 720}, ui::LayoutRules::retail,
                 {14.5 * k720, 720 - 185.5 * k720, 175 * k720, 175 * k720}, {205 * k720, 720 - 31 * k720, 24 * k720, 24 * k720},
                 495.94, 15},
        Expected{{1920, 1080}, ui::LayoutRules::aspect_correct,
                 {14.5 * k1080, 1080 - 185.5 * k1080, 175 * k1080, 175 * k1080},
                 {205 * k1080, 1080 - 31 * k1080, 24 * k1080, 24 * k1080}, 743.91, 23},
        Expected{{1920, 1080}, ui::LayoutRules::retail,
                 {14.5 * k1080, 1080 - 185.5 * k1080, 175 * k1080, 175 * k1080},
                 {205 * k1080, 1080 - 31 * k1080, 24 * k1080, 24 * k1080}, 743.91, 23},
    };
    for (const auto& item : cases) {
        const auto space = ui::reference_space(item.viewport, item.rules);
        const auto placement = ui::place_shell(space, 1077);
        expect(near(placement.left, 0) && near(placement.bottom, item.viewport.height)
                   && near(placement.scale, item.viewport.height / 768.0),
               "a 16:9 screen puts the shell in its lower-left corner at H/768");
        const auto minimap = ui::shell_to_screen(*shell.minimap, placement);
        const auto options = ui::shell_to_screen(shell.options->rect, placement);
        const auto planet = ui::shell_to_screen(shell.planet_name->rect, placement);
        expect(near(minimap.x, item.minimap.x) && near(minimap.y, item.minimap.y, 1e-2)
                   && near(minimap.width, item.minimap.width) && near(minimap.height, item.minimap.height),
               "the minimap frame lands on the radar rect");
        expect(near(options.x, item.options.x) && near(options.y, item.options.y) && near(options.width, item.options.width),
               "the options button lands on b_option_t");
        // Section 1.4: the planet name's centre measured 495.5 (720) and 731.0
        // (1061) px from the top on the rig; 1080 lines predict 743.9.
        expect(near(planet.centre_y(), item.planet_centre_y, 0.01), "the planet name centre matches the prediction");
        const auto em = ui::font_pixels({shell.planet_name->point_size, false, 1.0}, ui::font_screen_height(space)).em_height;
        expect(em == item.planet_em, "the planet name is EaW-Bold 10 pt at UI-F1 size");
        const auto corner = ui::shell_point_to_screen(-1, -1, placement);
        expect(near(corner.x, -placement.scale) && near(corner.y, item.viewport.height + placement.scale),
               "a shell vertex maps with y up to the screen's y down");
    }
    // 21:9: aspect-correct centres the 16:9 safe area, retail keeps the corner.
    const auto wide = ui::reference_space({2560, 1080}, ui::LayoutRules::aspect_correct);
    const auto wide_retail = ui::reference_space({2560, 1080}, ui::LayoutRules::retail);
    expect(near(ui::place_shell(wide, 1077).left, (2560 - 1920) / 2.0), "ultrawide centres the HUD's safe area");
    expect(near(ui::place_shell(wide_retail, 1077).left, 0), "retail rules keep the screen corner");
}

std::vector<std::byte> text_file(const std::string& key, const std::u16string& value) {
    std::vector<std::byte> out;
    const auto u32 = [&](const std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    };
    u32(1);
    u32(data::ui::text_key_crc(key));
    u32(static_cast<std::uint32_t>(value.size()));
    u32(static_cast<std::uint32_t>(key.size()));
    for (const char16_t c : value) {
        out.push_back(static_cast<std::byte>(c & 0xFF));
        out.push_back(static_cast<std::byte>(c >> 8));
    }
    for (const char c : key) out.push_back(static_cast<std::byte>(c));
    return out;
}

void planet_names() {
    test::ui::TempTree tree("hud-planet");
    const auto xml = tree.root / "XML";
    test::ui::write_text(xml / "GameObjectFiles.xml",
                         "<Game_Object_Files><File>planets.xml</File><File>units.xml</File></Game_Object_Files>");
    test::ui::write_text(xml / "planets.xml", "<Planets><Planet Name=\"Coruscant\"><Text_ID> TEXT_OBJECT_STAR_SYSTEM_CORUSCANT"
                                              " </Text_ID></Planet><Planet Name=\"Modded_System_42\"><Text_ID>"
                                              "TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Text_ID></Planet><Planet Name=\"Nameless\"/>"
                                              "<Planet Name=\"Unlisted\"><Text_ID>TEXT_NOT_THERE</Text_ID></Planet></Planets>");
    for (const auto* registry : {"HardpointDataFiles.xml", "FactionFiles.xml", "CampaignFiles.xml", "SFXEventFiles.xml"})
        test::ui::write_text(xml / registry, "<Files/>");
    test::ui::write_text(xml / "units.xml", R"xml(<Units>
<SpaceUnit Name="Kuat"><Text_ID>TEXT_UNIT</Text_ID></SpaceUnit>
<SpaceProp Name="HoverAsteroid"><Behavior>SPACE_OBSTACLE, IDLE</Behavior>
<Is_Impassable_Asteroid>Yes</Is_Impassable_Asteroid><Is_Valid_Target>No</Is_Valid_Target><Text_ID/></SpaceProp>
<SpaceProp Name="HoverValid"><Variant_Of_Existing_Type>HoverAsteroid</Variant_Of_Existing_Type>
<Is_Valid_Target>Yes</Is_Valid_Target><Text_ID>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Text_ID></SpaceProp>
<SpaceProp Name="HoverSelectable"><Variant_Of_Existing_Type>HoverAsteroid</Variant_Of_Existing_Type>
<SpaceBehavior>selectable</SpaceBehavior></SpaceProp>
<SpaceProp Name="HoverTooltip"><Variant_Of_Existing_Type>HoverAsteroid</Variant_Of_Existing_Type>
<Behavior>SPACE_OBSTACLE, dummy_tooltip</Behavior></SpaceProp>
<SpaceProp Name="HoverInherited"><Variant_Of_Existing_Type>HoverTooltip</Variant_Of_Existing_Type></SpaceProp>
<SpaceProp Name="HoverNearMatch"><Behavior>NOT_SELECTABLE DUMMY_TOOLTIP_EXTRA</Behavior></SpaceProp>
<SpaceUnit Name="Popup"><Encyclopedia_Text>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT, TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Encyclopedia_Text>
<MP_Encyclopedia_Text>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</MP_Encyclopedia_Text>
<Encyclopedia_Unit_Class>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Encyclopedia_Unit_Class>
<Encyclopedia_Good_Against>CardUpgrade, Missing, Kuat, CardUpgrade</Encyclopedia_Good_Against>
<Encyclopedia_Vulnerable_To>Missing CardUpgrade</Encyclopedia_Vulnerable_To>
<Population_Value>2</Population_Value><Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability>
<Type>POWER_TO_WEAPONS</Type><Alternate_Name_Text>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Alternate_Name_Text>
<Alternate_Description_Text>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Alternate_Description_Text>
<Alternate_Icon_Name>overridden-ability.tga</Alternate_Icon_Name></Unit_Ability></Unit_Abilities_Data></SpaceUnit>
<Squadron Name="PopupSquadron"><Squadron_Units>Popup, Kuat</Squadron_Units><Population_Value>1</Population_Value></Squadron>
<Squadron Name="CycleA"><Squadron_Units>CycleB</Squadron_Units></Squadron>
<Squadron Name="CycleB"><Squadron_Units>CycleA</Squadron_Units></Squadron>
<SpaceUnit Name="EmptyMP"><Encyclopedia_Text>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Encyclopedia_Text>
<MP_Encyclopedia_Text> , </MP_Encyclopedia_Text></SpaceUnit>
<SpaceUnit Name="UnknownMP"><Encyclopedia_Text>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Encyclopedia_Text>
<MP_Encyclopedia_Text>TEXT_UNKNOWN</MP_Encyclopedia_Text></SpaceUnit>
<UpgradeObject Name="CardUpgrade"><Icon_Name>owner-icon.tga</Icon_Name>
<Text_ID>TEXT_OBJECT_STAR_SYSTEM_CORUSCANT</Text_ID><Tactical_Build_Cost_Multiplayer>950</Tactical_Build_Cost_Multiplayer>
<Is_Visible_On_Radar>No</Is_Visible_On_Radar><Radar_Icon_Name>owner-radar.tga</Radar_Icon_Name>
<Abilities><Combat_Bonus_Ability Name="CardUpgrade"><Icon_Name>ability-icon.tga</Icon_Name>
<Tactical_Build_Cost_Multiplayer>0</Tactical_Build_Cost_Multiplayer><Is_Visible_On_Radar>Yes</Is_Visible_On_Radar>
<Radar_Icon_Name>ability-radar.tga</Radar_Icon_Name></Combat_Bonus_Ability></Abilities></UpgradeObject>
</Units>)xml");
    const std::array mounts{vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "planet fixture mounts");
    if (!mounted) return;
    auto catalog = data::load_catalog(mounted.value(), data::Profile::foc);
    expect(static_cast<bool>(catalog), "planet fixture catalog loads");
    if (!catalog) return;
    const auto bytes = text_file("TEXT_OBJECT_STAR_SYSTEM_CORUSCANT", u"Coruscant");
    assets::Source source;
    source.logical_path = "data/text/mastertextfile_english.dat";
    auto text = data::ui::load_text_database(bytes, source);
    expect(static_cast<bool>(text), "planet fixture text DB loads");
    if (!text) return;
    const auto* objects = &catalog.value().catalog;
    expect(!ui::world_encyclopedia_admitted("HoverAsteroid", objects)
        && !ui::world_encyclopedia_admitted("HoverValid", objects),
        "CARD-20: impassable mouse contacts and valid-target/name metadata alone admit no world tooltip");
    expect(ui::world_encyclopedia_admitted("HoverSelectable", objects)
        && ui::world_encyclopedia_admitted("HoverTooltip", objects)
        && ui::world_encyclopedia_admitted("HoverInherited", objects),
        "CARD-20: selectable and dummy-tooltip behaviours, including inherited mod data, admit invalid-target types");
    expect(!ui::world_encyclopedia_admitted("HoverNearMatch", objects)
        && !ui::world_encyclopedia_admitted("Missing", objects)
        && !ui::world_encyclopedia_admitted("HoverSelectable", nullptr),
        "CARD-20: exact behaviour tokens and an effective game-object definition are required");
    const auto ability = ui::ability_card_looks("PopupSquadron", ui::ability_index("POWER_TO_WEAPONS"), objects, &text.value());
    expect(ability.name == "Coruscant" && ability.description == "Coruscant" && ability.icon == "overridden-ability.tga",
        "squadron ability hover uses first craft and independently authored localized overrides");
    const auto cycle = ui::ability_card_looks("CycleA", ui::ability_index("POWER_TO_WEAPONS"), objects, &text.value());
    expect(cycle.name.empty() && cycle.description.empty() && cycle.icon.empty(),
        "cyclic squadron ability roster terminates without an encyclopedia");
    const auto squadron_popup = ui::unit_card_looks("PopupSquadron", objects, &text.value());
    expect(squadron_popup.ability_icons == std::vector<std::string>{"overridden-ability.tga"} && squadron_popup.population == -1,
        "static ability art is independent of live activation/ownership; population belongs to the company type");
    const auto legacy = objects->resolve("CardUpgrade");
    expect(legacy && legacy.value().category == data::Category::ability, "HUD fixture warms the global ability winner");
    const auto card = ui::unit_card_looks("CardUpgrade", objects, &text.value());
    expect(card.icon == "owner-icon.tga" && card.name == "Coruscant" && card.build_cost == 950,
           "build card icon, title and cost come from the owning game object despite a same-name ability");
    const auto popup = ui::unit_card_looks("Popup", objects, &text.value());
    expect(popup.description == "Coruscant" && popup.unit_class == "Coruscant",
           "P-2/P-3: skirmish uses the multiplayer list and localized class key");
    expect(popup.good_against_icons == std::vector<std::string>{"owner-icon.tga", "", ""}
        && popup.vulnerable_to_icons == std::vector<std::string>{"", "owner-icon.tga"},
        "P-4: three icons maximum, missing type and missing icon occupy blank slots, owning namespace resolves");
    expect(ui::unit_card_looks("Popup", objects, &text.value(), false).description == "Coruscant\nCoruscant",
           "P-1/P-2: campaign retains ordinary comma-separated description lines");
    const auto empty_mp = ui::unit_card_looks("EmptyMP", objects, &text.value());
    expect(empty_mp.description == "Coruscant\nCoruscant" && empty_mp.good_against_icons.empty()
        && empty_mp.vulnerable_to_icons.empty() && empty_mp.unit_class.empty(),
        "P-1/P-2/P-4: empty MP list falls back to space-separated lines and absent matchups stay absent");
    expect(ui::unit_card_looks("UnknownMP", objects, &text.value()).description.empty(),
           "P-2: nonempty MP list replaces generic text even if localization is missing");
    const auto radar = ui::minimap_type_looks("CardUpgrade", objects);
    expect(!radar.visible && radar.icon == "owner-radar.tga", "radar type looks use the same owning object namespace");

    const auto found = ui::planet_name(std::string("coruscant"), objects, &text.value());
    expect(found.source == ui::PlanetNameSource::text && found.text == "Coruscant"
               && found.text_id == "TEXT_OBJECT_STAR_SYSTEM_CORUSCANT" && found.diagnostics.empty(),
           "a map's planet resolves through its Text_ID, case-insensitively");
    const auto renamed = ui::planet_name(std::string("Modded_System_42"), objects, &text.value());
    expect(renamed.source == ui::PlanetNameSource::text && renamed.text == "Coruscant"
        && renamed.context == "Modded_System_42" && renamed.text_id == found.text_id,
        "a renamed campaign planet resolves its authored Text_ID without a stock name list");
    for (const auto* context : {"Nameless", "Unlisted", "Kuat", "Nowhere"}) {
        const auto fallback = ui::planet_name(std::string(context), objects, &text.value());
        expect(fallback.source == ui::PlanetNameSource::context_name && fallback.text == context
                   && fallback.diagnostics.size() == 1U,
               "no Text_ID, no text entry, a non-planet or no object shows the context name with one warning");
    }
    const auto none = ui::planet_name(std::nullopt, objects, &text.value());
    expect(none.source == ui::PlanetNameSource::none && none.text.empty() && none.diagnostics.size() == 1U,
           "a map without a context name shows no planet name");
    expect(ui::planet_name(std::string("Coruscant"), nullptr, nullptr).source == ui::PlanetNameSource::context_name,
           "without a catalogue the context name is shown");
}

void reinforcement_panes() {
    data::ui::ShellAnchors shell;
    for (std::size_t rows = 0; rows < 5; ++rows)
        shell.add(anchor("pool_ALT" + std::to_string(rows), {0, -100, 194, 100},
                         "MeshAlpha.fx", "panel.tga", true));
    shell.add(anchor("frame", {0, -100, 5, 100}, "MeshAlpha.fx", "frame.tga", true));
    shell.add(anchor("r_close", {50, -280, 100, 20}));
    const std::string xml = R"(<CommandBarComponents><CommandBarComponent Name="r_close">
<Type>TextButton</Type><Icon_Texture_Name>normal.tga</Icon_Texture_Name>
<Mouse_Over_Texture_Name>hover.tga</Mouse_Over_Texture_Name>
<Selected_Texture_Name>pressed.tga</Selected_Texture_Name><Scale>1.25</Scale>
<Font_Name>FixtureFace</Font_Name><Font_Point_Size>9</Font_Point_Size>
<Text_Offset>3 -2</Text_Offset><Text_Color>37 38 47 255</Text_Color>
<Text_Outline>False</Text_Outline><Text_Emboss>True</Text_Emboss><Swap_Texture>True</Swap_Texture>
</CommandBarComponent></CommandBarComponents>)";
    data::ui::CommandBarCatalog catalog;
    std::vector<core::Diagnostic> diagnostics;
    vfs::AssetRecord record;
    record.canonical_path = "data/xml/commandbarcomponents.xml";
    expect(data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(), xml.size())),
               record, catalog, diagnostics).has_value(), "close-button fixture parses");
    assets::Source source;
    const auto database = data::ui::load_text_database(text_file("TEXT_BUTTON_CLOSE", u"Fermer"), source);
    expect(database.has_value(), "localized close label loads");
    if (!database) return;
    const auto pane = ui::reinforce_pane(shell, catalog, &database.value());
    for (std::size_t rows = 0; rows < 5; ++rows) {
        expect(pane.meshes[rows].size() == 2
                   && pane.meshes[rows][0].name == "pool_ALT" + std::to_string(rows)
                   && pane.meshes[rows][1].name == "frame",
               "PU-67: each row count chooses only its own background and shared frame");
    }
    expect(pane.close_label == "Fermer" && pane.close_text_key == "TEXT_BUTTON_CLOSE",
           "PU-72: the close label is resolved from the supplied database, not an English literal");
    expect(pane.close && pane.close->normal == "normal.tga" && pane.close->mouse_over == "hover.tga"
               && pane.close->pressed == "pressed.tga" && near(pane.close->scale, 1.25) && pane.close_swap_texture,
           "PU-72: all button states and their scale come from the component");
    expect(pane.close_text && pane.close_text->text.face == "FixtureFace"
               && pane.close_text->text.point_size == 9 && pane.close_text->text.colour.r == 37
               && pane.close_text->text.colour.g == 38 && pane.close_text->text.emboss
               && !pane.close_text->text.outline && near(pane.close_text->text_offset.x, 3)
               && near(pane.close_text->text_offset.y, -2),
           "PU-72: the label retains its authored font, colour, offset and emboss style");
    const auto missing = ui::reinforce_pane(shell, catalog);
    expect(missing.close_label.empty() && missing.diagnostics.size() == pane.diagnostics.size() + 1,
           "a missing database diagnoses the absent label without substituting hardcoded text");
}

// GHUD-S52: the retail focused organize shell holds one land_art panel per stock terrain index.
void organize_corpus() {
    auto filesystem = test::ui::foc_corpus("organize shell");
    if (!filesystem) return;
    auto catalog = data::ui::load_command_bar(*filesystem);
    if (!catalog) return;
    const auto* component = catalog.value().catalog.find("i_main_organize");
    expect(component != nullptr, "FoC's catalogue names the organize shell");
    if (!component) return;
    auto shell = data::ui::load_shell_anchors(*filesystem, component->text(data::ui::Field::model_name));
    expect(static_cast<bool>(shell), "the FoC organize shell loads");
    if (!shell) return;
    const auto look = ui::hud_shell(shell.value().shell, catalog.value().catalog, ui::HudFaction::empire, true);
    std::set<std::uint32_t> panels;
    for (const auto& mesh : look.meshes) {
        if (mesh.alternate && mesh.name.starts_with("land_art_ALT")) panels.insert(*mesh.alternate);
    }
    std::set<std::uint32_t> stock;
    // land_art_ALT8 (the destroyed-planet alternate) is authored hidden, so it is not a decorative mesh.
    for (std::uint32_t index = 0; index <= 12; ++index) if (index != 8U) stock.insert(index);
    if (panels != stock) {
        for (const auto& anchor : shell.value().shell.anchors()) {
            if (anchor.name.starts_with("land_art_ALT"))
                std::cout << "organize panel " << anchor.name << " shader=" << anchor.shader << " texture=" << anchor.base_texture
                          << " visible=" << anchor.visible << " triangles=" << anchor.triangles.size() << '\n';
        }
    }
    expect(panels == stock, "the organize shell keeps its visible land_art panels whatever the faction");
}

void corpus() {
    auto filesystem = test::ui::foc_corpus("HUD shell");
    if (!filesystem) return;
    auto catalog = data::ui::load_command_bar(*filesystem);
    expect(static_cast<bool>(catalog), "FoC command bar loads");
    if (!catalog) return;
    const auto model = ui::tactical_shell_model(catalog.value().catalog);
    expect(model == "i_tactical_controls.alo", "FoC's i_main_skirmish is the tactical shell");
    auto shell = data::ui::load_shell_anchors(*filesystem, model);
    expect(static_cast<bool>(shell), "the FoC tactical shell loads");
    if (!shell) return;
    const auto timing = ui::encyclopedia_timing(*filesystem);
    expect(timing && near(timing->delay, 0.75) && near(timing->minimum, 1.0) && near(timing->fade_rate, 4.0),
        "encyclopedia timing comes from the mounted effective constants");
    // #349: the retail button art sizes, from MT_CommandBar, around the bones.
    auto atlas = assets::load_mega_texture(*filesystem, ui::command_bar_mega_texture(catalog.value().catalog));
    expect(static_cast<bool>(atlas), "MT_CommandBar's directory loads");
    if (!atlas) return;
    const auto art = [&](const ui::HudShellButton& button) {
        const auto* entry = atlas.value().find(button.normal);
        expect(entry != nullptr, "every drawn button texture is in MT_CommandBar");
        return entry == nullptr ? button.rect
                                : ui::button_quad(button, static_cast<float>(entry->rectangle.width),
                                                  static_cast<float>(entry->rectangle.height));
    };
    const auto retail = ui::hud_shell(shell.value().shell, catalog.value().catalog, ui::HudFaction::rebel);
    expect(retail.options && art(*retail.options) == (data::ui::ReferenceRect{199, 6.5F, 36, 25}),
           "b_option_t draws its 36 x 25 art on its bone at (217, 19), wider than its 24 x 24 mesh");
    // The faceplate's time panel carries divider lines in its texture; FoC hides
    // them under the four panel buttons, whose art must cover the whole panel.
    expect(retail.panel_buttons.size() == 4U, "the retail time panel has four buttons");
    std::vector<data::ui::ReferenceRect> panel;
    for (const auto& button : retail.panel_buttons) panel.push_back(art(button));
    bool covered = true;
    for (float y = 206.5F; y <= 267.0F; y += 0.5F) {
        for (float x = 2.0F; x <= 59.0F; x += 0.5F) {
            covered = covered && std::any_of(panel.begin(), panel.end(), [&](const data::ui::ReferenceRect& rect) {
                return x >= rect.x && x <= rect.right() && y >= rect.y && y <= rect.top();
            });
        }
    }
    expect(covered, "the panel buttons' art covers the faceplate's time panel and its divider lines");
    // #425: 24 unit card slots in 12 columns, each with a health and a shield bar, and a border per
    // column with its four pieces (full, left, centre, right).
    expect(retail.card_slots.size() == 24U && retail.card_borders.size() == 12U, "24 card slots and 12 column borders");
    for (const auto& slot : retail.card_slots) {
        expect(slot.health && slot.shield && slot.health->overlay.size() == 11U && slot.health->max_level == 10
                   && slot.health->smooth && slot.shield->smooth,
               "each card slot has smooth 11-level health and shield bars");
        expect(slot.face == "EmpireAtWar-Medium" && slot.point_size == 6 && slot.outline
                   && slot.count_offset == (data::ui::Vec2{0.0F, -16.0F}),
               "the count text is outlined EmpireAtWar-Medium 6 pt, 16 units below the bone");
    }
    for (const auto& border : retail.card_borders) {
        expect(border.alternates.size() == 4U && border.alternates.front() == "i_special_border_full.tga",
               "each border has its full, left, centre and right pieces");
    }
    if (retail.card_slots.size() == 24U) {
        // A column is two slots: 0 and 1 share an x, 2 is one column to the right.
        const auto& first = retail.card_slots[0].card;
        const auto& second = retail.card_slots[1].card;
        const auto& third = retail.card_slots[2].card;
        std::cout << "card slot 0 " << first.rect.x << ',' << first.rect.y << ' ' << first.rect.width << 'x'
                  << first.rect.height << ", slot 1 " << second.rect.x << ',' << second.rect.y << ", slot 2 "
                  << third.rect.x << ',' << third.rect.y << '\n';
        expect(std::abs(first.rect.x - second.rect.x) < 1.0F && third.rect.x > first.rect.x + 40.0F,
               "slots 2c and 2c + 1 form column c");
    }
    const auto masks = ui::vfs_shell_masks(*filesystem);
    for (const auto faction : {ui::HudFaction::empire, ui::HudFaction::rebel, ui::HudFaction::underworld}) {
        const auto hud = ui::hud_shell(shell.value().shell, catalog.value().catalog, faction);
        expect(hud.diagnostics.empty(), "the FoC shell has every P2-20a part");
        expect(hud.ability_buttons.size() == 24U, "#454: the FoC shell has special_button_00 to _23");
        expect(hud.population && hud.population->text.name == "Text_Planetary_Pop"
            && hud.population->text.point_size == 6 && hud.population->text_offset.x == -10
            && hud.population->right_justified && hud.population->text.colour.r == 255,
            "tactical population retains its own authored component, distinct from galactic Text_Pop_Cap");
        std::cout << "HUD shell corpus " << ui::to_string(faction) << ":";
        for (const auto& mesh : hud.meshes) std::cout << ' ' << mesh.name << '(' << mesh.texture << ')';
        std::cout << '\n';
        expect(hud.meshes.size() == 3U && hud.meshes.back().name == "radar", "droid, faceplate and radar per faction");
        const auto view = ui::hud_view_model(shell.value().shell, catalog.value().catalog, ui::alt_variant(faction), masks);
        expect(view.diagnostics.empty() && !view.faceplates.empty(), "the faction's faceplate masks read from the game");
        // UI-I2 on the real mask: the minimap well is opaque art, the sky over
        // the unit-card strip's right end is not.
        expect(view.hit_test({100, 100}) && !view.hit_test({700, 250}), "the faceplate mask stops only on art");
    }
    auto objects = data::load_catalog(*filesystem, data::Profile::foc);
    auto text = data::ui::load_language_text_database(*filesystem, "ENGLISH");
    expect(objects && text, "FoC objects and text load");
    if (!objects || !text) return;
    // CARD-20: inventory the actual predicate over every effective stock type
    // that the existing space mouse-admission path can publish for world hover.
    std::size_t hover_types{};
    std::size_t admitted_types{};
    std::vector<std::string> excluded_types;
    for (const auto& definition : objects.value().catalog.definitions()) {
        if (!definition.namespace_winner || definition.category != data::Category::game_object) continue;
        const auto effective = objects.value().catalog.resolve(definition.id, data::Category::game_object);
        if (!effective) continue;
        const auto field = [&](const std::string_view tag) {
            const auto* found = effective.value().value(tag);
            return found ? std::string_view(found->value.raw_text) : std::string_view{};
        };
        const auto& kind = definition.type_name;
        if (field("Space_Model_Name").empty() && field("SpaceBehavior").empty()
            && kind != "SpaceUnit" && kind != "SpaceProp" && kind != "SpaceStructure"
            && kind != "StarBase" && kind != "Squadron") continue;
        const auto flag = [&](const std::string_view tag, const bool fallback = false) {
            return data::retail::boolean(field(tag)).value_or(fallback);
        };
        std::set<std::string> behaviors;
        for (const auto tag : {std::string_view{"Behavior"}, std::string_view{"SpaceBehavior"}}) {
            const auto raw = field(tag);
            auto begin = raw.find_first_not_of(" ,\t\r\n");
            while (begin != std::string_view::npos) {
                const auto end = raw.find_first_of(" ,\t\r\n", begin);
                std::string token(raw.substr(begin, end == std::string_view::npos ? end : end - begin));
                for (char& c : token) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                behaviors.insert(std::move(token));
                begin = end == std::string_view::npos ? end : raw.find_first_not_of(" ,\t\r\n", end);
            }
        }
        const bool mouse = behaviors.contains("SELECTABLE") || behaviors.contains("TACTICAL_BUILD_OBJECTS")
            || behaviors.contains("DUMMY_TOOLTIP") || flag("Is_Impassable_Asteroid")
            || (flag("Collidable_By_Projectile_Living") && flag("Is_Valid_Target", true));
        if (!mouse) continue;
        ++hover_types;
        if (ui::world_encyclopedia_admitted(definition.id, &objects.value().catalog)) ++admitted_types;
        else excluded_types.push_back(definition.id + " [" + kind + "]");
    }
    std::sort(excluded_types.begin(), excluded_types.end());
    std::cout << "CARD-20 stock world-hover audit: " << hover_types << " mouse-admitted types, "
              << admitted_types << " encyclopedia-admitted, " << excluded_types.size() << " excluded\n";
    for (const auto& type : excluded_types) std::cout << "CARD-20 excluded: " << type << '\n';
    const auto reactor = ui::unit_card_looks("US_BlackMarket_Reactors_L1_Upgrade", &objects.value().catalog, &text.value());
    expect(reactor.icon == "i_multi_black_market_eng1.tga" && reactor.build_cost == 850,
           "stock same-name Underworld build card keeps its owning upgrade icon and price");
    const auto name = ui::planet_name(std::string("Coruscant"), &objects.value().catalog, &text.value());
    expect(name.source == ui::PlanetNameSource::text && name.text == "Coruscant"
               && name.text_id == "TEXT_OBJECT_STAR_SYSTEM_CORUSCANT",
           "_mp_space_coruscant's planet resolves through Planets.xml and the text DB");
    const auto construction = ui::unit_card_looks("UC_Empire_Mineral_Extractor", &objects.value().catalog, &text.value());
    expect(!construction.name.empty() && !construction.description.empty(),
           "WBP-36 construction portraits resolve their localized name and description");
    const auto mine = ui::unit_card_looks("Empire_Mineral_Extractor", &objects.value().catalog, &text.value());
    expect(mine.description.find('\n') != std::string::npos,
           "WBP-36 comma-separated multiplayer encyclopedia entries retain every paragraph");
}

} // namespace

void hud_shell_contracts() {
    const auto fly_style = ui::hud_flytext_style(synthetic_catalog());
    expect(fly_style && fly_style->text.face == "Custom-Flytext" && fly_style->text.point_size == 11
        && fly_style->text.colour.r == 20 && fly_style->text.colour.a == 80
        && fly_style->text.emboss && fly_style->text.outline && fly_style->left_justified
        && fly_style->text_offset.x == 3 && fly_style->text_offset.y == -5,
        "HCB-64: flytext uses the shared XML readout style, including mod font/colour/offset overrides");
    expect(!ui::hud_flytext_style(data::ui::CommandBarCatalog{}), "HCB-64: missing flytext stays absent");
    ui::HudFlyTextView award{"+250", 100, {320, 240}, false};
    const auto fresh = ui::hud_flytext_presentation(award, 100);
    const auto held = ui::hud_flytext_presentation(award, 130);
    const auto faded = ui::hud_flytext_presentation(award, 145);
    const auto end = ui::hud_flytext_presentation(award, 160);
    expect(fresh && fresh->text == "+250" && fresh->alpha == 255 && fresh->position.y == 240
        && held && held->alpha == 255 && near(held->position.y, 205)
        && faded && faded->alpha == 127 && near(faded->position.y, 187.5)
        && end && end->alpha == 0 && near(end->position.y, 170)
        && !ui::hud_flytext_presentation(award, 161) && !ui::hud_flytext_presentation(award, 99),
        "HCB-64: logical age moves 70 screen pixels over 60 frames, fades after 30, expires after 60");
    award.behind_camera = true;
    expect(!ui::hud_flytext_presentation(award, 100), "HCB-64: a behind-camera projection hides the feedback");
    award.behind_camera = false;
    award.text.clear();
    expect(!ui::hud_flytext_presentation(award, 100), "HCB-64: an empty caller value has no feedback");
    award.text = "+1";
    award.created_frame = UINT64_MAX - 30;
    expect(ui::hud_flytext_presentation(award, UINT64_MAX)->alpha == 255,
        "HCB-64: frame subtraction remains safe at the integer limit");
    {
        test::ui::TempTree tree("queue-timing");
        const std::array mounts{vfs::MountSpec{"base", tree.root, "data", {}}};
        for (const auto& [tint, delay] : std::array{
            std::pair{"255, 128, 128, 215", "750"}, std::pair{"12, 34, 56, 78", "125"}}) {
            test::ui::write_text(tree.root / "XML/GameConstants.xml", "<GameConstants><Right_Queue_Tint>"
                + std::string(tint) + "</Right_Queue_Tint><Encyclopedia_Delay>" + delay + "</Encyclopedia_Delay>"
                "<Encyclopedia_Min_Display_Time>1</Encyclopedia_Min_Display_Time>"
                "<Encyclopedia_Fade_Rate>4</Encyclopedia_Fade_Rate></GameConstants>");
            auto mounted = vfs::Vfs::mount(mounts);
            expect(static_cast<bool>(mounted), "queue and timing fixture mounts");
            if (!mounted) continue;
            const auto colour = ui::production_queue_tint(mounted.value());
            auto timing = ui::encyclopedia_timing(mounted.value());
            const bool modified = std::string_view(delay) == "125";
            expect(colour && *colour == (modified ? data::ui::Rgba8{12, 34, 56, 78} : data::ui::Rgba8{255, 128, 128, 215}),
                "PU-63: perturbed XML tint reaches production presentation");
            expect(timing && std::abs(timing->delay - (modified ? 0.125 : 0.75)) < 0.00001,
                "CARD-18: perturbed XML milliseconds become hover seconds");
            if (timing) {
                timing->service(5, 0.0, true);
                timing->service(5, 0.2, true);
                expect((timing->shown == 5) == modified, "CARD-18: XML delay changes hover admission");
            }
        }
        for (const auto raw : {"", "-1,2,3,4", "1,2,3,256", "1,2,3,4,5", "1,2,three,4", "1.5,2,3,4", "1,2,3,99999999999999999"}) {
            test::ui::write_text(tree.root / "XML/GameConstants.xml", "<GameConstants><Right_Queue_Tint>"
                + std::string(raw) + "</Right_Queue_Tint></GameConstants>");
            auto mounted = vfs::Vfs::mount(mounts);
            expect(mounted && !ui::production_queue_tint(mounted.value()), "malformed queue tint has no stock fallback");
        }
    }
    {
        test::ui::TempTree tree("movie-letterbox");
        const std::array mounts{vfs::MountSpec{"base", tree.root, "data", {}}};
        for (const auto& [value, valid] : std::array{
            std::pair{"0.085f", true}, std::pair{"0.12", true}, std::pair{"0", true},
            std::pair{"-0.1", false}, std::pair{"0.5", false}, std::pair{"nan", false},
            std::pair{"0.1junk", false}}) {
            test::ui::write_text(tree.root / "XML/GameConstants.xml",
                std::string("<GameConstants><Camera_FX_Manager_Letterbox_Height> ") + value
                + " </Camera_FX_Manager_Letterbox_Height></GameConstants>");
            auto mounted = vfs::Vfs::mount(mounts);
            expect(static_cast<bool>(mounted), "movie letterbox fixture mounts");
            if (!mounted) continue;
            const auto height = ui::movie_letterbox_height(mounted.value());
            expect(static_cast<bool>(height) == valid, "HCB-11c: letterbox validates the authored fraction");
            if (height && std::string_view(value) == "0.12")
                expect(near(height.value(), 0.12), "HCB-11c: a mod's letterbox height reaches presentation");
        }
    }
    {
        test::ui::TempTree tree("beacon-delay");
        const std::array mounts{vfs::MountSpec{"base", tree.root, "data", {}}};
        for (const auto& [value, valid] : std::array{
            std::pair{"5000", true}, std::pair{"1200", true}, std::pair{"0", true},
            std::pair{"-1", false}, std::pair{"nan", false}, std::pair{"5000ms", false}}) {
            test::ui::write_text(tree.root / "XML/GameConstants.xml",
                std::string("<GameConstants><BeaconPlaceDelay> ") + value + " </BeaconPlaceDelay></GameConstants>");
            auto mounted = vfs::Vfs::mount(mounts);
            expect(static_cast<bool>(mounted), "beacon delay fixture mounts");
            if (!mounted) continue;
            const auto delay = ui::beacon_place_delay(mounted.value());
            expect(delay.has_value() == valid, "HCB-12a: beacon delay validates authored milliseconds");
            if (delay && std::string_view(value) == "1200")
                expect(near(*delay, 1.2), "HCB-12a: a mod's beacon delay reaches presentation in seconds");
        }
    }
    const auto tech_shell = ui::hud_shell(synthetic_shell(), synthetic_catalog(), ui::HudFaction::rebel);
    expect(tech_shell.tactical_tech && tech_shell.tactical_tech->text.face == "Arial Bold"
        && tech_shell.tactical_tech->text.point_size == 8 && tech_shell.tactical_tech->left_justified
        && tech_shell.tactical_tech->origin.x == 64 && tech_shell.tactical_tech->text_offset.y == -2,
        "HCB-61: tactical tech uses authored font, origin, offset and left justification");
    expect(ui::tactical_tech_text("Technologie", {4, ui::HudFaction::rebel, false, true}) == "Technologie 4",
        "HCB-61: campaign tech uses the supplied localized label and displayed level");
    expect(ui::tactical_tech_text("Tech", {3, ui::HudFaction::empire, true, true}).empty(),
        "HCB-61: multiplayer space hides tech");
    expect(ui::tactical_tech_text("Tech", {3, ui::HudFaction::underworld, false, true}).empty(),
        "HCB-61: Consortium campaign hides tech");
    expect(ui::tactical_tech_text("Tech", {3, ui::HudFaction::underworld, true, false}) == "Tech 3",
        "HCB-61: multiplayer land retains tech for Consortium");
    expect(tech_shell.reinforcement_counter && tech_shell.garrison_respawn_counter
        && tech_shell.reinforcement_counter->button.normal == "reinforce.tga"
        && tech_shell.reinforcement_counter->button.blank == "dark.tga"
        && tech_shell.reinforcement_counter->build == "timer.tga"
        && near(tech_shell.reinforcement_counter->button.scale, 0.6)
        && tech_shell.reinforcement_counter->colour.a == 40
        && tech_shell.garrison_respawn_counter->button.normal == "garrison.tga"
        && near(tech_shell.garrison_respawn_counter->button.scale, 0.9),
        "HCB-62: independent timer art is authored without any shell or unit-card anchor");
    expect(!ui::hud_timer_style(synthetic_catalog(), "unknown")
        && !ui::hud_timer_style(synthetic_catalog(), "s_select_00"),
        "HCB-62: missing components and shell-bound cards are not timer styles");
    expect(ui::hud_timer_progress(100, 700, 600) == 0.0
        && ui::hud_timer_progress(400, 700, 600) == 0.5
        && !ui::hud_timer_progress(700, 700, 600)
        && !ui::hud_timer_progress(800, 700, 600)
        && !ui::hud_timer_progress(1, std::nullopt, 600)
        && !ui::hud_timer_progress(1, 2, 0),
        "WSL-29: timer progress starts at zero and disappears at expiry, absent wait or zero delay");
    expect(ui::hud_timer_progress(0, UINT64_MAX, 600) == 0.0,
        "HCB-62: a future deadline clamps without unsigned underflow");
    ui::EncyclopediaTiming timing{0.75, 1.0, 4.0};
    timing.service(5, 0.0, true);
    timing.service(5, 0.74, true);
    expect(timing.shown == 0, "encyclopedia waits for the complete hover delay");
    timing.service(5, 0.01, true);
    expect(timing.shown == 5 && timing.alpha > 0, "encyclopedia admits the delayed candidate");
    timing.service(5, 0.25, true);
    expect(timing.alpha == 1, "encyclopedia fades in at the authored rate");
    timing.service(0, 0.25, true);
    expect(timing.shown == 5, "encyclopedia keeps its minimum display time after hover leaves");
    timing.service(0, 1.0, false);
    expect(timing.shown == 0 && timing.alpha == 0, "an unadmitted candidate cannot restart the popup");
    timing = {0.75, 1.0, 4.0};
    timing.service(5, 0, true);
    timing.service(5, 0.5, true);
    timing.service(6, 0.5, true);
    expect(timing.candidate == 6 && timing.hovered_seconds == 0 && timing.shown == 0,
           "switching hover identities discards the previous candidate's delay");
    timing.service(6, 0.5, true);
    expect(timing.shown == 0, "the replacement candidate waits for its own full delay");
    timing.service(6, 0.25, true);
    expect(timing.shown == 6 && timing.remaining == 1 && timing.alpha == 1,
           "the replacement candidate starts its own minimum display time");
    timing.service(7, 0.25, true);
    expect(timing.candidate == 7 && timing.hovered_seconds == 0 && timing.shown == 6,
           "a new hover identity keeps the displayed popup while restarting admission");
    timing.service(7, 0.5, true);
    expect(timing.shown == 6 && timing.alpha == 1, "the old popup remains during the new hover delay");
    timing.service(7, 0.25, true);
    expect(timing.shown == 7 && timing.remaining == 1 && timing.alpha == 1,
           "switching the displayed popup restarts its minimum time and fade");
    timing.service(7, 0.5, false);
    expect(timing.candidate == 0 && timing.shown == 7 && timing.alpha == 1,
           "disabled admission clears the candidate but retains minimum display");
    timing.service(7, 0.375, false);
    expect(timing.shown == 7 && timing.alpha == 1, "the popup holds until its minimum expires");
    timing.service(7, 0.125, false);
    expect(timing.shown == 7 && timing.remaining == 0 && timing.alpha == 0.5,
           "disabled admission fades the old popup at the authored rate");
    timing.service(7, 0.125, false);
    expect(timing.shown == 0 && timing.alpha == 0, "fade completion clears the displayed identity");
    timing.service(7, 0.5, true);
    expect(timing.candidate == 7 && timing.hovered_seconds == 0 && timing.shown == 0,
           "reenabling admission starts a fresh hover delay");
    timing.service(7, 0.5, true);
    expect(timing.shown == 0, "disabled elapsed time does not count toward admission");
    timing.service(7, 0.25, true);
    expect(timing.shown == 7 && timing.alpha == 1, "the readmitted identity appears after its own delay");
    const auto special = ui::hud_shell(synthetic_shell(), synthetic_catalog(), ui::HudFaction::rebel).special_weapon;
    expect(special && special->button.alternates == std::vector<std::string>{"ion.tga", "hyper.tga", "plasma.tga"}
        && special->build == "dial.tga" && special->button.rect.width == 30,
        "WSW-28/29: special slot retains authored faction icons, dial and hit rectangle");
    for (const auto faction : {ui::HudFaction::rebel, ui::HudFaction::empire, ui::HudFaction::underworld}) {
        const auto shell = ui::hud_shell(synthetic_shell(), synthetic_catalog(), faction);
        const auto expected = faction == ui::HudFaction::rebel ? "ion.tga"
            : faction == ui::HudFaction::empire ? "hyper.tga" : "plasma.tga";
        expect(shell.special_weapon && ui::special_weapon_icon(shell.special_weapon->button, faction) == expected,
               "WSW-28: each faction selects its authored HUD alternate, independently of enum order");
    }
    ui::HudShellButton partial;
    partial.normal = "weapon-build-icon.tga";
    partial.alternates = {"custom-ion.tga", "custom-hyper.tga"};
    expect(ui::special_weapon_icon(partial, ui::HudFaction::rebel) == "custom-ion.tga"
        && ui::special_weapon_icon(partial, ui::HudFaction::empire) == "custom-hyper.tga"
        && ui::special_weapon_icon(partial, ui::HudFaction::underworld).empty(),
        "WSW-28: custom alternates retain their identity; missing slots do not use the build icon");
    partial.alternates.clear();
    expect(ui::special_weapon_icon(partial, ui::HudFaction::rebel).empty(),
           "WSW-28: absent HUD alternates stay absent");
    retreat_button();
    shell_parts();
    organize_shell_alternates();
    allegiance_emblem_indices();
    shell_layout();
    planet_names();
    reinforcement_panes();
    corpus();
    organize_corpus();
}
