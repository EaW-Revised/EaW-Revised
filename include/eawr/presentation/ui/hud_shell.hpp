#pragma once

// The tactical space HUD shell (ticket P2-20a, #83; design docs/ui/ui-layer.md
// section 1.3). Engine-free: what the shell draws for one faction, read from
// the active mod's data. The shell model (`i_main_skirmish`'s Model_Name)
// supplies the geometry, and CommandBarComponents.xml the looks of the bound
// components. P2-20a draws four parts:
//
// - the shell's visible decorative art (the faction faceplate and the help
//   droid) as textured triangles, MeshAlpha alpha-blended;
// - the minimap frame: the `radar` mesh's MeshAdditive scan lines over the
//   faceplate's minimap well (the minimap itself is P2-20c);
// - the options button `b_option_t` with its four state textures;
// - the time panel's buttons (help, holocron, pause, fast forward) as inert
//   art in their normal state: FoC draws them over the faceplate's panel,
//   whose texture carries divider lines that only these buttons hide;
// - the planet name `Text_Planet_tactical` in its component font.
//
// Units are shell units (UI-L1 reference units), origin bottom-left, y up.

#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/presentation/ui/hud.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data {
class Catalog;
}
namespace eawr::data::ui {
struct TextDatabase;
}

namespace eawr::presentation::ui {

namespace diagnostic_codes {
inline constexpr std::string_view hud_shell_part = "EAWR-UI-0320";   // a P2-20a part is missing from shell or catalogue
inline constexpr std::string_view hud_planet_name = "EAWR-UI-0321";  // the planet name falls back or is unavailable
} // namespace diagnostic_codes

// The local player's faction picks the `_ALT<n>` shell variant (section 1.3).
enum class HudFaction : std::uint8_t { empire, rebel, underworld };
[[nodiscard]] std::uint32_t alt_variant(HudFaction faction) noexcept;
[[nodiscard]] std::string_view to_string(HudFaction faction) noexcept;
[[nodiscard]] std::optional<HudFaction> hud_faction_from(std::string_view text) noexcept;

// The tactical HUD's shell component and the model it names. Retail:
// `i_main_skirmish`, `i_tactical_controls.alo`.
inline constexpr std::string_view tactical_shell_component = "i_main_skirmish";
inline constexpr std::string_view tactical_shell_fallback_model = "i_tactical_controls.alo";
[[nodiscard]] std::string tactical_shell_model(const data::ui::CommandBarCatalog& catalog);
// The command bar's mega-texture (the first component that names one; retail
// `MT_CommandBar`) as the logical path of its `.mtd`.
[[nodiscard]] std::string command_bar_mega_texture(const data::ui::CommandBarCatalog& catalog);

enum class ShellBlend : std::uint8_t { alpha, additive };

// One textured shell mesh, drawn as its triangles with their authored
// (repeating) UVs; the texture is a file under Data/Art/Textures.
struct HudShellMesh {
    std::string name;
    ShellBlend blend{ShellBlend::alpha};
    std::string texture;
    float z{};
    std::vector<data::ui::ShellTriangle> triangles;
};

struct HudShellButton {
    std::string name;
    data::ui::ReferenceRect rect; // the mesh's extent
    // Where the engine centres the button's state art: the component bone's
    // origin (the mesh centre when the shell gives no bone).
    assets::Vec2f origin;
    float scale{1.0F}; // the component's Scale
    // CommandBarComponents.xml state textures (the MT_CommandBar atlas, then
    // texture files); empty when the component names none.
    std::string normal;
    std::string mouse_over;
    std::string pressed;
    std::string disabled;
    std::string tooltip; // text-DB key
    std::vector<std::string> alternates; // Icon_Alternate_Texture_Name (a card border's pieces)
};

// #425: a Bar component (a unit card's health or shield). The engine draws the level's back and
// overlay quads at their textures' size times Scale around the bone plus Offset; a smooth bar
// narrows the overlay to its percent and keeps its left edge.
struct HudBar {
    std::string name;
    assets::Vec2f origin; // the bone's origin (the mesh centre without one)
    data::ui::Vec2 offset;
    float scale{1.0F};
    std::vector<std::string> back;    // Bar_Texture_Name per level
    std::vector<std::string> overlay; // Bar_Overlay_Name per level
    std::int32_t max_level{10};       // Max_Bar_Level: levels 0 to max_level
    bool smooth{};
};

// #425: one unit card slot `s_select_NN` with its `s_health_NN` and `s_shield_NN` bars. The card's
// portrait (the type's Icon_Name) is its base quad at texture size around the bone; the count text
// (text 2, "x<n>") sits at the bone plus Text_Offset2 in the card's font.
// #454: where a card draws its ability marks (docs/behaviour/foc-ability-buttons.md AB-08): the first
// ability's icon, dial and autofire box at Icon_Offset, Build_Dial_Offset and Overlay_Offset, the
// second's at Upper_Effect_Offset, Build_Dial2_Offset and Overlay2_Offset, all from the bone.
struct HudCardMarks {
    data::ui::Vec2 icon;
    data::ui::Vec2 dial;
    data::ui::Vec2 overlay;
    data::ui::Vec2 second_icon;
    data::ui::Vec2 second_dial;
    data::ui::Vec2 second_overlay;
    std::string build;    // Build_Texture_Name: the recharge dial
    std::string overlay_texture;  // Overlay_Texture_Name: the autofire box
    std::string overlay2_texture; // Overlay2_Texture_Name
};

struct HudCardSlot {
    HudShellButton card;
    HudCardMarks marks;
    data::ui::Vec2 count_offset;
    std::string face;
    std::int32_t point_size{};
    data::ui::Rgba8 colour{255, 255, 255, 255};
    bool outline{};
    std::optional<HudBar> health;
    std::optional<HudBar> shield;
};

// #454: an ability button `special_button_NN` (AB-05..AB-07): its base, the icon drawn over it, the
// press flash (`pressed`), the disabled art, the recharge dial and the autofire outline frames
// (`alternates`) it cycles at Anim_FPS.
struct HudAbilityButton {
    HudShellButton button;
    std::string blank;     // Blank_Texture_Name
    std::string build;     // Build_Texture_Name
    float anim_fps{5.0F};  // Anim_FPS
};

struct HudShellText {
    std::string name;
    data::ui::ReferenceRect rect;
    std::string face;
    std::int32_t point_size{};
    data::ui::Rgba8 colour{255, 255, 255, 255};
    bool outline{};
    bool emboss{};
    std::optional<std::int32_t> max_text_width;
};

struct HudShell {
    std::string model;
    HudFaction faction{HudFaction::rebel};
    // Visible decorative MeshAlpha/MeshAdditive meshes of the variant plus the
    // radar scan lines, in draw order: farthest (lowest z) first, then shell order.
    std::vector<HudShellMesh> meshes;
    std::optional<data::ui::ReferenceRect> minimap;
    std::optional<HudShellButton> options;
    // The time panel's buttons in shell order, drawn as art only (their
    // behaviour is decision D7 and later tickets).
    std::vector<HudShellButton> panel_buttons;
    std::optional<HudShellText> planet_name;
    // #425: the unit card slots in component order (s_select_00, 01, ...; a column is two slots)
    // and the column borders (special_border_00, 01, ...). Empty when the shell has none.
    std::vector<HudCardSlot> card_slots;
    std::vector<HudShellButton> card_borders;
    // #454: the ability buttons special_button_00, 01, ... (two per column border, 24 in FoC).
    std::vector<HudAbilityButton> ability_buttons;
    std::vector<core::Diagnostic> diagnostics;
};

// The time panel's buttons, in draw order.
inline constexpr std::array<std::string_view, 4> tactical_panel_buttons{
    "b_droid_help_tactical", "b_story_arc_t", "b_play_pause_t", "b_fast_forward_t"};

// Never fails: a part the shell or catalogue lacks is left out with one
// EAWR-UI-0320 warning.
[[nodiscard]] HudShell hud_shell(const data::ui::ShellAnchors& shell, const data::ui::CommandBarCatalog& catalog,
                                 HudFaction faction);

// A button's state art at its texture's size, not the mesh's: the engine sizes
// each state quad to the texture's texel width and height times the
// component's Scale, in shell units, and centres it on the component's bone.
// So `b_option_t`'s 36 x 25 art overhangs its 24 x 24 mesh.
[[nodiscard]] data::ui::ReferenceRect button_quad(const HudShellButton& button, float texel_width,
                                                  float texel_height) noexcept;

// A shell point on the screen (UI-L2): origin bottom-left and y up in the
// shell, top-left and y down on the screen, in pixels.
[[nodiscard]] ReferencePoint shell_point_to_screen(double x, double y, const ShellPlacement& shell) noexcept;

// Faceplate alpha masks read from Data/Art/Textures (`<stem>.tga`, then
// `.dds`), cached per texture name; an unreadable texture yields null, which
// hud_view_model diagnoses. `filesystem` must outlive the lookup.
[[nodiscard]] ShellMaskLookup vfs_shell_masks(const vfs::Vfs& filesystem);

// The planet name the HUD shows. A tactical map names its planet in root
// field 0x09 (assets::Map::context_name, e.g. `Coruscant`); the planet's
// `Text_ID` (TEXT_OBJECT_STAR_SYSTEM_CORUSCANT) is looked up in the text DB.
enum class PlanetNameSource : std::uint8_t {
    text,         // the planet object's Text_ID, resolved in the text DB
    context_name, // no object, Text_ID or text entry: the map's context name as written
    none,         // the map names no planet: nothing is shown
};
[[nodiscard]] std::string_view to_string(PlanetNameSource source) noexcept;
struct PlanetName {
    std::string text; // UTF-8
    PlanetNameSource source{PlanetNameSource::none};
    std::string context;
    std::string text_id;
    std::vector<core::Diagnostic> diagnostics;
};
[[nodiscard]] PlanetName planet_name(const std::optional<std::string>& context_name, const data::Catalog* objects,
                                     const data::ui::TextDatabase* text);

// #425: what a unit card shows for an object type: its Icon_Name (empty without one) and its
// display name, the Text_ID through the text DB (the type name when either is missing).
struct UnitCardLooks {
    std::string icon;
    std::string name; // UTF-8
};
[[nodiscard]] UnitCardLooks unit_card_looks(std::string_view type, const data::Catalog* objects,
                                            const data::ui::TextDatabase* text);

} // namespace eawr::presentation::ui
