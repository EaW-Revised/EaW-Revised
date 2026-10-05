#include "dialog_catalog_support.hpp"

namespace dialog_catalog_test_support {

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


} // namespace dialog_catalog_test_support
