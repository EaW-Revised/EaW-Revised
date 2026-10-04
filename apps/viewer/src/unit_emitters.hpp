#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/particles/attachment_lifecycle.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/vfs/vfs.hpp"
#include "particle_adapter.hpp"
#include "space_populate.hpp"

#include <godot_cpp/classes/node3d.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

// The particle proxies of the live battle's units (#394, docs/behaviour/battle-presentation.md
// BP-40 to BP-46): what a FoC space unit's model runs on its own bones while it flies.
// - Engine emitters (proxy names starting "pe", as the Nebulon-B's pe_nebulonengines and the
//   Tartan's pe_tartanengine_*) run while the unit's engines are online and stop for good once
//   they are off-line (its last engine hardpoint destroyed). While online they draw at FoC's
//   brightness, 0.2 + 0.8 x speed / maximum speed: dim at rest, full at top speed.
// - Turbo engine ("pte"), power-to-weapons ("pptw") and missile-shield ("pgw") emitters are
//   hidden when the object is created. While TURBO or SPOILER_LOCK runs the turbo engines
//   replace the engine emitters, and POWER_TO_WEAPONS shows its effect (#76, AB-31, AB-32); a
//   proxy the model authors hidden is shown by the same emitter-type switch (BP-43).
// - Ion-stun ("pi") emitters run while the unit is ion stunned (space-damage IS-09), authored
//   hidden or not: FoC loads the authored flag into the code flag the stun clears. When the stun
//   ends they stop emitting and their particles drain.
// - A hardpoint's damage emitters (the proxies below its Damage_Particles bone) run from the
//   moment it is destroyed; the emitters below a destroyed hardpoint's Engine_Particles bone
//   stop when it sets Engine_Death_Hide_Engine_Particles.
// - A death clone runs its own model's proxies (#421, BP-46): each rides its bone on the clone's
//   death clip pose, starts when the clip shows that bone and stops emitting when it hides it,
//   its particles draining (BP-48, the debug build, over the clip's per-frame
//   bone visibility); a bone shown again starts a fresh instance. So the corvette's pieces trail
//   p_rebelsmokedeath until each one vanishes, and each vanishing piece fires its
//   p_explosion_big00; a station's fire billows and explosion chains emit from the meshes of
//   its pieces. When the clone leaves, what still runs stops and drains.
// Admission is the static space map's attached plan (particles::plan_map_effects over
// scene::hidden_hardpoint_proxies) plus those emitter-type rules, but each emitter moves with
// the ship: its frame is the ship's drawn model transform composed with the proxy bone's bind
// frame, set every drawn frame. The emitters advance in 30 Hz samples; a frame drawn between
// two samples draws them again at its own pose and camera (#433): the particles that live in
// their emitter's frame (FoC's linked particles, BP-40, as the engine glows) follow it in rotation
// as well as translation, so the glow stays on the nozzles through a turn.
// A frame that reaches many ticks at once (a presentation stall, #406) runs each sample it
// catches up on at that sample's own presented tick: the ship's pose, visibility, hardpoint
// states and engines come from the snapshots of that tick, so a damage emitter is born at its
// hardpoint's death and aged from there, and a moving ship leaves its trail. A sample whose
// snapshots have left the session's history has no known pose: its ships run nothing (as when
// hidden, BP-44), and their emitters start again at the oldest sample the history holds.
//
// A pure consumer of the population's live ship poses and hardpoint states and of the session's
// snapshots; it never feeds back, so the session's hashes are unchanged.
class UnitEmitters final {
public:
    UnitEmitters(godot::Node3D& host, const vfs::Vfs& filesystem);
    ~UnitEmitters();
    UnitEmitters(const UnitEmitters&) = delete;
    UnitEmitters& operator=(const UnitEmitters&) = delete;

    // What a unit's snapshot says about its engine and ability emitters: the engines online
    // (BP-42), TURBO or SPOILER_LOCK running, POWER_TO_WEAPONS running (#76, AB-31, AB-32).
    struct Modes final {
        bool engines_online{true};
        bool turbo{};
        bool power_to_weapons{};
        bool ion_stunned{};  // IS-09: the stun shows the ion-stun ("pi") emitters
        bool invulnerability{}; // WHE-52: the active Falcon mode shows "pem" emitters
        friend bool operator==(const Modes&, const Modes&) = default;
    };

    using SnapshotAt = std::function<std::shared_ptr<const sim::tactical::TacticalSnapshot>(std::uint64_t)>;
    // R-LIT-01/R-LIT-04: live bump particles use the same scene sun and fill
    // as static map particles, including map hazards admitted to the session.
    void follow_lighting(const GodotRenderer& renderer);
    // #421: a death clone as it is drawn at a presented tick: its model transform and its death
    // clip's pose (bones in the model's order, with their visibility).
    struct ClonePose final {
        sim::math::Mat3x4 model_to_world{};
        std::vector<animation::BonePose> bones;
    };
    // Nullopt when live ship `ship` is no death clone shown at that presented tick.
    using ClonePoseAt = std::function<std::optional<ClonePose>(std::size_t ship, double presented_tick)>;
    // #456: the model transform of model projectile slot `ship` at a presented tick; nullopt
    // when its projectile was not in flight then.
    using ProjectilePoseAt = std::function<std::optional<sim::math::Mat3x4>(std::size_t ship, double presented_tick)>;
    // #535: a unit's fog fade opacity (space-fog-presentation.md FW-16), 1 when it is not fading.
    using FadeOpacity = std::function<float(sim::EntityId entity)>;
    // One presentation frame, after SpacePopulation::pose_live. The emitter clock follows
    // `presented_tick` (one 30 Hz sample per session tick), as the battle effects' does, and
    // starts like theirs at the first frame or at the birth of the oldest tick in `reached`.
    // False (failure() set) when the particle backend failed.
    // `previous`/`latest` are the snapshots the frame interpolates between: the engines' state
    // and the speed of each unit come from them. The samples the frame catches up on before its
    // own read `snapshot_at` as `viewer` sees them, or every unit's when `reveal`
    // (--eawr-live-reveal) is set. The death clones' poses come from `clone_pose_at` and the
    // model projectiles' from `projectile_pose_at` (#456), at each sample's own presented tick;
    // a revealed model projectile or launch-slot death clone draws the same as an owned one.
    [[nodiscard]] bool frame(const SpacePopulation& population, std::span<const platform::LiveTickEvents> reached,
                             const SnapshotAt& snapshot_at, sim::tactical::PlayerId viewer,
                             const sim::tactical::TacticalSnapshot& previous,
                             const sim::tactical::TacticalSnapshot& latest, const ClonePoseAt& clone_pose_at,
                             const ProjectilePoseAt& projectile_pose_at, const FixedCamera& camera,
                             double presented_tick, bool reveal = false, const FadeOpacity& fade_opacity = {},
                             std::span<const sim::EntityId> unfogged_props = {});
    // #638: the pool the particle systems step on (null: the main thread alone); it must outlive
    // this object's frames.
    void set_workers(const particles::StepExecutor* workers) noexcept { registry_->set_executor(workers); }
    [[nodiscard]] core::Result<void> set_particle_detail(const particles::ParticleDetail detail) {
        return registry_->set_detail(detail);
    }
    void measure_preparation(bool enabled) noexcept {
        measure_preparation_ = enabled;
        emitter_prepare_ms_ = clone_prepare_ms_ = 0.0;
    }
    [[nodiscard]] double emitter_prepare_ms() const noexcept { return emitter_prepare_ms_; }
    [[nodiscard]] double clone_prepare_ms() const noexcept { return clone_prepare_ms_; }
    [[nodiscard]] double* clone_prepare_timer() noexcept { return measure_preparation_ ? &clone_prepare_ms_ : nullptr; }
    void release();
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    // The report's "unit_emitters" member, followed by ",\n".
    void write_report(std::ostream& output) const;
    // --eawr-perf-trace (#601): the running emitters' particles after the last clock sample.
    [[nodiscard]] std::uint64_t particles() const noexcept { return particles_; }

private:
    bool measure_preparation_{};
    double emitter_prepare_ms_{};
    double clone_prepare_ms_{};
    // One admitted proxy of a ship: its effect and its frame in the ship's model space.
    struct Wanted final {
        std::size_t proxy{};
        std::string proxy_name;
        std::string effect;
        sim::math::Mat3x4 local{};
        std::uint32_t seed{};
        std::size_t capacity{};
        // An EnhancedMesh system (the engine emitters) emits from the mesh on the proxy bone's
        // parent: its geometry and that bone's bind frame in the ship's model space.
        std::optional<particles::MeshBinding> mesh;
        std::optional<sim::math::Mat3x4> mesh_local;
        bool failed{};  // could not start; reported once per plan, not retried
        bool engine{};  // an engine emitter (BP-42): drawn at the ship's engine brightness
        bool ion_stun{};  // an ion-stun ("pi") emitter (IS-09)
        bool power_to_weapons{};  // AB-32: drains when the ability hides it (BP-48)
        bool invulnerability{};
    };
    struct Running final {
        std::size_t proxy{};
        particles::EffectHandle handle{};
        sim::math::Mat3x4 local{};
        std::optional<sim::math::Mat3x4> mesh_local;
        bool engine{};
        bool ion_stun{};  // an ion-stun ("pi") emitter (IS-09)
        bool power_to_weapons{};
        std::uint64_t born{};  // the clock sample it was started for
        bool invulnerability{};
        std::size_t log{};     // its start_log_ row, or start_log_limit
        // An engine emitter an ability swap hid (BP-65): it no longer emits, its residual
        // particles are drawn until they are gone, then it is released.
        bool draining{};
        std::uint64_t drain_from{};  // the clock sample it began draining at
    };
    // One started emitter: the unit, the proxy, the session tick whose state started it, the
    // clock sample it was born at, its origin then, its age when a frame first drew it, and how
    // many frames between samples drew it again (#433).
    struct StartRow final {
        sim::EntityId entity{};
        std::string proxy;
        std::uint64_t tick{};
        std::uint64_t born{};
        std::optional<std::array<float, 3>> origin;
        std::optional<std::uint64_t> first_age;
        std::uint64_t presented{};
    };
    static constexpr std::size_t start_log_limit = 256;
    // #421: one proxy of a death clone: the instances its bone's visibility starts and drains.
    struct CloneProxy final {
        std::size_t proxy{};
        std::string name;
        std::size_t bone{};
        std::optional<particles::AttachmentLifecycle> life;
        std::optional<particles::EmitterFrame> last;  // the frame of its last sample
        // An EnhancedMesh system emits from the mesh on the proxy bone's parent, posed by the
        // clip like the proxy: that bone, and the mesh frame of the last sample.
        std::optional<std::size_t> mesh_bone;
        std::optional<particles::MeshFrame> last_mesh;
        // Moves with the proxy until its CPU batch has joined and its owner completes it.
        std::optional<particles::PreparedAttachmentStep> pending{};
    };
    // A clone emitter's start: the unit, the proxy, and the session tick and clock sample of the
    // sample that started it.
    struct CloneStartRow final {
        sim::EntityId entity{};
        std::string proxy;
        std::uint64_t tick{};
        std::uint64_t born{};
    };
    struct Ship final {
        sim::EntityId entity{};
        struct AttachedModel final {
            std::size_t hardpoint{};
            std::size_t proxy_base{};
            scene::Placement placement;
            sim::math::Mat3x4 local{};
        };
        bool attachments_planned{};
        std::vector<AttachedModel> attachments;
        bool planned{};
        std::vector<scene::HardpointState> states;  // the states the plan was made for
        Modes modes;                                // and the engines' and abilities' modes
        // BP-45: FoC's engine emitter brightness from the ship's last step (1 until it has one).
        float engine_brightness{1.0F};
        std::vector<Wanted> wanted;
        std::vector<Running> running;
        // #421: a death clone's proxies, planned at its first shown sample.
        bool clone_planned{};
        std::vector<CloneProxy> clone;
    };
    struct ModelFrames final {
        std::vector<std::optional<sim::math::Mat3x4>> bones;
        bool bind_pose{};
    };
    struct EffectSystem final {
        std::optional<particles::SystemDefinition> system;
        std::size_t capacity{};
        std::string cause;
    };

    [[nodiscard]] const assets::Texture* resolve_texture(std::string_view name);
    [[nodiscard]] const EffectSystem& effect_system(const std::string& path);
    [[nodiscard]] const ModelFrames* model_frames(const scene::Placement& placement, const assets::Model*& model);
    void plan(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
              std::span<const scene::HardpointState> states, Modes modes);
    void plan_model(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
                    const scene::Placement& placement, const sim::math::Mat3x4& attachment,
                    std::size_t proxy_base, std::span<const scene::HardpointAttachment> hardpoints,
                    std::span<const scene::HardpointState> states, Modes modes);
    void stop_all(Ship& ship, const char* reason);
    using Emitting = std::map<sim::EntityId, std::map<std::string, std::uint64_t>>;
    [[nodiscard]] Emitting emitting_units() const;
    // Replans a ship whose hardpoint states or engines changed, stops what the plan no longer
    // admits and starts what it newly admits, born at clock sample `born` for session tick `tick`.
    void update(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
                std::span<const scene::HardpointState> states, Modes modes, std::uint64_t tick,
                std::uint64_t born);
    // Stands every running emitter of ship `index` at its bone on model transform `pose`.
    void place(std::size_t index, const sim::math::Mat3x4& pose);
    // Advances every running emitter by one 30 Hz sample.
    [[nodiscard]] bool advance_sample();
    // #433: draws every running emitter again at the frame last set, without advancing.
    [[nodiscard]] bool present_running();
    // #421: a death clone's proxies, from the model its placement draws.
    void plan_clone(Ship& ship, const SpacePopulation::LiveShipEmitterView& view);
    // One sample of death clone `index` at `pose` (nullopt: not shown, so what runs leaves),
    // for session tick `tick` at clock sample `born`.
    void step_clone(std::size_t index, const SpacePopulation::LiveShipEmitterView& view,
                    const std::optional<ClonePose>& pose, std::uint64_t tick, std::uint64_t born);
    // A clone's proxies that are still live go on draining where they last stood.
    void orphan_clone(Ship& ship);
    // Prepares a clone sample; advance_sample batches and completes it in owner order.
    bool step_clone_proxy(CloneProxy& proxy, bool visible, const particles::EmitterFrame& frame,
                          const particles::MeshFrame* mesh, sim::EntityId entity, std::uint64_t tick,
                          std::uint64_t born);

    godot::Node3D* host_;
    const vfs::Vfs* filesystem_;
    scene::VfsAssetCache cache_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    // #638: the handles of one batched advance or present and their statistics, reused.
    std::vector<particles::EffectHandle> batch_handles_;
    std::vector<float> batch_deltas_;
    std::vector<particles::EffectFrameStats> batch_stats_;
    std::map<std::string, ModelFrames> frames_;
    std::map<std::string, EffectSystem> systems_;
    std::vector<Ship> ships_;
    particles::CameraFrame camera_frame_{};
    std::optional<double> clock_start_;
    std::uint64_t samples_{};
    // Report counters.
    std::uint64_t frames_count_{};
    std::uint64_t max_running_{};
    std::uint64_t plans_{};
    std::map<std::string, std::uint64_t> started_;        // proxy -> effect starts
    Emitting emitting_at_release_;
    std::map<std::string, std::uint64_t> stopped_;        // reason -> effect stops
    std::map<std::string, std::uint64_t> not_admitted_;   // proxy: cause -> ships
    std::map<std::string, std::uint64_t> start_failed_;   // effect: cause -> count
    std::uint64_t max_particles_{};
    std::uint64_t particles_{};
    std::uint64_t caught_up_{};        // samples run at their own tick before a frame's own
    std::uint64_t unknown_samples_{};  // of those, samples whose snapshots had left the history
    std::uint64_t presented_frames_{};   // frames between samples that drew a running emitter (#433)
    std::uint64_t presented_effects_{};  // running emitters drawn on those frames
    std::vector<StartRow> start_log_;
    // #421: the death clones' emitters.
    std::vector<CloneProxy> clone_orphans_;
    std::uint64_t clone_ships_{};
    std::map<std::string, std::uint64_t> clone_started_;  // proxy -> instances started
    std::map<std::string, std::uint64_t> clone_hidden_;   // proxy -> hides (stopped, draining)
    std::map<std::string, std::uint64_t> clone_not_run_;  // proxy: cause -> clones
    std::map<std::string, std::uint64_t> clone_failed_;   // proxy: cause -> count
    std::uint64_t engine_drains_started_{};
    std::uint64_t engine_drains_finished_{};
    std::uint64_t engine_drains_cut_short_{};
    // IS-09: ion-stun emitters the stun's end hid, drained like the engines' (BP-65).
    std::uint64_t ion_stun_drains_started_{};
    std::uint64_t ion_stun_drains_finished_{};
    std::uint64_t ion_stun_drains_cut_short_{};
    std::uint64_t ion_stun_max_particles_{};
    std::uint64_t ion_stun_dropped_at_capacity_{};
    std::uint64_t power_to_weapons_drains_started_{};
    std::uint64_t power_to_weapons_drains_finished_{};
    std::uint64_t power_to_weapons_drains_cut_short_{};
    std::uint64_t invulnerability_drains_started_{};
    std::uint64_t invulnerability_drains_finished_{};
    std::uint64_t invulnerability_drains_cut_short_{};
    std::uint64_t clone_drains_released_{};
    std::uint64_t clone_drains_cut_short_{};
    std::uint64_t clone_drains_reset_{};
    std::uint64_t clone_particles_now_{};
    std::uint64_t clone_max_particles_{};
    std::uint64_t clone_dropped_at_capacity_{};
    std::uint64_t clone_max_live_{};
    std::vector<CloneStartRow> clone_log_;
    // BP-45: FoC's engine emitter brightness, 0.2 + 0.8 x speed / maximum speed, per unit (last
    // frame), as the engine emitters draw it.
    std::map<sim::EntityId, double> engine_brightness_;
    std::string failure_;
    bool released_{};
};

} // namespace eawr::presentation::godot_backend
