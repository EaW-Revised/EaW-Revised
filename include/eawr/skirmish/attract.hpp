#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The main-menu demo attract battle's engine-free rules (docs/behaviour/main-menu-attract.md,
// MAT-01 to MAT-07, MAT-13): the four GameConstants tags it reads, and the manager that decides
// when a map starts, advances or stops. Time is the caller's monotonic millisecond clock; nothing
// here reads a clock, a thread or a host file.
namespace eawr::skirmish {

// GameConstants as the attract reads them (MAT-01, MAT-03, MAT-04, MAT-05).
struct AttractSettings final {
    bool menu_attract{};                       // Main_Menu_Demo_Attract_Mode
    std::vector<std::string> maps;             // Demo_Attract_Maps, file names under data/art/maps/
    std::vector<std::int64_t> cycle_delays;    // Demo_Attract_Map_Cycle_Delay_Seconds, by map position
    std::int64_t start_timeout_seconds{};      // Demo_Attract_Start_Timeout_Seconds

    friend bool operator==(const AttractSettings&, const AttractSettings&) = default;
};

// The delay of a map whose position has no entry in the delay list (MAT-04).
inline constexpr std::int64_t attract_default_delay_seconds = 120;
// The idle time, in milliseconds, the cycle check waits for with a dialog up (MAT-04): the main
// menu's own value and every other dialog's.
inline constexpr std::uint64_t attract_menu_idle_ms = 8000;
inline constexpr std::uint64_t attract_dialog_idle_ms = 20000;

// Reads the four tags from the root of gameconstants.xml (a repeated element appends to a list
// tag, a repeated scalar keeps its last value). A tag that is absent leaves its default: the
// attract off, no maps, no delays, no timeout. Marks every tag it reads as used.
[[nodiscard]] AttractSettings attract_settings(const data::XmlNode& constants_root);

// Mounted game data: loads data/xml/gameconstants.xml and reads it as above.
[[nodiscard]] core::Result<AttractSettings> read_attract_settings(const vfs::Vfs& filesystem);

// The logical path of a configured map: `data/art/maps/` plus the name, canonicalised (case and
// separators folded). Empty for an empty name or a name that leaves the maps directory.
[[nodiscard]] std::string attract_map_path(const std::string& name);

// MAT-09: the AI player type the game gives each editor player index of a demo map; every other
// player runs none. These two are the game's own fixed choice (in the stock data each equals its
// faction's Basic_AI); the engine takes the names from nowhere else.
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::string>> attract_ai_players();

// MAT-10: the tactical-camera state a demo map saves in its camera chunk. The view stays at it for
// the whole map. Each value is the stored binary32 bit pattern (the simulation boundary keeps
// floating types out of this library; the viewer decodes them): degrees and TED units.
struct AttractCamera final {
    std::uint32_t pitch_degrees_bits{};  // down from the horizon
    std::uint32_t yaw_degrees_bits{};
    std::array<std::uint32_t, 3> look_at_bits{};  // source x, y, height
    std::uint32_t distance_bits{};
    std::uint32_t fov_degrees_bits{};    // the XML-style angle (horizontal on 4:3)

    friend bool operator==(const AttractCamera&, const AttractCamera&) = default;
};

// Reads the saved camera from a loaded map's raw chunks (chunk 1/261, minis 0 pitch, 1 yaw,
// 2 look-at, 3 distance, 8 field of view). Nullopt when the chunk or one of those minis is missing,
// is not a finite binary32, or (distance, field of view) is not a positive normal number.
[[nodiscard]] std::optional<AttractCamera> attract_camera(const assets::Map& map);

// Keys the attract itself reads while it runs with no dialog up (MAT-07).
enum class AttractKey : std::uint8_t { other, backspace, enter, escape, minus };

// What a service call or a key asks the host to do.
enum class AttractAction : std::uint8_t {
    none,
    advance, // load the map advance() chose
    stop,    // tear everything down (MAT-13)
};

// One service call's view of the front end.
struct AttractFrame final {
    std::uint64_t now_ms{};
    bool dialog_active{};     // any menu dialog is up (the main menu is one)
    bool main_menu_active{};  // the main menu is the active dialog
    bool foreground{true};    // the application window is in the foreground
};

// MAT-02..MAT-07: the clocks, the map index and the decisions. Deterministic; the host performs
// the actions it returns.
class AttractManager final {
public:
    explicit AttractManager(AttractSettings settings) : settings_(std::move(settings)) {}

    [[nodiscard]] const AttractSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    // The attract runs a map (MAT-01's "already running").
    [[nodiscard]] bool active() const noexcept { return active_; }
    // The index of the map the last start picked; empty before the first start.
    [[nodiscard]] std::optional<std::size_t> map_index() const noexcept {
        return index_ < 0 ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(index_));
    }
    [[nodiscard]] std::uint64_t idle_since_ms() const noexcept { return idle_ms_; }
    [[nodiscard]] std::uint64_t map_since_ms() const noexcept { return map_ms_; }

    // MAT-02: the idle and map clocks start at `now_ms`; the manager is initialised, not running.
    void init(std::uint64_t now_ms) noexcept;

    // MAT-03: picks the next map and marks the attract running; the map clock restarts. Nullopt
    // (and nothing changes) when the manager is not initialised or no map is configured. The
    // returned index names `settings().maps`.
    [[nodiscard]] std::optional<std::size_t> advance(std::uint64_t now_ms) noexcept;

    // MAT-04, MAT-05: one frame. `paused` is the game's pause flag for this frame (the idle clock
    // is older than the start timeout); the manager holds it so the host can pause the battle.
    struct Service final {
        bool paused{};
        AttractAction action{AttractAction::none};
    };
    [[nodiscard]] Service service(const AttractFrame& frame) noexcept;

    // MAT-06, MAT-07: a key press. The idle clock restarts. `consumed` is true when the attract
    // took the key (no dialog up, running); the host must not pass it on.
    struct KeyResult final {
        bool consumed{};
        AttractAction action{AttractAction::none};
    };
    [[nodiscard]] KeyResult key(AttractKey pressed, const AttractFrame& frame) noexcept;

    // MAT-06: pointer motion and window activation.
    void pointer_moved(std::uint64_t now_ms, bool main_menu_active) noexcept;
    void window_activated(std::uint64_t now_ms) noexcept { idle_ms_ = now_ms; }

    // MAT-13: stops everything; the next init() starts over (the map index is kept).
    void shutdown() noexcept;

private:
    [[nodiscard]] std::int64_t delay_seconds() const noexcept;
    [[nodiscard]] bool map_expired(std::uint64_t now_ms) const noexcept;

    AttractSettings settings_;
    bool initialized_{};
    bool active_{};
    std::int64_t index_{-1};
    std::uint64_t idle_ms_{};
    std::uint64_t map_ms_{};
    bool paused_{};
};

} // namespace eawr::skirmish
