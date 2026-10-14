#pragma once

#include "eawr/skirmish/start.hpp"
#include "live_session_view.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/input_event.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

struct BattleContent;
class EawrLoadingScreen;
class GodotShaderCache;
class LanMatch;
class TacticalHud;

// A self-contained viewer mode that renders one TED map's terrain and skydome,
// and with `--eawr-populate` also the map's static placements (P1-11). It owns
// its own renderer, VFS mount and frame loop, so the shared viewer host only
// has to parse `--eawr-map` and hand over; the mode reads `--eawr-populate`
// from the user arguments itself.
//
// A kind-2 (space) map has no terrain and is composed by SpaceEnvironment
// (E-space-primary-sky-v1): environment 0's primary sky only, under the
// explicit `--eawr-space-camera` fixed camera, with the optional
// `--eawr-space-control` negative/lifecycle controls. Land composition is
// unchanged and never reached for a space map. `--eawr-map-camera-config` on
// a space map selects the opt-in space tactical camera (P1 #30): the config is
// parsed and its Space_Mode constants loaded here, and input, focus, pointer
// exit and resize are forwarded to SpaceEnvironment.
class MapMode final {
public:
    [[nodiscard]] std::string_view failure() const noexcept;
    [[nodiscard]] bool briefing_queue_empty() const;
    [[nodiscard]] bool campaign_briefing(std::string_view event_name);
    // The live battle's command bar, for a campaign parent's controls (HCB-15); null without one.
    [[nodiscard]] TacticalHud* tactical_hud() const noexcept;
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        // Optional --eawr-profile override; empty selects the installed layers.
        std::string profile;
        // Logical VFS path of the map, for example
        // "data/art/maps/_land_planet_alderaan_02.ted".
        std::string map_path;
        // UI-11: a campaign battle names its conflict planet independently of the map.
        // The HUD resolves this object identity through its effective Text_ID.
        std::optional<std::string> campaign_planet{};
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        // Frames rendered before the timed window is closed and the capture is
        // taken. A fixed count keeps the recorded frame time comparable.
        std::uint32_t warmup_frames{30};
        std::uint32_t timed_frames{120};
        // Compose static placements from the scene builder. Defaults to the
        // `--eawr-populate` user argument so the shared host needs no change.
        std::optional<bool> populate;
        // Presentation-only land particle composition. The command-line
        // --eawr-map-effects off is an explicit negative control.
        bool map_effects{true};
        std::uint32_t particle_seed{20260922U};
        std::uint32_t particle_frames{60U};
        // Maximum live CPU particles across all map effect placements.
        std::uint32_t particle_capacity{8192U};
        // Effective static bind-pose selectors for tagged attached proxies.
        // An absent selector keeps that proxy unsupported in the CPU plan.
        std::optional<std::uint32_t> effect_alt;
        std::optional<std::uint32_t> effect_lod;
        std::uint32_t attached_capacity{256U};
        // Explicit CLI budgets remain hard limits. Space defaults otherwise
        // accommodate the authored population of prewarmed ambient effects.
        bool particle_capacity_explicit{};
        bool attached_capacity_explicit{};
        // --eawr-map-effect-animation <none|idle> (P1 #29). `none` places
        // attachments at the bind pose, as before. `idle` binds each land placement's
        // corpus-named idle clip and lets its sampled proxy-bone visibility
        // drive the admitted attachments (respawn, one drain). Populated meshes
        // play the same corpus-named clip on the same clock either way (#32).
        enum class EffectAnimation : std::uint8_t { none, idle };
        EffectAnimation effect_animation{EffectAnimation::none};
        // P1-04 lighting. Empty means "take the user argument": --eawr-lighting
        // <sh|hemisphere|off> (default off), --eawr-shadows <on|off> (default
        // off) and --eawr-environment <default|map> (default: alo-viewer's
        // default environment; map: the TED record's unconfirmed candidate).
        std::string lighting;
        std::string shadows;
        std::string environment;
        // --eawr-camera-interactive (P1 #30 owner check): the map camera from
        // --eawr-map-camera-config stays live until the window is closed. No
        // capture is taken; the timed window is simply never reached.
        bool interactive{};
        // --eawr-camera-zoom with --eawr-map-camera-config: replaces the
        // config's initial (and reset) zoom. NaN marks a malformed argument.
        std::optional<float> camera_zoom;
        // R-SETUP-04: the UI enters the same live skirmish preparation as CLI options.
        std::optional<skirmish::FixtureOptions> setup;
        // Reuse the setup screen's installed-content snapshot through return.
        std::shared_ptr<const BattleContent> content;
        std::shared_ptr<GodotShaderCache> shaders;
        // #1923: the battle loading screen the host already shows (it owns the node). The live
        // battle then waits behind it for Begin (BLD-12); without one, a live battle that waits for
        // Begin builds its own.
        EawrLoadingScreen* loading{};
        bool loading_begin_barrier{true};
        // ADR-019: compose the ordinary tactical presentation over the retained child.
        std::shared_ptr<const LiveSessionView::PreparedBattle> borrowed_battle;
        std::shared_ptr<platform::TacticalFeed> borrowed_feed;
        sim::tactical::PlayerId borrowed_player{1};
        std::shared_ptr<LanMatch> lan;
        // MNU-41: a recording the Load Game screen chose; the live session replays it as
        // `--eawr-live-session replay --eawr-live-replay <path>` does.
        std::filesystem::path replay_input;
        // MAT-08 to MAT-12: the main-menu demo attract battle on `map_path`: the map's own forces
        // run by the AI player types `ai` names by editor player index (MAT-09), seen through the
        // map's saved camera. No HUD, no loading screen, no world input, no battle sound.
        struct Attract final {
            std::vector<std::pair<std::uint32_t, std::string>> ai;
        };
        std::optional<Attract> attract;
    };

    explicit MapMode(Options options);
    ~MapMode();
    MapMode(MapMode&&) noexcept;
    MapMode& operator=(MapMode&&) noexcept;
    MapMode(const MapMode&) = delete;
    MapMode& operator=(const MapMode&) = delete;

    // Mounts the profile, loads the map, builds terrain, resolves the skydome
    // and uploads everything. A false return has already written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);

    // One frame of the run. Returns the process exit code once the run has
    // finished and the report has been written.
    [[nodiscard]] std::optional<int> process(double delta);
    // The window's close request: a live space view writes its report before the engine quits.
    void close_requested();
    void input(const godot::Ref<godot::InputEvent>& event);
    void observe_pointer(const godot::Ref<godot::InputEvent>& event);
    void focus(bool focused);
    void pointer_left();
    void viewport_changed(float width, float height);

    // The demo attract (Options::attract): what the runner and its test read from the battle.
    struct AttractStatus final {
        bool live{};                   // a session is up
        bool paused{};                 // the session's pause (MAT-05)
        bool hud{};                    // a HUD exists (MAT-19: never)
        bool battle_input{};           // a selection and order input exists (WGUI-76: never)
        std::uint64_t latest_tick{};   // the newest completed tick
        std::size_t units{};           // units the session drew at the first sample
        std::size_t moved{};           // of them, those that left their first-sample position
        std::size_t damaged{};         // of them, those with less hull than at the first sample
        std::size_t lost{};            // of them, those no longer in the battle (destroyed)
        std::size_t alive{};           // units in the newest sampled snapshot
        std::size_t joined{};          // of those, units the first sample did not hold (MAT-15 refills)
        // The attract camera (placeholder for the battle camera, #3005): its eye now, its eye at the
        // first update and the updates so far; render basis.
        std::array<float, 3> camera_eye{};
        std::array<float, 3> camera_first_eye{};
        std::size_t camera_updates{};
        // MAT-18: the battle's sound gate. Refused = speech, announcement and GUI events and queued
        // speech kept off the voice path; queued and started must stay 0 (nothing reached it).
        std::uint64_t speech_refused{};
        std::uint64_t speech_queued{};
        std::uint64_t speech_started{};
        // MAT-18: 3D sound events refused before the queue (retail's multiplier 0); started stays 0.
        std::uint64_t sfx_3d_refused{};
        std::uint64_t sfx_3d_started{};
    };
    [[nodiscard]] AttractStatus attract_status();
    // MAT-05: holds or releases the battle (the start timeout). A no-op without a session.
    void set_attract_paused(bool paused);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
