#include "dialog_catalog_support.hpp"

namespace dialog_catalog_test_support {

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

ui::DialogCatalog catalog_fixture() {
    auto symbols = ui::parse_resource_header(header_fixture, source("data/resources/guidialog/resource.h"));
    auto script = ui::parse_dialog_script(script_fixture, source("data/resources/guidialog/guidialogs.rc"));
    auto skin = ui::parse_dialog_skin(bytes_of(skin_fixture), source("data/xml/guidialogs.xml"));
    expect(symbols && script && skin, "catalogue fixture parts parse");
    if (!symbols || !script || !skin) return {};
    return ui::build_dialog_catalog(std::move(symbols.value()), std::move(script.value()),
                                    std::move(skin.value()));
}


} // namespace dialog_catalog_test_support
