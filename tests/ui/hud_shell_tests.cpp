// P2-20a (#83): the tactical HUD shell model. Synthetic shells pin which meshes
// and parts are drawn and where they land at 1280x720 and 1920x1080 under both
// rule sets; with EAWR_EAW_GAME_ROOT set, the FoC shell and Coruscant's planet
// name are read from the game.

#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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
    return shell;
}

data::ui::CommandBarCatalog synthetic_catalog() {
    const std::string xml = R"(<CommandBarComponents>
<CommandBarComponent Name="i_main_commandbar"><Type>Shell</Type><Model_Name>i_galactic_controls.alo</Model_Name>
  <Mega_Texture_Name>MT_CommandBar</Mega_Texture_Name></CommandBarComponent>
<CommandBarComponent Name="i_main_skirmish"><Type>Shell</Type><Model_Name>i_tactical_controls.alo</Model_Name></CommandBarComponent>
<CommandBarComponent Name="radar"><Type>Button</Type><Size>171 175</Size></CommandBarComponent>
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
</CommandBarComponents>)";
    data::ui::CommandBarCatalog catalog;
    std::vector<core::Diagnostic> diagnostics;
    vfs::AssetRecord record;
    record.canonical_path = "data/xml/commandbarcomponents.xml";
    const auto parsed =
        data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(), xml.size())), record, catalog, diagnostics);
    expect(parsed.has_value() && diagnostics.empty(), "synthetic command bar parses cleanly");
    return catalog;
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
    expect(rebel.ability_buttons.empty(), "#454: a shell without special_button_NN draws no ability buttons");
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
    expect(partial.diagnostics.size() == 8U && !partial.minimap && !partial.options && !partial.planet_name
               && partial.panel_buttons.empty(),
           "a missing faceplate, radar, options button, panel button and planet name are reported once each");

    for (const auto* text : {"empire", "Rebel", "UNDERWORLD"}) expect(ui::hud_faction_from(text).has_value(), "factions parse");
    expect(!ui::hud_faction_from("pirate"), "an unknown faction is refused");
    expect(ui::alt_variant(ui::HudFaction::underworld) == 2U, "Underworld is ALT2");
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
                                              " </Text_ID></Planet><Planet Name=\"Nameless\"/>"
                                              "<Planet Name=\"Unlisted\"><Text_ID>TEXT_NOT_THERE</Text_ID></Planet></Planets>");
    for (const auto* registry : {"HardpointDataFiles.xml", "FactionFiles.xml", "CampaignFiles.xml", "SFXEventFiles.xml"})
        test::ui::write_text(xml / registry, "<Files/>");
    test::ui::write_text(xml / "units.xml", "<Units><SpaceUnit Name=\"Kuat\"><Text_ID>TEXT_UNIT</Text_ID></SpaceUnit></Units>");
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

    const auto found = ui::planet_name(std::string("coruscant"), objects, &text.value());
    expect(found.source == ui::PlanetNameSource::text && found.text == "Coruscant"
               && found.text_id == "TEXT_OBJECT_STAR_SYSTEM_CORUSCANT" && found.diagnostics.empty(),
           "a map's planet resolves through its Text_ID, case-insensitively");
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
    const auto name = ui::planet_name(std::string("Coruscant"), &objects.value().catalog, &text.value());
    expect(name.source == ui::PlanetNameSource::text && name.text == "Coruscant"
               && name.text_id == "TEXT_OBJECT_STAR_SYSTEM_CORUSCANT",
           "_mp_space_coruscant's planet resolves through Planets.xml and the text DB");
}

} // namespace

void hud_shell_contracts() {
    shell_parts();
    shell_layout();
    planet_names();
    corpus();
}
