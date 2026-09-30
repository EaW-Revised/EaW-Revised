#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/input_event.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

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
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        // Optional --eawr-profile override; empty selects the installed layers.
        std::string profile;
        // Logical VFS path of the map, for example
        // "data/art/maps/_land_planet_alderaan_02.ted".
        std::string map_path;
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
    void focus(bool focused);
    void pointer_left();
    void viewport_changed(float width, float height);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
