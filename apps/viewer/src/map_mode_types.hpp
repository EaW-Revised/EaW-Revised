#pragma once

#include "map_mode_assets.hpp"

namespace eawr::presentation::godot_backend {

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
    explicit State(Options value);
    ~State();

    Options options;
    std::unique_ptr<GodotRenderer> renderer;
    // E-space-primary-sky-v1: a kind-2 map is composed by SpaceEnvironment;
    // the land members below stay unused for it. The populated placements
    // (P1-11 #32) outlive it, since it releases them on its renderer.
    // --eawr-live-session (#80): declared before the population and the space
    // view, whose hooks point at it, so it outlives them.
    LiveSessionView::Options live_options;
    std::unique_ptr<LiveSessionView> live_session;
    // #660: keep one capture target through impact; a later volley never steals the camera.
    std::optional<std::uint64_t> followed_projectile;
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
    // #638: the pool the live battle's particle systems step on; declared before the objects
    // that use it, so it goes last.
    std::unique_ptr<ParticleWorkers> particle_workers;
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
    // #638: the main thread's ms in this frame's unit emitters, battle effects and breakoff props.
    double particle_ms{};
    double emitter_frame_ms{};
    double effects_frame_ms{};
    double debris_frame_ms{};
    double hud_ms{};
    double audio_ms{};
    double fog_ms{};
    std::uint64_t perf_trace_tick{};  // the newest tick whose cost the perf trace wrote
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
    std::shared_ptr<const vfs::Vfs> filesystem;
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
    void refresh_fog_snapshots();

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
    // #425, #454, #530: the command bar's card slots (the selection's unit cards, or the station's
    // build buttons while it is the production object, PU-60) and the ability bar.
    void sync_cards();
    // The unit tables' type names by session type ID (filled once).
    void fill_type_names();
    // #530 (PU-63 to PU-68): the build queue, credits, pool pane and population from the local
    // player's economy, and the pool slots' types for a pick.
    void sync_production();
    std::vector<sim::tactical::TypeId> pool_types;
    EawrProductionPanel::View production_view;
    presentation::ui::PoolLayoutCache production_pool_cache;
    std::size_t production_pool_pending{};
    presentation::ui::BuildMenuCache production_menu_cache;
    std::array<std::vector<sim::tactical::QueueEntry>, sim::tactical::build_queue_count> production_queues;
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
    std::map<sim::tactical::TypeId, std::array<double, 3>> minimap_hazard_sizes;

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
    void build_attached_plan(scene::VfsAssetCache& cache, const SpacePopulation* hardpoints = nullptr);
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
    void release_particles();
    std::shared_ptr<const data::Catalog> catalog;
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
    std::vector<sim::AssetId> effect_clock_assets;
    std::optional<std::uint32_t> effect_sample;
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
    [[nodiscard]] bool fail_ready(std::string message);
    [[nodiscard]] bool ready_space(Node3D& host, const assets::Map& map,
                                   const assets::ObjectTypeCatalog& catalog, const std::string& catalog_failure);
    [[nodiscard]] bool ready_land_population(const assets::Map& map, std::vector<sim::RenderInstance>& instances,
                                             sim::AssetId& next_asset, sim::EntityId& next_entity);
    [[nodiscard]] std::string terrain_material_program() const;
    void record_terrain_slots(const terrain::Mesh& mesh);
    [[nodiscard]] bool upload_terrain_surfaces(const terrain::Mesh& mesh, const MaterialDescription& terrain_material,
                                              const assets::Texture& unused_texture, std::vector<sim::RenderInstance>& instances,
                                              sim::AssetId& next_asset, sim::EntityId& next_entity);
    void compose_skydome(const assets::Map& map, const assets::ObjectTypeCatalog& catalog,
                         const std::string& catalog_failure, const land_look::TextureLookup& lookup_texture,
                         sim::AssetId& next_asset, sim::EntityId& next_entity, std::vector<sim::RenderInstance>& instances);

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
    void write_report_scene(std::ostream& output) const;
    void write_report_lighting(std::ostream& output) const;
    void write_report_fog(std::ostream& output) const;
    void write_report_population(std::ostream& output) const;
    void write_report_camera(std::ostream& output) const;
    void write_report_effects(std::ostream& output, std::uint64_t particle_allocation, std::uint64_t particle_live_at_capture) const;

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
        camera::Mode mode, std::string& failure, const eawr::viewer::MapCameraConfig* generated = nullptr) const;
};

} // namespace eawr::presentation::godot_backend
