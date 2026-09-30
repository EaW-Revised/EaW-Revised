#pragma once

// Private to the map mode translation units (map_mode*.cpp): the mode's
// State, the particle provider it owns, and the helpers the units share.
#include "map_mode.hpp"
#include "capture_viewport.hpp"
#include "fog_mode.hpp"
#include "idle_clips.hpp"
#include "land_look.hpp"
#include "battle_input.hpp"
#include "live_session_view.hpp"
#include "shutdown_trace.hpp"
#include "live_fog_view.hpp"
#include "overview_fade_view.hpp"
#include "battle_audio.hpp"
#include "perf_trace.hpp"
#include "unit_emitters.hpp"
#include "space_environment.hpp"
#include "space_populate.hpp"
#include "space_fog.hpp"
#include "particle_adapter.hpp"
#include "map_camera.hpp"
#include "ui/perf_overlay_view.hpp"
#include "ui/tactical_hud.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/lighting/wind.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/presentation/terrain/terrain.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_with_modifiers.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <limits>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace map_mode_detail {

[[nodiscard]] std::string json(std::string_view value);
[[nodiscard]] bool ieq(std::string_view left, std::string_view right);
[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, std::string_view root,
    std::string_view name, std::span<const std::string_view> suffixes);
[[nodiscard]] assets::Texture placeholder_texture();
[[nodiscard]] std::string effect_name(terrain::SurfaceEffect effect);
[[nodiscard]] Ref<Image> decode_png(const std::vector<std::byte>& bytes);

// Checks the land map camera self-test records; a run with any other count fails.
constexpr std::size_t map_camera_selftest_check_count = 13;
constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
constexpr std::array<std::string_view, 1> model_suffixes{".alo"};

struct SlotRecord final {
    std::uint8_t slot{};
    std::string effect_program;
    std::string effect_technique;
    std::string effect_pass;
    std::string declared_primary;
    std::string resolved_primary;
    std::string declared_secondary;
    std::string resolved_secondary;
    std::uint64_t cells{};
    bool texture_resolved{};
};

struct ParticlePlacement final {
    struct Emitter final {
        std::size_t index{};
        std::string blend;
        std::uint64_t resource{};
        std::uint64_t fog_handle{};
        std::size_t quads{};
        std::uint8_t minimum_attenuation{255};
        std::uint8_t maximum_attenuation{};
        // Peak vertex alpha of the emitter's stream at the last advance.
        float maximum_alpha{};
        bool fog_bound{};
        std::string fog_source_sha256;
        std::uint32_t fog_team{};
        std::uint64_t fog_tick{};
        std::uint64_t fog_revision{};
    };
    std::string identity;
    std::string object_id;
    std::string logical_path;
    std::string sha256;
    bool confirmed_particle_model{};
    bool attached{};
    // Driven by an idle-clip owner (--eawr-map-effect-animation idle): its
    // generations are spawned and released by the owner, not by `handle`.
    bool owned{};
    std::size_t owner_index{};
    // Instances the owner held after its last sample (not reset by release).
    std::size_t live_instances{};
    std::size_t effect_plan_index{};
    std::uint32_t record_ordinal{};
    std::uint32_t seed{};
    std::uint32_t capacity{};
    std::int64_t scale_raw{};
    std::array<std::int64_t, 12> transform_raw{};
    particles::EmitterFrame frame;
    particles::EffectHandle handle{};
    particles::EffectFrameStats stats;
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
    bool has_bounds{};
    std::uint64_t changed_pixels{};
    std::vector<Emitter> emitters;
    std::string status{"unresolved"};
    std::vector<std::string> causes;
};

// Idle-clip owners handed from the plan builder (P1 #29,
// --eawr-map-effect-animation idle). One Player per placement is bound once;
// owned and watched entries name an attached plan record, its placement's
// clip, its proxy bone and the placement's Q24 transform.
struct MapOwnerInput final {
    struct Clip final {
        std::uint64_t scene_ordinal{};
        std::shared_ptr<const animation::Player> player;
    };
    struct Owned final {
        std::size_t plan_index{};
        std::size_t clip{};
        std::size_t bone{};
        sim::math::Mat3x4 placement{};
    };
    struct Watched final {
        std::size_t plan_index{};
        std::size_t clip{};
        std::size_t bone{};
    };
    std::vector<Clip> clips;
    std::vector<Owned> owned;
    std::vector<Watched> watched;
    // Admitted attached allocation plus the owners' drain headroom.
    std::size_t live_capacity_limit{};
};

// Dry-run backend for an idle-clip owner's system at prepare time, before any
// generation exists: it accepts exactly the plans the Godot backend accepts on
// drawability, material validation and texture resolution, and creates no
// RenderingServer resource. Shader compilation and texture upload are only
// exercised by the first real spawn.
class OwnerProbeBackend final : public particles::RenderBackend {
public:
    explicit OwnerProbeBackend(GodotParticleBackend::TextureResolver resolver) : resolver_(std::move(resolver)) {}
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override {
        if (!plan.drawable) { cause_ = "plan is not drawable: " + plan.cause; return 0U; }
        if (const auto valid = validate_material(GodotParticleBackend::material_for(plan)); !valid) {
            cause_ = valid.error().message;
            return 0U;
        }
        if (resolver_(plan.texture) == nullptr) {
            cause_ = "colour texture '" + plan.texture + "' did not resolve";
            return 0U;
        }
        return next_++;
    }
    void update_emitter(std::uint64_t, const particles::VertexStream&) override {}
    void destroy_emitter(std::uint64_t) override {}
    std::string failure_cause() const override { return cause_; }

private:
    GodotParticleBackend::TextureResolver resolver_;
    std::string cause_;
    std::uint64_t next_{1};
};

// All state here belongs to the viewer. Nothing is submitted to the render
// snapshot or serialized into replay input.
class MapParticleProvider final {
public:
    MapParticleProvider(Node3D& host, const vfs::Vfs& filesystem,
        GodotRenderer* renderer = nullptr, FogMode* fog = nullptr)
        : filesystem_(&filesystem), backend_(std::make_unique<GodotParticleBackend>(host,
              [this](const std::string_view name) { return resolve_texture(name); },
              renderer && fog ? GodotParticleBackend::FogCallbacks{
                  .register_material = [renderer](const RID& material, const RID& shader) {
                      return renderer->register_external_fog_material(material, shader);
                  },
                  .unregister_material = [renderer](const std::uint64_t handle) {
                      renderer->unregister_external_fog_material(handle);
                  },
                  .attenuation_at_source_xy = [fog](float x, float y) {
                      const auto* grid = fog->source().find(fog->team());
                      return grid ? presentation::fog::attenuation_at(*grid, {x, y}) : std::uint8_t{0};
                  },
              } : GodotParticleBackend::FogCallbacks{})),
          registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {}

    [[nodiscard]] bool prepare(const assets::Map& map, const scene::Scene& scene,
        std::uint32_t seed, std::uint32_t capacity);
    [[nodiscard]] bool prepare_attached(const particles::MapEffectPlan& plan, const scene::Scene& scene,
        const MapOwnerInput* owners = nullptr);
    // Follows a re-made attached plan (#136): records match by scene and
    // proxy ordinal. An emitter whose record stays admitted with the same
    // effect, seed, capacity and frame keeps running; the others are
    // released and dropped, and newly admitted records start now. Static
    // emitters only (the space path has no idle-clip owners).
    [[nodiscard]] bool sync_attached(const particles::MapEffectPlan& before, const particles::MapEffectPlan& after,
        const scene::Scene& scene);
    [[nodiscard]] bool advance(float delta, const particles::CameraFrame& camera);
    // Bump-mapped emitters are lit by the scene's sun and fill, as the
    // renderer's bump hull adapters are (#238).
    void follow_lighting(const GodotRenderer& renderer) {
        const auto& lighting = renderer.lighting();
        if (!lighting) return;
        backend_->set_lighting({.toward_light = lighting->toward_light, .diffuse = lighting->sun_diffuse,
            .specular = lighting->specular, .fill = lighting->sph_fill});
    }
    void release();
    [[nodiscard]] const std::vector<ParticlePlacement>& placements() const { return placements_; }
    [[nodiscard]] std::vector<ParticlePlacement>& placements_mutable() { return placements_; }
    [[nodiscard]] std::size_t live_rids() const { return backend_->live_rids(); }
    [[nodiscard]] std::size_t live_resources() const { return registry_->live_backend_resources(); }
    [[nodiscard]] std::uint32_t frames() const { return frames_; }
    [[nodiscard]] bool failed() const { return failed_; }
    void capture_fog_evidence(const GodotRenderer& renderer, const FogMode& fog) {
        const auto resources = backend_->fog_emitter_evidence();
        const auto consumers = renderer.external_fog_consumers();
        const auto selected = fog.source().find(fog.team());
        const auto& sources = fog.sources();
        const auto source = std::find_if(sources.begin(), sources.end(),
            [&](const auto& entry) { return entry.team == fog.team(); });
        for (ParticlePlacement& placement : placements_) {
            for (auto& emitter : placement.emitters) {
                emitter.fog_source_sha256 = source == sources.end() ? "" : source->sha256;
                emitter.fog_team = fog.team();
                emitter.fog_tick = fog.tick();
                emitter.fog_revision = selected ? selected->revision() : 0;
                const auto resource = std::find_if(resources.begin(), resources.end(),
                    [&](const auto& item) { return item.resource == emitter.resource; });
                const auto consumer = std::find_if(consumers.begin(), consumers.end(),
                    [&](const auto& item) { return item.handle == emitter.fog_handle; });
                if (resource != resources.end()) {
                    emitter.quads = resource->quads;
                    emitter.minimum_attenuation = resource->minimum_attenuation;
                    emitter.maximum_attenuation = resource->maximum_attenuation;
                }
                emitter.fog_bound = consumer != consumers.end() && consumer->bound;
            }
        }
    }
    [[nodiscard]] std::size_t active_count() const {
        return static_cast<std::size_t>(std::count_if(placements_.begin(), placements_.end(),
            [](const ParticlePlacement& placement) { return placement.handle != 0; }));
    }
    // The clock keeps running while any static instance is live or any owner
    // is unreleased, so an owner hidden at its first sample still advances.
    [[nodiscard]] bool has_work() const {
        return active_count() != 0 || std::any_of(owners_.begin(), owners_.end(),
            [](const Owned& owned) { return !owned.owner.released(); });
    }
    [[nodiscard]] bool has_owners() const { return !owners_.empty(); }
    // Anything drawn now, including an owner's drain with no active generation.
    [[nodiscard]] bool has_live() const {
        return active_count() != 0 || std::any_of(owners_.begin(), owners_.end(),
            [](const Owned& owned) { return owned.owner.live_instances() != 0; });
    }
    [[nodiscard]] const particles::MapAttachmentOwner* owner_for(const std::size_t plan_index) const {
        for (const Owned& owned : owners_) {
            if (placements_[owned.placement].effect_plan_index == plan_index) return &owned.owner;
        }
        return nullptr;
    }
    [[nodiscard]] std::optional<std::uint32_t> watch_edges(const std::size_t plan_index) const {
        for (const Watched& watched : watches_) {
            if (watched.plan_index == plan_index) return watched.watch.visible_edges();
        }
        return std::nullopt;
    }

private:
    struct Clip final {
        std::shared_ptr<const animation::Player> player;
        std::optional<animation::Pose> pose;
    };
    struct Owned final {
        std::size_t placement{};
        std::size_t clip{};
        particles::MapAttachmentOwner owner;
    };
    struct Watched final {
        std::size_t plan_index{};
        std::size_t clip{};
        particles::MapHiddenProxyWatch watch;
    };
    [[nodiscard]] particles::AttachmentLifecycle::Spawn owned_spawner(std::size_t placement,
        particles::SystemDefinition system, std::size_t capacity,
        std::optional<particles::MeshBinding> mesh_binding);
    [[nodiscard]] const assets::Texture* resolve_texture(std::string_view name);
    // Starts one admitted plan record (a placement is appended either way).
    void start_attached(const particles::MapEffectPlan& plan, std::size_t index, const scene::Scene& scene,
        const MapOwnerInput* owners);
    const vfs::Vfs* filesystem_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    std::vector<ParticlePlacement> placements_;
    // Owners hold the registry but are not RAII: release() releases them
    // before any static handle, and before the registry and backend go away.
    std::vector<Clip> clips_;
    std::vector<Owned> owners_;
    std::vector<Watched> watches_;
    std::size_t live_capacity_limit_{};
    std::uint32_t frames_{};
    bool failed_{};
};

} // namespace map_mode_detail

using namespace map_mode_detail;

// The UI-05 font cache mounted from `directory` (empty when it is missing).
[[nodiscard]] presentation::ui::FontCache load_hud_font_cache(const std::filesystem::path& directory);

// `--eawr-perf-overlay on|off` (map_mode_hud.cpp, #558): an error message, or empty with `requested`
// set when the overlay starts shown. The F3 key shows it in any run either way.
[[nodiscard]] std::string parse_perf_overlay_argument(const godot::PackedStringArray& arguments, bool& requested);

// `--eawr-hud tactical|off` and its options (map_mode_hud.cpp); an error message,
// or empty with `hud` set when the HUD is drawn: when asked for, and by default in
// a live session. `faction_given` says whether --eawr-hud-faction chose the variant.
[[nodiscard]] std::string parse_hud_arguments(const godot::PackedStringArray& arguments, bool live_session,
                                              std::optional<TacticalHud::Options>& hud, bool& faction_given);

struct MapMode::State final {
    explicit State(Options value) : options(std::move(value)) {}
    ~State() {
        shutdown_trace::mark("map mode teardown begins");
        space.reset();
        shutdown_trace::mark("space view freed");
        release_particles();
        particles.reset();
        shutdown_trace::mark("particles freed");
        renderer.reset();
        shutdown_trace::mark("renderer freed");
    }

    Options options;
    std::unique_ptr<GodotRenderer> renderer;
    // E-space-primary-sky-v1: a kind-2 map is composed by SpaceEnvironment;
    // the land members below stay unused for it. The populated placements
    // (P1-11 #32) outlive it, since it releases them on its renderer.
    // --eawr-live-session (#80): declared before the population and the space
    // view, whose hooks point at it, so it outlives them.
    LiveSessionView::Options live_options;
    std::unique_ptr<LiveSessionView> live_session;
    std::unique_ptr<SpacePopulation> space_population;
    bool space_populate_reject_upload_test{};
    std::optional<SpacePopulation::Options::DebugShip> space_place_object;
    // --eawr-space-place-at, repeatable (#70 movement evidence).
    std::vector<SpacePopulation::Options::PlacedShip> space_place_at;
    // --eawr-space-hardpoint-state, for the debug ship only (#136).
    std::vector<std::pair<std::string, scene::HardpointState>> space_hardpoint_states;
    // --eawr-map-idle-offset <ticks> (#145 space, #157 land): 30 Hz ticks
    // added to the populated idle clock, so a fixed capture shows a later
    // moment.
    std::optional<std::uint32_t> idle_offset;
    std::unique_ptr<SpaceEnvironment> space;
    // #82: the local player's selection and orders on the live session; declared after the
    // session, the population and the space view, so it goes first.
    std::unique_ptr<BattleInput> battle;
    // #80: the live battle's shots, hits and explosions; released with the population, declared
    // after the session it reads.
    std::unique_ptr<BattleEffects> battle_effects;
    // #84: the live battle's sound; `--eawr-audio on|off` (default on when interactive).
    std::unique_ptr<BattleAudio> battle_audio;
    std::string audio_argument;
    std::unique_ptr<UnitEmitters> unit_emitters;
    std::optional<PerfTrace> perf_trace;  // --eawr-perf-trace (#601)
    std::unique_ptr<DebrisProps> debris_props;  // #391
    std::unique_ptr<LiveFogView> live_fog;      // #494
    std::string space_camera;
    std::string space_control;
    // --eawr-space-fog-admit (repeatable): the caller-declared XML element
    // types a space fog run composes as synthetic units (P1-07 #28).
    std::vector<std::string> space_fog_admit;
    std::filesystem::path map_camera_config_path;
    std::filesystem::path map_camera_unlocked_capture_path;
    std::unique_ptr<eawr::viewer::MapCameraBridge> map_camera;
    std::string map_camera_config_sha256;
    std::string map_camera_bindings_sha256;
    std::string map_camera_tactical_xml_sha256;
    std::string map_camera_gameconstants_xml_sha256;
    std::vector<camera::FieldProvenance> map_camera_constant_sources;
    std::vector<camera::AppliedOverride> map_camera_constant_overrides;
    std::uint64_t map_camera_focus_notifications{};
    std::uint64_t map_camera_resize_notifications{};
    bool map_camera_selftest{};
    bool map_free_selftest{};
    bool map_camera_terminal_baseline_test{};
    bool map_camera_terminal_hold_test{};
    // Space map only: the terminal pan is released right after its step.
    bool map_camera_terminal_release_test{};
    bool map_free_terminal_hold_test{};
    bool map_free_terminal_release_test{};
    bool map_free_terminal_forward_held_at_step{};
    bool map_camera_settle_started{};
    std::uint32_t map_camera_settle_frames{};
    std::uint64_t map_camera_steps_at_freeze{};
    Node3D* map_camera_host{};
    Vector2i map_camera_window_size{};
    std::vector<std::byte> map_camera_terminal_before;
    std::size_t map_camera_terminal_changed_pixels{};
    bool map_camera_resize_active_at_capture{};
    std::array<float, 3> map_camera_mark{};
    bool map_camera_focus_pan_moved{};
    bool map_camera_resize_pan_moved{};
    bool map_camera_edge_pan_moved{};
    float map_camera_zoom_mark{};
    float map_camera_yaw_mark{};
    float map_camera_pitch_mark{};
    float map_camera_orbit_radius_mark{};
    std::uint64_t map_camera_generation_mark{};
    std::vector<std::pair<std::string, bool>> map_camera_checks;
    FixedCamera map_camera_initial_frame;
    std::array<float, 3> map_free_mark{};
    std::uint64_t map_free_generation_mark{};
    std::vector<std::pair<std::string, bool>> map_free_checks;
    std::optional<vfs::Vfs> filesystem;
    std::optional<FogMode> fog;
    std::uint64_t fog_renderer_stream{};
    std::vector<std::filesystem::path> fog_paths;
    std::vector<std::string> fog_expected_hashes;
    std::optional<std::uint32_t> fog_team;
    std::optional<std::uint64_t> fog_revision;
    std::optional<std::uint64_t> fog_tick;
    bool fog_paint_requested{};
    bool fog_inject_input{};
    std::uint32_t fog_ignored_inputs{};
    std::filesystem::path fog_paint_evidence;
    std::map<std::string, std::string> fog_paint_hashes;
    std::map<std::string, std::uint64_t> fog_paint_uploads;
    std::map<std::string, std::uint64_t> fog_paint_updates;
    std::vector<std::string> fog_unsupported;
    std::vector<std::string> fog_uncomposed;
    void refresh_fog_snapshots() {
        if (!fog || !snapshot) return;
        std::vector<sim::RenderInstance> all(snapshot->instances().begin(), snapshot->instances().end());
        snapshot = fog->snapshot(std::move(all));
        if (terrain_snapshot) {
            std::vector<sim::RenderInstance> terrain(
                terrain_snapshot->instances().begin(), terrain_snapshot->instances().end());
            terrain_snapshot = fog->snapshot(std::move(terrain));
        }
        if (fog_renderer_stream != fog->stream()) {
            renderer->reset_fog_stream(fog->stream());
            fog_renderer_stream = fog->stream();
        }
    }

    // P2-20a (#83): the tactical HUD shell over the map, when asked for.
    std::optional<TacticalHud::Options> hud_options;
    bool hud_faction_given{};
    std::unique_ptr<TacticalHud> hud;
    // Builds the HUD when asked for; false with `failure` set when the shell
    // cannot be read.
    [[nodiscard]] bool build_hud(godot::Node3D& host, const std::optional<std::string>& context_name);
    // #558: the performance overlay (docs/ui/perf-overlay.md), built the first time it is shown.
    // `--eawr-perf-overlay on` shows it from the first frame; the F3 key toggles it in any run.
    bool perf_requested{};
    godot::Node3D* perf_host{};
    EawrPerfOverlay* perf{};
    std::shared_ptr<FontProvider> perf_fonts;
    std::uint64_t perf_last_tick{};
    void set_perf_overlay(bool shown);
    // Once per presented frame, before the view draws: hands the overlay the new tick costs.
    void sync_perf_overlay();
    // The report's "perf_overlay" line (always there: a run without the overlay reports it hidden).
    [[nodiscard]] std::string perf_report_json() const;
    // #453, #459: hands the live battle's time panel and outcome to the HUD. Called right after
    // each live frame, so a capture of that frame shows them.
    void sync_battle_hud();
    // #455: hands the live battle to the HUD's minimap (what the local player sees, the fog
    // revealers of the local team, the camera's outline). Called right after each live frame.
    void sync_minimap();
    std::uint64_t minimap_syncs{};
    // #848 (docs/behaviour/foc-battle-selection.md V-5a to V-5g): the battle UI in the overview
    // levels x1 and x2. Once per live frame, after the camera took this frame's level and before
    // the battle input draws: hides what the level hides and starts the fade on a level change.
    void sync_overview_ui();
    std::string overview_level;
    presentation::ui::OverviewUi overview_ui;
    std::unique_ptr<OverviewFadeView> overview_fade;
    // The report's "overview_ui" object.
    [[nodiscard]] std::string overview_report_json() const;
    // MM-09: the reference plane's height, the mean Z of the lobby players' units at the first frame.
    std::optional<double> minimap_height;
    std::map<sim::tactical::TypeId, std::string> minimap_type_names;

    std::string profile;
    std::vector<std::string> layers;
    std::string map_hash;
    std::string map_kind;
    bool semantic_complete{};

    std::uint32_t chunk_count{};
    std::uint64_t vertex_count{};
    std::uint64_t triangle_count{};
    std::uint32_t uploaded_surfaces{};
    std::vector<SlotRecord> slots;

    std::string skydome_object;
    std::string skydome_model_path;
    std::string skydome_model_hash;
    std::string skydome_status{"absent"};
    bool skydome_drawn{};

    std::string water_status{"unsupported"};
    std::string water_cause;
    std::uint64_t water_records{};
    std::string nebula_status{"not_applicable"};
    std::uint64_t environment_records{};

    // P1-06 #27 land look: blend layers, sky surfaces, approximate water and
    // the default framing (land_look.hpp).
    land_look::TerrainBlendReport terrain_blend;
    land_look::WaterComposition water;
    std::string requested_view;
    std::optional<float> requested_zoom;
    std::optional<float> requested_yaw;
    std::optional<std::array<float, 2>> requested_target;
    float water_capture_time{};
    std::string camera_view{"overview"};
    std::string camera_view_cause;
    std::optional<land_look::TacticalDefault> tactical;
    bool benchmark{};

    FixedCamera camera;
    // Normalized half-extents of the terrain's projected footprint under the
    // top-down capture camera, which is what makes the coverage check
    // attributable to terrain rather than to "something drew".
    float footprint_half_width{};
    float footprint_half_height{};
    float unlocked_changed_pixel_coverage{};

    // P1-11 static placements. The scene is the builder's output; everything
    // below it is presentation composition of that output.
    bool populate{};
    std::unique_ptr<MapParticleProvider> particles;
    particles::MapEffectPlan attached_plan;
    // P1 #29 --eawr-map-effect-animation idle: the clip bound for each
    // placement with an admitted or bind-hidden-bone attachment, the owners
    // handed to the provider, and the reserved drain headroom.
    struct AttachedClip final {
        const scene::Placement* placement{};
        std::string status;
        std::string detail;
    };
    std::map<std::uint64_t, AttachedClip> attached_clips;
    MapOwnerInput attached_owners;
    std::size_t attached_budget{};
    std::size_t attached_drain_headroom{};
    std::string attached_animation_failure;
    [[nodiscard]] bool effect_animation_idle() const {
        return options.effect_animation == Options::EffectAnimation::idle;
    }
    // `hardpoints`: the space path, where each placed object's XML
    // HardPoints decide its damage emitters (#136). A hardpoint takes its
    // state from the population; one it has not composed is intact, so its
    // emitters are hidden. A marker placement attaches nothing (#284).
    void build_attached_plan(scene::VfsAssetCache& cache, const SpacePopulation* hardpoints = nullptr) {
        struct Candidate final {
            const assets::Model* model{};
            const scene::Placement* placement{};
            std::vector<std::optional<sim::math::Mat3x4>> frames;
            std::vector<particles::EffectEvidence> effects;
            particles::VisibilityEvidence visibility{particles::VisibilityEvidence::unknown};
            std::vector<std::uint8_t> hardpoint_hidden;
        };
        const scene::ObjectResolver resolve = [this](const std::string_view id) -> std::optional<data::EffectiveObject> {
            if (!catalog) return std::nullopt;
            auto resolved = catalog->resolve(id);
            if (!resolved) return std::nullopt;
            return std::move(resolved.value());
        };
        std::vector<Candidate> candidates;
        candidates.reserve(scene->placements.size());
        std::map<std::string, particles::EffectKind> kinds;
        std::map<std::string, std::size_t> capacities;
        for (const scene::Placement& placement : scene->placements) {
            if (placement.model_path.empty()) continue;
            if (hardpoints && hardpoints->is_marker(placement.scene_ordinal)) continue;
            const assets::Model* model = cache.model(placement.model_path);
            if (model == nullptr || model->proxies.empty()) continue;
            Candidate candidate;
            candidate.model = model;
            candidate.placement = &placement;
            candidate.frames.resize(model->bones.size());
            candidate.effects.resize(model->proxies.size());
            if (auto player = animation::Player::create(*model)) {
                if (auto pose = player.value().sample({}); pose && pose.value().bones.size() == model->bones.size()) {
                    candidate.visibility = particles::VisibilityEvidence::bind_pose;
                    for (std::size_t bone = 0; bone < model->bones.size(); ++bone) {
                        candidate.frames[bone] = particles::fixed_model_frame(
                            pose.value().bones[bone].model_asset, scene::fixed_from_binary32);
                    }
                }
            }
            for (std::size_t ordinal = 0; ordinal < model->proxies.size() && ordinal < placement.effects.size(); ++ordinal) {
                const std::string& path = placement.effects[ordinal].resolved;
                if (path.empty()) continue;
                auto found = kinds.find(path);
                if (found == kinds.end()) {
                    particles::EffectKind kind = particles::EffectKind::unknown;
                    if (auto bytes = filesystem->open(path)) {
                        if (auto system = particles::load_alo(bytes.value(), path); system && !system.value().emitters.empty()) {
                            kind = particles::EffectKind::particle;
                            if (map_kind == "space" && !options.attached_capacity_explicit)
                                capacities[path] = particles::prewarmed_capacity(system.value(), options.attached_capacity);
                        } else if (cache.model(path)) {
                            kind = particles::EffectKind::non_particle;
                        }
                    }
                    found = kinds.emplace(path, kind).first;
                }
                const auto capacity = capacities.find(path);
                candidate.effects[ordinal] = {found->second,
                    capacity == capacities.end() ? options.attached_capacity : capacity->second};
            }
            if (hardpoints && !placement.object_id.empty()) {
                if (const auto object = resolve(placement.object_id)) {
                    const scene::SpaceObjectTags tags = scene::space_object_tags(*object, resolve);
                    candidate.hardpoint_hidden = scene::hidden_hardpoint_proxies(*model, tags.hardpoints,
                        hardpoints->hardpoint_states(placement.scene_ordinal));
                }
            }
            candidates.push_back(std::move(candidate));
        }
        std::vector<particles::MapEffectPlacementInput> inputs;
        inputs.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            inputs.push_back({candidate.model, candidate.placement, candidate.frames, candidate.effects,
                candidate.visibility, options.effect_alt, options.effect_lod, candidate.hardpoint_hidden});
        }
        const std::size_t ordinary = static_cast<std::size_t>(scene->count(scene::Cause::model_particle_system));
        std::size_t budget = options.particle_capacity > ordinary
            ? options.particle_capacity - ordinary : 0;
        if (map_kind == "space" && !options.particle_capacity_explicit) {
            // Bound the automatic allocation; explicit capture controls still
            // exercise exhaustion. Include every candidate so TED order cannot
            // starve a later ambient field at the normal space-map density.
            constexpr std::size_t automatic_limit = 131072;
            std::size_t requested = 0;
            for (const auto& candidate : candidates)
                for (const auto& effect : candidate.effects)
                    requested = std::min(automatic_limit, requested + effect.requested_capacity);
            budget = std::max(budget, requested);
        }
        attached_plan = particles::plan_map_effects(inputs, options.particle_seed, budget);
        attached_budget = budget;
        if (effect_animation_idle()) bind_attached_clips(candidates);
    }
    // Space: re-plans the attached effects over the composed population
    // scene with its hardpoint states and has the provider follow (#136).
    [[nodiscard]] bool sync_space_attached(Node3D& host, SpacePopulation& population);
    // The report's populate.attached_effects for the current plan.
    [[nodiscard]] SpacePopulation::AttachedEffects space_attached_effects() const;
    // Binds one Player per relevant placement to its corpus-named idle clip.
    // Admitted records become owners; bind-hidden-bone records are only
    // watched. A placement without a usable clip keeps the static path.
    template <class Candidates> void bind_attached_clips(const Candidates& candidates) {
        for (const auto& candidate : candidates) {
            const std::uint64_t ordinal = candidate.placement->scene_ordinal;
            const auto relevant = [ordinal](const particles::MapEffectRecord& record) {
                return record.scene_ordinal == ordinal
                    && (record.status == particles::MapEffectStatus::admitted
                        || record.cause == particles::MapEffectCause::hidden_bone);
            };
            if (std::none_of(attached_plan.records.begin(), attached_plan.records.end(), relevant)) continue;
            AttachedClip clip;
            clip.placement = candidate.placement;
            const std::string& path = candidate.placement->idle_animation;
            if (candidate.placement->idle_animation_status != "corpus_naming_observed" || path.empty()) {
                clip.status = "clip_absent";
                clip.detail = "idle_animation_status is " + candidate.placement->idle_animation_status;
            } else if (auto decoded = assets::load_animation(*filesystem, path); !decoded) {
                clip.status = "clip_decode_failed";
                clip.detail = core::format_diagnostic(decoded.error());
            } else if (auto player = animation::Player::create(*candidate.model, &decoded.value()); !player) {
                clip.status = "clip_binding_failed";
                clip.detail = core::format_diagnostic(player.error());
            } else if (auto exact = particles::map_owner_sample(player.value(), 0); !exact) {
                // The owner clock needs an integral frame rate (exact n/30 s).
                clip.status = "clip_rate_unsupported";
                clip.detail = core::format_diagnostic(exact.error());
            } else {
                clip.status = "bound";
                const std::size_t clip_index = attached_owners.clips.size();
                attached_owners.clips.push_back({ordinal,
                    std::make_shared<const animation::Player>(std::move(player.value()))});
                for (std::size_t index = 0; index < attached_plan.records.size(); ++index) {
                    const particles::MapEffectRecord& record = attached_plan.records[index];
                    if (!relevant(record)) continue;
                    const std::size_t bone = candidate.model->proxies[record.proxy_ordinal].bone;
                    if (record.status == particles::MapEffectStatus::admitted) {
                        attached_owners.owned.push_back({index, clip_index, bone,
                            candidate.placement->transform->matrix});
                    } else {
                        attached_owners.watched.push_back({index, clip_index, bone});
                    }
                }
            }
            attached_clips.emplace(ordinal, std::move(clip));
        }
        // One drain generation of headroom per owner, inside the budget the
        // admission plan was given; otherwise fail closed with no owner.
        std::vector<std::size_t> capacities;
        for (const auto& owned : attached_owners.owned) {
            capacities.push_back(attached_plan.records[owned.plan_index].capacity);
        }
        const auto headroom = particles::map_owner_headroom(capacities, particles::map_owner_max_draining);
        if (!headroom || !particles::map_owner_capacity_fits(
                attached_plan.allocated_capacity, *headroom, attached_budget)) {
            attached_animation_failure = "drain_headroom_exhausted: admitted attached capacity "
                + std::to_string(attached_plan.allocated_capacity) + " plus drain headroom "
                + (headroom ? std::to_string(*headroom) : std::string("(overflow)"))
                + " exceeds the attached budget " + std::to_string(attached_budget);
            attached_owners = {};
            return;
        }
        attached_drain_headroom = *headroom;
        attached_owners.live_capacity_limit = attached_plan.allocated_capacity + *headroom;
    }
    std::uint64_t particle_changed_outside{};
    // Idle-clip owners and static placements accounted empty at capture.
    std::size_t particle_owners_empty{};
    std::size_t particle_placements_empty{};
    std::size_t particle_placements_outside_view{};
    std::size_t particle_placements_subpixel_heat{};
    bool particle_evidence_verified{};
    std::size_t particle_rids_after_release{};
    std::size_t particle_resources_after_release{};
    std::size_t particle_rids_at_capture{};
    std::size_t particle_resources_at_capture{};
    std::size_t particle_fog_consumers_at_capture{};
    std::size_t particle_fog_consumers_after_release{};
    bool particle_fog_bound_at_capture{};
    void release_particles() {
        if (!particles) return;
        particles->release();
        particle_rids_after_release = particles->live_rids();
        particle_resources_after_release = particles->live_resources();
        if (renderer) particle_fog_consumers_after_release = renderer->fog_status().external_consumers;
    }
    std::optional<data::Catalog> catalog;
    std::optional<scene::Scene> scene;
    std::uint64_t renderer_assets{};
    std::uint64_t surfaces_uploaded{};
    std::uint64_t surfaces_unsupported{};
    std::uint64_t surfaces_failed{};
    std::uint64_t unit_instances{};
    std::uint64_t skinned_instances{};
    std::string first_surface_failure;
    // #32 team colour: a colorizing surface is uploaded once per team colour
    // its placements need, with Colorization bound to that colour.
    std::uint64_t team_colour_variants{};
    std::map<std::string, std::uint64_t> team_colour_by_status;
    std::map<std::string, std::array<std::uint8_t, 3>> team_colour_factions;
    // #32 unit idle clips: a drawn placement whose corpus-named idle clip
    // binds plays it on its skinned instances. #157: each placement plays its
    // clip by the retail rule (idle_clips.hpp) from its own start frame. The
    // clock is the particle owners' (sample n at frame n + 1, held after the
    // last particle frame, plus --eawr-map-idle-offset) so captures are
    // deterministic; the live view runs it on real time and never holds.
    struct AnimatedInstance final {
        sim::EntityId entity{};
        sim::AssetId asset{};
        std::size_t idle{};  // index into idle_placements
    };
    std::vector<std::shared_ptr<const animation::Player>> unit_clips;
    std::vector<IdlePlacement> idle_placements;
    std::vector<AnimatedInstance> animated_instances;
    std::map<std::string, std::uint64_t> unit_clip_status;
    // Object id -> its bound clip, playback and placement count, for the report.
    struct IdleObject final {
        std::string clip;
        animation::IdlePlayback playback{};
        std::uint64_t placements{};
    };
    std::map<std::string, IdleObject> idle_objects;
    // Object id -> the Idle_Anim_00_Rate_Mod text that did not parse (rate 1 kept).
    std::map<std::string, std::string> idle_rate_rejected;
    std::uint64_t idle_sample_failures{};
    double idle_clock_seconds{};
    std::uint64_t units_animated{};
    std::optional<std::uint32_t> unit_sample;
    void pose_units(std::uint32_t sample);
    // Render-basis bounds of each drawn placement, for the attributable
    // coverage check.
    struct Bounds final {
        std::array<float, 3> minimum{};
        std::array<float, 3> maximum{};
    };
    std::vector<Bounds> unit_bounds;
    std::shared_ptr<const sim::RenderSnapshot> terrain_snapshot;
    std::vector<std::byte> populated_png;
    std::string terrain_only_capture_hash;
    std::uint32_t comparison_frames{};
    std::uint64_t units_projected{};
    std::uint64_t units_with_coverage{};
    std::uint64_t units_expected_hidden{};
    std::uint64_t units_expected_visible{};
    bool fog_unit_submissions_verified{};
    std::uint64_t changed_inside_pixels{};
    std::uint64_t changed_outside_pixels{};
    std::uint64_t outside_pixels{};
    bool unit_evidence_verified{};

    // P1-04 lighting.
    lighting::Policy policy{lighting::Policy::off};
    bool shadows{};
    std::string environment_choice{"default"};
    // #201: --eawr-bloom <on|off> (default on). On, the scene blooms with
    // environment 0's parameters, or the loader defaults for a map without an
    // environment record.
    bool bloom{true};
    // #307: the default bloom was left off because the lighting policy is off.
    bool bloom_skipped_unlit{};
    std::optional<lighting::bloom::SceneBloom> scene_bloom;
    // The renderer took it (a RenderingDevice backend), so the main capture blooms.
    bool scene_bloom_applied{};
    // --eawr-environment-record N (#225): the TED environment record the land
    // view uses, an index into the map's environment list. Retail draws one per
    // battle at random (R-SEL-03); an eye check picks the one a retail capture
    // drew. It selects the skydome and, with --eawr-environment map, the
    // lighting, shadow colour and wind. Default 0; a space map takes only 0.
    std::optional<std::uint32_t> environment_record_argument;
    std::uint32_t environment_record{};
    std::string environment_record_name;
    lighting::Environment environment{lighting::alo_viewer_default_environment()};
    // #147 foliage wind: the environment record's wind with
    // --eawr-environment map (R-WX-01), none otherwise (the scene default).
    // Tree.fx and Grass.fx surfaces bend under it on the scene clock: in a
    // capture the idle clock's held sample (plus --eawr-map-idle-offset) in
    // seconds, in the live view real frame time from the idle offset on.
    std::optional<lighting::wind::EnvironmentWind> wind;
    float wind_clock_seconds{};
    std::uint64_t wind_tree_surfaces{};
    std::uint64_t wind_grass_surfaces{};
    std::uint64_t wind_widened_placements{};
    void advance_wind(double delta);
    GodotRenderer::LightingState configured_lighting;
    GodotRenderer::LightingState other_lighting;
    bool skydome_casts_shadows{true};
    struct Phase final {
        std::string name;
        std::function<void(State&)> apply;
        bool terrain_only{};
        bool started{};
    };
    std::deque<Phase> phases;
    std::map<std::string, std::vector<std::byte>> captures;
    // #201: the name of the configured frame the comparisons attribute
    // against. Bloom spreads light past what drew it, so with bloom the
    // comparison phases render without it, after a "bloom_off" phase that
    // redraws the configured scene without bloom; the main capture keeps it.
    [[nodiscard]] std::string configured_frame() const {
        return captures.contains("bloom_off") ? "bloom_off" : "configured";
    }
    std::map<std::string, std::string> capture_hashes;
    std::uint64_t shadow_regions{};
    std::uint64_t shadow_region_pixels{};
    double shadow_region_luminance_on{};
    double shadow_region_luminance_off{};
    double shadow_control_luminance_on{};
    double shadow_control_luminance_off{};
    std::uint64_t shadow_hull_pixels{};
    std::uint64_t shadow_hull_darkened{};
    std::uint64_t shadow_control_pixels{};
    std::uint64_t shadow_control_darkened{};
    std::string shadow_criterion;
    std::string shadow_evidence_status{"not_requested"};
    struct PolicyLuminance final {
        double mean{};
        double saturated_fraction{};
        std::uint64_t pixels{};
    };
    std::map<std::string, PolicyLuminance> policy_luminance;
    std::string policy_evidence_status{"not_requested"};

    std::shared_ptr<const sim::RenderSnapshot> snapshot;
    std::uint32_t frame{};
    std::chrono::steady_clock::time_point timing_start{};
    double timed_seconds{};
    std::string capture_hash;
    // The final read-back's size, reported beside the camera identity.
    std::optional<std::array<std::uint32_t, 2>> capture_size;
    float inside_coverage{};
    float outside_coverage{};
    bool evidence_verified{};
    bool completed{};
    std::string status{"failed"};
    std::string failure;

    [[nodiscard]] bool write_report() const;
    [[nodiscard]] bool verify_capture(const CaptureResult& capture);
    [[nodiscard]] bool verify_unlocked_capture(const CaptureResult& capture);
    [[nodiscard]] bool compose_placements(const assets::Map& map,
        std::vector<sim::RenderInstance>& instances, sim::AssetId& next_asset,
        sim::EntityId& next_entity);
    [[nodiscard]] bool verify_units(const std::vector<std::byte>& populated,
        const std::vector<std::byte>& terrain_only);
    void write_attached_owner(std::ostream& output, std::size_t index,
        const particles::MapEffectRecord& record) const;
    [[nodiscard]] bool verify_particles(const std::vector<std::byte>& with_effects,
        const std::vector<std::byte>& without_effects);
    [[nodiscard]] bool fog_can_reveal(const Bounds& bounds) const;
    [[nodiscard]] std::array<float, 2> project(const std::array<float, 3>& point) const;
    [[nodiscard]] GodotRenderer::LightingState lighting_state(lighting::Policy which, float max_distance) const;
    [[nodiscard]] std::vector<std::uint8_t> unit_mask(int32_t width, int32_t height) const;
    void measure_shadows();
    void measure_policies();
    [[nodiscard]] std::optional<int> finish();
    void camera_selftest_tick();
    void free_selftest_tick();
    // Reads and hashes the map camera config and its bindings, and loads the
    // mode's constants from the effective VFS. Empty `failure` on success.
    [[nodiscard]] std::optional<eawr::viewer::MapCameraSource> load_map_camera(
        camera::Mode mode, std::string& failure) const;
};

} // namespace eawr::presentation::godot_backend
