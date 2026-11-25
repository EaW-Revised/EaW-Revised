#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/build_tray_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::vfs { class Vfs; }

// The two Revised layer files of the galactic build tray (docs/ui/galactic-build-tray.md, GBT-11 and
// GBT-12): `Data/XML/RevisedBuildTray.xml` (the handle and the tray layout) and
// `Data/XML/RevisedBuildTrayGroups.xml` (the grouping). Engine-free: parse and validate a document,
// and pick the winning file across layers. Nothing here draws or reads game objects.
//
// Layer rule (GBT-11, as for the planet search): layers are tried lowest first and a valid file
// replaces what is below it whole; an invalid file is skipped, keeps the last valid definition and sets
// `invalid` so the caller reports one diagnostic. No file in any layer: no definition, no tray.
namespace eawr::presentation::ui::build_tray {

inline constexpr std::string_view layout_path = "Data/XML/RevisedBuildTray.xml";
inline constexpr std::string_view groups_path = "Data/XML/RevisedBuildTrayGroups.xml";

struct HandleCandidate final {
    std::string anchor;  // a shell component name, `viewport` or `production_bar`
    std::string side;    // above, below, left, right, inside_top_left ... inside_bottom_right
    double x{}, y{};
    friend bool operator==(const HandleCandidate&, const HandleCandidate&) = default;
};

// GBT-02: the handle is replaced whole, like the project HUD layer's Gift.
struct HandleDefinition final {
    bool enabled{true};
    bool stock_art{true};  // no texture named: the stock art of the mounted command atlas
    double width{48}, height{20};
    std::string caption{"TEXT_EAWR_BUILD_TRAY_MORE"}, tooltip{"TEXT_EAWR_BUILD_TRAY_TOOLTIP"};
    std::string normal, hover, pressed, disabled;
    std::vector<HandleCandidate> candidates{{"production_bar", "above", 0, 0}};
    friend bool operator==(const HandleDefinition&, const HandleDefinition&) = default;
};

// Bar: the box around the bar's slots. Fixed: `Width`. Automatic: from the box's width up to `Max_Width_Share`
// of the view, the narrowest width at which the groups fit the row budget and the free height without misc (GBT-06).
enum class WidthMode : std::uint8_t { bar, fixed, automatic };

// GBT-10: Bar: the panel's back and frame are the bar's own shell art, sampled behind and just above its slots.
// Component: the art of `Back_Component` (its blank and alternate textures).
enum class BackSource : std::uint8_t { bar, component };

// GBT-11. An omitted element takes the built-in default named here.
struct TrayLayout final {
    bool enabled_by_default{true};
    HandleDefinition handle;
    std::string dialog_name{"IDD_EAWR_BUILD_TRAY"};
    std::string entry_template{"b_Create_00"};
    double entry_scale{0.75}, compact_entry_scale{0.6};
    double entry_gap{2}, group_gap{12}, row_gap{4}, panel_padding{6};
    double header_scale{0.8};
    std::string header_template;            // command bar component whose text look the headers use; empty: the entry's
    std::string divider_texture;
    std::size_t max_rows{7};                // 6 unit rows and the buildings row
    bool fold_single_choice_groups{true};
    WidthMode width_mode{WidthMode::automatic};
    double width{};                         // Width_Mode Fixed only
    double max_width_share{0.6};            // Width_Mode Automatic only
    BackSource back_source{BackSource::bar};
    std::string back_component;             // Back_Source Component; with Bar, used only when the bar has no art there
    double frame_width{3};                  // reference units; 0: no frame
    std::string galaxy_top_component;       // exactly one of these two, or neither
    std::optional<double> galaxy_top_y;
    std::vector<std::string> planet_view_cap_components;
    friend bool operator==(const TrayLayout&, const TrayLayout&) = default;
};

// GBT-12.
struct TrayGroups final {
    GroupDefinition definition;
};

template <typename T>
struct Parsed final {
    std::optional<T> value;
    std::string error;  // the first reason the document was rejected
};

[[nodiscard]] Parsed<TrayLayout> parse_tray_layout(const data::XmlDocument& document);
[[nodiscard]] Parsed<TrayGroups> parse_tray_groups(const data::XmlDocument& document);

template <typename T>
struct Loaded final {
    std::optional<T> value;
    bool invalid{};           // some layer's file was rejected: report one diagnostic
    std::string first_error;
};

[[nodiscard]] Loaded<TrayLayout> load_tray_layout(const vfs::Vfs& files);
[[nodiscard]] Loaded<TrayGroups> load_tray_groups(const vfs::Vfs& files);

// The category enum in authored order: the element names of `Data/XML/Enum/GameObjectCategoryType.xml` as the
// mounted layers resolve it. Empty when no such file loads.
[[nodiscard]] CategoryEnum load_category_enum(const vfs::Vfs& files);

// GBT-13: the active layer's default for the Tweaks switch: the winning layout file's
// `Enabled_By_Default`, or on when no valid layout file exists. The overload on a loaded layout lets the
// caller also report `invalid` (one EAWR-UI diagnostic, GBT-11).
[[nodiscard]] bool layer_enabled_by_default(const Loaded<TrayLayout>& layout);
[[nodiscard]] bool layer_enabled_by_default(const vfs::Vfs& files);

// GBT-12, checks that need the mod's category enum, which the parser does not have. Each string is one
// problem; any problem makes the file invalid for that mod (keep the last valid definition and report
// one diagnostic): an extra group named like a category, `Buildings` or `Misc`; an assignment or an
// `After` naming a group the file and the enum do not define. A caption for a group nothing defines is
// not a problem: the default file carries captions for the stock names, whatever the mod defines.
[[nodiscard]] std::vector<std::string> check_against_categories(const GroupDefinition& definition,
    const CategoryEnum& categories);

// GBT-12: the number of type assignments whose type (for a prefix, every type with it) no loaded type
// defines. One info diagnostic carries the count; it flags a stale patch.
[[nodiscard]] std::size_t stale_assignments(const GroupDefinition& definition, const TypeIndex& types);

} // namespace eawr::presentation::ui::build_tray
