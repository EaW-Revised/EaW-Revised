// Command-bar catalogue contracts (UI-03 #170). The synthetic component files
// are invented here; with EAWR_EAW_GAME_ROOT set, the FoC catalogue is also
// loaded read-only and its counts pinned against design section 1.1.

#include "eawr/data/ui/command_bar.hpp"

#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
using data::ui::ComponentType;
using data::ui::Field;
using data::ui::FieldKind;

void expect(const bool condition, const std::string& message) {
    test::ui::expect(condition, message.c_str());
}

// Mounts a temporary tree whose Data folder is <root>/data.
std::optional<vfs::Vfs> mount(const std::filesystem::path& root) {
    const std::array specs{vfs::MountSpec{"base", root / "data", "data", {}}};
    auto mounted = vfs::Vfs::mount(specs);
    expect(static_cast<bool>(mounted), "temporary tree mounts");
    if (!mounted) return std::nullopt;
    return std::move(mounted.value());
}

std::size_t count_code(const std::vector<core::Diagnostic>& diagnostics, const std::string_view code) {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
        [&](const core::Diagnostic& diagnostic) { return diagnostic.code == code; }));
}

constexpr std::string_view list_xml = R"(<?xml version="1.0" ?>
<CommandBar_Component_Files>
	<File>Components_A.xml</File>
	<File>components_b.xml</File>
	<File>Missing.xml</File>
</CommandBar_Component_Files>
)";

constexpr std::string_view file_a = R"(<?xml version="1.0"?>
<CommandBarComponents>
    <CommandBarComponent Name="i_main_skirmish">
        <Model_Name>i_tactical_controls.alo</Model_Name>
        <Type>Shell</Type>
        <Group>Shells</Group>
        <Model_Offset_X>True</Model_Offset_X>
        <Hidden> True </Hidden>
    </CommandBarComponent>
    <CommandBarComponent Name="b_option_t">
        <Type>TextButton</Type>
        <Icon_Texture_Name>i_options.tga</Icon_Texture_Name>
        <Tooltip_Text>TEXT_BUTTON_MAIN_MENU</Tooltip_Text>
        <Click_Shift>True</Click_Shift>
        <Click_Shift>False</Click_Shift>
        <Font_Name>Arial   Bold</Font_Name>
        <Font_Point_Size>7</Font_Point_Size>
        <Text_Color>255 212 33 255</Text_Color>
        <Text_Color2>247 198 0</Text_Color2>
        <Color>300 0 -1 128</Color>
        <Text_Offset>-10 0</Text_Offset>
        <Icon_Offset>15</Icon_Offset>
        <Size>37` 10</Size>
        <Scale>1.0</Scale>
        <Base_Layer>5</Base_Layer>
        <Base_Layer></Base_Layer>
        <Snap_Drag>maybe</Snap_Drag>
        <Alternate_Font_Name>EmpireAtWar-Medium, Arial Bold ,EmpireAtWar-Bold</Alternate_Font_Name>
        <Mystery_Tag>kept</Mystery_Tag>
    </CommandBarComponent>
    <CommandBarComponent Name="special_border_00">
        <Type>textbutton</Type>
        <Icon_ALternate_Texture_Name>
            i_border_full.tga
            i_border_left.tga <!-- a comment splits the text -->
            i_border_right.tga
        </Icon_ALternate_Texture_Name>
        <Group>Fleet 0</Group>
        <Group>Tactical_Selection</Group>
        <Group>fleet 0</Group>
        <Group></Group>
        <Tooltip_Text></Tooltip_Text>
        <Tooltip_Text>TEXT_A TEXT_B</Tooltip_Text>
    </CommandBarComponent>
    <CommandBarComponent Name="s_health_00"><Type>Bar</Type><Max_Bar_Level>10</Max_Bar_Level><Smooth_Bar>true</Smooth_Bar></CommandBarComponent>
    <CommandBarComponent Name="radar"><Type>Button</Type><Size>171 175</Size></CommandBarComponent>
    <CommandBarComponent Name="Text_Allegiance_A"><Type>Icon</Type><Cross_Fade>True</Cross_Fade></CommandBarComponent>
    <CommandBarComponent Name="land_art_ALT0"><Type>Button</Type></CommandBarComponent>
    <CommandBarComponent Name="no_type"><Group>Shells</Group></CommandBarComponent>
    <CommandBarComponent Name="odd_type"><Type>Slider</Type></CommandBarComponent>
    <CommandBarComponent><Type>Button</Type></CommandBarComponent>
    <Stray/>
</CommandBarComponents>
)";

constexpr std::string_view file_b = R"(<?xml version="1.0"?>
<CommandBarComponents>
    <CommandBarComponent Name="RADAR"><Type>Button</Type><Size>175 175</Size></CommandBarComponent>
    <CommandBarComponent Name="b_camera_t"><Type>Button</Type></CommandBarComponent>
</CommandBarComponents>
)";

void schema_contracts() {
    expect(data::ui::field_count == 88U, "the schema covers the 88 FoC value tags besides Type and Group");
    for (std::size_t i = 0; i < data::ui::field_count; ++i) {
        const auto field = static_cast<Field>(i);
        const auto round_trip = data::ui::field_from_tag(data::ui::to_string(field));
        expect(round_trip && *round_trip == field, "field tag round-trips: " + std::string(data::ui::to_string(field)));
    }
    expect(data::ui::field_from_tag("icon_alternate_texture_name") == Field::icon_alternate_texture_name,
           "tags match case-insensitively");
    expect(!data::ui::field_from_tag("Type") && !data::ui::field_from_tag("Group"),
           "Type and Group are structural, not fields");
    expect(data::ui::field_kind(Field::font_name) == FieldKind::text, "Font_Name is one string");
    expect(data::ui::field_kind(Field::blank_texture_name) == FieldKind::tokens, "texture names are lists");
    expect(data::ui::field_kind(Field::text_color) == FieldKind::color, "Text_Color is a colour");
    expect(data::ui::field_kind(Field::size) == FieldKind::vec2, "Size is a vector");
    expect(data::ui::component_type_from("TEXTBUTTON") == ComponentType::text_button, "types match case-insensitively");
    expect(!data::ui::component_type_from("Slider"), "unknown type is rejected");
}

void alt_contracts() {
    const auto alt = data::ui::split_alt("Empire_Faceplate_ALT0");
    expect(alt.base == "Empire_Faceplate" && alt.variant == 0U, "ALT0 suffix splits");
    expect(data::ui::split_alt("b_Help_Droid_Underworld_alt2").variant == 2U, "suffix matches case-insensitively");
    expect(data::ui::split_alt("x_ALT12").variant == 12U, "multi-digit variant");
    expect(!data::ui::split_alt("b_ALT").variant, "suffix needs digits");
    expect(!data::ui::split_alt("_ALT1").variant, "suffix needs a base name");
    expect(!data::ui::split_alt("b_alternate0").variant, "only an exact _ALT suffix counts");
    expect(!data::ui::split_alt("radar").variant && data::ui::split_alt("radar").base == "radar", "plain names pass");
    expect(alt.shown_for(data::ui::alt_variant::empire) && !alt.shown_for(data::ui::alt_variant::rebel),
           "ALT0 shows only for Empire");
    expect(data::ui::split_alt("radar").shown_for(data::ui::alt_variant::underworld), "plain names show for all");
}

void fixture_contracts() {
    test::ui::TempTree tree("command-bar");
    test::ui::write_text(tree.root / "data/xml/CommandBarComponentFiles.xml", list_xml);
    test::ui::write_text(tree.root / "data/xml/Components_A.xml", file_a);
    test::ui::write_text(tree.root / "data/xml/Components_B.xml", file_b);
    auto filesystem = mount(tree.root);
    if (!filesystem) return;
    auto loaded = data::ui::load_command_bar(*filesystem);
    expect(static_cast<bool>(loaded), "synthetic command bar loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    const auto& diagnostics = loaded.value().diagnostics;
    namespace codes = data::ui::diagnostic_codes;

    expect(catalog.source_files().size() == 2U, "both present files are read in list order");
    expect(count_code(diagnostics, codes::command_bar_file) == 1U, "the missing listed file is reported");
    expect(catalog.components().size() == 8U, "valid components are kept, invalid ones skipped");
    expect(count_code(diagnostics, codes::component_invalid) == 3U, "no Type, unknown Type and no Name are reported");
    expect(catalog.count(ComponentType::button) == 3U && catalog.count(ComponentType::text_button) == 2U &&
               catalog.count(ComponentType::bar) == 1U && catalog.count(ComponentType::shell) == 1U &&
               catalog.count(ComponentType::icon) == 1U,
           "component types are counted");

    const auto* option = catalog.find("B_OPTION_T");
    expect(option != nullptr, "find is case-insensitive");
    if (option) {
        expect(option->type == ComponentType::text_button, "TextButton type");
        expect(!option->flag(Field::click_shift, true), "repeated flag: last value wins");
        expect(option->text(Field::font_name) == "Arial Bold", "text keeps inner spaces, collapsed");
        expect(option->integer(Field::font_point_size) == 7, "integer field");
        expect(option->color(Field::text_color) == data::ui::Rgba8{255, 212, 33, 255}, "four-component colour");
        expect(option->color(Field::text_color2) == data::ui::Rgba8{247, 198, 0, 255}, "three-component colour is opaque");
        expect(option->color(Field::color) == data::ui::Rgba8{255, 0, 0, 128}, "colour channels clamp to 0..255");
        expect(option->vec2(Field::text_offset) == data::ui::Vec2{-10.0F, 0.0F}, "vector field");
        expect(option->vec2(Field::icon_offset) == data::ui::Vec2{15.0F, 0.0F}, "one-component vector has y = 0");
        expect(option->vec2(Field::size) == data::ui::Vec2{37.0F, 10.0F}, "stray character after a number is dropped");
        expect(option->number(Field::scale) == 1.0F, "number field");
        expect(!option->integer(Field::base_layer), "an empty repeat clears a number (derived)");
        expect(!option->has(Field::snap_drag), "an unreadable flag is not stored");
        const auto fonts = option->list(Field::alternate_font_name);
        expect(fonts.size() == 3U && fonts[1] == "Arial Bold", "comma list keeps names with spaces");
        const auto icon = option->list(Field::icon_texture_name);
        expect(icon.size() == 1U && icon[0] == "i_options.tga", "single texture is a one-element list");
        expect(option->unknown_fields.size() == 1U && option->unknown_fields[0].tag == "Mystery_Tag" &&
                   option->unknown_fields[0].value == "kept",
               "unknown tags are kept unparsed");
        expect(option->groups.empty(), "no Group means no groups");
        expect(option->line > 1U, "component keeps its source line");
    }
    expect(count_code(diagnostics, codes::field_unknown) == 2U, "unknown tag and stray element are reported");
    // Click_Shift, Base_Layer and Tooltip_Text repeat.
    expect(count_code(diagnostics, codes::field_repeated) == 3U, "each repeated tag is reported");
    // Color clamp, Icon_Offset, Size, empty Base_Layer and Snap_Drag.
    expect(count_code(diagnostics, codes::field_value) == 5U, "each unreadable or partial value is reported");

    const auto* border = catalog.find("special_border_00");
    expect(border != nullptr, "component with a lower-case type loads");
    if (border) {
        const auto textures = border->list(Field::icon_alternate_texture_name);
        expect(textures.size() == 3U && textures[2] == "i_border_right.tga",
               "misspelt-case tag and comment-split list parse as one list");
        expect(border->groups == std::vector<std::string>{"Fleet 0", "Tactical_Selection"},
               "every Group is a membership, deduplicated case-insensitively, empty ignored");
        expect(border->in_group("FLEET 0"), "group lookup is case-insensitive");
        const auto tooltips = border->list(Field::tooltip_text);
        expect(tooltips.size() == 2U && tooltips[0] == "TEXT_A", "tooltip keys are a list; last repeat wins");
    }

    const auto* radar = catalog.find("radar");
    expect(radar != nullptr && radar->vec2(Field::size) == data::ui::Vec2{175.0F, 175.0F},
           "a later file's component replaces the earlier one");
    expect(count_code(diagnostics, codes::component_duplicate) == 1U, "the replaced component is reported");
    expect(catalog.components().size() > 4U && catalog.components()[4].name == "RADAR", "the replacement keeps the original position");

    const auto* land = catalog.find("land_art_ALT0");
    expect(land && land->alt.base == "land_art" && land->alt.variant == 0U, "component names carry ALT variants");
    const auto* shell = catalog.shell_for_model("I_Tactical_Controls.alo");
    expect(shell && shell->name == "i_main_skirmish" && shell->flag(Field::hidden) &&
               shell->flag(Field::model_offset_x) && !shell->flag(Field::model_offset_y),
           "shell component is found by model name");
    const auto* bar = catalog.find("s_health_00");
    expect(bar && bar->integer(Field::max_bar_level) == 10 && bar->flag(Field::smooth_bar), "Bar fields");
}

void failure_contracts() {
    {
        test::ui::TempTree tree("command-bar-empty");
        test::ui::write_text(tree.root / "data/xml/other.xml", "<x/>");
        auto filesystem = mount(tree.root);
        if (!filesystem) return;
        auto loaded = data::ui::load_command_bar(*filesystem);
        expect(!loaded && loaded.error().code == data::ui::diagnostic_codes::command_bar_list,
               "a missing file list fails the load");
    }
    {
        test::ui::TempTree tree("command-bar-malformed");
        test::ui::write_text(tree.root / "data/xml/CommandBarComponentFiles.xml",
                   "<CommandBar_Component_Files><File>Bad.xml</File></CommandBar_Component_Files>");
        test::ui::write_text(tree.root / "data/xml/Bad.xml", "<CommandBarComponents><CommandBarComponent Name=\"x\">");
        auto filesystem = mount(tree.root);
        if (!filesystem) return;
        auto loaded = data::ui::load_command_bar(*filesystem);
        expect(loaded && loaded.value().catalog.components().empty() &&
                   count_code(loaded.value().diagnostics, data::ui::diagnostic_codes::command_bar_file) == 1U,
               "a malformed listed file is reported and skipped");
    }
    {
        test::ui::TempTree tree("command-bar-doctype");
        test::ui::write_text(tree.root / "data/xml/CommandBarComponentFiles.xml",
                   "<!DOCTYPE x [<!ENTITY a \"b\">]><CommandBar_Component_Files/>");
        auto filesystem = mount(tree.root);
        if (!filesystem) return;
        expect(!data::ui::load_command_bar(*filesystem), "a DOCTYPE in the file list is refused");
    }
}

void command_bar_corpus() {
    auto filesystem = test::ui::foc_corpus("command bar");
    if (!filesystem) return;
    auto loaded = data::ui::load_command_bar(*filesystem);
    expect(static_cast<bool>(loaded), "FoC command bar loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    const auto& diagnostics = loaded.value().diagnostics;
    std::map<std::string, std::size_t> by_code;
    std::size_t errors = 0;
    for (const auto& diagnostic : diagnostics) {
        ++by_code[diagnostic.code];
        if (diagnostic.severity == core::Severity::error) {
            ++errors;
            std::cerr << "  " << core::format_diagnostic(diagnostic) << '\n';
        }
    }
    std::size_t fields = 0;
    std::size_t unknown = 0;
    for (const auto& component : catalog.components()) {
        fields += component.fields.size();
        unknown += component.unknown_fields.size();
    }
    std::cout << "command bar corpus: " << catalog.components().size() << " components ("
              << catalog.count(ComponentType::text_button) << " TextButton, " << catalog.count(ComponentType::button)
              << " Button, " << catalog.count(ComponentType::bar) << " Bar, " << catalog.count(ComponentType::shell)
              << " Shell, " << catalog.count(ComponentType::icon) << " Icon), " << fields << " typed values, "
              << unknown << " unknown; diagnostics:";
    for (const auto& [code, count] : by_code) std::cout << ' ' << code << '=' << count;
    std::cout << '\n';

    expect(catalog.components().size() == 846U, "FoC has 846 command-bar components");
    expect(catalog.count(ComponentType::text_button) == 621U, "621 TextButton");
    expect(catalog.count(ComponentType::button) == 114U, "114 Button");
    expect(catalog.count(ComponentType::bar) == 85U, "85 Bar");
    expect(catalog.count(ComponentType::shell) == 18U, "18 Shell");
    expect(catalog.count(ComponentType::icon) == 8U, "8 Icon");
    expect(errors == 0U, "no FoC component is invalid");
    expect(unknown == 0U, "every FoC tag is in the schema");
    expect(by_code.count(std::string(data::ui::diagnostic_codes::component_duplicate)) == 0U,
           "FoC component names are unique");

    const auto* shell = catalog.shell_for_model("i_tactical_controls.alo");
    expect(shell && shell->name == "i_main_skirmish", "the tactical shell component is i_main_skirmish");
    const auto* planet = catalog.find("Text_Planet_tactical");
    expect(planet && planet->text(Field::font_name) == "EmpireAtWar-Bold" &&
               planet->integer(Field::font_point_size) == 10 &&
               planet->color(Field::text_color) == data::ui::Rgba8{255, 212, 33, 255} &&
               planet->flag(Field::text_outline),
           "planet name: EmpireAtWar-Bold 10, 255,212,33, outline (design section 1.3)");
    const auto* health = catalog.find("s_health_00");
    expect(health && health->type == ComponentType::bar && health->integer(Field::max_bar_level) == 10 &&
               health->flag(Field::smooth_bar) && health->in_group("Tactical_Selection_Health"),
           "health bar: Bar, 10 levels, smooth (design section 1.3)");
    const auto* card = catalog.find("s_select_00");
    expect(card && card->type == ComponentType::text_button && card->in_group("Tactical_Selection"),
           "unit card: TextButton in Tactical_Selection (design section 1.3)");
    const auto* land = catalog.find("land_art_ALT0");
    expect(land && land->alt.variant == 0U, "FoC ALT component name splits");
    const auto* queue = catalog.find("queue06");
    expect(queue && queue->groups.size() == 2U, "queue06 belongs to both of its groups");
}

} // namespace

void command_bar_contracts() {
    schema_contracts();
    alt_contracts();
    fixture_contracts();
    failure_contracts();
    command_bar_corpus();
}
