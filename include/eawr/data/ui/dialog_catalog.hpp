#pragma once

// Engine-free dialog catalogue (#169): the guidialogs.rc geometry plus the
// GUIDialogs.xml skin (textures, fonts, tooltip) resolved per dialog and
// control. docs/ui/ui-layer.md sections 1.1, 1.2 and 3.6.

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/ui/dialog_script.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data::ui {

namespace diagnostic_codes {
inline constexpr std::string_view skin_xml = "EAWR-UI-0204";
inline constexpr std::string_view skin_value = "EAWR-UI-0205";
inline constexpr std::string_view override_duplicate = "EAWR-UI-0206";
inline constexpr std::string_view override_unmatched = "EAWR-UI-0207";
} // namespace diagnostic_codes

// Retail locations in the effective FoC VFS (Patch2.meg wins).
inline constexpr std::string_view dialog_script_path = "Data/Resources/GUIDialog/guidialogs.rc";
inline constexpr std::string_view dialog_skin_path = "Data/XML/GUIDialogs.xml";

struct Rgba final {
    std::uint8_t red{}, green{}, blue{}, alpha{};
    friend bool operator==(const Rgba&, const Rgba&) = default;
};

// One font description. Fields the XML leaves out stay unset; the most
// specific description is taken whole (no per-field fallback).
struct FontSpec final {
    std::optional<std::string> face;
    std::optional<double> size;
    std::optional<std::int32_t> character_padding;
    std::optional<double> stretch_factor;
    std::optional<Rgba> top_color;
    std::optional<Rgba> bottom_color;
    std::optional<bool> emboss;
    std::optional<bool> outline;
    std::uint32_t line{};
};

// The Default font roles, named as the XML elements.
enum class FontRole : std::uint8_t {
    global_default,
    push_button,
    list_box,
    combo_box,
    edit_box,
    ime_edit_box,
    l_text,
    r_text,
    overlay_caption_text,
};
[[nodiscard]] std::string_view to_string(FontRole role) noexcept;
[[nodiscard]] std::optional<FontRole> font_role_from_name(std::string_view name) noexcept;

struct TextureSlot final {
    std::string slot;
    // As written; "none" marks a slot explicitly left empty.
    std::string texture;
    std::uint32_t line{};
};

struct TextureSet final {
    std::string name;
    std::vector<TextureSlot> slots;
    std::uint32_t line{};
    [[nodiscard]] const TextureSlot* find(std::string_view slot) const noexcept;
};

struct RoleFont final {
    FontRole role{FontRole::global_default};
    FontSpec spec;
};

// Default and dialog entries hold role fonts; control entries hold one spec.
struct FontSet final {
    std::string name;
    std::optional<FontSpec> spec;
    std::vector<RoleFont> roles;
    std::uint32_t line{};
    [[nodiscard]] const FontSpec* find(FontRole role) const noexcept;
};

struct TooltipEntry final {
    std::string name;
    std::string text_id;
    std::uint32_t line{};
};

struct DialogSkin final {
    assets::Source source;
    // Textures File and Compressed_File: the atlas stem in Data/Art/Textures.
    std::string texture_file;
    std::string compressed_texture_file;
    TextureSet default_textures;
    // File order. A repeated name is reported and the first entry wins
    // (open until observed, as for text keys).
    std::vector<TextureSet> texture_overrides;
    FontSet default_fonts;
    std::vector<FontSet> font_overrides;
    std::vector<TooltipEntry> tooltips;
    std::vector<core::Diagnostic> diagnostics;

    [[nodiscard]] const TextureSet* texture_override(std::string_view name) const noexcept;
    [[nodiscard]] const FontSet* font_override(std::string_view name) const noexcept;
    [[nodiscard]] const TooltipEntry* tooltip(std::string_view name) const noexcept;
};

[[nodiscard]] core::Result<DialogSkin> parse_dialog_skin(std::span<const std::byte> bytes,
                                                         assets::Source source);

enum class OverrideLevel : std::uint8_t { none, default_set, dialog, control };
[[nodiscard]] std::string_view to_string(OverrideLevel level) noexcept;

struct TextureResolution final {
    OverrideLevel level{OverrideLevel::none};
    // Unset when no level names the slot or the winning entry is "none".
    std::optional<std::string> texture;
};

struct FontResolution final {
    FontRole role{FontRole::global_default};
    OverrideLevel level{OverrideLevel::none};
    const FontSpec* spec{};
};

// Role by statement or class name: PUSHBUTTON/DEFPUSHBUTTON/PUSHBOX use
// Push_Button, LISTBOX List_Box, COMBOBOX Combo_Box, EDITTEXT Edit_Box, class
// IMEEditBox IME_Edit_Box, LTEXT L_Text, RTEXT R_Text; every other control
// uses Global_Default.
[[nodiscard]] FontRole font_role(const DialogControl& control) noexcept;

struct DialogCatalog final {
    ResourceSymbols symbols;
    DialogScript script;
    DialogSkin skin;
    // Override names that match no dialog and no control id.
    std::vector<core::Diagnostic> diagnostics;

    // Precedence: control entry (by id name), dialog entry, Default.
    [[nodiscard]] TextureResolution texture(const Dialog& dialog, const DialogControl* control,
                                            std::string_view slot) const;
    // Precedence: control spec, the dialog's role font, the Default role font,
    // then the Default Global_Default font.
    [[nodiscard]] FontResolution font(const Dialog& dialog, const DialogControl& control) const;
    [[nodiscard]] const TooltipEntry* tooltip(const DialogControl& control) const noexcept;
};

[[nodiscard]] DialogCatalog build_dialog_catalog(ResourceSymbols symbols, DialogScript script,
                                                 DialogSkin skin);

// Reads dialog_script_path, the resource header it includes from the same
// folder, and dialog_skin_path through the VFS.
[[nodiscard]] core::Result<DialogCatalog> load_dialog_catalog(const vfs::Vfs& filesystem);

enum class CaptionKind : std::uint8_t { empty, text_key, missing_key, literal };
[[nodiscard]] std::string_view to_string(CaptionKind kind) noexcept;
// A caption is a text key when the database holds it. Otherwise upper-case
// identifier text with an underscore (TEXT_X, TXT_X) is a missing key and
// anything else is literal placeholder text; both render as written (UI-T3).
[[nodiscard]] CaptionKind classify_caption(std::string_view text, const TextDatabase& database);

// True when a standalone texture of that name exists.
using TextureProbe = std::function<bool(std::string_view texture)>;
// Probes Data/Art/Textures/<stem>.tga and .dds, as texture references do.
[[nodiscard]] TextureProbe vfs_texture_probe(const vfs::Vfs& filesystem);

struct TextureAudit final {
    std::string texture;
    std::size_t references{};
    bool in_atlas{};
    bool standalone{};
};

// Acceptance evidence: what parses, resolves, or is listed as unresolved.
struct CatalogAudit final {
    std::size_t dialogs{};
    std::size_t controls{};
    std::map<std::string, std::size_t> statements;
    std::map<std::string, std::size_t> control_classes;
    std::vector<TextureAudit> textures;
    std::size_t none_slots{};
    std::vector<std::string> unresolved_textures;
    std::map<std::string, std::size_t> font_faces;
    std::map<OverrideLevel, std::size_t> font_levels;
    std::vector<std::string> unresolved_fonts;
    std::map<CaptionKind, std::size_t> control_captions;
    std::map<CaptionKind, std::size_t> dialog_captions;
    std::vector<std::string> missing_caption_keys;
    std::vector<std::string> literal_captions;
    std::vector<std::string> unresolved_tooltip_keys;
    std::vector<std::string> unmatched_overrides;
    std::vector<std::string> duplicate_overrides;
    std::vector<std::string> unresolved_ids;
};

[[nodiscard]] CatalogAudit audit_dialog_catalog(const DialogCatalog& catalog,
                                                const assets::MegaTexture& atlas,
                                                const TextDatabase& text,
                                                const TextureProbe& standalone);

} // namespace eawr::data::ui
