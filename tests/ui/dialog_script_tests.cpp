#include "dialog_catalog_support.hpp"

namespace dialog_catalog_test_support {

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


} // namespace dialog_catalog_test_support
