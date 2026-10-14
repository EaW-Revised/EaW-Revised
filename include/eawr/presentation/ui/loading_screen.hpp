#pragma once

// The skirmish battle loading screen (docs/behaviour/loading-screen.md, rules BLD-01 to BLD-14):
// what the dialog shows for a map (a model built from the game data), the load progress behind
// its bar and the line wrapping of its history text. Engine-free; the viewer draws it.

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

namespace diagnostic_codes {
inline constexpr std::string_view loading_planet = "EAWR-UI-0340"; // the map names no planet the data describes
inline constexpr std::string_view loading_text = "EAWR-UI-0341";   // a text key is missing from the text database
} // namespace diagnostic_codes

// BLD-01: a skirmish battle uses this dialog of the dialog resources, whatever the map.
inline constexpr std::string_view skirmish_load_dialog = "IDD_SKIRMISH_BATTLE_LOAD_DIALOG";

enum class LoadingBattle : std::uint8_t { space, land };
enum class LoadingMission : std::uint8_t { ordinary, tutorial, story, corruption };

struct LoadingScreenInput {
    LoadingBattle battle{LoadingBattle::space};
    // The map's logical path, for its preview texture (BLD-06).
    std::string map_path;
    // The map's root field 0x09 (assets::Map::context_name): the name of the planet the battle is at.
    std::optional<std::string> context_name;
    // The map's own display name, used only when the data describe no planet (BLD-09).
    std::string map_name;
    const data::Catalog* objects{};
    data::ui::TextLookup* text{};
    // Whether a logical file exists in the mounted data (the preview texture, BLD-06).
    std::function<bool(std::string_view)> exists;
    bool campaign{}; // WGP-31: location before prepare text; campaign dialog.
    LoadingMission mission{LoadingMission::ordinary}; // explicit retained admission
    std::optional<std::string> mission_map; // dialog map, never selected map/script inference
};

// What the dialog's gadgets show. Every string is UTF-8; a texture is a file name as the data write
// it (`load_royal_guard.tga`), looked up under Data/Art/Textures by the viewer.
struct LoadingScreenContent {
    std::string dialog{skirmish_load_dialog};
    std::string title;        // IDC_STATIC_PLANET_NAME: "Space Battle at Coruscant"
    std::string history;      // IDC_LIST_REFERENCE
    std::string tactical, advantage; // WGP-31 campaign non-selectable lists
    std::string mission_heading, mission_body; // WGP-169: MT/MB keys, white and nonselectable
    std::string weather;      // IDC_STATIC_WEATHER
    std::string terrain;      // IDC_STATIC_TERRAIN
    std::string population;   // IDC_STATIC_NATIVE
    std::string native_icon;  // IDC_NATIVE_ICON, empty when the planet names none
    std::string weather_icon; // IDC_WEATHER_ICON, empty when the planet names none
    std::string overlay_icon; // IDC_PLANET_OVERLAY
    std::string map_icon;     // IDC_MAP_ICON: a logical path, empty when the map has no preview texture
    std::string planet;       // the Planet object the data name, empty when none
    std::vector<core::Diagnostic> diagnostics;
};

[[nodiscard]] LoadingScreenContent loading_screen_content(const LoadingScreenInput& input);

// BLD-04: the weather icon the dialog shows for a planet's `Encyclopedia_Weather_Icon`: the name
// without its first four letters when it starts with `i_pa`, behind `load`; any other name stays.
[[nodiscard]] std::string loading_weather_icon(std::string_view encyclopedia_icon);

// BLD-06: the preview texture of a map: the map file's name with its extension replaced by .dds,
// under data/art/textures. Empty for a path without a file name.
[[nodiscard]] std::string loading_map_icon_path(std::string_view map_path);
// WGP-169: preserve the path, uppercase, truncate at the first .TED.
[[nodiscard]] std::string loading_mission_text_key(std::string_view prefix, std::string_view dialog_map);

// BLD-05: the history text as the lines of a list box `max_width` wide. A line breaks before the first
// word that no longer fits; a word wider than the line stands alone on one.
[[nodiscard]] std::vector<std::string> loading_wrap_lines(
    std::string_view text, double max_width, const std::function<double(std::string_view)>& measure);

// BLD-10, BLD-11: the load's stages, in the order the viewer's preparation runs them. A stage's weight
// is its share of the bar from the measured preparation costs (docs/behaviour/loading-screen.md);
// the map retains a small allowance because its decode precedes the first report. Shares add up to one.
struct LoadStage {
    std::string_view name;
    double weight;
};
inline constexpr std::size_t load_stage_count = 5;
inline constexpr std::array<LoadStage, load_stage_count> battle_load_stages{{
    {"map", 0.05},
    {"session", 0.20},
    {"effects", 0.10},
    {"interface", 0.05},
    {"scene", 0.60},
}};
inline constexpr std::size_t load_stage_map = 0;
inline constexpr std::size_t load_stage_session = 1;
inline constexpr std::size_t load_stage_effects = 2;
inline constexpr std::size_t load_stage_interface = 3;
inline constexpr std::size_t load_stage_scene = 4;

// The honest progress of the load: the fraction of the stages' work items done, never a clock. A
// stage reports `done` of `total` items; earlier stages count as done. The value never decreases and
// reaches one only through complete().
// WGP-112..114: completion and player acceptance are separate. The owner
// supplies topmost admission; unsafe dispatched input cannot arm a later Begin.
class CampaignLoadingGate final {
public:
    void complete(bool success) noexcept {
        pending_ = false;
        if (!success) { failed_ = true; ready_ = false; requested_ = false; }
        else if (!failed_) ready_ = true;
    }
    [[nodiscard]] bool unsafe() const noexcept { return pending_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    void request(bool topmost) noexcept { if (ready_ && topmost && !accepted_) requested_ = true; }
    [[nodiscard]] bool take(bool topmost) noexcept {
        if (!ready_ || !requested_ || !topmost || accepted_) return false;
        requested_ = false; accepted_ = true; return true;
    }
private:
    bool pending_{true}, ready_{}, requested_{}, accepted_{}, failed_{};
};

class LoadProgress final {
public:
    void report(std::size_t stage, std::uint64_t done, std::uint64_t total) noexcept;
    void complete() noexcept;
    [[nodiscard]] double fraction() const noexcept { return finished_ ? 1.0 : fraction_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::size_t stage() const noexcept { return stage_; }
    [[nodiscard]] std::string_view stage_name() const noexcept;
    // The number of reports taken, for a test or a report.
    [[nodiscard]] std::uint64_t reports() const noexcept { return reports_; }

private:
    double fraction_{};
    std::size_t stage_{};
    std::uint64_t reports_{};
    bool finished_{};
};

} // namespace eawr::presentation::ui
