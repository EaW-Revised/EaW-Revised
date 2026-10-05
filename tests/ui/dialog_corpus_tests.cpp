#include "dialog_catalog_support.hpp"

namespace dialog_catalog_test_support {

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

} // namespace dialog_catalog_test_support
