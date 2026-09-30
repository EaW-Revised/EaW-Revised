#pragma once

// Retail dialog geometry: the Visual C++ resource script guidialogs.rc and the
// resource.h it includes (docs/ui/ui-layer.md, section 1.1). Engine-free; the
// game reads the script text at start-up, so this is a tokenizer and a parser
// for the DIALOG/DIALOGEX subset of the resource-script grammar.

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data::ui {

namespace diagnostic_codes {
inline constexpr std::string_view rc_syntax = "EAWR-UI-0201";
inline constexpr std::string_view rc_symbol = "EAWR-UI-0202";
inline constexpr std::string_view rc_unresolved_id = "EAWR-UI-0203";
} // namespace diagnostic_codes

enum class RcTokenKind : std::uint8_t { identifier, number, string, punctuation };

struct RcToken final {
    RcTokenKind kind{RcTokenKind::identifier};
    // Identifier and punctuation spelling, number literal as written, or the
    // decoded string (quotes removed, "" and backslash escapes applied).
    std::string text;
    std::int64_t value{};
    std::uint32_t line{1};
    std::uint32_t column{1};
};

// A preprocessor line, kept verbatim without its leading '#'.
struct RcDirective final {
    std::string text;
    std::uint32_t line{1};
};

struct RcTokens final {
    std::vector<RcToken> tokens;
    std::vector<RcDirective> directives;
};

// Comments are dropped and preprocessor lines are set aside; conditional
// blocks are not evaluated, so APSTUDIO-only resources reach the parser and
// are skipped there by type.
[[nodiscard]] core::Result<RcTokens> tokenize_rc(std::string_view text,
                                                 const assets::Source& source);

struct ResourceSymbols final {
    assets::Source source;
    // #define NAME <integer> in file order; the first definition of a name wins.
    std::map<std::string, std::int64_t, std::less<>> values;
    std::vector<core::Diagnostic> diagnostics;

    [[nodiscard]] std::optional<std::int64_t> find(std::string_view name) const;
};

[[nodiscard]] core::Result<ResourceSymbols> parse_resource_header(std::string_view text,
                                                                  assets::Source source);

// Dialog units as written in the script.
struct RcRect final {
    std::int32_t x{}, y{}, width{}, height{};
    friend bool operator==(const RcRect&, const RcRect&) = default;
};

struct DialogControl final {
    // Statement keyword as written: LTEXT, PUSHBUTTON, EDITTEXT, CONTROL, ...
    std::string statement;
    // Window class: explicit for CONTROL, implied by the keyword otherwise.
    std::string class_name;
    // Absent for the forms without text (EDITTEXT, COMBOBOX, LISTBOX, SCROLLBAR).
    std::optional<std::string> text;
    // Symbol or literal number as written; the value is filled by resolve_ids.
    std::string id_name;
    std::optional<std::int64_t> id;
    RcRect rect;
    // Style terms as written, e.g. "WS_TABSTOP", "NOT WS_VISIBLE", "0x80".
    std::vector<std::string> style;
    std::vector<std::string> extended_style;
    std::optional<std::int64_t> help_id;
    std::uint32_t line{};
};

struct DialogFont final {
    std::int32_t point_size{};
    std::string face;
    std::optional<std::int32_t> weight;
    std::optional<std::int32_t> italic;
    std::optional<std::int32_t> charset;
};

struct Dialog final {
    std::string name;
    std::optional<std::int64_t> id;
    // DIALOGEX, or the older DIALOG form.
    bool extended{true};
    RcRect rect;
    std::optional<std::int64_t> help_id;
    std::vector<std::string> style;
    std::vector<std::string> extended_style;
    std::optional<std::string> caption;
    std::optional<DialogFont> font;
    std::optional<std::string> menu;
    std::optional<std::string> window_class;
    std::vector<DialogControl> controls;
    std::uint32_t line{};
};

// A resource the parser does not model (TEXTINCLUDE, DESIGNINFO, ...).
struct SkippedResource final {
    std::string name;
    std::string type;
    std::uint32_t line{};
};

// A dialog the parser could not read. Its EAWR-UI-0201 error in
// DialogScript::diagnostics names it, the defect's line and the reason.
struct RejectedDialog final {
    std::string name;
    std::uint32_t line{};
};

struct DialogScript final {
    assets::Source source;
    std::vector<Dialog> dialogs;
    std::vector<RejectedDialog> rejected;
    // Targets of #include lines, in order (for example resource.h).
    std::vector<std::string> includes;
    std::vector<SkippedResource> skipped;
    std::vector<core::Diagnostic> diagnostics;

    [[nodiscard]] const Dialog* find(std::string_view name) const noexcept;
};

// A malformed dialog is rejected on its own (listed in rejected, with one
// EAWR-UI-0201 error) and the rest still parse, as mod scripts need. Only a
// tokenizer error fails the whole script.
[[nodiscard]] core::Result<DialogScript> parse_dialog_script(std::string_view text,
                                                             assets::Source source);

// Fills Dialog::id and DialogControl::id from the symbols. IDC_STATIC, which
// the script takes from the Windows resource headers rather than resource.h,
// is -1 unless resource.h defines it. Each unresolved name gives one warning.
void resolve_ids(DialogScript& script, const ResourceSymbols& symbols);

} // namespace eawr::data::ui
