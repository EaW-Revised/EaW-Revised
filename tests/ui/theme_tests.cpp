// Theme model contracts (UI-06 #229): the kit slot table, texture resolution
// against a mega-texture directory and standalone files, the per-dialog and
// per-control variations and their chains, and font roles through UI-F1 to
// UI-F3. The fixtures are written here. With EAWR_EAW_GAME_ROOT set, the FoC
// skin and atlas directory are read through the FoC VFS as well, and with
// EAWR_MOD_HUD_ROOTS each listed mod's.

#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/dialog_script.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include "ui_test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
namespace data = eawr::data::ui;
namespace model = eawr::presentation::ui;

void expect(const bool condition, const std::string& message) {
    test::ui::expect(condition, message.c_str());
}

assets::Source source(const std::string& path) {
    return {path, "synthetic", "test", vfs::AssetOrigin::loose, 0U};
}

constexpr std::string_view header_fixture = R"(#define IDD_MENU    101
#define IDD_PLAIN   102
#define IDC_RESUME  1001
#define IDC_CLOSE   1002
#define IDC_TITLE   1003
#define IDC_LIST    1004
#define IDC_CHECK   1005
#define IDC_COMBO   1006
)";

constexpr std::string_view script_fixture = "#include \"resource.h\"\r\n"
    "IDD_MENU DIALOGEX 0, 0, 321, 328\r\n"
    "STYLE DS_SETFONT | WS_POPUP\r\n"
    "CAPTION \"Dialog\"\r\n"
    "FONT 8, \"Arial Black\", 700, 0, 0x0\r\n"
    "BEGIN\r\n"
    "    PUSHBUTTON      \"TEXT_RESUME\",IDC_RESUME,41,275,239,24\r\n"
    "    PUSHBUTTON      \"TEXT_QUIT\",IDC_CLOSE,41,232,239,24\r\n"
    "    LTEXT           \"TEXT_TITLE\",IDC_TITLE,20,11,289,26\r\n"
    "    LISTBOX         IDC_LIST,5,70,100,40,LBS_NOINTEGRALHEIGHT | WS_VSCROLL\r\n"
    "END\r\n"
    "\r\n"
    "IDD_PLAIN DIALOGEX 0, 0, 100, 50\r\n"
    "STYLE DS_SETFONT | WS_CHILD\r\n"
    "CAPTION \"Dialog\"\r\n"
    "FONT 8, \"Arial\", 400, 0, 0x0\r\n"
    "BEGIN\r\n"
    "    PUSHBUTTON      \"TEXT_RESUME\",IDC_CLOSE,1,2,3,4\r\n"
    "    CONTROL         \"\",IDC_CHECK,\"Button\",BS_AUTOCHECKBOX | WS_TABSTOP,1,2,3,4\r\n"
    "    COMBOBOX        IDC_COMBO,5,50,100,60,CBS_DROPDOWNLIST\r\n"
    "    LTEXT           \"TEXT_TITLE\",IDC_TITLE,20,11,289,26\r\n"
    "END\r\n";

constexpr std::string_view skin_fixture = R"(<?xml version="1.0" encoding="utf-8"?>
<GUIDialogs>
    <Textures File="MT_Fixture" Compressed_File="MT_FixtureCompressed">
        <Default>
            <Frame_Top_Left>i_frame_tl.tga</Frame_Top_Left>
            <Frame_Background>i_frame_bg.tga</Frame_Background>
            <Button_Left>i_button_l.tga</Button_Left>
            <Button_Middle>i_button_m.tga</Button_Middle>
            <Button_Right>i_button_r.tga</Button_Right>
            <Button_Right>i_button_second.tga</Button_Right>
            <Check_On>i_missing.tga</Check_On>
            <Radio_Off>none</Radio_Off>
            <Scanlines>i_scan.tga</Scanlines>
            <Glow_Top>i_button_m.tga</Glow_Top>
        </Default>
        <IDD_MENU>
            <Frame_Background>i_menu_bg.tga</Frame_Background>
            <Button_Middle>i_menu_button.tga</Button_Middle>
        </IDD_MENU>
        <IDC_CLOSE>
            <Button_Middle>i_close.tga</Button_Middle>
            <Button_Left>none</Button_Left>
        </IDC_CLOSE>
        <IDC_LIST>
            <Small_Frame_Top>i_list_top.tga</Small_Frame_Top>
        </IDC_LIST>
        <IDC_NOWHERE>
            <Button_Middle>i_close.tga</Button_Middle>
        </IDC_NOWHERE>
        <IDC_CLOSE>
            <Button_Middle>i_button_second.tga</Button_Middle>
        </IDC_CLOSE>
    </Textures>
    <Fonts>
        <Default>
            <Global_Default>
                <Name>EmpireAtWar-Medium</Name>
                <Size>7</Size>
                <Top_Color>195,171,2,255</Top_Color>
                <Bottom_Color>195,171,2,255</Bottom_Color>
                <Emboss>No</Emboss>
                <Outline>No</Outline>
            </Global_Default>
            <Push_Button>
                <Name>EmpireAtWar-Bold</Name>
                <Character_Padding>0</Character_Padding>
                <Stretch_Factor>1</Stretch_Factor>
                <Size>8</Size>
                <Top_Color>253,248,212,255</Top_Color>
                <Bottom_Color>253,248,212,255</Bottom_Color>
                <Emboss>Yes</Emboss>
                <Outline>No</Outline>
            </Push_Button>
            <List_Box>
                <Name>Arial Unicode MS</Name>
                <Size>8</Size>
                <Top_Color>255,255,255,255</Top_Color>
                <Bottom_Color>255,255,255,255</Bottom_Color>
                <Outline>Yes</Outline>
            </List_Box>
            <L_Text>
                <Name>EmpireAtWar-Medium</Name>
                <Character_Padding>1</Character_Padding>
                <Stretch_Factor>1.3</Stretch_Factor>
                <Size>7</Size>
                <Top_Color>253,248,212,255</Top_Color>
                <Bottom_Color>253,248,212,255</Bottom_Color>
            </L_Text>
        </Default>
        <IDD_MENU>
            <L_Text>
                <Name>EmpireAtWar-Bold</Name>
                <Size>14</Size>
                <Top_Color>195,171,2,255</Top_Color>
                <Bottom_Color>229,177,159,255</Bottom_Color>
            </L_Text>
        </IDD_MENU>
        <IDC_TITLE>
            <Name>EmpireAtWar-Stencil</Name>
            <Size>10</Size>
        </IDC_TITLE>
        <IDC_CLOSE>
            <Name>EmpireAtWar-Light</Name>
            <Size>9</Size>
            <Top_Color>1,2,3,4</Top_Color>
            <Bottom_Color>5,6,7,8</Bottom_Color>
            <Outline>Yes</Outline>
        </IDC_CLOSE>
    </Fonts>
    <Tooltips/>
</GUIDialogs>
)";

data::DialogCatalog catalog_fixture() {
    auto symbols = data::parse_resource_header(header_fixture, source("Data/Resources/GUIDialog/resource.h"));
    auto script = data::parse_dialog_script(script_fixture, source("Data/Resources/GUIDialog/guidialogs.rc"));
    auto skin = data::parse_dialog_skin(std::as_bytes(std::span(skin_fixture.data(), skin_fixture.size())),
                                        source("Data/XML/GUIDialogs.xml"));
    expect(symbols && script && skin, "theme fixture sources parse");
    if (!symbols || !script || !skin) return {};
    expect(script.value().dialogs.size() == 2U, "theme fixture has two dialogs");
    return data::build_dialog_catalog(std::move(symbols.value()), std::move(script.value()),
                                      std::move(skin.value()));
}

assets::MegaTexture atlas_fixture() {
    assets::MegaTexture atlas;
    atlas.source = source("Data/Art/Textures/MT_Fixture.mtd");
    atlas.backing_page_stem = "Data/Art/Textures/MT_Fixture";
    const auto add = [&](const std::string& name, const std::uint32_t x, const std::uint32_t y) {
        atlas.entries.push_back({name, {x, y, 14U, 12U}, true, false, false});
    };
    add("i_frame_tl.tga", 1U, 2U);
    add("i_frame_bg.tga", 20U, 2U);
    add("I_BUTTON_L.TGA", 40U, 2U); // directory lookups ignore case
    add("i_button_m.tga", 60U, 2U);
    add("i_button_r.tga", 80U, 2U);
    add("i_button_second.tga", 100U, 2U);
    add("i_menu_bg.tga", 1U, 30U);
    add("i_menu_button.tga", 20U, 30U);
    add("i_close.tga", 40U, 30U);
    add("i_list_top.tga", 60U, 30U);
    return atlas;
}

model::FontCache cache_fixture() {
    model::FontCache cache;
    cache.directory = "synthetic";
    for (const std::string_view face : {"EmpireAtWar-Bold", "EmpireAtWar-Medium"}) {
        model::CachedFace cached;
        cached.face = std::string(face);
        cached.logical_path = model::font_cache_path(face);
        if (face == "EmpireAtWar-Bold") {
            // The retail Bold face's metrics; Medium stays without OS/2.
            cached.names.units_per_em = 1000U;
            cached.names.ascender = 688;
            cached.names.descender = -312;
            cached.names.win_ascent = 959U;
            cached.names.win_descent = 253U;
        }
        cache.faces.push_back(std::move(cached));
    }
    return cache;
}

struct Fixture final {
    data::DialogCatalog catalog = catalog_fixture();
    assets::MegaTexture atlas = atlas_fixture();
    model::FontCache fonts = cache_fixture();

    [[nodiscard]] model::ThemeModel build(const model::ReferenceSpace& space,
                                          const std::string& language = "ENGLISH") const {
        model::ThemeSources sources;
        sources.catalog = &catalog;
        sources.atlas = &atlas;
        sources.standalone = [](const std::string_view texture) -> std::optional<std::string> {
            if (texture == "i_scan.tga") return std::string("Data/Art/Textures/i_scan.dds");
            return std::nullopt;
        };
        sources.fonts = &fonts;
        sources.system = [](const std::string_view face) { return face == "Arial"; };
        sources.language = language;
        return model::build_theme_model(sources, space);
    }
};

std::size_t count_code(const model::ThemeModel& theme, const std::string_view code, const std::string_view part = {}) {
    return static_cast<std::size_t>(std::count_if(theme.diagnostics.begin(), theme.diagnostics.end(),
        [&](const core::Diagnostic& diagnostic) {
            return diagnostic.code == code && diagnostic.message.find(part) != std::string::npos;
        }));
}

void slot_table_contracts() {
    std::set<std::string_view> names;
    for (const model::KitSlot& slot : model::kit_slots) names.insert(slot.name);
    expect(names.size() == 87U, "the kit draws 87 distinct slots");
    const model::KitSlot* slot = model::find_kit_slot("Combo_Box_Left_Cap");
    expect(slot != nullptr && slot->part == model::KitPart::combo, "slots name their kit part");
    expect(model::find_kit_slot("combo_box_left_cap") == nullptr, "slot names match case-sensitively");
    expect(model::find_kit_slot("Scanlines")->part == model::KitPart::scanlines, "Scanlines is its own part");
    std::map<model::KitPart, std::size_t> parts;
    for (const model::KitSlot& entry : model::kit_slots) ++parts[entry.part];
    expect(parts[model::KitPart::frame] == 17U && parts[model::KitPart::small_frame] == 9U
               && parts[model::KitPart::button] == 12U && parts[model::KitPart::scroll] == 12U
               && parts[model::KitPart::trackbar] == 12U && parts[model::KitPart::dial] == 10U,
           "frame 16 pieces plus background, small frame 8 plus background, 3 x 4 button states");
}

void scale_contracts(const Fixture& fixture) {
    const auto wide = fixture.build(model::reference_space({1280U, 720U}));
    expect(wide.scale_x == 0.9375 && wide.scale_y == 0.9375 && wide.font_screen_height == 720U,
           "aspect-correct pieces scale uniformly by H/768 at 1280x720");
    const auto retail = fixture.build(model::reference_space({1280U, 720U}, model::LayoutRules::retail));
    expect(retail.scale_x == 1.25 && retail.scale_y == 0.9375, "retail pieces stretch by W/1024 and H/768");
    const auto narrow = fixture.build(model::reference_space({1280U, 1024U}));
    expect(narrow.scale_x == 1.25 && narrow.scale_y == 1.25 && narrow.font_screen_height == 960U,
           "below 4:3 pieces scale by W/1024 and fonts take H = 768 x scale");
}

void texture_contracts(const Fixture& fixture) {
    const auto theme = fixture.build(model::reference_space({1280U, 720U}));
    const model::ThemeStyle& defaults = theme.defaults;
    expect(defaults.name == "EawrUi" && defaults.base.empty(), "the Default set is the EawrUi base style");
    expect(defaults.textures.size() == 9U, "Default keeps its first Button_Right and every other slot once");
    const model::ThemeTexture* corner = defaults.texture("Frame_Top_Left");
    expect(corner != nullptr && corner->origin == model::TextureOrigin::atlas
               && corner->rectangle == assets::AtlasRectangle{1U, 2U, 14U, 12U} && corner->has_alpha,
           "an atlas slot carries its MTD rectangle");
    expect(defaults.texture("Button_Left")->origin == model::TextureOrigin::atlas, "atlas names match without case");
    expect(defaults.texture("Button_Right")->texture == "i_button_r.tga", "a repeated slot keeps its first texture");
    const model::ThemeTexture* scan = defaults.texture("Scanlines");
    expect(scan != nullptr && scan->origin == model::TextureOrigin::standalone
               && scan->logical_path == "Data/Art/Textures/i_scan.dds",
           "a texture outside the atlas resolves as a standalone file");
    expect(defaults.texture("Radio_Off")->origin == model::TextureOrigin::none, "'none' is an empty slot");
    const model::ThemeTexture* missing = defaults.texture("Check_On");
    expect(missing != nullptr && missing->origin == model::TextureOrigin::missing
               && count_code(theme, model::diagnostic_codes::theme_texture, "i_missing.tga") == 1U,
           "a texture that resolves nowhere is kept as missing with one EAWR-UI-0602");
    expect(defaults.texture("Glow_Top") != nullptr
               && count_code(theme, model::diagnostic_codes::theme_slot, "Glow_Top") == 1U,
           "a slot no kit part draws is kept with one EAWR-UI-0601");
    expect(count_code(theme, model::diagnostic_codes::theme_texture, "the Default set has no") == 87U - 8U,
           "every kit slot the Default set lacks is reported once");
}

void variation_contracts(const Fixture& fixture) {
    const auto theme = fixture.build(model::reference_space({1280U, 720U}));
    std::vector<std::string> names;
    for (const model::ThemeStyle& style : theme.variations) names.push_back(style.name + " <- " + style.base);
    const std::vector<std::string> expected{
        "EawrUi__IDD_MENU <- EawrUi",
        "EawrUi__IDC_CLOSE <- EawrUi",
        "EawrUi__IDC_LIST <- EawrUi",
        "EawrUi__IDC_TITLE <- EawrUi",
        "EawrUi__IDD_MENU__IDC_CLOSE <- EawrUi__IDD_MENU",
        "EawrUi__IDD_MENU__IDC_TITLE <- EawrUi__IDD_MENU",
        "EawrUi__IDD_MENU__IDC_LIST <- EawrUi__IDD_MENU",
    };
    expect(names == expected, "dialog and control entries become variations, chained where both apply");
    expect(theme.unmatched == std::vector<std::string>{"IDC_NOWHERE"}, "an entry naming nothing gets no style");
    const model::ThemeStyle* close = theme.find("EawrUi__IDC_CLOSE");
    expect(close != nullptr && close->level == data::OverrideLevel::control && close->textures.size() == 2U
               && close->texture("Button_Middle")->texture == "i_close.tga",
           "a repeated entry keeps its first set");

    const data::Dialog* menu = fixture.catalog.script.find("IDD_MENU");
    const data::Dialog* plain = fixture.catalog.script.find("IDD_PLAIN");
    const auto control = [](const data::Dialog* dialog, const std::string_view id) {
        const auto found = std::find_if(dialog->controls.begin(), dialog->controls.end(),
                                        [id](const data::DialogControl& entry) { return entry.id_name == id; });
        return &*found;
    };
    expect(theme.variation(*menu, nullptr) == "EawrUi__IDD_MENU", "a dialog with an entry takes its variation");
    expect(theme.variation(*menu, control(menu, "IDC_RESUME")) == "EawrUi__IDD_MENU",
           "a control without an entry takes its dialog's variation");
    expect(theme.variation(*menu, control(menu, "IDC_CLOSE")) == "EawrUi__IDD_MENU__IDC_CLOSE",
           "a control with an entry in a dialog with an entry takes the chained variation");
    expect(theme.variation(*plain, control(plain, "IDC_CLOSE")) == "EawrUi__IDC_CLOSE",
           "the same control in a plain dialog takes its own variation");
    expect(theme.variation(*plain, control(plain, "IDC_CHECK")) == "EawrUi", "no entry anywhere is the base type");

    // Walking a variation's chain gives what the catalogue resolves for that
    // control, for every slot the kit draws plus the unknown one.
    std::vector<std::string_view> slots{"Glow_Top", "Unknown_Slot"};
    for (const model::KitSlot& slot : model::kit_slots) slots.push_back(slot.name);
    std::size_t compared{};
    for (const data::Dialog* dialog : {menu, plain}) {
        std::vector<const data::DialogControl*> targets{nullptr};
        for (const data::DialogControl& entry : dialog->controls) targets.push_back(&entry);
        for (const data::DialogControl* target : targets) {
            const std::string style = theme.variation(*dialog, target);
            for (const std::string_view slot : slots) {
                const data::TextureResolution expected_texture = fixture.catalog.texture(*dialog, target, slot);
                const model::ThemeTexture* actual = theme.texture(style, slot);
                const bool same = expected_texture.level == data::OverrideLevel::none
                    ? actual == nullptr
                    : actual != nullptr
                        && (expected_texture.texture ? actual->texture == *expected_texture.texture
                                                     : actual->origin == model::TextureOrigin::none);
                expect(same, dialog->name + " " + (target ? target->id_name : std::string("frame")) + " "
                                 + std::string(slot) + " resolves as the catalogue does");
                ++compared;
            }
            if (target == nullptr) continue;
            const data::FontResolution expected_font = fixture.catalog.font(*dialog, *target);
            const model::ThemeFont* font = theme.font(style, expected_font.role);
            expect(font != nullptr && expected_font.spec != nullptr && font->face == *expected_font.spec->face
                       && font->point_size == static_cast<std::int32_t>(*expected_font.spec->size)
                       && font->level == expected_font.level,
                   dialog->name + " " + target->id_name + " takes the catalogue's font");
        }
    }
    expect(compared == (5U + 5U) * slots.size(), "every control and frame of both dialogs was compared");
}

void font_contracts_720(const Fixture& fixture) {
    const auto theme = fixture.build(model::reference_space({1280U, 720U}));
    const model::ThemeStyle& defaults = theme.defaults;
    expect(defaults.fonts.size() == 9U, "the default style has every font role");
    const model::ThemeFont* button = defaults.font(data::FontRole::push_button);
    expect(button != nullptr && button->resolved.face == "EmpireAtWar-Bold"
               && button->resolved.source == model::FaceSource::cache && !button->resolved.substituted,
           "Push_Button resolves to the cached EmpireAtWar-Bold");
    expect(button->pixels.em_height == 12 && button->pixels.glyph_height == 12, "8 pt is 12 px at 720 lines (UI-F1)");
    expect(button->cell_ascent == 12 && button->cell_descent == 3,
           "a cached face's GDI cell is its usWinAscent and usWinDescent at the glyph height");
    expect(defaults.font(data::FontRole::global_default)->cell_ascent == 0,
           "a face without OS/2 metrics keeps the engine's");
    expect(button->emboss && !button->outline && button->top_color == data::Rgba{253, 248, 212, 255}
               && button->defaulted.empty(),
           "Push_Button keeps its flags and colours");
    const model::ThemeFont* text = defaults.font(data::FontRole::l_text);
    expect(text->pixels.em_height == 11 && text->pixels.glyph_height == 14 && text->pixels.width_em == 11
               && text->character_padding == 1 && text->stretch_factor == 1.3,
           "L_Text at 7 pt and Stretch_Factor 1.3 is 14 px high with an 11 px width (UI-F2)");
    expect(text->defaulted == std::vector<std::string>{"Emboss", "Outline"},
           "flags the entry leaves out get neutral values and are named");
    const model::ThemeFont* combo = defaults.font(data::FontRole::combo_box);
    expect(combo->face == "EmpireAtWar-Medium" && combo->point_size == 7
               && combo->level == data::OverrideLevel::default_set,
           "a role the Default set lacks takes Global_Default");
    const model::ThemeFont* list = defaults.font(data::FontRole::list_box);
    expect(list->resolved.face == "EmpireAtWar-Medium" && list->resolved.substituted
               && list->resolved.unavailable == std::vector<std::string>{"Arial Unicode MS"},
           "a system face this machine lacks falls to EaW-Medium (UI-F3)");

    const model::ThemeFont* dialog_text = theme.font("EawrUi__IDD_MENU", data::FontRole::l_text);
    expect(dialog_text != nullptr && dialog_text->level == data::OverrideLevel::dialog
               && dialog_text->top_color == data::Rgba{195, 171, 2, 255}
               && dialog_text->bottom_color == data::Rgba{229, 177, 159, 255}
               && dialog_text->pixels.em_height == 22,
           "a dialog role font keeps its gradient and sizes 14 pt to 22 px");
    expect(theme.font("EawrUi__IDD_MENU", data::FontRole::push_button) == button,
           "roles the dialog entry lacks fall back to Default");
    const model::ThemeFont* title = theme.font("EawrUi__IDC_TITLE", data::FontRole::r_text);
    expect(title != nullptr && title->level == data::OverrideLevel::control && title->face == "EmpireAtWar-Stencil"
               && title->resolved.face == "EmpireAtWar-Medium" && title->top_color == data::Rgba{255, 255, 255, 255}
               && std::find(title->defaulted.begin(), title->defaulted.end(), "Top_Color") != title->defaulted.end(),
           "a control entry stands in for every role and is taken whole with project-authored colours");
    expect(count_code(theme, model::diagnostic_codes::theme_font, "IDC_TITLE") == 1U,
           "an entry without colours is reported once");
    const model::ThemeFont* chained = theme.font("EawrUi__IDD_MENU__IDC_TITLE", data::FontRole::l_text);
    expect(chained != nullptr && chained->face == "EmpireAtWar-Stencil",
           "a control entry wins over its dialog's role font");

    const auto japanese = fixture.build(model::reference_space({1280U, 720U}), "JAPANESE");
    const model::ThemeFont* japanese_button = japanese.defaults.font(data::FontRole::push_button);
    expect(japanese_button->resolved.face == "EmpireAtWar-Medium" && japanese_button->resolved.substituted,
           "Japanese starts the UI-F3 chain at the Unicode face");
    const auto large = fixture.build(model::reference_space({1920U, 1080U}));
    expect(large.defaults.font(data::FontRole::global_default)->pixels.em_height == 16,
           "7 pt is 16 px at 1080 lines (UI-F1)");
}

void standalone_contracts() {
    const test::ui::TempTree tree("theme-standalone");
    test::ui::write_text(tree.root / "Art" / "Textures" / "i_scan.dds", "dds");
    test::ui::write_text(tree.root / "Art" / "Textures" / "i_both.tga", "tga");
    test::ui::write_text(tree.root / "Art" / "Textures" / "i_both.dds", "dds");
    test::ui::write_text(tree.root / "Art" / "Textures" / "menu_back.jpg", "jpg");
    const vfs::MountSpec mount{.layer_id = "fixture", .data_root = tree.root, .active_archives = {}};
    auto mounted = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    expect(static_cast<bool>(mounted), "the standalone fixture mounts");
    if (!mounted) return;
    const model::StandaloneTextures probe = model::vfs_standalone_textures(mounted.value());
    const auto found = [&](const std::string_view name) { return probe(name).value_or("none"); };
    expect(found("i_scan.tga") == "Data/Art/Textures/i_scan.dds", "a .tga reference finds the .dds file");
    expect(found("I_BOTH") == "Data/Art/Textures/I_BOTH.tga", "the .tga file is tried first");
    expect(found("menu_back.jpg") == "Data/Art/Textures/menu_back.jpg", "another image file is found as written");
    expect(found("i_absent.tga") == "none", "a texture no file holds is not found");
}

// Read-only corpus: the retail skin and atlas directory build a complete theme.
void corpus_contracts() {
    auto filesystem = test::ui::foc_corpus("theme model");
    if (!filesystem) return;
    auto catalog = data::load_dialog_catalog(*filesystem);
    expect(static_cast<bool>(catalog), "the FoC dialog catalogue loads");
    if (!catalog) return;
    const std::string mtd = "Data/Art/Textures/" + catalog.value().skin.texture_file + ".mtd";
    auto atlas = assets::load_mega_texture(*filesystem, mtd);
    expect(static_cast<bool>(atlas), "the skin's MTD directory loads");
    if (!atlas) return;
    const model::FontCache empty;
    model::ThemeSources sources;
    sources.catalog = &catalog.value();
    sources.atlas = &atlas.value();
    sources.standalone = model::vfs_standalone_textures(*filesystem);
    sources.fonts = &empty;
    const auto theme = model::build_theme_model(sources, model::reference_space({1280U, 720U}));
    std::map<model::TextureOrigin, std::size_t> origins;
    for (const model::ThemeTexture& texture : theme.defaults.textures) ++origins[texture.origin];
    std::map<data::OverrideLevel, std::size_t> levels;
    for (const model::ThemeStyle& style : theme.variations) {
        ++levels[style.base == "EawrUi" ? style.level : data::OverrideLevel::none];
    }
    std::cout << "theme model corpus: " << theme.defaults.textures.size() << " Default slots ("
              << origins[model::TextureOrigin::atlas] << " atlas, " << origins[model::TextureOrigin::standalone]
              << " standalone), " << levels[data::OverrideLevel::dialog] << " dialog and "
              << levels[data::OverrideLevel::control] << " control variations, "
              << levels[data::OverrideLevel::none] << " chained, " << theme.unmatched.size() << " unmatched entries, "
              << theme.diagnostics.size() << " diagnostics\n";
    for (const core::Diagnostic& diagnostic : theme.diagnostics) {
        std::cout << "  " << core::format_diagnostic(diagnostic) << '\n';
    }
    expect(theme.defaults.textures.size() == 87U, "the retail Default set fills all 87 kit slots");
    // Unmatched names are counted once across both sections; the catalogue
    // audit lists them per section.
    expect(levels[data::OverrideLevel::dialog] == 22U && levels[data::OverrideLevel::control] == 269U
               && levels[data::OverrideLevel::none] == 302U && theme.unmatched.size() == 28U,
           "22 dialog and 269 control variations, 302 chained, 28 unmatched entry names");
    expect(theme.diagnostics.empty(), "the retail skin builds without theme diagnostics");
    expect(origins[model::TextureOrigin::atlas] == 86U && origins[model::TextureOrigin::standalone] == 1U
               && theme.defaults.texture("Scanlines")->origin == model::TextureOrigin::standalone,
           "86 Default slots are atlas entries and Scanlines is a texture file");
    expect(count_code(theme, model::diagnostic_codes::theme_slot) == 0U, "every retail slot has a kit part");
    for (const model::KitSlot& slot : model::kit_slots) {
        expect(theme.defaults.texture(slot.name) != nullptr, std::string(slot.name) + " is in the retail Default set");
    }
    expect(theme.defaults.fonts.size() == 9U, "all nine retail font roles");
    const model::ThemeFont* button = theme.defaults.font(data::FontRole::push_button);
    expect(button->face == "EmpireAtWar-Bold" && button->point_size == 8 && button->emboss,
           "retail Push_Button is EmpireAtWar-Bold 8 pt, embossed");
    expect(theme.find("EawrUi__IDD_BATTLE_END_DIALOG") != nullptr
               && theme.find("EawrUi__IDD_BATTLE_END_DIALOG")->textures.size() == 17U,
           "the battle-end dialog re-skins its whole frame");
    const data::Dialog* menu = catalog.value().script.find("IDD_GAME_OPTIONS_DIALOG");
    expect(menu != nullptr, "the in-game menu dialog exists");
    if (menu != nullptr) {
        const auto title = std::find_if(menu->controls.begin(), menu->controls.end(),
                                        [](const data::DialogControl& control) { return control.statement == "LTEXT"; });
        expect(title != menu->controls.end(), "the in-game menu has its title");
        if (title != menu->controls.end()) {
            const model::ThemeFont* font = theme.font(theme.variation(*menu, &*title), data::FontRole::l_text);
            expect(font != nullptr && font->face == "EmpireAtWar-Bold" && font->point_size == 14
                       && font->pixels.em_height == 22,
                   "the in-game menu title is EmpireAtWar-Bold 14 pt from its control entry");
        }
    }
}

// Read-only mod corpus (docs/ui/mod-hud-survey.md section 6): each listed
// mod's skin and atlas build a theme, and whatever a mod changes in
// GUIDialogs.xml (slots, font entries) costs diagnostics, never the theme.
void mod_corpus_contracts() {
    for (const auto& mod : test::ui::mod_corpora("theme model")) {
        auto catalog = data::load_dialog_catalog(mod.filesystem);
        expect(static_cast<bool>(catalog), mod.name + ": the dialog catalogue loads");
        if (!catalog) continue;
        const std::string mtd = "Data/Art/Textures/" + catalog.value().skin.texture_file + ".mtd";
        auto atlas = assets::load_mega_texture(mod.filesystem, mtd);
        expect(static_cast<bool>(atlas), mod.name + ": the skin's MTD directory loads");
        if (!atlas) continue;
        const model::FontCache empty;
        model::ThemeSources sources;
        sources.catalog = &catalog.value();
        sources.atlas = &atlas.value();
        sources.standalone = model::vfs_standalone_textures(mod.filesystem);
        sources.fonts = &empty;
        const auto theme = model::build_theme_model(sources, model::reference_space({1280U, 720U}));
        std::map<model::TextureOrigin, std::size_t> origins;
        for (const model::ThemeTexture& texture : theme.defaults.textures) ++origins[texture.origin];
        std::cout << "theme model mod " << mod.name << ": " << theme.defaults.textures.size() << " Default slots ("
                  << origins[model::TextureOrigin::atlas] << " atlas, " << origins[model::TextureOrigin::standalone]
                  << " standalone, " << origins[model::TextureOrigin::missing] << " missing), "
                  << theme.variations.size() << " variations, " << theme.diagnostics.size() << " diagnostics\n";
        for (const core::Diagnostic& diagnostic : theme.diagnostics) {
            std::cout << "  " << core::format_diagnostic(diagnostic) << '\n';
        }
        expect(theme.defaults.textures.size() >= 87U && theme.defaults.fonts.size() == 9U,
               mod.name + ": the mod's Default set still fills the kit");
    }
}

} // namespace

std::vector<std::byte> jpeg_header(const std::uint16_t width, const std::uint16_t height) {
    // SOI, an APP0 segment, then a baseline SOF0 frame header.
    const std::vector<int> raw{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46,
        0xFF, 0xC0, 0x00, 0x0B, 0x08, height >> 8, height & 0xFF, width >> 8, width & 0xFF, 0x01, 0x01, 0x11, 0x00,
        0xFF, 0xD9};
    std::vector<std::byte> bytes;
    for (const int value : raw) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

void jpeg_limit_contracts() {
    // Review of #240: a mod JPEG must be bounded before Godot's decoder allocates width x height.
    const auto small = jpeg_header(640, 480);
    const auto frame = model::jpeg_frame(small);
    expect(frame && frame->width == 640U && frame->height == 480U, "the SOF0 frame size is read without decoding");
    expect(model::jpeg_refusal(small).empty(), "a normal JPEG may be decoded");
    expect(!model::jpeg_refusal(jpeg_header(65535, 65535)).empty(), "a frame over 8192x8192 is refused");
    expect(!model::jpeg_refusal(jpeg_header(0, 480)).empty(), "a zero dimension is refused");
    auto truncated = small;
    truncated.resize(12U);
    expect(!model::jpeg_frame(truncated) && !model::jpeg_refusal(truncated).empty(), "a truncated header is refused");
    std::vector<std::byte> not_jpeg(16U, std::byte{0x42});
    expect(!model::jpeg_refusal(not_jpeg).empty(), "data without SOI is refused");
}

void theme_contracts() {
    jpeg_limit_contracts();
    const Fixture fixture;
    slot_table_contracts();
    scale_contracts(fixture);
    texture_contracts(fixture);
    variation_contracts(fixture);
    font_contracts_720(fixture);
    standalone_contracts();
    corpus_contracts();
    mod_corpus_contracts();
}
