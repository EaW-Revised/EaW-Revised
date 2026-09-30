#pragma once

// Typed catalogue of the command-bar components (`CommandBarComponents.xml`,
// listed by `CommandBarComponentFiles.xml`); design docs/ui/ui-layer.md section 1.1
// and section 1.3, ticket UI-03 (#170).
//
// Parsing policy (data facts, not retail code):
// - Tags match ASCII case-insensitively, so the authored
//   `Icon_ALternate_Texture_Name` is `Icon_Alternate_Texture_Name`.
// - A repeated tag keeps the last occurrence that yields a value; each repeat
//   is reported. `Group` is the exception: every non-empty occurrence is a
//   group membership, in authored order.
// - An empty value clears a list or text field but is ignored, and reported,
//   for flag, number, vector and colour fields.
// - Numbers accept a leading numeric prefix per token (`37` followed by a
//   stray character reads 37) and report the rest. A vector with one
//   component has y = 0; a colour with three components has alpha 255.
// - A later component with the same name (case-insensitive) replaces the
//   earlier one in place and is reported.

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eawr::data::ui {

enum class ComponentType : std::uint8_t { button, text_button, bar, shell, icon };

// A trailing `_ALT<n>` on a component or shell-mesh name selects a faction
// variant: ALT0 Empire, ALT1 Rebel, ALT2 Underworld (design section 1.3). Names
// without the suffix belong to every variant.
namespace alt_variant {
inline constexpr std::uint32_t empire = 0;
inline constexpr std::uint32_t rebel = 1;
inline constexpr std::uint32_t underworld = 2;
} // namespace alt_variant

struct AltName {
    std::string base;
    std::optional<std::uint32_t> variant;

    [[nodiscard]] bool shown_for(std::uint32_t selected) const noexcept {
        return !variant || *variant == selected;
    }
};

[[nodiscard]] AltName split_alt(std::string_view name);

enum class FieldKind : std::uint8_t {
    flag,       // True/False
    integer,    // one whole number
    number,     // one real number
    text,       // one string; may contain spaces (`Arial Bold`, `Fleet 0`)
    tokens,     // whitespace-separated list (textures, tooltip keys)
    names,      // comma-separated list (font names)
    vec2,       // "x y"
    color,      // "r g b [a]", 0..255
};

// Every component tag in the FoC data except the structural `Type` and
// `Group`. Order matches the schema table in command_bar.cpp.
enum class Field : std::uint8_t {
    // flags
    animate_back,
    animate_upper_effect,
    blink_fade,
    can_animate,
    can_drag_stack,
    click_shift,
    cross_fade,
    dialog_scene,
    disable_darken,
    disabled,
    disabled_darken,
    drag_and_drop,
    drag_back,
    drag_select,
    ghost_base_only,
    hidden,
    left_justified,
    loop_anim,
    lower_effect_additive,
    manual_offset,
    model_offset_x,
    model_offset_y,
    no_hidden_collision,
    no_shell,
    offset_render,
    outlined_bar,
    pixel_align,
    right_justified,
    selected_alpha,
    should_ghost,
    should_render_at_drag_pos,
    smooth_bar,
    snap_drag,
    snap_location,
    stackable,
    swap_texture,
    tab,
    text_emboss,
    text_outline,
    toggle,
    tutorial_scene,
    // integers
    anim_fps,
    base_layer,
    font_point_size,
    max_bar_level,
    max_text_width,
    // numbers
    blink_duration,
    blink_rate,
    scale,
    scale_duration,
    // vectors
    build_dial2_offset,
    build_dial_offset,
    default_offset,
    default_offset_widescreen,
    disabled_offset,
    icon_offset,
    lower_effect_offset,
    mouse_over_offset,
    offset,
    overlay2_offset,
    overlay_offset,
    size,
    text_offset,
    text_offset2,
    upper_effect_offset,
    // colours
    color,
    text_color,
    text_color2,
    // text
    click_sfx,
    font_name,
    mega_texture_name,
    model_name,
    mouse_over_sfx,
    // token lists
    bar_overlay_name,
    bar_texture_name,
    blank_texture_name,
    build_texture_name,
    disabled_texture_name,
    flash_texture_name,
    icon_alternate_texture_name,
    icon_texture_name,
    lower_effect_texture_name,
    mouse_over_texture_name,
    overlay2_texture_name,
    overlay_texture_name,
    selected_texture_name,
    tooltip_text,
    // comma lists
    alternate_font_name,
};

inline constexpr std::size_t field_count = static_cast<std::size_t>(Field::alternate_font_name) + 1U;

struct Vec2 {
    float x{};
    float y{};

    friend bool operator==(const Vec2&, const Vec2&) = default;
};

struct Rgba8 {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{255};

    friend bool operator==(const Rgba8&, const Rgba8&) = default;
};

using FieldValue = std::variant<bool, std::int32_t, float, std::string, std::vector<std::string>, Vec2, Rgba8>;

struct FieldEntry {
    Field field{};
    FieldValue value;
    std::uint64_t line{};
};

struct UnknownField {
    std::string tag;
    std::string value;
    std::uint64_t line{};
};

struct CommandBarComponent {
    std::string name;
    AltName alt;
    ComponentType type{ComponentType::button};
    std::vector<std::string> groups;
    std::vector<FieldEntry> fields; // first-occurrence order, one entry per field
    std::vector<UnknownField> unknown_fields;
    std::string logical_path;
    std::uint64_t line{};

    [[nodiscard]] const FieldValue* find(Field field) const noexcept;
    [[nodiscard]] bool has(Field field) const noexcept { return find(field) != nullptr; }
    [[nodiscard]] bool flag(Field field, bool fallback = false) const noexcept;
    [[nodiscard]] std::optional<std::int32_t> integer(Field field) const noexcept;
    [[nodiscard]] std::optional<float> number(Field field) const noexcept;
    [[nodiscard]] std::string_view text(Field field) const noexcept;
    [[nodiscard]] std::span<const std::string> list(Field field) const noexcept; // tokens and names
    [[nodiscard]] std::optional<Vec2> vec2(Field field) const noexcept;
    [[nodiscard]] std::optional<Rgba8> color(Field field) const noexcept;
    [[nodiscard]] bool in_group(std::string_view group) const noexcept;
};

class CommandBarCatalog {
public:
    [[nodiscard]] const std::vector<CommandBarComponent>& components() const noexcept { return components_; }
    [[nodiscard]] const std::vector<std::string>& source_files() const noexcept { return source_files_; }
    // ASCII case-insensitive.
    [[nodiscard]] const CommandBarComponent* find(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t count(ComponentType type) const noexcept;
    // The Shell component whose Model_Name is the given model (case-insensitive).
    [[nodiscard]] const CommandBarComponent* shell_for_model(std::string_view model_name) const noexcept;

    // Adds or replaces (same name) a component; returns true when it replaced one.
    bool add(CommandBarComponent component);
    void add_source_file(std::string logical_path) { source_files_.push_back(std::move(logical_path)); }

private:
    std::vector<CommandBarComponent> components_;
    std::vector<std::string> source_files_;
};

struct CommandBarLoad {
    CommandBarCatalog catalog;
    std::vector<core::Diagnostic> diagnostics;
};

inline constexpr std::string_view command_bar_list_path = "data/xml/commandbarcomponentfiles.xml";

// Parses one component file into `catalog`. Fails only when the XML itself is
// unreadable; per-component problems become diagnostics.
[[nodiscard]] core::Result<void> parse_command_bar_components(std::span<const std::byte> bytes,
    const vfs::AssetRecord& record, CommandBarCatalog& catalog, std::vector<core::Diagnostic>& diagnostics);

// Reads the file list and every listed file from data/xml/ in list order.
[[nodiscard]] core::Result<CommandBarLoad> load_command_bar(const vfs::Vfs& filesystem);

[[nodiscard]] std::string_view to_string(ComponentType type) noexcept;
[[nodiscard]] std::optional<ComponentType> component_type_from(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(Field field) noexcept; // authored tag spelling
[[nodiscard]] FieldKind field_kind(Field field) noexcept;
[[nodiscard]] std::optional<Field> field_from_tag(std::string_view tag) noexcept;

namespace diagnostic_codes {
inline constexpr std::string_view command_bar_list = "EAWR-UI-0301";       // list file missing or malformed
inline constexpr std::string_view command_bar_file = "EAWR-UI-0302";       // listed file missing or malformed
inline constexpr std::string_view component_invalid = "EAWR-UI-0303";      // no name, no or unknown Type
inline constexpr std::string_view component_duplicate = "EAWR-UI-0304";    // same name again
inline constexpr std::string_view field_repeated = "EAWR-UI-0305";         // same tag twice in a component
inline constexpr std::string_view field_value = "EAWR-UI-0306";            // value not readable as its kind
inline constexpr std::string_view field_unknown = "EAWR-UI-0307";          // tag not in the schema
} // namespace diagnostic_codes

} // namespace eawr::data::ui
