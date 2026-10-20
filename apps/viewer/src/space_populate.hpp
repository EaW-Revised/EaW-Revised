#pragma once

#include "idle_clips.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/space/population_index.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// `--eawr-populate` on a kind-2 (space) map (P1-11 #32). The static scene
// builder reads the map (Space_Model_Name chain); scene::classify_space_placements
// classifies map props and scene::classify_live_unit classifies composed units.
// Every drawn surface is uploaded through its legacy
// selector with its authored material values: one renderer asset per (model,
// surface and team colour), shared by placements, posed at bind. With a
// lighting policy the scene light is set before those uploads, so with shadows
// on the hulls compile the shadow-receiving variant and the directional light
// casts. Placements that are not drawn are reported with their reason; nothing
// is substituted. SpaceEnvironment owns the renderer and calls in.
class SpacePopulation final {
public:
    // Clear of the sky surfaces (1..), the foreground controls (1000) and the
    // space fog units (2001..).
    static constexpr sim::AssetId first_asset = 10001;
    static constexpr sim::EntityId first_entity = 10001;

    struct Options final {
        struct DebugShip final {
            std::string object_id;
            std::uint32_t spawn_record{55};
            // --eawr-space-hardpoint-state <HardPoint id>=<state>, applied in order.
            std::vector<std::pair<std::string, scene::HardpointState>> hardpoint_states{};
        };
        std::string map_sha256;
        const data::Catalog* catalog{};
        lighting::Policy policy{lighting::Policy::off};
        bool shadows{};
        lighting::Environment environment{lighting::alo_viewer_default_environment()};
        std::string environment_source{"default"};
        std::uint32_t animation_frames{60};
        // --eawr-camera-interactive: the idle clock is the view's real time
        // and never holds; otherwise it holds at animation_frames - 1.
        bool live_clock{};
        // --eawr-map-idle-offset: 30 Hz ticks added to the idle clock, so a
        // fixed capture can show any moment of the idle clips.
        std::uint32_t idle_offset{};
        std::function<std::optional<std::array<std::uint8_t, 3>>(const scene::Placement&)> team_colour;
        bool reject_upload_for_test{};
        std::optional<DebugShip> debug_ship;
        // --eawr-space-place-at (#70 movement evidence): inert FoC space units at explicit
        // source positions and yaws, such as the samples of a movement trace. Not composed
        // with debug_ship.
        struct PlacedShip final {
            std::string object_id;
            assets::SourceVec3 position{};
            float yaw_degrees{};
            // #80: a live-session unit when nonzero. Any drawable type is
            // accepted, one that draws nothing is reported, not fatal, and its
            // pieces follow pose_live() from the session's snapshots.
            sim::EntityId live_entity{};
            std::optional<std::array<std::uint8_t, 3>> team_colour{};
            // Without team_colour: the map record whose placement's colour it takes.
            std::optional<std::uint32_t> colour_record{};
            // #81: a live ship that plays this clip (a death clone's DIE
            // clip) under pose_live_clips() instead of its idle clip.
            std::string clip{};
            // #76: a second clip it switches to (a SPOILER_LOCK craft's UNDEPLOY beside its DEPLOY
            // clip, AB-31); LiveClipPose::alternate picks it.
            std::string alternate_clip{};
            // #81: a death clone (counted apart from the session's units), with
            // or without a clip.
            bool death_clone{};
            // #391: a hardpoint's breakoff prop (counted apart as well).
            bool breakoff{};
            // #79: a slot for a craft launched after tick zero (counted apart as well).
            bool launch_slot{};
            bool placement_preview{}; // WR-12: a visual clone, with attached particle emitters hidden
            // #456: a model slot for a projectile in flight (BattleEffects' pools; counted apart).
            bool projectile_slot{};
            bool beacon_model{}; // HCB-12: reusable beacon model, counted apart from session units.
            bool ability_proxy_owner{}; // WAB-75: explicit ability-created visual slot.
            bool construction{};
            // #427: its type has a DEFEND ability, so its SHIELD sub-object is composed as the
            // shield shell pose_live() shows while LivePose::defend_active (BP-22).
            bool defend_shell{};
            // WST-12: its type has STEALTH, so its STEALTH sub-object is composed as the cloak
            // pose_live() shows while LivePose::stealth_active, hiding BASEMESH and the hardpoints.
            bool stealth_shell{};
        };
        std::vector<PlacedShip> placed_ships;
        // #80: TED records whose objects the live session owns; the session's
        // snapshot draws them (as placed ships), so the static map does not.
        std::vector<std::uint32_t> session_records;
        // Re-plans the attached particle effects over scene() with the
        // current hardpoint states, starting and stopping their emitters
        // (#136). Called once at the end of compose and after each
        // set_hardpoint_state that changes a state; false fails the caller.
        std::function<bool(SpacePopulation&)> attached_effects;
        // BLD-10: the placements compose has uploaded, of all it decided on (done, total), for the
        // battle loading screen's bar. Called before each placement and once at the end.
        std::function<void(std::size_t, std::size_t)> progress;
    };

    explicit SpacePopulation(Options options);
    SpacePopulation(const SpacePopulation&) = delete;
    SpacePopulation& operator=(const SpacePopulation&) = delete;

    // The lighting environment for `--eawr-environment default|map`, as the
    // land path reads it: alo-viewer's default, or the map's environment 0
    // under the candidate TED mapping. Nullopt with `failure` set otherwise.
    [[nodiscard]] static std::optional<lighting::Environment> environment(
        const assets::Map& map, std::string_view choice, std::string& failure);

    // False when nothing can be composed (no catalog, or a drawn surface fails
    // to upload); failure() says why and everything uploaded is released.
    [[nodiscard]] bool compose(GodotRenderer& renderer, const assets::Map& map, const vfs::Vfs& filesystem,
                               const FixedCamera& camera, std::span<const std::uint32_t> environment_records,
                               sim::AssetId first_asset, sim::EntityId first_entity);
    // Append newly demanded slots without replacing any existing ship, piece or playback state.
    [[nodiscard]] bool append_live_ships(GodotRenderer& renderer, const vfs::Vfs& filesystem,
                                        std::span<const Options::PlacedShip> ships);
    void pose_units(GodotRenderer& renderer, std::uint32_t sample);
    // Live frames first capture a disappearing model's last submitted palette,
    // then consume this same idle sample once before current live posing.
    void defer_unit_sample(std::uint32_t sample) noexcept { pending_unit_sample_ = sample; }
    void pose_pending_units(GodotRenderer& renderer);

    // #80: where a live-session unit is drawn this frame. `ship` indexes
    // Options::placed_ships; position (source units), the facing yaw and
    // the bank roll (degrees, #351) are the simulation's, converted by
    // live_unit_transform().
    struct LivePose final {
        std::size_t ship{};
        sim::math::Vec3 position{};
        sim::math::Fixed yaw_degrees{};
        sim::math::Fixed roll_degrees{};
        // R-ROT-01's pitch (positive lowers the nose): a squadron craft's climb or dive (#506,
        // space-fighters FM-02) or a tumbling breakoff prop's (#391); zero for a ship.
        sim::math::Fixed pitch_degrees{};
        // Hardpoint states in the type's HardPoints order; empty keeps them.
        std::vector<scene::HardpointState> hardpoints{};
        // #427: the unit's DEFEND ability runs, so its shield shell shows (BP-22).
        bool defend_active{};
        // WST-12: the unit's cloak is on: the STEALTH sub-object shows instead of the hull.
        bool stealth_active{};
        bool blast_charging{};
        bool blast_charged{};
        std::optional<double> construction_hull{};
        // Reusable launch slots bind to the simulation unit currently occupying them.
        sim::EntityId entity{};
        struct TurretPose { sim::math::Fixed yaw{}, pitch{}; bool xy_only{}, barrel{}; };
        std::optional<TurretPose> turret{};
    };
    // Moves every live ship's pieces to its pose and hides the live ships
    // `poses` does not list, then refreshes instances(). False (failure()
    // set) when a transform leaves the Q24 range.
    [[nodiscard]] bool pose_live(std::span<const LivePose> poses, const particles::StepExecutor* workers = nullptr);
    void trace_frames(bool enabled) noexcept { trace_frames_ = enabled; }
    [[nodiscard]] double compose_ms() const noexcept { return compose_ms_; }
    [[nodiscard]] double refresh_ms() const noexcept { return refresh_ms_; }
    [[nodiscard]] double idle_frame_ms() const noexcept { return idle_frame_ms_; }
    [[nodiscard]] double idle_sample_ms() const noexcept { return idle_sample_ms_; }
    [[nodiscard]] double idle_upload_ms() const noexcept { return idle_upload_ms_; }
    [[nodiscard]] const SkinPoseWork& idle_skin_work() const noexcept { return idle_skin_work_; }
    [[nodiscard]] std::size_t idle_placement_count() const noexcept { return idle_placements_.size(); }
    [[nodiscard]] std::size_t idle_surface_count() const noexcept { return animated_instances_.size(); }
    // #81: the clip a live ship with PlacedShip::clip bound at compose; null
    // when it has none or it did not bind (the report says why).
    [[nodiscard]] const animation::Player* live_clip(std::size_t ship, bool alternate = false) const noexcept;
    // #81: poses those ships' skinned pieces at `position` of their clip,
    // blended from the bind pose by `blend_from` (0 is the clip's pose).
    // Bones the clip hides are collapsed to their origin. False (failure()
    // set) when the renderer refuses a pose.
    struct LiveClipPose final {
        std::size_t ship{};
        animation::ClipPosition position{};
        float blend_from{};
        bool alternate{}; // PlacedShip::alternate_clip (#76)
    };
    [[nodiscard]] bool pose_live_clips(GodotRenderer& renderer, std::span<const LiveClipPose> poses,
        const particles::StepExecutor* workers = nullptr);
    [[nodiscard]] bool live_clip_terminal_hidden(std::size_t ship) const noexcept;
    [[nodiscard]] std::span<const animation::BonePose> terminal_clip_bones(std::size_t ship) const noexcept;
    [[nodiscard]] std::span<const animation::BonePose> held_clip_bones(std::size_t ship,
        animation::ClipPosition position, float blend_from) const noexcept;
    [[nodiscard]] bool pose_live_turrets(GodotRenderer& renderer, std::span<const LivePose> poses,
        std::span<const LiveClipPose> clips);
    // #81: a live ship that is gone for good (a death clone removed or faded
    // out): drops its pieces and instance poses. Reusable launch clones keep
    // immutable compose templates and shared uploads until restored for a new owner.
    void retire_live_ship(GodotRenderer& renderer, std::size_t ship, bool reusable = false);
    void restore_live_ship(GodotRenderer& renderer, std::size_t ship);
    // #4218: the unit now on `ship` takes its particle seeds (the placement ordinals) and its idle start from
    // `identity_ship`, the slot the session's reservation policy bound it to; the same ship is the identity of an
    // unreused slot. Integer state only: no instance or allocation per frame.
    void bind_live_identity(std::size_t ship, std::size_t identity_ship);
    // The placed ships that compose drew; the others are listed with a reason
    // in the report.
    [[nodiscard]] bool live_ship_drawn(std::size_t ship) const noexcept;
    // Whether the unit emitters have anything to do for this slot besides stopping what it runs:
    // it is drawn and not retired, and is posed this frame or is a death clone (which they step
    // on its clip). A slot that fails this and runs nothing needs no visit (5000+ pooled slots).
    [[nodiscard]] bool live_ship_emitter_visit(std::size_t ship) const noexcept;
    // #82: a drawn live ship's pick volume this frame: the box of its composed
    // pieces in its model space (source basis) and the model transform the
    // last pose_live() gave it. Empty when it is not drawn or not posed.
    struct LiveShipBox final {
        sim::math::Mat3x4 model_to_world{};
        std::array<float, 3> low{};
        std::array<float, 3> high{};
    };
    [[nodiscard]] std::optional<LiveShipBox> live_ship_box(std::size_t ship) const;
    // MM-17: composition's model identity, independent of fog, posing or current visibility.
    // Missing population/placement information is unknown, not a model-free object.
    [[nodiscard]] std::optional<bool> live_ship_model_present(std::size_t ship) const noexcept {
        return composed_ && scene_ && ship < live_models_.size() ? live_models_[ship] : std::nullopt;
    }
    // #394: what the emitters that follow a drawn live ship read each frame:
    // its composed placement (model and proxy references), its hardpoints and
    // their current states, and the model transform the last pose_live() gave
    // it (source basis, scale included) while it is shown. Empty when the ship
    // was not drawn or is retired.
    struct LiveShipEmitterView final {
        const scene::Placement* placement{};
        std::span<const scene::HardpointAttachment> hardpoints;
        std::span<const scene::HardpointState> states;
        std::optional<sim::math::Mat3x4> model_to_world;
        bool death_clone{};
        // #456: a model projectile's slot; its pose between samples comes from the projectile.
        bool projectile{};
        sim::EntityId entity{};
        // #4218: the placement whose ordinals seed this unit's emitters (the ship's own unless a reused slot
        // carries the identity of the slot the reservation policy would have given the unit).
        const scene::Placement* identity{};
    };
    // #421: the model transform pose_live() gives a drawn live ship at `pose`; nullopt when the
    // ship is not drawn or retired, or the transform leaves the Q24 range.
    [[nodiscard]] std::optional<sim::math::Mat3x4> live_transform(const LivePose& pose) const;
    [[nodiscard]] std::size_t live_ship_count() const noexcept { return live_decisions_.size(); }
    // Composed asset references, including unshown reinforcement/death/projectile
    // slots. Loading consumers may prepare immutable data without posing owners.
    [[nodiscard]] std::optional<LiveShipEmitterView> live_ship_emitter_assets(std::size_t ship) const;
    [[nodiscard]] std::optional<LiveShipEmitterView> live_ship_emitter_view(std::size_t ship) const;
    struct LiveBoneFrame final { sim::math::Mat3x4 transform; bool visible{}; };
    // PS-02: the same posed bone and model transform as the drawn hull.
    [[nodiscard]] std::optional<LiveBoneFrame> live_bone_frame(sim::EntityId entity, std::uint32_t bone) const;
    // #427: the entities of a live ship's own model surfaces (hull, engine and damage-decal
    // surfaces), without its hardpoints' attached models and its shield shell: what its
    // light scale reaches (BP-21).
    [[nodiscard]] std::span<const sim::EntityId> live_hull_entities(std::size_t ship) const;
    // Borrowed entity spans remain valid until composition, retirement or release.
    // #535: every entity of a live ship's composed pieces: its hull surfaces, its hardpoints'
    // attached models and its shield shell alike, the set the fog fade's opacity is offered to
    // (space-fog-presentation.md FW-19); each piece's adapter decides whether it dithers.
    [[nodiscard]] std::span<const sim::EntityId> live_ship_entities(std::size_t ship) const;
    // FW-19: propagate only changed ship opacity, including currently gated pieces.
    void set_live_opacity(GodotRenderer& renderer, std::size_t ship, float opacity);
    // One reusable lookup for a frame containing fog transitions.
    void prepare_fog_model_capture();
    // FW-26: independent non-shield pieces, retained uploads and frozen skin
    // palettes. These copies never enter picking, bars or emitter providers.
    [[nodiscard]] bool remember_fog_model(GodotRenderer& renderer, sim::EntityId entity, std::size_t ship,
        std::span<const animation::BonePose> neutral_pose = {},
        std::optional<std::array<float, 3>> neutral_colour = {},
        // FW-25: first knowledge uses the immutable composed start placement,
        // including intact art, before the object has ever been submitted.
        bool initial = false);
    void draw_fog_models(GodotRenderer& renderer, const space::FogGhosts& memory,
                         std::vector<sim::RenderInstance>& output);
    [[nodiscard]] std::size_t fog_model_pieces(sim::EntityId entity) const noexcept;
    // #427: the shield shells' effect clock (seconds).
    void set_shield_time(GodotRenderer& renderer, float seconds);
    void release(GodotRenderer& renderer);

    // The report's populate.attached_effects. A space map's attached particle
    // effects are planned by the land attachment plan over this scene and run
    // by the map particle provider, which reports them under map_particles.
    struct AttachedEffects final {
        bool composed{};
        std::size_t records{};
        // Damage emitters of intact/damaged hardpoints and engine emitters of
        // destroyed hardpoints that the plan hid (#136, #329).
        std::size_t hardpoint_hidden{};
        // Records the plan admitted, and those the aggregate particle budget
        // (--eawr-map-particle-capacity) turned away.
        std::size_t admitted{};
        std::size_t capacity_exhausted{};
        std::string cause{"not planned"};
    };
    void set_attached_effects(AttachedEffects summary) { attached_effects_ = std::move(summary); }

    [[nodiscard]] const std::vector<sim::RenderInstance>& instances() const noexcept { return instances_; }

    // The presentation hook #72 drives (#136): a hardpoint's state selects
    // its art (scene::hardpoint_art). Every hardpoint of a composed placement
    // starts intact. Sets every HardPoints entry of the placement whose id
    // matches (ASCII case-insensitive), refreshes instances() and, when a
    // state changed, has Options::attached_effects start or stop the
    // hardpoint's damage emitters. False when the placement is not drawn,
    // lists no such hardpoint, or the emitters could not follow (failure()
    // says why). The hardpoint's Model_To_Attach and Damage_Decal pieces are
    // uploaded at compose, so no renderer call is needed; a consumer
    // re-submits instances().
    [[nodiscard]] bool set_hardpoint_state(std::uint64_t scene_ordinal, std::string_view hardpoint,
                                           scene::HardpointState state);
    // The composed scene: the map's placements, then the debug ship.
    [[nodiscard]] const scene::Scene* scene() const noexcept { return scene_ ? &*scene_ : nullptr; }
    // The states of a composed placement's HardPoints entries, in XML order;
    // empty when the placement is not drawn (every hardpoint intact).
    [[nodiscard]] std::span<const scene::HardpointState> hardpoint_states(std::uint64_t scene_ordinal) const;
    // True when the placement is a catalog marker (role marker): editor-only
    // in retail, so it draws neither its hull nor its model's proxies (#284).
    [[nodiscard]] bool is_marker(std::uint64_t scene_ordinal) const;
    [[nodiscard]] bool owns(sim::AssetId asset) const noexcept { return passes_.contains(asset); }
    [[nodiscard]] std::optional<RenderPass> pass(sim::AssetId asset) const;
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] std::size_t live_assets() const noexcept { return uploaded_.size(); }

    // The report's "populate" and "lighting" members, each followed by ",\n".
    void write_report(std::ostream& output) const;

private:
    struct ComposeContext;
    struct ComposeBatch;
    struct SurfaceUpload;
    void prepare_compose_slots(bool appending);
    bool classify_arrivals(ComposeContext& context, std::size_t first_ship, std::size_t first_decision);
    std::optional<std::size_t> bind_composed_idle(ComposeContext& context, ComposeBatch& batch,
        const scene::Placement& placement, const assets::Model& model);
    std::optional<std::size_t> bind_composed_clip(ComposeContext& context, const scene::Placement& placement,
        const assets::Model& model, const std::string& path);
    const SurfaceUpload* upload_composed_surface(ComposeContext& context, ComposeBatch& batch, const std::string& path,
        const assets::Model& model, std::uint32_t mesh_index, std::uint32_t submesh_index,
        const scene::LegacySelector& selector, const std::string& texture_path);
    IdlePlacement composed_idle_placement(ComposeContext& context, const scene::Placement& placement, std::size_t clip);
    void compose_hull_pieces(ComposeContext& context, ComposeBatch& batch, scene::SpacePlacementDecision& decision,
        std::size_t decision_index, const scene::Placement& placement, const assets::Model* model,
        const std::optional<std::size_t>& live_ship, const std::optional<std::array<std::uint8_t, 3>>& colour,
        bool cloak_ship);
    void compose_shell_pieces(ComposeContext& context, ComposeBatch& batch, scene::SpacePlacementDecision& decision,
        const scene::Placement& placement, const assets::Model* model, const std::optional<std::size_t>& live_ship,
        bool cloak_ship);
    void compose_piece_animation(ComposeContext& context, ComposeBatch& batch, const scene::Placement& placement,
        const assets::Model* model, const std::optional<std::size_t>& live_ship, std::size_t first_piece);
    void compose_hardpoint_pieces(ComposeContext& context, ComposeBatch& batch,
        scene::SpacePlacementDecision& decision, std::size_t decision_index, const scene::Placement& placement,
        const assets::Model* model, const std::optional<std::size_t>& live_ship,
        const std::optional<std::array<std::uint8_t, 3>>& colour, bool cloak_ship);
    void compose_draw_pieces(ComposeContext& context, ComposeBatch& batch, std::size_t first_decision, bool appending);
    float commit_composed_pieces(ComposeContext& context, ComposeBatch& batch, sim::EntityId& next_entity);
    bool finish_composed_range(ComposeContext& context, bool appending, sim::AssetId next_asset,
        sim::EntityId next_entity, std::size_t receiving_before, std::size_t variant_failures_before,
        float debug_farthest);
    [[nodiscard]] bool compose_range(ComposeContext&, std::size_t first_ship, std::size_t first_decision,
                                    sim::AssetId first_asset, sim::EntityId first_entity);
    void schedule_arrival_sample() noexcept;
    assets::Source compose_source_;
    FixedCamera compose_camera_{};
    sim::AssetId next_arrival_asset_{std::numeric_limits<sim::AssetId>::max() / 32U};
    sim::EntityId next_arrival_piece_{std::numeric_limits<sim::EntityId>::max() / 32U};
    [[nodiscard]] std::optional<SurfaceUpload> upload_surface(
        GodotRenderer& renderer, const vfs::Vfs& filesystem, scene::VfsAssetCache& cache,
        std::map<std::string, assets::Texture>& textures, sim::AssetId& next_asset,
        const assets::Model& model, std::uint32_t mesh_index, std::uint32_t submesh_index,
        const scene::LegacySelector& selector, const std::string& texture_path,
        const std::optional<std::array<std::uint8_t, 3>>& colour, const std::string& identity);
    [[nodiscard]] std::optional<SurfaceUpload> upload_shell(
        GodotRenderer& renderer, const vfs::Vfs& filesystem, scene::VfsAssetCache& cache,
        sim::AssetId& next_asset, const assets::Model& model, std::uint32_t mesh_index,
        std::uint32_t submesh_index, std::string& status, bool neutral_distortion = false);
    // A piece of hardpoint art: shown by the state of hardpoint `hardpoint`
    // of decisions_[decision].
    struct HardpointGate final {
        enum class Art : std::uint8_t { attached_model, damage_decal, engine_particle };
        std::size_t decision{};
        std::size_t hardpoint{};
        Art art{Art::attached_model};
    };
    struct GatedInstance final {
        sim::RenderInstance instance;
        std::optional<HardpointGate> gate;
        // A live ship's piece (#80): its ship and its frame in the ship's model space.
        std::optional<std::size_t> live;
        sim::math::Mat3x4 local{sim::math::identity_matrix()};
        // #427: its ship's shield shell, drawn while the ship's DEFEND runs.
        bool shield{};
        std::optional<std::uint32_t> alternate{};
        // WST-12: its ship's STEALTH sub-object (shown while cloaked), or a BASEMESH surface or
        // attached hardpoint model (hidden while cloaked).
        bool cloak{};
        bool cloak_hidden{};
        std::optional<bool> blast_charged{}; // BP-74: false=buildup, true=charged
        std::optional<std::array<std::uint8_t, 3>> colour; // immutable compose tint, restored on slot reuse
    };
    void rebuild_piece_index();
    void refresh_instances();
    [[nodiscard]] std::size_t hidden_decals(std::size_t decision) const;

    Options options_;
    bool trace_frames_{};
    double compose_ms_{};
    double refresh_ms_{};
    double idle_frame_ms_{};
    double idle_sample_ms_{};
    double idle_upload_ms_{};
    SkinPoseWork idle_skin_work_{};
    std::optional<scene::Scene> scene_;
    std::vector<scene::SpacePlacementDecision> decisions_;
    // Every composed piece, and those the hardpoint states show.
    std::vector<GatedInstance> pieces_;
    std::vector<std::uint32_t> live_alternates_;
    std::vector<std::uint32_t> live_alternate_counts_;
    space::PopulationIndex piece_index_;
    space::AttachmentMarks attachment_marks_;
    std::vector<std::uint8_t> compose_errors_;
    std::vector<sim::RenderInstance> instances_;
    struct FogPiece {
        sim::EntityId source{};
        sim::RenderInstance instance;
    };
    std::map<sim::EntityId, std::vector<FogPiece>> fog_models_;
    std::vector<sim::EntityId> fog_drawn_entities_;
    std::optional<std::uint32_t> pending_unit_sample_;
    std::optional<std::uint32_t> last_unit_sample_;
    sim::EntityId next_fog_piece_{std::numeric_limits<sim::EntityId>::max() / 16U};
    // Aligned with decisions_, then with each decision's hardpoints.
    std::vector<std::vector<scene::HardpointState>> hardpoint_states_;
    std::vector<sim::AssetId> uploaded_;
    // AVC-02: only animated additive materials, never static placement transforms.
    std::vector<sim::AssetId> effect_clock_assets_;
    std::optional<std::uint32_t> effect_sample_;
    std::map<sim::AssetId, RenderPass> passes_;
    // "<model> surface <n> shader <name>: <diagnostic>" per failed upload.
    std::vector<std::string> upload_failures_;
    std::uint64_t surfaces_uploaded_{};
    std::uint64_t instance_count_{};
    std::uint64_t attachments_drawn_{};
    std::uint64_t skinned_instances_{};
    std::uint64_t team_colour_variants_{};
    std::map<std::string, std::uint64_t> team_colour_status_;
    std::map<std::string, std::uint64_t> animation_status_;
    std::map<std::string, std::string> bound_idle_hulls_;
    std::map<std::string, std::string> static_hulls_;
    // One per animated placement (#145, idle_clips.hpp); instances index it.
    struct AnimatedInstance final { sim::EntityId entity{}; sim::AssetId asset{}; std::size_t idle{}; };
    std::vector<std::shared_ptr<const animation::Player>> clips_;
    std::vector<IdlePlacement> idle_placements_;
    std::map<std::size_t, std::size_t> idle_live_ships_;
    std::map<sim::EntityId, std::size_t> contact_live_ships_;
    std::vector<std::vector<animation::BonePose>> contact_bones_;
    struct TurretBinding {
        std::shared_ptr<const animation::Player> player;
        animation::Pose pose;
        std::vector<animation::BonePose> bind_bones;
        std::size_t turret{};
        std::optional<std::size_t> barrel, idle;
    };
    std::vector<std::optional<TurretBinding>> turret_bindings_;
    std::vector<AnimatedInstance> animated_instances_;
    std::map<std::string, animation::IdlePlayback> idle_playbacks_;
    // Object id -> the Idle_Anim_00_Rate_Mod text that did not parse (rate 1 kept).
    std::map<std::string, std::string> idle_rate_rejected_;
    std::uint64_t idle_sample_failures_{};
    std::uint64_t animated_placements_{};
    std::optional<std::uint32_t> animation_sample_;
    std::uint64_t shadow_receiving_{};
    std::uint64_t shadow_variant_failures_{};
    float shadow_max_distance_{};
    GodotRenderer::LightingState lighting_;
    AttachedEffects attached_effects_;
    std::string failure_;
    std::string debug_ship_status_{"not_requested"};
    // #80: per placed ship, its decision index when drawn, and whether the
    // last pose_live() listed it; the reasons a live ship was not drawn.
    std::vector<std::optional<std::size_t>> live_decisions_;
    std::vector<std::optional<bool>> live_models_;
    std::vector<bool> live_shown_;
    std::vector<bool> live_death_clone_;  // per placed ship, PlacedShip::death_clone (fixed at compose)
    // #427: per placed ship, whether the last pose_live() gave it an active DEFEND; the
    // shield shell uploads and, per object id, what became of its shell.
    std::vector<bool> live_defend_;
    // WST-12: per placed ship, whether the last pose_live() gave it an active cloak; per object
    // id, what became of its STEALTH sub-object.
    std::vector<bool> live_stealth_;
    std::vector<bool> live_blast_charging_;
    std::vector<bool> live_blast_charged_;
    std::map<std::string, std::uint64_t> blast_meshes_shown_;
    std::map<std::string, std::string> stealth_shells_;
    std::uint64_t stealth_shells_shown_{};
    std::vector<sim::AssetId> shield_assets_;
    std::map<std::string, std::string> shield_shells_;
    std::uint64_t shield_shells_shown_{};
    // #82: each live ship's model-space box (min, max) and last pose transform.
    std::vector<std::optional<std::pair<std::array<float, 3>, std::array<float, 3>>>> live_bounds_;
    std::vector<std::optional<sim::math::Mat3x4>> live_transforms_;
    std::vector<bool> backdrop_ships_;
    std::vector<std::string> live_undrawn_;
    std::vector<std::string> launch_slots_undrawn_;
    std::vector<std::string> projectile_slots_undrawn_;
    // #81: per placed ship, the index in clips_ of its PlacedShip::clip, and
    // the skinned pieces that clip poses; "<object> <clip>" -> binding status.
    struct LiveClipInstance final { std::size_t ship{}; sim::EntityId entity{}; sim::AssetId asset{}; };
    std::vector<std::optional<std::size_t>> live_clips_;
    std::vector<std::optional<std::size_t>> live_alternate_clips_; // #76
    std::vector<LiveClipInstance> live_clip_instances_;
    struct HeldClip {
        animation::ClipPosition position;
        std::vector<animation::BonePose> bones;
        bool hidden{};
    };
    std::map<std::size_t, HeldClip> held_clips_;
    struct PreparedLiveClip {
        const animation::Player* player{};
        bool terminal{}, cached{}, hidden{};
        std::vector<std::vector<std::uint8_t>> geometry;
        std::optional<animation::Pose> pose;
        std::string failure;
    };
    std::vector<PreparedLiveClip> prepared_clips_;
    std::span<const LiveClipPose> pending_clip_poses_;
    void prepare_live_clip(std::size_t index);
    const std::function<void(std::size_t)> live_clip_task_{[this](const std::size_t index) { prepare_live_clip(index); }};
    // Immutable compose templates for reusable launch-clone slots. No per-instance
    // renderer state or pose survives parking; shared mesh uploads stay warm.
    struct ParkedShip {
        std::vector<GatedInstance> pieces;
        std::vector<LiveClipInstance> clips;
        std::vector<AnimatedInstance> animated;
    };
    std::map<std::size_t, ParkedShip> parked_ships_;
    std::map<std::string, std::string> live_clip_status_;
    std::uint64_t live_clip_poses_{};
    std::uint64_t live_clip_hidden_bones_{};
    std::vector<bool> live_retired_;
    std::vector<std::size_t> live_identity_;  // by live ship; a short vector or the ship's own index = its own slot
    [[nodiscard]] const scene::Placement* live_placement(std::size_t ship) const noexcept;
    std::size_t live_released_assets_{};
    std::size_t session_records_skipped_{};
    std::vector<std::uint32_t> skipped_session_records_;
    // Historical compose assignments: reports are also written after release.
    std::vector<std::vector<sim::AssetId>> hull_surface_assets_;
    // The debug object's XML type and placed yaw (its marker's own yaw).
    std::string debug_ship_type_{"SpaceUnit"};
    float debug_ship_yaw_{};
    std::size_t map_placements_{};
    // Set once compose has planned the attached effects; a state change
    // re-plans them only from then on.
    bool composed_{};
};

} // namespace eawr::presentation::godot_backend
