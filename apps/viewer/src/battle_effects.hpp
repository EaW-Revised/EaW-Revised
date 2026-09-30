#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_session.hpp"
#include "particle_adapter.hpp"
#include "space_populate.hpp"

#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/space/projectiles.hpp"
#include "eawr/presentation/space/shield_hits.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

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
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// The live battle's presentation (#80, docs/behaviour/battle-presentation.md): what FoC draws for
// the authoritative tactical session's shots, hits, shield hits and deaths. It only reads the
// published snapshots (TacticalSnapshot projectiles, combat events and destruction events) and
// never feeds anything back: the session's hashes are the same with or without it.
//
// - Projectiles in flight are drawn by their type's Projectile_Custom_Render (BP-01): laser kites
//   (p_particle_master.tga, Projectile_Laser_Color) and laser beams (W_Laser_Pill.tga), batched
//   per texture as additive quads/triangles through the particle backend (PrimAdditive), with
//   FoC's camera-facing geometry (BP-02 to BP-09).
// - Projectiles whose type draws its Space_Model_Name (BP-01; the concussion missile) are drawn
//   as that model (#456, BP-60 to BP-62): each flies on a placed ship of a fixed pool per
//   projectile type, set up before the population is composed, posed along the projectile's
//   facing every frame, and its model's particle proxies run through UnitEmitters like a unit's.
// - A projectile hit spawns Projectile_Absorbed_By_Shields_Particle when the shield took all of
//   it, else the armor-reduced or the plain detonation particle (BP-10, BP-11), at the contact
//   point the hit event carries, also for the lethal hit whose target left the session that tick.
//   A shield hit moves to where the projectile's flight, cast from its frame-step start, first
//   meets the target's collision meshes (the SHIELD mesh among them) and faces that triangle's
//   normal when the model has a SHIELD sub-object; without one it faces back along the flight
//   (BP-17 to BP-19).
// - A hit also spawns one entry of the target type's Damage_Hit_Particles (a shield hit: of its
//   Shield_Hit_Particles), drawn by the presentation from the projectile's ID (BP-63, BP-64).
// - A destroyed unit spawns its Death_Explosions particle, a destroyed hardpoint its
//   Death_Explosion_Particles.
// Particle effects run on the presentation clock (30 Hz samples) and detach after their
// Particle_Lifetime_Frames, draining their particles. An event of tick t is born at presented
// tick t - 1 (the frame starts moving towards t), so when a frame reaches several ticks each
// effect is aged only by the samples after its own tick. The clock starts at the first frame,
// or at the oldest event that frame reaches when the session ran ahead of it (#370 re-review 2);
// an effect whose lifetime is already over by the frame that reaches its tick is not spawned.
class BattleEffects final {
public:
    // Where a unit the local player sees stands this frame: the session's pose interpolated for
    // the frame (source units; facing yaw in degrees about +Z, forward +X).
    struct UnitFrame final {
        std::array<double, 3> position{};
        double yaw_degrees{};
        sim::tactical::TypeId type{};
        double roll_degrees{};  // bank about the forward axis (#351)
        double pitch_degrees{};  // a squadron craft's pitch (#506); zero for other units
    };
    using UnitLookup = std::function<std::optional<UnitFrame>(sim::EntityId)>;

    BattleEffects(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog);
    ~BattleEffects();
    BattleEffects(const BattleEffects&) = delete;
    BattleEffects& operator=(const BattleEffects&) = delete;

    // Resolves each unit type's weapon slots to a projectile look and its death explosion from
    // the tables and the XML catalog. Never fails the view: what does not resolve is reported.
    void prepare(const units::UnitTables& tables, const sim::tactical::CombatTable& combat);

    // #456 BP-62: appends `model_slots` placed ships for each projectile type that draws a model
    // (after prepare()), before the population is composed.
    static constexpr std::size_t model_slots = 32;
    void plan_projectile_models(std::vector<SpacePopulation::Options::PlacedShip>& placed_ships);
    // #456 BP-60 to BP-62: the model projectiles in flight this frame, from LiveSessionView::frame
    // before it poses the population: each one the local player sees (BP-09) keeps or takes a
    // slot and is appended to `live` at its interpolated pose.
    void pose_projectile_models(const sim::tactical::TacticalSnapshot& previous,
                                const sim::tactical::TacticalSnapshot& latest, double alpha, const UnitLookup& units,
                                std::vector<SpacePopulation::LivePose>& live);
    using SnapshotAt = std::function<std::shared_ptr<const sim::tactical::TacticalSnapshot>(std::uint64_t)>;
    // The pose of the projectile placed ship `ship` holds at presented tick `tick`, from the
    // snapshots of that tick (for UnitEmitters' catch-up samples); nullopt when `ship` is no
    // model slot, holds nothing, or its projectile was not in flight then.
    [[nodiscard]] std::optional<SpacePopulation::LivePose> projectile_model_pose_at(
        std::size_t ship, const SnapshotAt& snapshot_at, double tick);

    // One presentation frame. `reached` holds the events of the ticks newly reached since the
    // last frame (they fire now, oldest first); `previous`/`latest` and `alpha` place the
    // projectiles in flight; `units` finds the units the local player sees. `camera` is the view
    // this frame renders. The effect clock follows `presented_tick` (one 30 Hz sample per tick),
    // so effects keep battle time in a paced capture. False (failure() set) when the particle
    // backend failed.
    // `snapshot_at` finds the projectile a hit ended (BP-64).
    [[nodiscard]] bool frame(std::span<const platform::LiveTickEvents> reached,
        const sim::tactical::TacticalSnapshot& previous, const sim::tactical::TacticalSnapshot& latest, double alpha,
        const UnitLookup& units, const FixedCamera& camera, double presented_tick, const SnapshotAt& snapshot_at);
    void release();
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    // The report's "battle_effects" member, followed by ",\n".
    void write_report(std::ostream& output) const;
    // --eawr-perf-trace (#601): the effects alive and their particles after the last clock sample.
    [[nodiscard]] std::size_t live_effects() const noexcept { return effects_.size(); }
    [[nodiscard]] std::uint64_t particles() const noexcept { return particles_; }

private:
    enum class Render : std::uint8_t { none, model, beam, kite };
    struct ProjectileLook final {
        std::string projectile;
        Render render{Render::none};
        float width{};
        float length{};
        std::array<float, 2> slot{};
        std::array<float, 4> colour{1.0F, 1.0F, 1.0F, 1.0F};
        std::string detonation;        // Projectile_Object_Detonation_Particle
        std::string armor_reduced;     // Projectile_Object_Armor_Reduced_Detonation_Particle
        std::string shield_absorbed;   // Projectile_Absorbed_By_Shields_Particle
        double step_length{};          // the shot's Max_Speed: its frame step (0: unknown)
    };
    struct TypeLooks final {
        std::map<std::uint32_t, ProjectileLook> weapons; // by weapon slot key (HardPoints index or object_weapon)
        std::string death_explosion;                     // Death_Explosions
        std::string spin_explosion;                      // Spin_Away_On_Death_Explosion (#447)
        std::vector<std::string> hardpoint_explosions;   // Death_Explosion_Particles, HardPoints order
        std::vector<sim::math::Vec3> hardpoint_points;   // combat points, HardPoints order (none: origin)
        // BP-63: Damage_Hit_Particles and Shield_Hit_Particles, in XML order.
        std::vector<std::string> damage_hits;
        std::vector<std::string> shield_hits;
        // Whether the model has a SHIELD sub-object (BP-17); what a projectile can hit (its
        // collidable meshes and the SHIELD mesh, BP-19) in model space (bind pose), built once
        // per type and cast against in model space; the type's Scale_Factor.
        bool shield_mesh{};
        space::ShieldCollisionMesh collision;
        double scale{1.0};
    };
    struct ParticleType final {
        std::string model_path;   // logical ALO path, empty when unresolved
        std::uint32_t lifetime_frames{};
        bool attached_to_collision{};  // Particle_Attach_To_Collision (BP-18)
        std::optional<particles::SystemDefinition> system;
        std::string cause;
    };
    struct LiveEffect final {
        particles::EffectHandle handle{};
        std::string particle;
        std::uint64_t born{};  // the first clock sample that advances it
        std::uint32_t age{};
        std::uint32_t lifetime{};
        bool detached{};
        bool drawn{};
        std::size_t log{};  // its spawn_log_ row, or spawn_log_limit
    };
    // One spawned effect: the tick of its event, reason:particle, its age when a frame first
    // drew it (none: gone before one did).
    struct SpawnRow final {
        std::uint64_t tick{};
        std::string key;
        std::optional<std::uint32_t> first_age;
    };
    struct Batch final {
        std::uint64_t resource{};
        particles::VertexStream stream;
    };

    [[nodiscard]] const ParticleType* particle_type(const std::string& name);
    [[nodiscard]] const assets::Texture* resolve_texture(std::string_view name);
    // False when the particle backend failed (failure() set).
    [[nodiscard]] bool spawn(const std::string& particle, const std::array<double, 3>& position,
                             const particles::Basis3& basis, const std::string& reason, std::uint64_t tick);
    [[nodiscard]] bool draw_projectiles(const sim::tactical::TacticalSnapshot& previous,
        const sim::tactical::TacticalSnapshot& latest, double alpha, const UnitLookup& units, const FixedCamera& camera);
    // Runs the effect clock up to `target` samples; each effect takes the samples from its birth.
    [[nodiscard]] bool advance_until(std::uint64_t target);
    // One 30 Hz sample of one effect; `gone` when it was released.
    [[nodiscard]] bool step_effect(LiveEffect& effect, bool& gone);
    [[nodiscard]] Batch* batch(Render render);
    // The look of a projectile's weapon: its shooter's type (remembered while it flies, the
    // shooter may have died) and weapon slot. Null when unknown.
    [[nodiscard]] const ProjectileLook* look_of(const sim::tactical::Projectile& projectile) const;
    void note_types(const sim::tactical::TacticalSnapshot& previous, const sim::tactical::TacticalSnapshot& latest);
    // BP-63, BP-64: one entry of `list` for the hit `event` ended, drawn from its projectile's ID.
    [[nodiscard]] const std::string* hit_pick(const std::vector<std::string>& list, space::HitParticleList kind,
                                              const sim::tactical::CombatEvent& event, const SnapshotAt& snapshot_at);

    godot::Node3D* host_;
    const vfs::Vfs* filesystem_;
    const data::Catalog* catalog_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    std::map<sim::tactical::TypeId, TypeLooks> types_;
    std::map<std::string, ParticleType> particle_types_;
    std::vector<LiveEffect> effects_;
    // The last frame of every unit seen, for the explosions of units that left the session.
    std::map<sim::EntityId, UnitFrame> last_seen_;
    // The type of every unit the session has held (shooters of hits and projectiles may have died).
    std::map<sim::EntityId, sim::tactical::TypeId> entity_types_;
    // The shooter type of each projectile in flight (its shooter may die before it lands).
    std::map<std::uint64_t, sim::tactical::TypeId> projectile_shooters_;
    // #456 BP-62: one slot pool per model projectile type, its placed ships, and which pool and
    // slot each of those ships is; the projectiles drawn as models this frame.
    struct ModelPool final {
        space::ProjectileModelSlots slots{model_slots};
        std::vector<std::size_t> ships;
    };
    std::map<std::string, ModelPool> model_pools_;
    std::map<std::size_t, std::pair<std::string, std::size_t>> model_ship_slot_;
    std::vector<std::uint64_t> modelled_now_;  // ascending
    std::uint64_t models_drawn_{};
    // #491: each model projectile's last posed tick (this frame's own draw or a catch-up sample,
    // whichever is later): a regression check that a trail runs to its projectile's true last
    // tick whatever --eawr-live-step paces the session at (BP-62).
    std::map<std::uint64_t, std::uint64_t> model_last_posed_tick_;
    Batch kites_;
    Batch beams_;
    std::optional<double> clock_start_;
    std::uint64_t samples_{};
    std::uint64_t particles_{};  // summed over the effects the last clock sample advanced
    // The clock sample an event of the tick being fired is born at, the sample this frame
    // presents, and the frame's camera.
    std::uint64_t birth_{};
    std::uint64_t due_{};
    particles::CameraFrame camera_frame_{};
    std::uint32_t seed_{1};
    // Report counters.
    std::uint64_t frames_{};
    std::uint64_t projectiles_drawn_{};
    std::uint64_t projectiles_hidden_{};
    std::uint64_t max_kites_{};
    std::uint64_t kites_head_leading_{};   // kite frames whose head vertex is furthest along the flight
    std::uint64_t kites_head_trailing_{};  // kite frames drawn back to front (BP-02)
    std::uint64_t max_beams_{};
    std::map<std::string, std::uint64_t> not_drawn_;     // projectile type -> frames it was in flight undrawn
    std::map<std::string, std::uint64_t> spawned_;       // reason:particle -> count
    std::map<std::string, std::uint64_t> spawn_failed_;  // reason:particle -> count
    std::map<std::string, std::uint64_t> expired_;       // reason:particle -> skipped, lifetime over
    std::map<std::uint64_t, std::uint64_t> hit_events_;   // tick -> projectile hits the local player saw
    // BP-64: hit particle draws keyed by the hit's projectile ID, and by the event (no match).
    std::uint64_t hit_picks_by_projectile_{};
    std::uint64_t hit_picks_by_event_{};
    // Shield hits by the rule that faced them (BP-17): the normal of the collision triangle met,
    // back along the flight because the model has no SHIELD sub-object, or back along the flight
    // because the line met none of the model's collision triangles. Of the casts that met one,
    // those that met it within the projectile's frame step and those that went on past it
    // (BP-19); and what the casts cost.
    std::uint64_t shield_on_mesh_{};
    std::uint64_t shield_no_mesh_{};
    std::uint64_t shield_mesh_missed_{};
    std::uint64_t shield_in_step_{};
    std::uint64_t shield_ahead_{};
    space::ShieldCastStats shield_casts_;
    // The first shield hits as placed: tick, target, the event's contact, the particle's position
    // and its facing direction (source units).
    struct ShieldSample final {
        std::uint64_t tick{};
        sim::EntityId target{};
        std::array<double, 3> contact{};
        std::array<double, 3> placed{};
        space::Vec3d direction{};
    };
    std::vector<ShieldSample> shield_samples_;
    // Shielded unit type -> whether its model has a SHIELD sub-object, its collision triangles.
    std::map<std::string, std::pair<bool, std::size_t>> shield_meshes_;
    std::vector<SpawnRow> spawn_log_;
    std::uint64_t effects_dropped_{};
    std::uint64_t max_live_effects_{};
    std::vector<std::string> unresolved_;
    std::string failure_;
    bool released_{};
};

} // namespace eawr::presentation::godot_backend
