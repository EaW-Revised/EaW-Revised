#pragma once

#include "map_input.hpp"

#include "eawr/platform/campaign_end_view.hpp"
#include "eawr/presentation/ui/front_end.hpp"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// The galactic mode (`--eawr-galactic <campaign>`, docs/galactic-architecture.md
// section 9): the galaxy scene of one snapshot as seen from one local seat:
// planets at their positions with their models, trade-route lines, fleet
// markers in orbit and in transit, ownership colours and a basic orbit camera.
// It owns its renderer, VFS mount and frame loop and parses its own options,
// like the unit mode. One LiveGameSession publishes immutable galaxy snapshots;
// seat-issued commands enter its inbox. The opt-in synthetic battle harness
// presents that same owner's retained child through the tactical view (ADR-019).
//
// With --eawr-capture it draws, captures, writes the report and exits.
// Without it the window stays open: WASD/arrows and right-drag pan, left-click
// selects, double-click toggles planet focus, wheel enters/leaves focus or the
// advanced-map state, Home restores overview (project bindings).
class GalacticHudFixture;

class GalacticMode final {
public:
    struct Options final {
        std::string campaign;
        std::optional<presentation::ui::CampaignStartRequest::Economy> custom_economy{};
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        // The local seat (ADR-020); 0 picks the snapshot's first active human seat.
        std::uint32_t seat{};
        bool seat_invalid{};
        bool coop_preview{}; // Explicit multi-human presentation qualification.
        std::uint32_t focus_planet{};
        std::uint32_t zoom_probe_planet{}; // Opt-in GPU continuity qualification.
        std::uint32_t flight_milliseconds{};
        bool camera_options_invalid{};
        bool paused{};
        std::string hud_fixture; // Capture command-line input, resolved to a provider.
        std::shared_ptr<GalacticHudFixture> hud_provider;
        // Qualification hook: queue one admitted offer of this object type for the local player at start.
        std::string queue_type;
        std::uint32_t queue_planet{}; // 0 = first planet the local player is offered the type on
        bool red_green_routes{}; // Profile adapter seam; explicit in synthetic captures.
        bool native_ai{true}; // --eawr-galactic-ai on|off: fresh campaign install, default on.
        bool native_ai_invalid{};
        bool galactic_reveal{}; // --eawr-galactic-reveal on|off: draw-only fog reveal, default off (#3931).
        bool galactic_reveal_invalid{};
        bool all_ai_capture{}; // Capture-only: preserve the camera perspective, replace simulation human seats.
        bool proof_options_invalid{};
        std::optional<std::uint64_t> capture_seed{};
        std::uint64_t simulation_ticks{1}; // driven capture target; never a render-frame clock
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        // Foundation harness: synthetic conflict on the M2 map, then return.
        bool synthetic_battle{};
        std::string space_cycle_proof; // opt-in ordinary-command GPU acceptance
        bool battle_invalid{};
        std::uint64_t battle_frames{120};
        std::uint32_t battle_cycles{1};
        // Save repository (`--eawr-galactic-save-dir`); empty is the viewer's user-data saves.
        std::filesystem::path save_directory;
        // #3035: start from a save (`--eawr-galactic-restore <name>`). The save's own campaign
        // identity prepares the campaign; an empty directory is the repository above, which a
        // restored campaign then saves into. Any refusal fails ready() with failure() set.
        struct RestoreSave {
            std::filesystem::path directory;
            std::string name;
        };
        std::optional<RestoreSave> restore_save;
    };

    // True when the command line asks for this mode (`--eawr-galactic`).
    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit GalacticMode(Options options);
    ~GalacticMode();
    GalacticMode(GalacticMode&&) noexcept;
    GalacticMode& operator=(GalacticMode&&) noexcept;
    GalacticMode(const GalacticMode&) = delete;
    GalacticMode& operator=(const GalacticMode&) = delete;

    // Builds the view, mounts content, uploads and frames. A false return has
    // written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once a capture run has written its report.
    [[nodiscard]] std::optional<int> process(double delta);
    // World input the host routes here (interactive runs only).
    void input(const godot::Ref<godot::InputEvent>& event);
    // HUD/widget producers use the same policy and seat-issued sink as world input.
    void input_gates(viewer::galactic::InputGates gates);
    bool select_planet(std::uint64_t planet, bool delayed = false);
    void navigate(viewer::galactic::Navigation direction);
    void select_hero(sim::galactic::TypeId type, bool inactive, bool twice);
    bool widget_drop(const viewer::galactic::DropIntent& intent);
    bool choose_battle(std::uint64_t conflict, bool tactical);
    // #2263 supplies rendered widgets, containment and drop-target resolution.
    bool begin_widget_drag(const viewer::galactic::InputWidget& widget, bool control,
        bool focused_fleet, const std::function<void()>& merge_home);
    bool release_widget_drag(const std::function<std::optional<viewer::galactic::DropIntent>(std::uint64_t)>& resolve);
    bool begin_ground_box(viewer::galactic::PointerPoint point, bool allied, bool valid_index);
    void move_ground_box(viewer::galactic::PointerPoint point);
    bool release_ground_box(std::span<const viewer::galactic::InputWidget> widgets);
    bool radar_click(viewer::galactic::PointerPoint point, float width, float height);
    // End held world input when focus leaves the window or a GUI takes it.
    void cancel_input();
    void focus(bool focused);
    // Pointer capture ended; keyboard pan can continue while the window is focused.
    void pointer_left();
    // The destination the campaign score published (exit button or Escape); the mode keeps
    // running and only the host decides what it means. Empty until the player chose.
    [[nodiscard]] std::optional<platform::CampaignScoreDestination> score_destination() const;
    // The in-game menu's Quit Game was confirmed; like the score destination, the host decides what
    // leaving means (the main menu when there is one).
    [[nodiscard]] bool leave_requested() const;
    // The window closed: an interactive run writes its report.
    void close();
    // Why ready() or a frame failed; empty while running (e.g. a refused restore_save).
    [[nodiscard]] std::string failure() const;

    struct State;

private:
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
