#pragma once

// The skirmish battle loading screen (docs/behaviour/loading-screen.md, BLD-01 to BLD-14): the
// game's own loading dialog built from the dialog resources, filled from the game data, with a bar
// that follows the battle's real preparation and a Begin button that appears when it is done. It
// covers the window (a canvas layer above everything) while the battle prepares and waits for
// Begin; the simulation does not tick until then (LiveSessionView::begin).
//
// The preparation runs on the main thread, so the bar is painted through the rendering server
// directly and the screen is drawn on the spot (RenderingServer::force_draw) between the stages'
// work items: a queued Control redraw would only run after the preparation ends. The first draw of
// the Controls needs one normal frame, so the screen is built a frame or two before the work starts.

#include "battle_content.hpp"
#include "eawr/presentation/ui/loading_screen.hpp"
#include "eawr/platform/campaign_loading_view.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class LoadingPlanet;

class EawrLoadingScreen final : public godot::CanvasLayer {
    GDCLASS(EawrLoadingScreen, godot::CanvasLayer)

public:
    struct Options final {
        // The installed content the setup screen mounted: the files and the object catalogue.
        std::shared_ptr<const BattleContent> content;
        // The selected skirmish map: its logical path, its root-field planet and its display name.
        std::string map_path;
        std::optional<std::string> context_name;
        std::string map_name;
        // WGP-31: real campaign location and actual owner, supplied by the
        // conflict publication rather than observer intelligence.
        bool campaign_space{};
        presentation::ui::LoadingBattle battle{presentation::ui::LoadingBattle::space};
        presentation::ui::LoadingMission mission{presentation::ui::LoadingMission::ordinary};
        std::optional<std::string> mission_map;
        std::function<bool()> topmost;
        std::string owner_name;
        std::array<float, 3> owner_colour{1, 1, 1};
        std::optional<double> basic_income;
        std::shared_ptr<const platform::CampaignLoadingView> campaign_preparation;
        // WGP-51: current viewer model and its instance transform, when resident.
        std::optional<std::string> planet_model;
        std::optional<std::array<float, 16>> planet_transform;
        // Empty: --eawr-font-cache, else EAWR_FONT_CACHE, else the checkout's out/fonts.
        std::filesystem::path font_cache;
    };

    EawrLoadingScreen();
    ~EawrLoadingScreen() override;

    // Builds the screen under `host`. False with failure() set when the dialog resources or the
    // kit could not be read; the caller then starts the battle without the screen.
    [[nodiscard]] bool build(godot::Node& host, Options options);
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }

    // BLD-10: the bar follows `progress`; the screen is drawn at once, at most every
    // `present_interval` (the stage boundaries and the last report always draw).
    void report(const presentation::ui::LoadProgress& progress);
    void observe_preparation(std::shared_ptr<const platform::CampaignLoadingView>);
    // WGP-167: native OS acknowledgement; failure never arms Begin.
    void show_failure(const std::string& message);
    // BLD-11: the load finished. The bar is full, Begin shows and the window's 3D view draws again
    // behind the screen, so its pipelines are ready when the player begins.
    void show_ready();
    // BLD-12: Begin was pressed (the button), once; request_begin() is the same press from a key.
    [[nodiscard]] bool take_begin() noexcept;
    void request_begin() noexcept;
    // BLD-13: the screen leaves. Without a Begin (a driven run that began itself) it only hides.
    void dismiss();
    // Presentation preparation may finish over a few rendered loading frames.
    // A deferred dismissal is retried only while this screen remains shown.
    void hold_dismiss(std::function<bool()> ready);
    void viewport_changed(float width, float height);

    [[nodiscard]] bool shown() const noexcept { return shown_; }
    // The frames the screen has been in the tree for: it draws on the spot only from the second
    // (its Controls need one normal frame first), so a host waits for two before the load starts.
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] bool ready_for_begin() const noexcept { return ready_ && shown_; }
    [[nodiscard]] std::optional<godot::Rect2> begin_rect() const;
    [[nodiscard]] std::string report_json() const;

    void _notification(int what);
    void _input(const godot::Ref<godot::InputEvent>& event) override;

protected:
    static void _bind_methods() {}

private:
    struct Resources;
    struct Layout;
    void on_begin_pressed() { request_begin(); }
    [[nodiscard]] bool topmost() const noexcept;
    bool build_layout(godot::Vector2 size);
    void paint_fill(double ratio);
    void present(bool force);
    void capture(const char* label);
    void set_3d(bool enabled);
    void discard_queued_input();

    std::unique_ptr<Resources> resources_;
    std::unique_ptr<Layout> layout_;
    std::unique_ptr<LoadingPlanet> planet_;
    std::chrono::steady_clock::time_point planet_started_{};
    std::string failure_;
    std::vector<std::string> notes_;
    godot::RID fill_item_;
    double ratio_{};
    std::size_t reports_{};
    std::size_t presents_{};
    std::size_t stage_{};
    std::uint64_t frames_{};
    std::uint64_t ready_frame_{};
    bool capture_ready_pending_{};
    std::chrono::steady_clock::time_point last_present_{};
    // The wall time each stage took on this run (the report's "stage_ms"): the evidence for the
    // stages' weights (docs/behaviour/loading-screen.md), never an input to the bar.
    std::array<double, presentation::ui::load_stage_count> stage_ms_{};
    std::chrono::steady_clock::time_point last_report_{};
    std::filesystem::path capture_directory_;
    std::vector<std::string> captures_;
    bool captured_half_{};
    bool shown_{};
    bool dismiss_pending_{};
    std::function<bool()> dismiss_ready_;
    bool ready_{};
    bool failed_{}, failure_acknowledged_{}, native_progress_{};
    std::shared_ptr<const platform::CampaignLoadingView> preparation_;
    bool begin_pressed_{};
    bool disabled_3d_{};
    presentation::ui::CampaignLoadingGate campaign_gate_;
    std::uint64_t unsafe_inputs_{}, accepted_begins_{}, drained_inputs_{};
    bool draining_{};
};

} // namespace eawr::presentation::godot_backend
