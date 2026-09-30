// Dialog catalogue contracts (#169): the resource-script tokenizer, the
// resource.h symbols, the DIALOGEX parser over every control form the retail
// script uses, GUIDialogs.xml overrides and their resolution. All fixtures are
// written here. With EAWR_EAW_GAME_ROOT set, the FoC dialog sources are also
// read through the FoC VFS and audited; with EAWR_MOD_HUD_ROOTS as well, each
// listed mod's dialog sources are loaded. Nothing from an install is written.

#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/dialog_script.hpp"
#include "eawr/data/ui/text_database.hpp"

#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using eawr::test::ui::expect;
namespace ui = eawr::data::ui;

eawr::assets::Source source(const std::string& path) {
    return {path, "synthetic", "test", eawr::vfs::AssetOrigin::loose, 0U};
}

std::span<const std::byte> bytes_of(const std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

constexpr std::string_view header_fixture = R"(//{{NO_DEPENDENCIES}}
// Fixture resource header
#define IDD_FIXTURE                     101
#define IDD_SECOND                      0x66
#define IDC_OK_BUTTON                   1001
#define IDC_OK2                         1002
#define IDC_CANCEL_BUTTON               (1003)
#define IDC_LOGO                        1004L
#define IDC_RIGHT                       1005 // trailing comment
#define IDC_CENTER                      1006
#define IDC_GROUP                       1007
#define IDC_EDIT                        1008
#define IDC_COMBO                       1009
#define IDC_LIST                        1010
#define IDC_IMAGE                       1011
#define IDC_CHECK                       1012
#define IDC_PROGRESS                    1013
#define IDC_SLIDER                      1014
#define IDC_IME                         1015
#define IDC_SECOND_TEXT                 1016
#define IDC_NEGATIVE                    -5
#define IDC_TWICE                       7
#define IDC_TWICE                       8
#define IDC_EXPRESSION                  IDC_OK_BUTTON + 1
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE        102
#define INCLUDE_GUARD
#endif
#endif
)";

// Every control form the retail script uses, wrapped the way the resource
// editor wraps long lines, plus the non-dialog resources it carries.
constexpr std::string_view script_fixture = "// Fixture resource script\r\n"
    "//\r\n"
    "#include \"resource.h\"\r\n"
    "#define APSTUDIO_READONLY_SYMBOLS\r\n"
    "#include \"afxres.h\"\r\n"
    "#if !defined(AFX_RESOURCE_DLL) || \\\r\n"
    "    defined(AFX_TARG_ENU)\r\n"
    "LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US\r\n"
    "#pragma code_page(1252)\r\n"
    "\r\n"
    "IDD_FIXTURE DIALOGEX 0, 0, 320, 200\r\n"
    "STYLE DS_SETFONT | DS_MODALFRAME | WS_POPUP | \r\n"
    "    WS_CAPTION\r\n"
    "CAPTION \"Fixture\"\r\n"
    "FONT 8, \"Arial\", 400, 0, 0x1\r\n"
    "BEGIN\r\n"
    "    DEFPUSHBUTTON   \"TEXT_OK\",IDC_OK_BUTTON,10,170,60,\r\n"
    "                    20\r\n"
    "    DEFPUSHBUTTON   \"TEXT_OK2\",IDC_OK2,10,170,60,20,WS_DISABLED\r\n"
    "    PUSHBUTTON      \"TEXT_CANCEL\",IDC_CANCEL_BUTTON,80,170,60,20\r\n"
    "    PUSHBUTTON      \"\",IDC_LOGO,-37,64,11,11,NOT WS_TABSTOP\r\n"
    "    LTEXT           \"TEXT_LEFT\",IDC_STATIC,5,5,100,8\r\n"
    "    RTEXT           \"Placeholder\",IDC_RIGHT,5,15,100,8\r\n"
    "    CTEXT           \"\",IDC_CENTER,5,25,100,8\r\n"
    "    GROUPBOX        \"TEXT_GROUP\",IDC_GROUP,2,2,300,150\r\n"
    "    EDITTEXT        IDC_EDIT,5,35,100,12,ES_AUTOHSCROLL\r\n"
    "    COMBOBOX        IDC_COMBO,5,50,100,60,CBS_DROPDOWNLIST | WS_VSCROLL | \r\n"
    "                    WS_TABSTOP\r\n"
    "    LISTBOX         IDC_LIST,5,70,100,40,LBS_NOINTEGRALHEIGHT | WS_VSCROLL\r\n"
    "    CONTROL         \"\",IDC_IMAGE,\"PETROGLYPH_DIALOG_IMAGE\",WS_TABSTOP,110,5,\r\n"
    "                    50,50\r\n"
    "    CONTROL         \"Check\",IDC_CHECK,\"Button\",BS_AUTOCHECKBOX | WS_TABSTOP,110,\r\n"
    "                    60,50,10\r\n"
    "    CONTROL         \"\",IDC_PROGRESS,\"msctls_progress32\",WS_BORDER,110,75,50,8\r\n"
    "    CONTROL         \"\",IDC_SLIDER,\"msctls_trackbar32\",TBS_BOTH | \r\n"
    "                    TBS_NOTICKS | WS_TABSTOP,110,85,50,12\r\n"
    "    CONTROL         \"\",IDC_IME,\"IMEEditBox\",WS_TABSTOP,110,100,50,12\r\n"
    "    CONTROL         \"\",IDC_UNDEFINED,\"IMELocaleIndicator\",0x0,110,115,50,\r\n"
    "                    12\r\n"
    "END\r\n"
    "\r\n"
    "IDD_SECOND DIALOGEX 10, 20, 100, 50\r\n"
    "STYLE DS_SETFONT | WS_CHILD\r\n"
    "CAPTION \"`\"\r\n"
    "FONT 7, \"Arial\", 400, 0, 0x0\r\n"
    "BEGIN\r\n"
    "    LTEXT           \"TEXT_LEFT\",IDC_SECOND_TEXT,1,2,3,4\r\n"
    "END\r\n"
    "\r\n"
    "#ifdef APSTUDIO_INVOKED\r\n"
    "GUIDELINES DESIGNINFO \r\n"
    "BEGIN\r\n"
    "    IDD_FIXTURE, DIALOG\r\n"
    "    BEGIN\r\n"
    "        LEFTMARGIN, 7\r\n"
    "        BOTTOMMARGIN, 193\r\n"
    "    END\r\n"
    "END\r\n"
    "#endif    // APSTUDIO_INVOKED\r\n"
    "\r\n"
    "1 TEXTINCLUDE \r\n"
    "BEGIN\r\n"
    "    \"resource.h\\0\"\r\n"
    "END\r\n"
    "\r\n"
    "2 TEXTINCLUDE \r\n"
    "BEGIN\r\n"
    "    \"#include \"\"afxres.h\"\"\\r\\n\"\r\n"
    "    \"\\0\"\r\n"
    "END\r\n"
    "#endif\r\n";

constexpr std::string_view skin_fixture = R"(<?xml version="1.0" encoding="utf-8"?>
<GUIDialogs>
    <Textures File="MT_Fixture" Compressed_File="MT_FixtureCompressed">
        <Default>
            <Frame_Top>i_frame_top.tga</Frame_Top>
            <Frame_Background>i_frame_mid.tga</Frame_Background>
            <Button_Middle>i_button.tga</Button_Middle>
            <Check_On>i_missing.tga</Check_On>
        </Default>
        <!-- overrides use the ids from the .rc file -->
        <IDD_FIXTURE>
            <Frame_Top>i_frame_top_small.tga</Frame_Top>
            <Frame_Background>none</Frame_Background>
        </IDD_FIXTURE>
        <IDC_OK_BUTTON>
            <Button_Middle>i_button_blank.tga</Button_Middle>
        </IDC_OK_BUTTON>
        <IDC_OK_BUTTON>
            <Button_Middle>i_button_second.tga</Button_Middle>
        </IDC_OK_BUTTON>
        <IDC_GONE>
            <Button_Middle>i_standalone.tga</Button_Middle>
        </IDC_GONE>
    </Textures>
    <Fonts>
        <Default>
            <Global_Default>
                <Name>Face-Medium</Name>
                <Size>7</Size>
                <Top_Color>195,171,2,255</Top_Color>
                <Bottom_Color>10,20,30,40</Bottom_Color>
                <Emboss>No</Emboss>
                <Outline>No</Outline>
            </Global_Default>
            <Push_Button>
                <Name>Face-Bold</Name>
                <Character_Padding>0</Character_Padding>
                <Stretch_Factor>1</Stretch_Factor>
                <Size>8</Size>
                <Emboss>Yes</Emboss>
                <Outline>No</Outline>
            </Push_Button>
            <L_Text>
                <Name>Face-Light</Name>
                <Size>7</Size>
            </L_Text>
            <Mystery_Role>
                <Name>Face-Light</Name>
            </Mystery_Role>
        </Default>
        <IDD_FIXTURE>
            <L_Text>
                <Name>Face-Dialog</Name>
                <Size>8</Size>
            </L_Text>
        </IDD_FIXTURE>
        <IDC_CANCEL_BUTTON>
            <Name>Face-Control</Name>
            <Size>6</Size>
            <Stretch_Factor>1.2</Stretch_Factor>
            <Character_Padding>1</Character_Padding>
        </IDC_CANCEL_BUTTON>
        <IDC_RIGHT>
            <Size>5</Size>
        </IDC_RIGHT>
        <IDC_EDIT>
            <Name>Face-Edit</Name>
            <Size>big</Size>
            <Top_Color>1,2,3</Top_Color>
            <Bottom_Color>1,2,3,4,5</Bottom_Color>
            <Emboss>Maybe</Emboss>
            <Glow>1</Glow>
        </IDC_EDIT>
    </Fonts>
    <Tooltips>
        <IDC_CHECK>
            <TextID>TEXT_TIP</TextID>
        </IDC_CHECK>
        <IDC_GONE_TIP>
            <TextID>TEXT_NO_TIP</TextID>
        </IDC_GONE_TIP>
    </Tooltips>
</GUIDialogs>
)";

std::vector<std::byte> text_database_bytes(const std::vector<std::string>& keys) {
    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const std::string& left, const std::string& right) {
        return ui::text_key_crc(left) < ui::text_key_crc(right);
    });
    std::vector<std::byte> bytes;
    const auto u32 = [&bytes](const std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    };
    u32(static_cast<std::uint32_t>(sorted.size()));
    for (const auto& key : sorted) {
        u32(ui::text_key_crc(key));
        u32(1U);
        u32(static_cast<std::uint32_t>(key.size()));
    }
    for (std::size_t index = 0U; index < sorted.size(); ++index) {
        bytes.push_back(std::byte{'v'});
        bytes.push_back(std::byte{0});
    }
    for (const auto& key : sorted) {
        for (const char character : key) bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

ui::TextDatabase text_fixture() {
    const auto bytes = text_database_bytes({"TEXT_OK", "TEXT_CANCEL", "TEXT_LEFT", "TEXT_GROUP", "TEXT_TIP"});
    auto loaded = ui::load_text_database(bytes, source("data/text/mastertextfile_test.dat"));
    expect(static_cast<bool>(loaded), "caption text database fixture loads");
    return loaded ? std::move(loaded.value()) : ui::TextDatabase{};
}

const ui::DialogControl* control(const ui::Dialog& dialog, const std::string_view id_name) {
    const auto found = std::find_if(dialog.controls.begin(), dialog.controls.end(),
                                    [id_name](const ui::DialogControl& entry) { return entry.id_name == id_name; });
    return found == dialog.controls.end() ? nullptr : &*found;
}

bool has_code(const std::vector<eawr::core::Diagnostic>& diagnostics, const std::string_view code,
              const std::string_view fragment) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const eawr::core::Diagnostic& entry) {
        return entry.code == code && entry.message.find(fragment) != std::string::npos;
    });
}

void tokenizer_contracts() {
    const std::string_view text =
        "// line comment\n"
        "/* block\n comment */ NAME 12 0x1F 7L -3 \"a\"\"b\" L\"wide\"\n"
        "  # define CONTINUED 1 \\\n  2\n"
        "\"esc\\r\\n\\t\\0\\\\\\\"\\x41\\q\" , | ( ) { } + ~\n";
    auto tokens = ui::tokenize_rc(text, source("fixture.rc"));
    expect(static_cast<bool>(tokens), "tokenizer accepts the fixture");
    if (!tokens) return;
    const auto& list = tokens.value().tokens;
    expect(list.size() == 17U, "tokenizer yields 17 tokens");
    if (list.size() != 17U) return;
    expect(list[0].kind == ui::RcTokenKind::identifier && list[0].text == "NAME", "identifier token");
    expect(list[0].line == 3U && list[0].column == 13U, "tokens carry line and column");
    expect(list[1].kind == ui::RcTokenKind::number && list[1].value == 12, "decimal number");
    expect(list[2].value == 0x1f && list[2].text == "0x1F", "hex number keeps its spelling");
    expect(list[3].value == 7, "L suffix is accepted");
    expect(list[4].kind == ui::RcTokenKind::punctuation && list[4].text == "-" && list[5].value == 3,
           "unary minus is a separate token");
    expect(list[6].kind == ui::RcTokenKind::string && list[6].text == "a\"b", "doubled quote");
    expect(list[7].kind == ui::RcTokenKind::string && list[7].text == "wide", "L prefix string");
    expect(list[8].text == std::string("esc\r\n\t\0\\\"A\\q", 12U), "escapes decode; unknown stays literal");
    expect(list[9].text == "," && list[10].text == "|" && list[16].text == "~", "punctuation");
    const auto& directives = tokens.value().directives;
    expect(directives.size() == 1U && directives[0].text == " define CONTINUED 1   2" &&
               directives[0].line == 4U,
           "preprocessor line with continuation is set aside");

    const auto rejects = [](const std::string_view bad, const char* message) {
        auto result = ui::tokenize_rc(bad, source("bad.rc"));
        expect(!result && result.error().code == ui::diagnostic_codes::rc_syntax, message);
    };
    rejects("\"open\nstring\"", "unterminated string");
    rejects("/* never closed", "unterminated block comment");
    rejects("NAME @", "unexpected character");
    rejects("0xZZ", "malformed hex literal");
    rejects("12ab", "malformed decimal literal");
    rejects("0x100000000", "literal wider than 32 bits");
    auto located = ui::tokenize_rc("A\n  @", source("bad.rc"));
    expect(!located && located.error().line == 2U && located.error().column == 3U,
           "syntax error names line and column");
}

void header_contracts() {
    auto symbols = ui::parse_resource_header(header_fixture, source("data/resources/guidialog/resource.h"));
    expect(static_cast<bool>(symbols), "resource header parses");
    if (!symbols) return;
    const auto& value = symbols.value();
    expect(value.find("IDD_FIXTURE") == 101, "decimal define");
    expect(value.find("IDD_SECOND") == 0x66, "hex define");
    expect(value.find("IDC_CANCEL_BUTTON") == 1003, "parenthesised define");
    expect(value.find("IDC_LOGO") == 1004, "suffixed define");
    expect(value.find("IDC_RIGHT") == 1005, "trailing comment is ignored");
    expect(value.find("IDC_NEGATIVE") == -5, "negative define");
    expect(value.find("IDC_TWICE") == 7, "first definition wins");
    expect(value.find("_APS_NEXT_RESOURCE_VALUE") == 102, "defines inside conditionals are read");
    expect(!value.find("INCLUDE_GUARD") && !value.find("IDC_EXPRESSION"), "valueless and expression defines are skipped");
    expect(has_code(value.diagnostics, ui::diagnostic_codes::rc_symbol, "IDC_TWICE"), "redefinition is reported");
    expect(has_code(value.diagnostics, ui::diagnostic_codes::rc_symbol, "IDC_EXPRESSION"), "non-integer define is reported");
    expect(value.diagnostics.size() == 2U, "only the two problems are reported");
}

void script_contracts() {
    auto parsed = ui::parse_dialog_script(script_fixture, source("data/resources/guidialog/guidialogs.rc"));
    if (!parsed) std::cerr << eawr::core::format_diagnostic(parsed.error()) << '\n';
    expect(static_cast<bool>(parsed), "fixture script parses");
    if (!parsed) return;
    auto& script = parsed.value();
    expect(script.dialogs.size() == 2U, "two dialogs");
    expect(script.includes == std::vector<std::string>{"resource.h", "afxres.h"}, "includes in order");
    expect(script.skipped.size() == 3U && script.skipped[0].type == "DESIGNINFO" &&
               script.skipped[1].type == "TEXTINCLUDE" && script.skipped[2].name == "2",
           "DESIGNINFO and TEXTINCLUDE resources are skipped by type");
    const auto* dialog = script.find("IDD_FIXTURE");
    expect(dialog != nullptr, "dialog found by name");
    if (dialog == nullptr) return;
    expect(dialog->extended && dialog->rect == ui::RcRect{0, 0, 320, 200}, "dialog geometry");
    expect(dialog->style == std::vector<std::string>{"DS_SETFONT", "DS_MODALFRAME", "WS_POPUP", "WS_CAPTION"},
           "wrapped dialog style");
    expect(dialog->caption == "Fixture", "dialog caption");
    expect(dialog->font && dialog->font->point_size == 8 && dialog->font->face == "Arial" &&
               dialog->font->weight == 400 && dialog->font->italic == 0 && dialog->font->charset == 1,
           "DIALOGEX font statement");
    expect(dialog->controls.size() == 17U, "every control statement is kept");

    const auto* ok = control(*dialog, "IDC_OK_BUTTON");
    expect(ok != nullptr && ok->statement == "DEFPUSHBUTTON" && ok->class_name == "Button" &&
               ok->text == "TEXT_OK" && ok->rect == ui::RcRect{10, 170, 60, 20} && ok->style.empty(),
           "DEFPUSHBUTTON wrapped over two lines");
    const auto* ok2 = control(*dialog, "IDC_OK2");
    expect(ok2 != nullptr && ok2->style == std::vector<std::string>{"WS_DISABLED"}, "button with a style");
    const auto* logo = control(*dialog, "IDC_LOGO");
    expect(logo != nullptr && logo->rect.x == -37 && logo->text == "" &&
               logo->style == std::vector<std::string>{"NOT WS_TABSTOP"},
           "negative coordinate and NOT style");
    const auto* left = control(*dialog, "IDC_STATIC");
    expect(left != nullptr && left->statement == "LTEXT" && left->class_name == "Static", "LTEXT");
    const auto* right = control(*dialog, "IDC_RIGHT");
    expect(right != nullptr && right->statement == "RTEXT" && right->text == "Placeholder", "RTEXT");
    const auto* center = control(*dialog, "IDC_CENTER");
    expect(center != nullptr && center->statement == "CTEXT", "CTEXT");
    const auto* group = control(*dialog, "IDC_GROUP");
    expect(group != nullptr && group->statement == "GROUPBOX" && group->class_name == "Button", "GROUPBOX");
    const auto* edit = control(*dialog, "IDC_EDIT");
    expect(edit != nullptr && edit->class_name == "Edit" && !edit->text &&
               edit->style == std::vector<std::string>{"ES_AUTOHSCROLL"},
           "EDITTEXT has no text");
    const auto* combo = control(*dialog, "IDC_COMBO");
    expect(combo != nullptr && combo->class_name == "ComboBox" && combo->style.size() == 3U,
           "COMBOBOX with a wrapped style");
    const auto* list = control(*dialog, "IDC_LIST");
    expect(list != nullptr && list->class_name == "ListBox" && list->rect == ui::RcRect{5, 70, 100, 40}, "LISTBOX");
    const auto* image = control(*dialog, "IDC_IMAGE");
    expect(image != nullptr && image->statement == "CONTROL" && image->class_name == "PETROGLYPH_DIALOG_IMAGE" &&
               image->rect == ui::RcRect{110, 5, 50, 50} && image->style == std::vector<std::string>{"WS_TABSTOP"},
           "CONTROL image wrapped over two lines");
    const auto* check = control(*dialog, "IDC_CHECK");
    expect(check != nullptr && check->class_name == "Button" && check->text == "Check" &&
               check->style == std::vector<std::string>{"BS_AUTOCHECKBOX", "WS_TABSTOP"},
           "CONTROL button");
    const auto* progress = control(*dialog, "IDC_PROGRESS");
    expect(progress != nullptr && progress->class_name == "msctls_progress32", "CONTROL progress bar");
    const auto* slider = control(*dialog, "IDC_SLIDER");
    expect(slider != nullptr && slider->class_name == "msctls_trackbar32" && slider->style.size() == 3U,
           "CONTROL trackbar");
    const auto* ime = control(*dialog, "IDC_IME");
    expect(ime != nullptr && ime->class_name == "IMEEditBox", "CONTROL IME edit box");
    const auto* locale = control(*dialog, "IDC_UNDEFINED");
    expect(locale != nullptr && locale->style == std::vector<std::string>{"0x0"} && locale->rect.height == 12,
           "CONTROL with a numeric style");

    auto symbols = ui::parse_resource_header(header_fixture, source("data/resources/guidialog/resource.h"));
    if (!symbols) return;
    ui::resolve_ids(script, symbols.value());
    expect(script.find("IDD_FIXTURE")->id == 101 && script.find("IDD_SECOND")->id == 0x66, "dialog ids resolve");
    expect(control(*script.find("IDD_FIXTURE"), "IDC_STATIC")->id == -1, "IDC_STATIC is -1");
    expect(control(*script.find("IDD_FIXTURE"), "IDC_CANCEL_BUTTON")->id == 1003, "control ids resolve");
    expect(!control(*script.find("IDD_FIXTURE"), "IDC_UNDEFINED")->id, "undefined id stays unresolved");
    expect(script.diagnostics.size() == 1U &&
               has_code(script.diagnostics, ui::diagnostic_codes::rc_unresolved_id, "IDC_UNDEFINED"),
           "one warning for the undefined id");

    // Grammar outside the retail corpus that the parser still has to accept.
    auto older = ui::parse_dialog_script(
        "IDI_APP ICON \"app.ico\"\n"
        "STRINGTABLE\nBEGIN\n  1 \"one\"\nEND\n"
        "7 DIALOG 1, 2, 3, 4\nSTYLE WS_POPUP\nFONT 8, \"Arial\"\n"
        "{\n  PUSHBUTTON \"Go\", 12, 1, 1, 10, 10, WS_TABSTOP, 0, 99\n}\n",
        source("older.rc"));
    expect(older && older.value().dialogs.size() == 1U && !older.value().dialogs[0].extended &&
               older.value().skipped.size() == 2U,
           "DIALOG form, braces, ICON file and STRINGTABLE");
    if (older && !older.value().dialogs.empty() && !older.value().dialogs[0].controls.empty()) {
        auto& button = older.value().dialogs[0].controls[0];
        expect(button.id_name == "12" && button.id == 12 && button.help_id == 99 &&
                   button.extended_style == std::vector<std::string>{"0"},
               "numeric id, extended style and help id");
    }

    // A malformed dialog is rejected on its own; the script still parses.
    const auto rejects = [](const std::string_view text, const char* message) {
        auto result = ui::parse_dialog_script(text, source("bad.rc"));
        expect(result && result.value().dialogs.empty() && result.value().rejected.size() == 1U &&
                   result.value().rejected[0].name == "IDD_X" && result.value().diagnostics.size() == 1U &&
                   result.value().diagnostics[0].code == ui::diagnostic_codes::rc_syntax &&
                   result.value().diagnostics[0].severity == eawr::core::Severity::error,
               message);
    };
    rejects("IDD_X DIALOGEX 0, 0, 1\nBEGIN\nEND\n", "dialog header with three numbers");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nCAPTION 5\nBEGIN\nEND\n", "non-string caption");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nBEGIN\n  SLIDER \"\",1,1,1,1,1\nEND\n", "unknown control keyword");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nBEGIN\n  LTEXT \"\",IDC_A,1,1,1\nEND\n", "too few fields");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nBEGIN\n  LTEXT \"\",IDC_A,1,X,1,1\nEND\n", "symbolic coordinate");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nBEGIN\n  LTEXT \"\",IDC_A,1,1,1,1\n", "missing END");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nBOGUS 1\nBEGIN\nEND\n", "unknown dialog statement");
    rejects("IDD_X DIALOGEX 0, 0, 1, 1\nBEGIN\n  LTEXT \"\",IDC_A,1,1,1,1,\n", "trailing comma");
}

// Malformed statements the installed mods ship (docs/ui/mod-hud-survey.md,
// gap G1), each in its own dialog between readable ones. Line numbers matter.
constexpr std::string_view tolerance_script =
    "#include \"resource.h\"\n"                                        // 1
    "IDD_GOOD_FIRST DIALOGEX 0, 0, 100, 50\n"
    "STYLE DS_SETFONT | WS_CHILD\n"
    "BEGIN\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"                           // 5
    "END\n"
    "IDD_EMPTY_FIELD DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"
    "    PUSHBUTTON \"TEXT_B\",IDC_B,265,550,215,,22\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"                           // 10
    "END\n"
    "IDD_MISSING_COMMA DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"
    "    CONTROL \"\",IDC_IMAGE\n"
    "            \"PETROGLYPH_DIALOG_IMAGE\",WS_TABSTOP,470,100,50,50\n" // 15
    "END\n"
    "IDD_DANGLING_STYLE DIALOGEX 0, 0, 100, 50\n"
    "STYLE DS_SETFONT | WS_POPUP | \n"
    "CAPTION \"Dialog\"\n"
    "FONT 8, \"Arial\", 400, 0, 0x1\n"                                // 20
    "BEGIN\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"
    "END\n"
    "IDD_GOOD_SECOND DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"                                                          // 25
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"
    "    LTEXT \"TEXT_A\",IDC_A2,1,1,10,10\n"
    "END\n"
    "IDD_STYLE_INTO_BEGIN DIALOGEX 0, 0, 100, 50\n"
    "STYLE WS_CHILD |\n"                                               // 30
    "BEGIN\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"
    "END\n"
    "IDD_COMMA_INTO_END DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"                                                          // 35
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10,\n"
    "END\n"
    "IDD_COMMA_INTO_CONTROL DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10,\n"                          // 40
    "    LTEXT \"TEXT_A\",IDC_A2,1,1,10,10\n"
    "END\n"
    "IDD_NO_END DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"                           // 45
    "IDD_UNKNOWN_CONTROL DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"
    "    SLIDER \"\",IDC_A,1,1,10,10\n"
    "    BEGIN\n"
    "    END\n"                                                        // 50
    "END\n"
    "IDD_NO_BEGIN DIALOGEX 0, 0, 100, 50\n"
    "STYLE WS_CHILD\n"
    "IDD_NO_ID DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"                                                          // 55
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"
    "END\n"
    "IDD_SHORT_HEADER DIALOGEX 0, 0, 100\n"
    "BEGIN\n"
    "END\n"                                                            // 60
    ",\n"
    "IDD_GOOD_LAST DIALOGEX 0, 0, 100, 50\n"
    "BEGIN\n"
    "    LTEXT \"TEXT_A\",IDC_A,1,1,10,10\n"
    "END\n"                                                            // 65
    "GUIDELINES DESIGNINFO\n"
    "BEGIN\n"
    "    IDD_GOOD_FIRST, DIALOG\n"
    "    BEGIN\n"
    "        LEFTMARGIN, 7\n"                                          // 70
    "    END\n"
    "END\n";

// IDD_NO_ID has no symbol, as FotR's added tech-tree dialogs.
constexpr std::string_view tolerance_header =
    "#define IDD_GOOD_FIRST 101\n#define IDD_GOOD_SECOND 102\n#define IDD_GOOD_LAST 103\n"
    "#define IDD_EMPTY_FIELD 104\n#define IDC_A 1001\n#define IDC_A2 1002\n";

// IDD_SELECT_MOD_DIALOG is the dialog every surveyed mod removes.
constexpr std::string_view tolerance_skin = R"(<GUIDialogs>
    <Textures File="MT_Fixture"><Default><Frame_Top>i_frame_top.tga</Frame_Top></Default>
        <IDD_SELECT_MOD_DIALOG><Frame_Top>i_frame_small.tga</Frame_Top></IDD_SELECT_MOD_DIALOG>
    </Textures>
    <Fonts><Default><Global_Default><Name>Face</Name><Size>7</Size></Global_Default></Default></Fonts>
</GUIDialogs>
)";

void rc_mod_tolerance_contracts() {
    auto parsed = ui::parse_dialog_script(tolerance_script, source("data/resources/guidialog/guidialogs.rc"));
    if (!parsed) std::cerr << eawr::core::format_diagnostic(parsed.error()) << '\n';
    expect(static_cast<bool>(parsed), "a script with malformed dialogs still parses");
    if (!parsed) return;
    const auto& script = parsed.value();
    std::vector<std::string> kept;
    for (const auto& dialog : script.dialogs) kept.push_back(dialog.name);
    expect(kept == std::vector<std::string>{"IDD_GOOD_FIRST", "IDD_GOOD_SECOND", "IDD_NO_ID", "IDD_GOOD_LAST"},
           "every readable dialog around the malformed ones is kept");
    expect(script.find("IDD_GOOD_SECOND") != nullptr && script.find("IDD_GOOD_SECOND")->controls.size() == 2U &&
               script.find("IDD_GOOD_LAST") != nullptr && script.find("IDD_GOOD_LAST")->controls.size() == 1U,
           "dialogs after a rejected one keep their controls");
    expect(script.skipped.size() == 1U && script.skipped[0].type == "DESIGNINFO",
           "resources after the last malformed dialog are still read");

    struct Expected final {
        std::string_view dialog;
        std::uint32_t dialog_line;
        std::uint32_t error_line;
        std::string_view reason;
    };
    constexpr std::array rejected{
        Expected{"IDD_EMPTY_FIELD", 7U, 9U, "after ',', found ','"},
        Expected{"IDD_MISSING_COMMA", 12U, 14U, "CONTROL has 2 fields"},
        Expected{"IDD_DANGLING_STYLE", 17U, 19U, "after '|', found 'CAPTION'"},
        Expected{"IDD_STYLE_INTO_BEGIN", 29U, 31U, "after '|', found 'BEGIN'"},
        Expected{"IDD_COMMA_INTO_END", 34U, 37U, "after ',', found 'END'"},
        Expected{"IDD_COMMA_INTO_CONTROL", 38U, 41U, "after ',', found 'LTEXT'"},
        Expected{"IDD_NO_END", 43U, 46U, "no END"},
        Expected{"IDD_UNKNOWN_CONTROL", 46U, 48U, "SLIDER"},
        Expected{"IDD_NO_BEGIN", 52U, 54U, "no BEGIN"},
        Expected{"IDD_SHORT_HEADER", 58U, 58U, "dialog header"},
    };
    expect(script.rejected.size() == rejected.size(), "each malformed dialog is rejected once");
    expect(script.diagnostics.size() == rejected.size() + 1U, "one error per rejected dialog plus the stray comma");
    for (std::size_t index = 0U; index < rejected.size() && index < script.rejected.size(); ++index) {
        const auto& want = rejected[index];
        const auto& got = script.rejected[index];
        expect(got.name == want.dialog && got.line == want.dialog_line, "rejected dialog name and line");
        const auto found = std::find_if(script.diagnostics.begin(), script.diagnostics.end(),
                                        [&](const eawr::core::Diagnostic& entry) {
                                            return entry.message.starts_with("dialog '" + std::string(want.dialog) + "'");
                                        });
        if (found == script.diagnostics.end()) std::cerr << "no diagnostic for " << want.dialog << '\n';
        expect(found != script.diagnostics.end() && found->code == ui::diagnostic_codes::rc_syntax &&
                   found->severity == eawr::core::Severity::error && found->line == want.error_line &&
                   found->message.find(want.reason) != std::string::npos,
               "EAWR-UI-0201 names the rejected dialog, the defect line and the reason");
        if (found != script.diagnostics.end() &&
            (found->line != want.error_line || found->message.find(want.reason) == std::string::npos)) {
            std::cerr << "  got: " << eawr::core::format_diagnostic(*found) << '\n';
        }
    }
    expect(std::any_of(script.diagnostics.begin(), script.diagnostics.end(),
                       [](const eawr::core::Diagnostic& entry) {
                           return entry.code == ui::diagnostic_codes::rc_syntax && entry.line == 61U;
                       }),
           "a stray token between resources is reported and skipped");

    // The same files through the VFS: missing dialogs and unresolved dialog
    // ids are diagnostics, never a load failure.
    eawr::test::ui::TempTree tree("dialog-tolerance");
    const auto data = tree.root / "Data";
    eawr::test::ui::write_text(data / "Resources" / "GUIDialog" / "guidialogs.rc", tolerance_script);
    eawr::test::ui::write_text(data / "Resources" / "GUIDialog" / "resource.h", tolerance_header);
    eawr::test::ui::write_text(data / "XML" / "GUIDialogs.xml", tolerance_skin);
    const std::array mounts{eawr::vfs::MountSpec{"mod", data, "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "tolerance fixture VFS mounts");
    if (!filesystem) return;
    auto catalog = ui::load_dialog_catalog(filesystem.value());
    if (!catalog) std::cerr << eawr::core::format_diagnostic(catalog.error()) << '\n';
    expect(static_cast<bool>(catalog), "a catalogue with rejected dialogs loads");
    if (!catalog) return;
    const auto& value = catalog.value();
    expect(value.script.dialogs.size() == 4U && value.script.rejected.size() == rejected.size(),
           "the loaded catalogue keeps the readable dialogs");
    const auto* no_id = value.script.find("IDD_NO_ID");
    expect(no_id != nullptr && !no_id->id &&
               has_code(value.script.diagnostics, ui::diagnostic_codes::rc_unresolved_id, "IDD_NO_ID"),
           "a dialog without a resource.h id is kept and reported");
    expect(value.script.find("IDD_SELECT_MOD_DIALOG") == nullptr &&
               has_code(value.diagnostics, ui::diagnostic_codes::override_unmatched, "IDD_SELECT_MOD_DIALOG"),
           "a skin entry for a removed dialog is reported");
}

ui::DialogCatalog catalog_fixture() {
    auto symbols = ui::parse_resource_header(header_fixture, source("data/resources/guidialog/resource.h"));
    auto script = ui::parse_dialog_script(script_fixture, source("data/resources/guidialog/guidialogs.rc"));
    auto skin = ui::parse_dialog_skin(bytes_of(skin_fixture), source("data/xml/guidialogs.xml"));
    expect(symbols && script && skin, "catalogue fixture parts parse");
    if (!symbols || !script || !skin) return {};
    return ui::build_dialog_catalog(std::move(symbols.value()), std::move(script.value()),
                                    std::move(skin.value()));
}

void skin_contracts() {
    auto parsed = ui::parse_dialog_skin(bytes_of(skin_fixture), source("data/xml/guidialogs.xml"));
    expect(static_cast<bool>(parsed), "skin fixture parses");
    if (!parsed) return;
    const auto& skin = parsed.value();
    expect(skin.texture_file == "MT_Fixture" && skin.compressed_texture_file == "MT_FixtureCompressed",
           "atlas stems");
    expect(skin.default_textures.slots.size() == 4U, "Default texture slots");
    expect(skin.texture_overrides.size() == 4U, "texture overrides keep duplicates in file order");
    expect(skin.texture_override("IDC_OK_BUTTON")->find("Button_Middle")->texture == "i_button_blank.tga",
           "first duplicate texture entry wins");
    expect(has_code(skin.diagnostics, ui::diagnostic_codes::override_duplicate, "IDC_OK_BUTTON"),
           "duplicate override is reported");
    const auto* global = skin.default_fonts.find(ui::FontRole::global_default);
    expect(global != nullptr && global->face == "Face-Medium" && global->size == 7.0 &&
               global->top_color == ui::Rgba{195, 171, 2, 255} &&
               global->bottom_color == ui::Rgba{10, 20, 30, 40} && global->emboss == false &&
               !global->stretch_factor,
           "Default Global_Default font");
    const auto* push = skin.default_fonts.find(ui::FontRole::push_button);
    expect(push != nullptr && push->emboss == true && push->stretch_factor == 1.0 &&
               push->character_padding == 0,
           "Default Push_Button font");
    expect(has_code(skin.diagnostics, ui::diagnostic_codes::skin_value, "Mystery_Role"), "unknown role reported");
    const auto* control_font = skin.font_override("IDC_CANCEL_BUTTON");
    expect(control_font != nullptr && control_font->spec && control_font->spec->stretch_factor == 1.2 &&
               control_font->spec->character_padding == 1 && control_font->roles.empty(),
           "control font entry is one spec");
    const auto* dialog_font = skin.font_override("IDD_FIXTURE");
    expect(dialog_font != nullptr && !dialog_font->spec && dialog_font->find(ui::FontRole::l_text) != nullptr,
           "dialog font entry holds role fonts");
    const auto* edit = skin.font_override("IDC_EDIT");
    expect(edit != nullptr && edit->spec && edit->spec->face == "Face-Edit" && !edit->spec->size &&
               !edit->spec->top_color && !edit->spec->bottom_color && !edit->spec->emboss,
           "malformed font values stay unset");
    for (const std::string_view fragment : {"Size 'big'", "Top_Color", "Bottom_Color", "Emboss 'Maybe'", "Glow"}) {
        expect(has_code(skin.diagnostics, ui::diagnostic_codes::skin_value, fragment), "bad font value reported");
    }
    expect(skin.tooltips.size() == 2U && skin.tooltip("IDC_CHECK")->text_id == "TEXT_TIP", "tooltip entry");

    // Character_Padding outside the int32 range (review of #177) is warned
    // about and left unset instead of reaching a float-to-int conversion.
    const std::string_view padding_skin = R"(<GUIDialogs><Fonts>
        <IDC_HUGE><Name>F</Name><Character_Padding>1e10</Character_Padding></IDC_HUGE>
        <IDC_ABOVE><Name>F</Name><Character_Padding>2147483648</Character_Padding></IDC_ABOVE>
        <IDC_TINY><Name>F</Name><Character_Padding>-1e10</Character_Padding></IDC_TINY>
        <IDC_BELOW><Name>F</Name><Character_Padding>-2147483649</Character_Padding></IDC_BELOW>
        <IDC_MAX><Name>F</Name><Character_Padding>2147483647</Character_Padding></IDC_MAX>
        <IDC_MIN><Name>F</Name><Character_Padding>-2147483648</Character_Padding></IDC_MIN>
    </Fonts></GUIDialogs>)";
    auto padding = ui::parse_dialog_skin(bytes_of(padding_skin), source("data/xml/padding.xml"));
    expect(static_cast<bool>(padding), "Character_Padding fixture parses");
    if (padding) {
        const auto& value = padding.value();
        for (const std::string_view name : {"IDC_HUGE", "IDC_ABOVE", "IDC_TINY", "IDC_BELOW"}) {
            const auto* set = value.font_override(name);
            expect(set != nullptr && set->spec && set->spec->face == "F" && !set->spec->character_padding,
                   "an out-of-range Character_Padding stays unset");
        }
        const auto out_of_range = std::count_if(
            value.diagnostics.begin(), value.diagnostics.end(), [](const eawr::core::Diagnostic& entry) {
                return entry.code == ui::diagnostic_codes::skin_value &&
                    entry.message.find("Character_Padding") != std::string::npos &&
                    entry.message.find("range") != std::string::npos;
            });
        expect(out_of_range == 4 && value.diagnostics.size() == 4U,
               "each out-of-range Character_Padding gives one EAWR-UI-0205 warning");
        const auto* max = value.font_override("IDC_MAX");
        const auto* min = value.font_override("IDC_MIN");
        expect(max != nullptr && max->spec && max->spec->character_padding == 2147483647 &&
                   min != nullptr && min->spec && min->spec->character_padding == -2147483647 - 1,
               "the int32 limits themselves are kept");
    }

    const std::string_view wrong_root = "<Dialogs/>";
    auto rejected = ui::parse_dialog_skin(bytes_of(wrong_root), source("x.xml"));
    expect(!rejected && rejected.error().code == ui::diagnostic_codes::skin_xml, "wrong root rejected");
    const std::string_view malformed = "<GUIDialogs><Textures></GUIDialogs>";
    auto broken = ui::parse_dialog_skin(bytes_of(malformed), source("x.xml"));
    expect(!broken && broken.error().code == ui::diagnostic_codes::skin_xml, "malformed XML rejected");
}

void resolution_contracts() {
    const auto catalog = catalog_fixture();
    const auto* fixture = catalog.script.find("IDD_FIXTURE");
    const auto* second = catalog.script.find("IDD_SECOND");
    expect(fixture != nullptr && second != nullptr, "catalogue holds both dialogs");
    if (fixture == nullptr || second == nullptr) return;
    const auto* ok = control(*fixture, "IDC_OK_BUTTON");
    const auto* cancel = control(*fixture, "IDC_CANCEL_BUTTON");

    auto texture = catalog.texture(*fixture, ok, "Button_Middle");
    expect(texture.level == ui::OverrideLevel::control && texture.texture == "i_button_blank.tga",
           "control texture entry wins");
    texture = catalog.texture(*fixture, cancel, "Button_Middle");
    expect(texture.level == ui::OverrideLevel::default_set && texture.texture == "i_button.tga",
           "control without an entry uses Default");
    texture = catalog.texture(*fixture, nullptr, "Frame_Top");
    expect(texture.level == ui::OverrideLevel::dialog && texture.texture == "i_frame_top_small.tga",
           "dialog texture entry beats Default");
    texture = catalog.texture(*fixture, ok, "Frame_Top");
    expect(texture.level == ui::OverrideLevel::dialog, "a control falls back to its dialog entry");
    texture = catalog.texture(*fixture, nullptr, "Frame_Background");
    expect(texture.level == ui::OverrideLevel::dialog && !texture.texture, "'none' leaves the slot empty");
    texture = catalog.texture(*second, nullptr, "Frame_Background");
    expect(texture.level == ui::OverrideLevel::default_set && texture.texture == "i_frame_mid.tga",
           "dialog without entries uses Default");
    texture = catalog.texture(*second, nullptr, "No_Such_Slot");
    expect(texture.level == ui::OverrideLevel::none && !texture.texture, "unknown slot resolves to nothing");

    auto font = catalog.font(*fixture, *control(*fixture, "IDC_STATIC"));
    expect(font.role == ui::FontRole::l_text && font.level == ui::OverrideLevel::dialog &&
               font.spec->face == "Face-Dialog",
           "LTEXT uses the dialog's L_Text font");
    font = catalog.font(*second, *control(*second, "IDC_SECOND_TEXT"));
    expect(font.level == ui::OverrideLevel::default_set && font.spec->face == "Face-Light",
           "LTEXT elsewhere uses the Default L_Text font");
    font = catalog.font(*fixture, *cancel);
    expect(font.role == ui::FontRole::push_button && font.level == ui::OverrideLevel::control &&
               font.spec->face == "Face-Control",
           "control font entry wins");
    font = catalog.font(*fixture, *ok);
    expect(font.level == ui::OverrideLevel::default_set && font.spec->face == "Face-Bold",
           "DEFPUSHBUTTON uses Push_Button");
    font = catalog.font(*fixture, *control(*fixture, "IDC_CENTER"));
    expect(font.role == ui::FontRole::global_default && font.spec->face == "Face-Medium",
           "CTEXT uses Global_Default");
    font = catalog.font(*fixture, *control(*fixture, "IDC_COMBO"));
    expect(font.role == ui::FontRole::combo_box && font.level == ui::OverrideLevel::default_set &&
               font.spec->face == "Face-Medium",
           "a role absent from Default falls back to Global_Default");
    expect(ui::font_role(*control(*fixture, "IDC_IME")) == ui::FontRole::ime_edit_box, "IME edit box role");
    expect(ui::font_role(*control(*fixture, "IDC_EDIT")) == ui::FontRole::edit_box, "edit box role");
    expect(ui::font_role(*control(*fixture, "IDC_LIST")) == ui::FontRole::list_box, "list box role");
    expect(ui::font_role(*control(*fixture, "IDC_RIGHT")) == ui::FontRole::r_text, "right text role");
    expect(ui::font_role(*control(*fixture, "IDC_CHECK")) == ui::FontRole::global_default,
           "CONTROL buttons use Global_Default");

    const auto* tip = catalog.tooltip(*control(*fixture, "IDC_CHECK"));
    expect(tip != nullptr && tip->text_id == "TEXT_TIP", "tooltip by control id");
    expect(catalog.diagnostics.size() == 2U &&
               has_code(catalog.diagnostics, ui::diagnostic_codes::override_unmatched, "IDC_GONE'") &&
               has_code(catalog.diagnostics, ui::diagnostic_codes::override_unmatched, "IDC_GONE_TIP"),
           "unmatched override names are reported");
}

void audit_contracts() {
    const auto catalog = catalog_fixture();
    const auto text = text_fixture();
    eawr::assets::MegaTexture atlas;
    for (const std::string_view name : {"I_FRAME_TOP.TGA", "i_frame_mid.tga", "i_button.tga",
                                        "i_frame_top_small.tga", "i_button_blank.tga", "i_button_second.tga"}) {
        atlas.entries.push_back({std::string(name), {0U, 0U, 1U, 1U}, true, false, false});
    }
    const auto probe = [](const std::string_view texture) { return texture == "i_standalone.tga"; };
    const auto audit = ui::audit_dialog_catalog(catalog, atlas, text, probe);
    expect(audit.dialogs == 2U && audit.controls == 18U, "audit counts dialogs and controls");
    expect(audit.statements.at("CONTROL") == 6U && audit.statements.at("LTEXT") == 2U &&
               audit.statements.at("DEFPUSHBUTTON") == 2U,
           "statement counts");
    expect(audit.control_classes.at("PETROGLYPH_DIALOG_IMAGE") == 1U && audit.control_classes.size() == 6U,
           "CONTROL class counts");
    expect(audit.textures.size() == 8U && audit.none_slots == 1U, "distinct texture names and none slots");
    expect(audit.unresolved_textures == std::vector<std::string>{"i_missing.tga"},
           "atlas, then standalone, then listed");
    const auto standalone = std::find_if(audit.textures.begin(), audit.textures.end(),
                                         [](const ui::TextureAudit& entry) { return entry.texture == "i_standalone.tga"; });
    expect(standalone != audit.textures.end() && standalone->standalone && !standalone->in_atlas,
           "standalone texture is found by the probe");
    expect(audit.unresolved_fonts.size() == 2U, "fonts without a face or size are listed");
    expect(audit.font_faces.at("Face-Medium") == 10U && audit.font_faces.at("Face-Bold") == 3U,
           "resolved faces are counted per control");
    expect(audit.control_captions.at(ui::CaptionKind::text_key) == 5U &&
               audit.control_captions.at(ui::CaptionKind::missing_key) == 1U &&
               audit.control_captions.at(ui::CaptionKind::literal) == 2U &&
               audit.control_captions.at(ui::CaptionKind::empty) == 7U,
           "control captions map to text keys");
    expect(audit.missing_caption_keys == std::vector<std::string>{"TEXT_OK2"}, "key-shaped missing caption");
    expect(audit.literal_captions == std::vector<std::string>{"Check", "Placeholder"}, "literal captions");
    expect(audit.dialog_captions.at(ui::CaptionKind::literal) == 2U, "dialog captions are literal");
    expect(audit.unresolved_tooltip_keys == std::vector<std::string>{"TEXT_NO_TIP"},
           "an unknown tooltip key is listed");
    expect(audit.unmatched_overrides == std::vector<std::string>{"Textures:IDC_GONE", "Tooltips:IDC_GONE_TIP"},
           "unmatched overrides are listed");
    expect(audit.duplicate_overrides == std::vector<std::string>{"Textures:IDC_OK_BUTTON"},
           "duplicate overrides are listed");
    expect(audit.unresolved_ids == std::vector<std::string>{"IDC_UNDEFINED"}, "unresolved ids are listed");

    expect(ui::classify_caption("", text) == ui::CaptionKind::empty, "empty caption");
    expect(ui::classify_caption("TEXT_OK", text) == ui::CaptionKind::text_key, "key caption");
    expect(ui::classify_caption("TXT_ABSENT_1", text) == ui::CaptionKind::missing_key, "key-shaped caption");
    for (const std::string_view literal : {"Custom1", "USER STRING", "150 cr", "0", "TEXT", "###"}) {
        expect(ui::classify_caption(literal, text) == ui::CaptionKind::literal, "literal caption");
    }
}

void vfs_contracts() {
    eawr::test::ui::TempTree tree("dialogs");
    const auto data = tree.root / "Data";
    eawr::test::ui::write_text(data / "Resources" / "GUIDialog" / "guidialogs.rc", script_fixture);
    eawr::test::ui::write_text(data / "Resources" / "GUIDialog" / "Resource.h", header_fixture);
    eawr::test::ui::write_text(data / "XML" / "GUIDialogs.xml", skin_fixture);
    eawr::test::ui::write_text(data / "Art" / "Textures" / "i_standalone.dds", "dds");
    const std::array mounts{eawr::vfs::MountSpec{"expansion", data, "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "dialog fixture VFS mounts");
    if (!filesystem) return;
    auto catalog = ui::load_dialog_catalog(filesystem.value());
    if (!catalog) std::cerr << eawr::core::format_diagnostic(catalog.error()) << '\n';
    expect(static_cast<bool>(catalog), "catalogue loads through the VFS");
    if (!catalog) return;
    expect(catalog.value().script.dialogs.size() == 2U, "VFS catalogue holds the dialogs");
    expect(catalog.value().symbols.source.logical_path == "data/resources/guidialog/resource.h",
           "the resource header is the script's own #include");
    const auto probe = ui::vfs_texture_probe(filesystem.value());
    expect(probe("i_standalone.tga") && probe("I_STANDALONE") && !probe("i_missing.tga"),
           "standalone probe tries .tga then .dds");

    const std::array empty_mounts{eawr::vfs::MountSpec{"expansion", tree.root / "Nothing", "data", {}}};
    std::filesystem::create_directories(tree.root / "Nothing");
    auto empty = eawr::vfs::Vfs::mount(empty_mounts);
    auto missing = empty ? ui::load_dialog_catalog(empty.value())
                         : eawr::core::Result<ui::DialogCatalog>::failure({});
    expect(!missing && missing.error().code == eawr::vfs::diagnostic_codes::not_found,
           "a missing script is a VFS not-found error");
}

template <typename Map>
std::size_t count_of(const Map& map, const typename Map::key_type& key) {
    const auto found = map.find(key);
    return found == map.end() ? 0U : found->second;
}

void print_list(const char* label, const std::vector<std::string>& items) {
    std::cout << "  " << label << " (" << items.size() << "):";
    for (const auto& item : items) std::cout << ' ' << item;
    std::cout << '\n';
}

// Read-only FoC corpus audit (#169 acceptance).
void corpus_contracts() {
    auto filesystem = eawr::test::ui::foc_corpus("dialog catalogue");
    if (!filesystem) return;
    auto catalog = ui::load_dialog_catalog(*filesystem);
    if (!catalog) std::cerr << eawr::core::format_diagnostic(catalog.error()) << '\n';
    expect(static_cast<bool>(catalog), "FoC dialog catalogue loads");
    auto text = ui::load_language_text_database(*filesystem, "English");
    expect(static_cast<bool>(text), "FoC text database loads");
    if (!catalog || !text) return;
    const auto& value = catalog.value();
    auto atlas = eawr::assets::load_mega_texture(
        *filesystem, "Data/Art/Textures/" + value.skin.texture_file + ".mtd");
    if (!atlas) std::cerr << eawr::core::format_diagnostic(atlas.error()) << '\n';
    expect(static_cast<bool>(atlas), "the GUIDialogs texture atlas loads");
    if (!atlas) return;
    const auto audit = ui::audit_dialog_catalog(value, atlas.value(), text.value(),
                                                ui::vfs_texture_probe(*filesystem));

    std::size_t in_atlas{};
    std::size_t standalone{};
    for (const auto& texture : audit.textures) {
        in_atlas += texture.in_atlas ? 1U : 0U;
        standalone += texture.standalone ? 1U : 0U;
    }
    std::cout << "dialog catalogue corpus: " << value.script.source.source_id << ", "
              << value.symbols.source.source_id << ", " << value.skin.source.source_id << '\n'
              << "  " << audit.dialogs << " dialogs, " << audit.controls << " controls, "
              << value.symbols.values.size() << " symbols, " << value.script.skipped.size()
              << " skipped resources\n  statements:";
    for (const auto& [statement, count] : audit.statements) std::cout << ' ' << statement << '=' << count;
    std::cout << "\n  CONTROL classes:";
    for (const auto& [name, count] : audit.control_classes) std::cout << ' ' << name << '=' << count;
    std::cout << "\n  textures: " << audit.textures.size() << " distinct names, " << in_atlas
              << " in " << value.skin.texture_file << ", " << standalone << " standalone, "
              << audit.none_slots << " 'none' slots\n";
    print_list("unresolved textures", audit.unresolved_textures);
    std::cout << "  fonts:";
    for (const auto& [face, count] : audit.font_faces) std::cout << " '" << face << "'=" << count;
    std::cout << "\n  font levels:";
    for (const auto& [level, count] : audit.font_levels) std::cout << ' ' << ui::to_string(level) << '=' << count;
    std::cout << '\n';
    print_list("unresolved fonts", audit.unresolved_fonts);
    std::cout << "  control captions:";
    for (const auto& [kind, count] : audit.control_captions) std::cout << ' ' << ui::to_string(kind) << '=' << count;
    std::cout << "\n  dialog captions:";
    for (const auto& [kind, count] : audit.dialog_captions) std::cout << ' ' << ui::to_string(kind) << '=' << count;
    std::cout << '\n';
    print_list("key-shaped captions missing from the text database", audit.missing_caption_keys);
    std::cout << "  literal placeholder captions: " << audit.literal_captions.size() << " distinct\n";
    print_list("unresolved tooltip keys", audit.unresolved_tooltip_keys);
    print_list("unmatched overrides", audit.unmatched_overrides);
    print_list("duplicate overrides", audit.duplicate_overrides);
    print_list("unresolved ids", audit.unresolved_ids);

    expect(value.script.source.source_id.find("Patch2.meg") != std::string::npos,
           "the script comes from Patch2.meg");
    expect(audit.dialogs == 111U && value.script.rejected.empty(), "all 111 dialogs parse");
    expect(audit.controls == 2555U, "2,555 controls");
    const std::map<std::string, std::size_t> statements{
        {"CONTROL", 954U}, {"LTEXT", 630U}, {"PUSHBUTTON", 288U}, {"DEFPUSHBUTTON", 51U},
        {"EDITTEXT", 150U}, {"RTEXT", 124U}, {"COMBOBOX", 120U}, {"LISTBOX", 93U},
        {"CTEXT", 80U}, {"GROUPBOX", 65U},
    };
    expect(audit.statements == statements, "control statement counts match the inventory");
    const std::map<std::string, std::size_t> classes{
        {"PETROGLYPH_DIALOG_IMAGE", 718U}, {"Button", 155U}, {"msctls_progress32", 51U},
        {"msctls_trackbar32", 28U}, {"IMEEditBox", 1U}, {"IMELocaleIndicator", 1U},
    };
    expect(audit.control_classes == classes, "CONTROL class counts match the inventory");
    expect(value.skin.default_textures.slots.size() == 87U, "Default has 87 texture slots");
    expect(value.skin.default_fonts.roles.size() == 9U, "Default has 9 font roles");
    expect(audit.unresolved_textures.size() == 3U, "3 textures resolve nowhere and are listed");
    expect(audit.unresolved_fonts.empty(), "every control resolves a font with a face and size");
    expect(count_of(audit.dialog_captions, ui::CaptionKind::text_key) == 0U,
           "no dialog CAPTION is a text key");
    expect(count_of(audit.control_captions, ui::CaptionKind::text_key) == 891U,
           "891 control captions are text keys");
    expect(count_of(audit.control_captions, ui::CaptionKind::empty) == 445U &&
               count_of(audit.control_captions, ui::CaptionKind::literal) == 841U,
           "445 empty and 841 literal placeholder control captions");
    expect(audit.missing_caption_keys.size() == 15U, "15 key-shaped captions are missing and listed");
    expect(audit.unresolved_tooltip_keys == std::vector<std::string>{"TEXT_COMBAT_EFF_TOOLTIP"},
           "the one tooltip key is not in the text database and is listed");
    expect(audit.textures.size() == 124U, "124 distinct texture names");
    expect(audit.unresolved_ids == std::vector<std::string>{"IDC_TEXT_AUDIO_3D_TECHNOLOGY"},
           "one control id is not in resource.h");
    expect(audit.unmatched_overrides.size() == 33U, "33 override names match nothing and are listed");
    expect(audit.duplicate_overrides.size() == 14U, "14 override names repeat and are listed");
}

// Read-only mod corpus (G1): every listed mod's dialog script loads, and a
// malformed dialog costs only that dialog.
void mod_corpus_contracts() {
    for (const auto& mod : eawr::test::ui::mod_corpora("dialog catalogue")) {
        auto catalog = ui::load_dialog_catalog(mod.filesystem);
        if (!catalog) std::cerr << mod.name << ": " << eawr::core::format_diagnostic(catalog.error()) << '\n';
        expect(static_cast<bool>(catalog), "a mod dialog catalogue loads");
        if (!catalog) continue;
        const auto& script = catalog.value().script;
        std::size_t controls{};
        for (const auto& dialog : script.dialogs) controls += dialog.controls.size();
        std::cout << "mod dialog catalogue " << mod.name << ": " << script.source.source_id << "\n  "
                  << script.dialogs.size() << " dialogs, " << controls << " controls, "
                  << script.rejected.size() << " rejected\n";
        std::size_t syntax_errors{};
        std::vector<std::string> unresolved;
        for (const auto& diagnostic : script.diagnostics) {
            if (diagnostic.code == ui::diagnostic_codes::rc_unresolved_id) unresolved.push_back(diagnostic.message);
            if (diagnostic.code != ui::diagnostic_codes::rc_syntax) continue;
            ++syntax_errors;
            std::cout << "  " << eawr::core::format_diagnostic(diagnostic) << '\n';
        }
        std::cout << "  unresolved ids: " << unresolved.size() << ", unmatched or other catalogue warnings: "
                  << catalog.value().diagnostics.size() << '\n';
        expect(syntax_errors == script.rejected.size(), "every syntax error in a mod script rejects one dialog");
        expect(!script.dialogs.empty(), "a mod script keeps its readable dialogs");
    }
}
} // namespace

void dialog_catalog_contracts() {
    tokenizer_contracts();
    header_contracts();
    script_contracts();
    rc_mod_tolerance_contracts();
    skin_contracts();
    resolution_contracts();
    audit_contracts();
    vfs_contracts();
    corpus_contracts();
    mod_corpus_contracts();
}
