// The galactic build tray's two Revised layer files (docs/ui/galactic-build-tray.md, GBT-11 and
// GBT-12): parsing and validation of the layout and the grouping, the checks that need the mod's
// category enum, and the layer rule (a valid higher file replaces a lower one whole, an invalid one is
// skipped and reported). Synthetic XML in temporary folders; no game data.

#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/build_tray_files.hpp"
#include "eawr/vfs/vfs.hpp"
#include "ui_test_support.hpp"
#include "../support/runtime_path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
namespace fs = std::filesystem;
namespace tray = presentation::ui::build_tray;
using test::ui::expect;

fs::path scratch_root() {
    static const auto root = fs::temp_directory_path()
        / ("eawr-build-tray-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    return root;
}

// One layer folder with `XML/<file>` in it; the logical prefix `data` makes it `data/xml/<file>`.
fs::path write_layer(const std::string& layer, const std::string& file, const std::string& text) {
    const auto root = scratch_root() / layer;
    const auto path = root / "XML" / fs::path(file);
    fs::create_directories(path.parent_path()); // `file` may name a subfolder, e.g. Enum/
    std::ofstream(path) << text;
    return root;
}

// Mounts the layers (highest first) and runs `use` with the filesystem.
template <typename Use>
void with_layers(const std::vector<fs::path>& roots, Use&& use) {
    std::vector<vfs::MountSpec> mounts;
    for (std::size_t index = 0; index < roots.size(); ++index)
        mounts.push_back({"layer" + std::to_string(index), roots[index], "data", {}});
    auto files = vfs::Vfs::mount(mounts);
    expect(bool(files), "the synthetic layers mount");
    if (files) use(files.value());
}

int counter{};

template <typename Parsed, typename Parse>
Parsed parse_text(Parse&& parse, const std::string& file, const std::string& text) {
    Parsed result;
    const auto layer = write_layer("single" + std::to_string(++counter), file, text);
    with_layers({layer}, [&](const vfs::Vfs& files) {
        const auto document = data::load_document(files, "Data/XML/" + file);
        expect(bool(document), "the synthetic file is well formed XML");
        if (document) result = parse(document.value());
    });
    return result;
}

tray::Parsed<tray::TrayLayout> layout_of(const std::string& text) {
    return parse_text<tray::Parsed<tray::TrayLayout>>(
        [](const data::XmlDocument& document) { return tray::parse_tray_layout(document); }, "RevisedBuildTray.xml", text);
}

tray::Parsed<tray::TrayGroups> groups_of(const std::string& text) {
    return parse_text<tray::Parsed<tray::TrayGroups>>(
        [](const data::XmlDocument& document) { return tray::parse_tray_groups(document); }, "RevisedBuildTrayGroups.xml", text);
}

std::string tray_with(const std::string& inner) {
    return "<Revised_Build_Tray><Tray>" + inner + "</Tray></Revised_Build_Tray>";
}

// ---- RevisedBuildTray.xml -------------------------------------------------------------------------------

void test_a_full_layout() {
    const auto parsed = layout_of(R"(<Revised_Build_Tray>
  <Enabled_By_Default>No</Enabled_By_Default>
  <Handle>
    <Enabled>true</Enabled>
    <Size>60 24</Size>
    <Caption>TEXT_MY_MORE</Caption>
    <Tooltip>TEXT_MY_TIP</Tooltip>
    <Normal_Texture>my_normal.tga</Normal_Texture>
    <Hover_Texture>my_hover.tga</Hover_Texture>
    <Candidate><Anchor>production_bar</Anchor><Side>above</Side><Offset>0 -2</Offset></Candidate>
    <Candidate><Anchor>viewport</Anchor><Side>inside_top_left</Side><Offset>40 120</Offset></Candidate>
  </Handle>
  <Tray>
    <Dialog_Name>IDD_MY_TRAY</Dialog_Name>
    <Entry_Template>b_Create_03</Entry_Template>
    <Entry_Scale>1.25</Entry_Scale>
    <Compact_Entry_Scale>0.9</Compact_Entry_Scale>
    <Entry_Gap>1</Entry_Gap><Group_Gap>10</Group_Gap><Row_Gap>3</Row_Gap><Panel_Padding>5</Panel_Padding>
    <Header_Scale>1.1</Header_Scale>
    <Header_Template>my_header_text</Header_Template>
    <Divider_Texture>my_divider.tga</Divider_Texture>
    <Max_Rows>5</Max_Rows>
    <Fold_Single_Choice_Groups>No</Fold_Single_Choice_Groups>
    <Width_Mode>Fixed</Width_Mode>
    <Width>420</Width>
    <Galaxy_Top_Limit><Y>96.5</Y></Galaxy_Top_Limit>
    <Planet_View_Cap_Component>g_planet_fleet</Planet_View_Cap_Component>
    <Planet_View_Cap_Component>q1_fleet</Planet_View_Cap_Component>
  </Tray>
</Revised_Build_Tray>)");
    expect(parsed.value.has_value(), "a complete layout parses");
    if (!parsed.value) { std::cerr << parsed.error << '\n'; return; }
    const auto& layout = *parsed.value;
    expect(!layout.enabled_by_default, "Enabled_By_Default No is read");
    expect(layout.handle.enabled && layout.handle.width == 60 && layout.handle.height == 24
            && layout.handle.caption == "TEXT_MY_MORE" && layout.handle.tooltip == "TEXT_MY_TIP"
            && layout.handle.normal == "my_normal.tga" && layout.handle.hover == "my_hover.tga" && !layout.handle.stock_art
            && layout.handle.candidates.size() == 2 && layout.handle.candidates[0].anchor == "production_bar"
            && layout.handle.candidates[0].y == -2 && layout.handle.candidates[1].side == "inside_top_left",
        "the handle keeps its size, text, art and ordered candidates");
    expect(layout.dialog_name == "IDD_MY_TRAY" && layout.entry_template == "b_Create_03" && layout.entry_scale == 1.25
            && layout.compact_entry_scale == 0.9 && layout.entry_gap == 1 && layout.group_gap == 10 && layout.row_gap == 3
            && layout.panel_padding == 5 && layout.header_scale == 1.1 && layout.divider_texture == "my_divider.tga"
            && layout.header_template == "my_header_text",
        "the tray names, scales and gaps are read");
    expect(layout.max_rows == 5 && !layout.fold_single_choice_groups && layout.width_mode == tray::WidthMode::fixed
            && layout.width == 420 && layout.galaxy_top_y == std::optional<double>(96.5) && layout.galaxy_top_component.empty()
            && layout.planet_view_cap_components == std::vector<std::string>({"g_planet_fleet", "q1_fleet"}),
        "the budget, width rule, galaxy top limit and the planet view cap components are read");
}

void test_defaults_and_the_handle() {
    const auto empty = layout_of("<Revised_Build_Tray/>");
    expect(empty.value && *empty.value == tray::TrayLayout{}, "an empty file takes every built-in default");
    expect(empty.value && empty.value->handle.stock_art && empty.value->handle.candidates.size() == 1
            && empty.value->handle.candidates[0].anchor == "production_bar",
        "the built-in handle is stock art above the production bar");

    const auto disabled = layout_of("<Revised_Build_Tray><Handle><Enabled>false</Enabled></Handle></Revised_Build_Tray>");
    expect(disabled.value && !disabled.value->handle.enabled, "a layer can disable the handle with Enabled alone");

    const auto replaced = layout_of(R"(<Revised_Build_Tray><Handle><Size>30 30</Size>
        <Candidate><Anchor>attack</Anchor><Side>left</Side><Offset>0 0</Offset></Candidate></Handle></Revised_Build_Tray>)");
    expect(replaced.value && replaced.value->handle.candidates.size() == 1 && replaced.value->handle.candidates[0].anchor == "attack"
            && replaced.value->handle.caption == tray::HandleDefinition{}.caption,
        "a Handle replaces the built-in candidates whole and only the text keys fall back");

    expect(!layout_of("<Revised_Build_Tray><Handle><Candidate><Anchor>a</Anchor><Side>above</Side><Offset>0 0</Offset></Candidate></Handle></Revised_Build_Tray>").value,
        "an enabled Handle without a Size is rejected");
    expect(!layout_of("<Revised_Build_Tray><Handle><Size>30 30</Size></Handle></Revised_Build_Tray>").value,
        "an enabled Handle without a Candidate is rejected");
    expect(!layout_of("<Revised_Build_Tray><Handle><Size>30 30</Size><Candidate><Anchor>a</Anchor><Side>sideways</Side><Offset>0 0</Offset></Candidate></Handle></Revised_Build_Tray>").value,
        "a Candidate with an unknown Side is rejected");
    expect(!layout_of("<Revised_Build_Tray><Handle><Size>30 30</Size><Candidate><Anchor>a</Anchor><Side>above</Side><Offset>0</Offset></Candidate></Handle></Revised_Build_Tray>").value,
        "a one-number Offset is rejected");
    expect(!layout_of("<Planet_Search/>").value, "another root element is not this file");
}

void test_the_close_button() {
    const auto defaults = layout_of("<Revised_Build_Tray/>");
    expect(defaults.value && defaults.value->close.enabled && defaults.value->close.candidates.size() == 1
            && defaults.value->close.candidates[0].anchor == "build_tray" && defaults.value->close.candidates[0].side == "inside_top_right"
            && defaults.value->close.width == 16 && defaults.value->close.height == 16 && defaults.value->close.art_components.empty()
            && defaults.value->close.caption == "TEXT_EAWR_BUILD_TRAY_CLOSE" && defaults.value->close.tooltip == "TEXT_BUTTON_CLOSE",
        "the built-in close icon is small, inside the panel's top right corner, in the bar's own button art");

    const auto replaced = layout_of(R"(<Revised_Build_Tray><Close_Button><Size>20 20</Size>
        <Art_Component>my_close</Art_Component><Art_Component>h_close</Art_Component>
        <Tooltip>TEXT_MY_CLOSE</Tooltip>
        <Candidate><Anchor>production_bar</Anchor><Side>Inside_Top_Left</Side><Offset>4 -4</Offset></Candidate>
        </Close_Button></Revised_Build_Tray>)");
    expect(replaced.value && replaced.value->close.width == 20 && replaced.value->close.height == 20
            && replaced.value->close.art_components == std::vector<std::string>({"my_close", "h_close"})
            && replaced.value->close.tooltip == "TEXT_MY_CLOSE" && replaced.value->close.caption == "TEXT_EAWR_BUILD_TRAY_CLOSE"
            && replaced.value->close.candidates.size() == 1 && replaced.value->close.candidates[0].anchor == "production_bar"
            && replaced.value->close.candidates[0].side == "inside_top_left" && replaced.value->close.candidates[0].x == 4,
        "a Close_Button replaces the art list and candidates whole; a Side is read without regard to case");

    const auto off = layout_of("<Revised_Build_Tray><Close_Button><Enabled>false</Enabled></Close_Button></Revised_Build_Tray>");
    expect(off.value && !off.value->close.enabled, "a layer can disable the close button with Enabled alone");
    const auto plain = layout_of("<Revised_Build_Tray><Close_Button><Size>20 20</Size><Candidate><Anchor>a</Anchor><Side>above</Side><Offset>0 0</Offset></Candidate></Close_Button></Revised_Build_Tray>");
    expect(plain.value && plain.value->close.art_components.empty(), "an Art_Component is optional: none named is the bar's button art");
    expect(!layout_of("<Revised_Build_Tray><Close_Button><Size>20 20</Size><Art_Component>h_close</Art_Component></Close_Button></Revised_Build_Tray>").value,
        "an enabled Close_Button without a Candidate is rejected");
    expect(!layout_of("<Revised_Build_Tray><Close_Button><Art_Component>h_close</Art_Component><Candidate><Anchor>a</Anchor><Side>above</Side><Offset>0 0</Offset></Candidate></Close_Button></Revised_Build_Tray>").value,
        "an enabled Close_Button without a Size is rejected");
    expect(!layout_of("<Revised_Build_Tray><Close_Button><Size>20 20</Size><Art_Component> </Art_Component><Candidate><Anchor>a</Anchor><Side>above</Side><Offset>0 0</Offset></Candidate></Close_Button></Revised_Build_Tray>").value,
        "an empty Art_Component is rejected");
    expect(!layout_of("<Revised_Build_Tray><Close_Button><Enabled>sometimes</Enabled></Close_Button></Revised_Build_Tray>").value,
        "Close_Button/Enabled must be true or false");
}

void test_width_and_backdrop() {
    const auto automatic = layout_of(tray_with("<Width_Mode>Auto</Width_Mode><Max_Width_Share>0.8</Max_Width_Share>"));
    expect(automatic.value && automatic.value->width_mode == tray::WidthMode::automatic && automatic.value->max_width_share == 0.8,
        "Width_Mode Auto with its share of the view");
    const auto budget = layout_of("<Revised_Build_Tray/>");
    expect(budget.value && budget.value->max_rows == 7, "the built-in row budget is 7: six unit rows and the buildings row");
    const auto bar = layout_of(tray_with("<Width_Mode>Bar</Width_Mode>"));
    expect(bar.value && bar.value->width_mode == tray::WidthMode::bar, "Width_Mode Bar is still read");
    expect(!layout_of(tray_with("<Max_Width_Share>0</Max_Width_Share>")).value
            && !layout_of(tray_with("<Max_Width_Share>1.5</Max_Width_Share>")).value,
        "Max_Width_Share must be from 0.1 to 1");

    const auto component = layout_of(tray_with("<Back_Source>Component</Back_Source><Back_Component>tooltip_back</Back_Component><Frame_Width>0</Frame_Width>"));
    expect(component.value && component.value->back_source == tray::BackSource::component
            && component.value->back_component == "tooltip_back" && component.value->frame_width == 0,
        "Back_Source Component with its component, and no frame");
    const auto fallback = layout_of(tray_with("<Back_Component>tooltip_back</Back_Component>"));
    expect(fallback.value && fallback.value->back_source == tray::BackSource::bar && fallback.value->back_component == "tooltip_back",
        "with Back_Source Bar a Back_Component is the art for a bar without art behind its slots");
    expect(!layout_of(tray_with("<Back_Source>Component</Back_Source>")).value, "Back_Source Component needs a Back_Component");
    expect(!layout_of(tray_with("<Back_Source>Blue</Back_Source>")).value, "an unknown Back_Source is rejected");
    expect(!layout_of(tray_with("<Frame_Width>-1</Frame_Width>")).value && !layout_of(tray_with("<Frame_Width>65</Frame_Width>")).value,
        "Frame_Width must be from 0 to 64");
}

void test_layout_ranges() {
    expect(!layout_of(tray_with("<Entry_Scale>3</Entry_Scale>")).value, "Entry_Scale above 2 is rejected");
    expect(!layout_of(tray_with("<Entry_Scale>large</Entry_Scale>")).value, "a non-numeric Entry_Scale is rejected");
    expect(!layout_of(tray_with("<Compact_Entry_Scale>1.5</Compact_Entry_Scale>")).value, "a compact step above the full step is rejected");
    const auto small = layout_of(tray_with("<Entry_Scale>0.6</Entry_Scale>"));
    expect(small.value && small.value->entry_scale == 0.6 && small.value->compact_entry_scale == 0.6,
        "an unset compact step never exceeds a smaller full step");
    expect(!layout_of(tray_with("<Entry_Gap>-1</Entry_Gap>")).value, "a negative gap is rejected");
    expect(!layout_of(tray_with("<Header_Scale>0.1</Header_Scale>")).value, "a header scale below 0.5 is rejected");
    expect(!layout_of(tray_with("<Max_Rows>1</Max_Rows>")).value && !layout_of(tray_with("<Max_Rows>11</Max_Rows>")).value
            && !layout_of(tray_with("<Max_Rows>6.5</Max_Rows>")).value,
        "Max_Rows must be an integer from 2 to 10");
    expect(layout_of(tray_with("<Max_Rows>2</Max_Rows>")).value && layout_of(tray_with("<Max_Rows>10</Max_Rows>")).value,
        "the ends of the Max_Rows range are valid");
    expect(!layout_of(tray_with("<Fold_Single_Choice_Groups>maybe</Fold_Single_Choice_Groups>")).value, "a flag must be Yes or No");
    expect(!layout_of(tray_with("<Width_Mode>Fixed</Width_Mode>")).value, "a fixed width needs a Width");
    expect(!layout_of(tray_with("<Width_Mode>Wide</Width_Mode>")).value, "an unknown width mode is rejected");
    expect(!layout_of(tray_with("<Width>0</Width>")).value, "a Width of 0 is rejected");
    expect(!layout_of(tray_with("<Dialog_Name> </Dialog_Name>")).value, "an empty dialog name is rejected");
    expect(!layout_of(tray_with("<Galaxy_Top_Limit><Component>a</Component><Y>10</Y></Galaxy_Top_Limit>")).value
            && !layout_of(tray_with("<Galaxy_Top_Limit/>")).value,
        "the galaxy top limit is exactly one of a component and a height");
    const auto component = layout_of(tray_with("<Galaxy_Top_Limit><Component>b_header</Component></Galaxy_Top_Limit>"));
    expect(component.value && component.value->galaxy_top_component == "b_header" && !component.value->galaxy_top_y,
        "a component top limit is read");
    expect(!layout_of(tray_with("<Planet_View_Cap_Component> </Planet_View_Cap_Component>")).value, "an empty cap component name is rejected");
}

// ---- RevisedBuildTrayGroups.xml -------------------------------------------------------------------------

void test_a_full_grouping() {
    const auto parsed = groups_of(R"(<Revised_Build_Tray_Groups>
  <Category_Priority>SpaceHero, LandHero
    Capital,Frigate</Category_Priority>
  <Building_Categories>Structure, SpaceStructure</Building_Categories>
  <Group_Caption Name="Capital">TEXT_CAPITAL</Group_Caption>
  <Group_Caption Name="Misc">TEXT_MISC</Group_Caption>
  <Extra_Group Name="Warlord_Choices"><Caption>TEXT_WARLORDS</Caption><Position>After Frigate</Position></Extra_Group>
  <Extra_Group Name="First"><Position>start</Position></Extra_Group>
  <Extra_Group Name="Last"/>
  <Type_Assignment><Type>Warlord_Choice_*</Type><Group>Warlord_Choices</Group><Sort_Key>10</Sort_Key></Type_Assignment>
  <Type_Assignment><Type>X-Wing</Type><Group>Buildings</Group></Type_Assignment>
</Revised_Build_Tray_Groups>)");
    expect(parsed.value.has_value(), "a complete grouping file parses");
    if (!parsed.value) { std::cerr << parsed.error << '\n'; return; }
    const auto& definition = parsed.value->definition;
    expect(definition.category_priority == std::vector<std::string>({"SpaceHero", "LandHero", "Capital", "Frigate"})
            && definition.building_categories == std::vector<std::string>({"Structure", "SpaceStructure"}),
        "the lists split on commas and white space");
    expect(definition.captions.size() == 2 && definition.captions[1].first == "Misc" && definition.captions[1].second == "TEXT_MISC",
        "captions keep their group name and text key");
    expect(definition.extra_groups.size() == 3 && definition.extra_groups[0].where == tray::ExtraGroup::Where::after
            && definition.extra_groups[0].after == "Frigate" && definition.extra_groups[0].caption == "TEXT_WARLORDS"
            && definition.extra_groups[1].where == tray::ExtraGroup::Where::start
            && definition.extra_groups[2].where == tray::ExtraGroup::Where::end,
        "extra groups read their position; End is the default");
    expect(definition.assignments.size() == 2 && definition.assignments[0].type == "Warlord_Choice_*"
            && definition.assignments[0].sort_key == std::optional<std::int64_t>(10)
            && definition.assignments[1].type == "X-Wing" && !definition.assignments[1].sort_key,
        "assignments keep a prefix pattern, a hyphenated name and an optional sort key");
    const auto empty = groups_of("<Revised_Build_Tray_Groups/>");
    expect(empty.value && empty.value->definition.category_priority.empty() && empty.value->definition.assignments.empty(),
        "an omitted list is empty, not the default's");
}

void test_grouping_rejections() {
    const auto wrap = [](const std::string& inner) { return "<Revised_Build_Tray_Groups>" + inner + "</Revised_Build_Tray_Groups>"; };
    expect(!groups_of(wrap("<Extra_Group Name=\"Misc\"/>")).value && !groups_of(wrap("<Extra_Group Name=\"buildings\"/>")).value,
        "an extra group cannot be named Buildings or Misc");
    expect(!groups_of(wrap("<Extra_Group Name=\"A\"/><Extra_Group Name=\"a\"/>")).value, "extra group names are unique, ignoring case");
    expect(!groups_of(wrap("<Extra_Group Name=\"A\"><Position>Middle</Position></Extra_Group>")).value
            && !groups_of(wrap("<Extra_Group Name=\"A\"><Position>After</Position></Extra_Group>")).value,
        "a position is Start, End or After <category>");
    expect(!groups_of(wrap("<Group_Caption Name=\"A\">K</Group_Caption><Group_Caption Name=\"a\">L</Group_Caption>")).value,
        "a group has at most one caption");
    expect(!groups_of(wrap("<Group_Caption Name=\"A\"> </Group_Caption>")).value, "a caption needs a text key");
    expect(!groups_of(wrap("<Type_Assignment><Type>A*B</Type><Group>G</Group></Type_Assignment>")).value
            && !groups_of(wrap("<Type_Assignment><Type>*</Type><Group>G</Group></Type_Assignment>")).value,
        "only a trailing * after a stem makes a prefix");
    expect(!groups_of(wrap("<Type_Assignment><Type>A</Type></Type_Assignment>")).value, "an assignment needs a group");
    expect(!groups_of(wrap("<Type_Assignment><Type>A</Type><Group>G</Group><Sort_Key>first</Sort_Key></Type_Assignment>")).value,
        "a sort key is an integer");
    expect(!groups_of(wrap("<Category_Priority>Capital, Fri gate!</Category_Priority>")).value, "a list holds plain names only");
    expect(!groups_of("<Revised_Build_Tray/>").value, "the layout root is not the grouping root");
}

void test_checks_against_the_category_enum() {
    const tray::CategoryEnum categories{{"Fighter", "Capital", "Structure", "All", "None"}};
    tray::GroupDefinition definition;
    definition.extra_groups = {{"Warlords", "", tray::ExtraGroup::Where::after, "Capital"}};
    definition.assignments = {{"A*", "Warlords", std::nullopt}, {"B", "Fighter", std::nullopt}, {"C", "Buildings", std::nullopt}};
    expect(tray::check_against_categories(definition, categories).empty(), "extra groups, categories and Buildings are valid targets");

    definition.extra_groups.push_back({"capital", "", tray::ExtraGroup::Where::end, {}});
    definition.extra_groups.push_back({"Late", "", tray::ExtraGroup::Where::after, "Gunship"});
    definition.assignments.push_back({"D", "Nowhere", std::nullopt});
    definition.assignments.push_back({"E", "All", std::nullopt});
    const auto problems = tray::check_against_categories(definition, categories);
    expect(problems.size() == 4, "an extra group named like a category, a position after an undefined category and two assignments to"
        " groups nothing defines (All is never a category) are four problems");
}

void test_stale_assignments() {
    presentation::ui::build_tray::TypeIndex types;
    types.add({"Warlord_Choice_One", {}, {}, false, {}, {}});
    types.add({"X-Wing", {}, {}, false, {}, {}});
    tray::GroupDefinition definition;
    definition.assignments = {{"Warlord_Choice_*", "G", std::nullopt}, {"x-wing", "G", std::nullopt},
        {"Gone", "G", std::nullopt}, {"Removed_*", "G", std::nullopt}};
    expect(tray::stale_assignments(definition, types) == 2,
        "an assignment naming no loaded type, exact or by prefix, is stale");
}

// ---- The layer rule -------------------------------------------------------------------------------------

void test_layers() {
    const std::string valid_lower = tray_with("<Max_Rows>4</Max_Rows><Entry_Gap>3</Entry_Gap>");
    const auto lower = write_layer("lower", "RevisedBuildTray.xml", valid_lower);
    const auto broken = write_layer("broken", "RevisedBuildTray.xml", tray_with("<Max_Rows>99</Max_Rows>"));
    const auto replacing = write_layer("replacing", "RevisedBuildTray.xml", tray_with("<Row_Gap>9</Row_Gap>"));
    const auto none = write_layer("none", "Other.xml", "<Other/>");

    with_layers({none}, [](const vfs::Vfs& files) {
        const auto result = tray::load_tray_layout(files);
        expect(!result.value && !result.invalid, "no file in any layer: no tray and nothing to report");
    });
    with_layers({lower}, [](const vfs::Vfs& files) {
        const auto result = tray::load_tray_layout(files);
        expect(result.value && result.value->max_rows == 4 && !result.invalid, "a single valid layer loads");
    });
    with_layers({broken, lower}, [](const vfs::Vfs& files) {
        const auto result = tray::load_tray_layout(files);
        expect(result.value && result.value->max_rows == 4 && result.invalid && !result.first_error.empty(),
            "an invalid higher file keeps the valid lower definition and is reported");
    });
    with_layers({replacing, lower}, [](const vfs::Vfs& files) {
        const auto result = tray::load_tray_layout(files);
        expect(result.value && result.value->row_gap == 9 && result.value->max_rows == 7 && result.value->entry_gap == 2 && !result.invalid,
            "a valid higher file replaces the lower one whole: nothing is inherited from it");
    });
    with_layers({broken}, [](const vfs::Vfs& files) {
        const auto result = tray::load_tray_layout(files);
        expect(!result.value && result.invalid, "an invalid only file means no tray, reported once");
    });
    const auto shipped_off = write_layer("shipped_off", "RevisedBuildTray.xml",
        "<Revised_Build_Tray><Enabled_By_Default>No</Enabled_By_Default></Revised_Build_Tray>");
    with_layers({shipped_off}, [](const vfs::Vfs& files) {
        expect(!tray::layer_enabled_by_default(files), "a layer that ships the tray off sets the switch's default");
    });
    with_layers({broken, shipped_off}, [](const vfs::Vfs& files) {
        expect(!tray::layer_enabled_by_default(files), "an invalid higher file leaves the valid lower default");
    });
    with_layers({none}, [](const vfs::Vfs& files) {
        expect(tray::layer_enabled_by_default(files), "with no file the switch's default is on");
    });

    const auto groups_lower = write_layer("groups_lower", "RevisedBuildTrayGroups.xml",
        "<Revised_Build_Tray_Groups><Category_Priority>Capital</Category_Priority></Revised_Build_Tray_Groups>");
    const auto groups_higher = write_layer("groups_higher", "RevisedBuildTrayGroups.xml",
        "<Revised_Build_Tray_Groups><Building_Categories>Structure</Building_Categories></Revised_Build_Tray_Groups>");
    with_layers({groups_higher, groups_lower}, [](const vfs::Vfs& files) {
        const auto result = tray::load_tray_groups(files);
        expect(result.value && result.value->definition.category_priority.empty()
                && result.value->definition.building_categories == std::vector<std::string>({"Structure"}) && !result.invalid,
            "a compat patch is the whole file: the lower priority list is not inherited");
    });
}

// The files the Revised layer installer ships (GBT-11, GBT-12) parse as valid definitions.
void test_the_shipped_files() {
    const auto directory = testing::runtime_path("EAWR_TEST_SOURCE_ROOT", "tools/revised_build_tray",
        EAWR_SOURCE_DIR "/tools/revised_build_tray");
    const std::array mounts{vfs::MountSpec{"project", directory, "data/xml", {}}};
    auto files = vfs::Vfs::mount(mounts);
    expect(bool(files), "the shipped build tray files mount without game data");
    if (!files) return;
    const auto layout = tray::load_tray_layout(files.value());
    expect(layout.value && !layout.invalid, "the shipped layout is valid");
    if (layout.value) {
        expect(layout.value->enabled_by_default && layout.value->handle.enabled && layout.value->max_rows == 7
                && layout.value->entry_template == "b_Create_00" && layout.value->fold_single_choice_groups,
            "the shipped layout is on by default with the 7 row budget and the bar's first slot as entry template");
        // Slice 6b: the shipped file spells out the built-in defaults (75 percent entries, automatic width, the
        // bar's own art behind the panel, the small close icon), except the header text look it names.
        auto defaults = tray::TrayLayout{};
        defaults.header_template = "Text_Tech_Level";
        expect(*layout.value == defaults, "the shipped layout is the built-in defaults plus its Header_Template");
        expect(layout.value->entry_scale == 0.75 && layout.value->compact_entry_scale == 0.6
                && layout.value->width_mode == tray::WidthMode::automatic && layout.value->back_source == tray::BackSource::bar
                && layout.value->back_component.empty() && layout.value->close.enabled
                && layout.value->close.art_components.empty() && layout.value->close.width == 16,
            "the shipped layout: 75 percent entries, automatic width, the bar's art, no named back art, the small close icon");
    }
    const auto groups = tray::load_tray_groups(files.value());
    expect(groups.value && !groups.invalid, "the shipped grouping is valid");
    if (groups.value) {
        const auto& definition = groups.value->definition;
        const auto position = [&](const std::string& name) {
            return std::find(definition.category_priority.begin(), definition.category_priority.end(), name)
                - definition.category_priority.begin();
        };
        expect(position("SpaceHero") < position("Capital") && position("Capital") < position("Fighter"),
            "the shipped priority list puts heroes before the size classes, largest first");
        expect(definition.building_categories == std::vector<std::string>({"Structure", "SpaceStructure"}),
            "the shipped building categories");
        expect(!definition.captions.empty() && definition.assignments.empty() && definition.extra_groups.empty(),
            "the shipped grouping has captions and no type assignments or extra groups");
    }
}

void test_the_category_enum() {
    const auto layer = write_layer("enum", "Enum/GameObjectCategoryType.xml",
        "<EnumDefinition><Fighter>0x01</Fighter><!-- a comment --><Capital>0x02</Capital><Hero>0x06</Hero>"
        "<All>0xFFFF</All></EnumDefinition>");
    with_layers({layer}, [](const vfs::Vfs& files) {
        const auto categories = tray::load_category_enum(files);
        expect(categories.names == std::vector<std::string>({"Fighter", "Capital", "Hero", "All"}),
            "the enum is its element names in authored order, comments skipped");
        expect(categories.index_of("capital") == std::optional<std::size_t>(1) && !categories.index_of("All"),
            "lookups ignore case and All is never a category");
    });
    const auto none = write_layer("noenum", "Other.xml", "<Other/>");
    with_layers({none}, [](const vfs::Vfs& files) {
        expect(tray::load_category_enum(files).names.empty(), "no enum file, no categories");
    });
}

} // namespace

int main() {
    test_a_full_layout();
    test_defaults_and_the_handle();
    test_the_close_button();
    test_width_and_backdrop();
    test_layout_ranges();
    test_a_full_grouping();
    test_grouping_rejections();
    test_checks_against_the_category_enum();
    test_stale_assignments();
    test_layers();
    test_the_shipped_files();
    test_the_category_enum();
    eawr::testing::remove_temp_directory(scratch_root());
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " build tray file check(s) failed\n";
        return 1;
    }
    std::cout << "build tray files: all checks passed\n";
    return 0;
}
