#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/units/unit_tables.hpp"
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
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// Breakoff props of the live battle (#391, docs/behaviour/battle-presentation.md BP-30 to
// BP-36): when the session destroys a hardpoint whose XML names a Death_Breakoff_Prop, FoC
// spawns that SpaceProp (SpaceBehavior DEBRIS) at the hardpoint's attachment point with the
// ship's facing. The prop drifts and tumbles by its Debris_* vectors every logical frame,
// carries its Debris_Attached_Particle (a fire) and, after a lifetime of whole seconds, dies
// with its Death_Explosions and is removed. Ours draws each prop as a placed ship set up at
// prepare, shown from its hardpoint's destruction event (presentation::space debris rule), with
// its fire and its explosion on the presentation clock (30 Hz samples) like the battle effects.
// A pure consumer of the session's events and snapshots: the session's hashes are unchanged.
class DebrisProps final {
public:
    DebrisProps(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog);
    ~DebrisProps();
    DebrisProps(const DebrisProps&) = delete;
    DebrisProps& operator=(const DebrisProps&) = delete;

    // Appends one placed ship per breakoff hardpoint of each session unit (the first `units`
    // entries of `placed_ships`) to `placed_ships`. What does not resolve is reported, never
    // guessed.
    void prepare(const units::UnitTables& tables, const sim::tactical::CombatTable& combat,
                 std::vector<SpacePopulation::Options::PlacedShip>& placed_ships, std::size_t units);

    using SnapshotAt = std::function<std::shared_ptr<const sim::tactical::TacticalSnapshot>(std::uint64_t)>;
    // One frame, from LiveSessionView::frame before it poses the population: the hardpoint
    // deaths of the ticks newly reached spawn their props, oldest tick first (a prop spawns
    // where the local player saw its ship at the event's tick, from that tick's snapshot; a
    // ship the player did not see then, or whose snapshot has left the history, throws none),
    // each live prop is appended to `live` at `presented_tick`, and a prop whose lifetime has
    // run out leaves and queues its explosion, in tick order with the spawns. Its ship is only
    // hidden (not listed), so a station hardpoint that is repaired and destroyed again throws
    // it once more. The first call starts the effect clock. `reveal` (--eawr-live-reveal) throws
    // props for a hidden ship's hardpoint deaths too.
    void pose(std::span<const platform::LiveTickEvents> reached, const SnapshotAt& snapshot_at,
              sim::tactical::PlayerId viewer, double presented_tick, std::vector<SpacePopulation::LivePose>& live,
              bool reveal = false);
    // The props' fires and explosions, after pose(), with the view this frame renders. False
    // (failure() set) when the particle backend failed.
    [[nodiscard]] bool effects(const FixedCamera& camera, double presented_tick);
    // #638: the pool the particle systems step on (null: the main thread alone); it must outlive
    // this object's frames.
    void set_workers(const particles::StepExecutor* workers) noexcept { registry_->set_executor(workers); }
    void release();
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    // The report's "breakoff_props" member, followed by ",\n".
    void write_report(std::ostream& output) const;

private:
    // One breakoff hardpoint of one unit and the placed ship that draws its prop.
    struct Prop final {
        sim::EntityId unit{};
        std::uint32_t hardpoint{};
        std::size_t ship{};
        std::string type;
        space::DebrisMotion motion{};
        std::array<double, 3> attachment{};  // unit frame
        std::string fire;                    // Debris_Attached_Particle
        std::string explosion;               // Death_Explosions (the first entry)
    };
    struct ParticleType final {
        std::uint32_t lifetime_frames{};
        std::optional<particles::SystemDefinition> system;
        std::string cause;
    };
    // A running particle effect; `follows` is the serial of the active prop it rides on.
    struct Effect final {
        particles::EffectHandle handle{};
        std::string particle;
        std::uint64_t born{};
        std::uint32_t age{};
        std::uint32_t lifetime{};
        bool detached{};
        std::optional<std::uint64_t> follows;
    };
    struct PendingExplosion final {
        std::string particle;
        double birth_tick{};  // presented tick
        space::DebrisPose pose{};
    };

    [[nodiscard]] const assets::Texture* resolve_texture(std::string_view name);
    [[nodiscard]] const ParticleType* particle_type(const std::string& name);
    [[nodiscard]] static particles::EmitterFrame frame_of(const space::DebrisPose& pose);
    [[nodiscard]] space::DebrisPose pose_at(const space::DebrisFlight& flight, double presented_tick) const;
    // A flight whose lifetime ran out: its fire goes and its explosion is queued.
    void retire(const space::DebrisFlights::Ended& ended);
    // False when the particle backend failed (failure() set). An effect that has already
    // ended by sample `due` is not started.
    [[nodiscard]] bool start(const std::string& particle, const space::DebrisPose& pose, std::uint64_t born,
                             std::uint64_t due, std::optional<std::uint64_t> follows, const std::string& reason);
    [[nodiscard]] bool step(Effect& effect, std::uint64_t sample, bool& gone);
    // The frame a fire that follows its flight stands at in sample `sample`; false (failure() set)
    // when it was refused.
    [[nodiscard]] bool follow(const Effect& effect, std::uint64_t sample);
    // What follows an effect's advance: its age, detach and release; `gone` when it was released.
    void after_step(Effect& effect, const particles::EffectFrameStats& advanced, bool& gone);
    [[nodiscard]] bool advance_until(std::uint64_t target);

    godot::Node3D* host_;
    const vfs::Vfs* filesystem_;
    const data::Catalog* catalog_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    // #638: the handles of one batched advance or present and their statistics, reused.
    std::vector<particles::EffectHandle> batch_handles_;
    std::vector<particles::EffectFrameStats> batch_stats_;
    std::map<std::string, ParticleType> particle_types_;
    std::vector<Prop> props_;
    std::map<std::pair<sim::EntityId, std::uint32_t>, std::size_t> prop_of_;
    space::DebrisFlights flights_;
    std::uint64_t fired_through_{};  // the newest flight serial whose fire started
    std::vector<PendingExplosion> pending_;
    std::vector<Effect> effects_;
    std::optional<double> clock_start_;
    std::uint64_t samples_{};
    particles::CameraFrame camera_frame_{};
    std::uint32_t seed_{1};
    // Report.
    std::vector<std::string> prepared_rows_;
    std::vector<std::string> spawn_rows_;
    std::vector<std::string> expired_rows_;
    std::uint64_t not_seen_{};
    std::uint64_t unknown_{};  // events whose tick had left the snapshot history
    std::uint64_t busy_{};
    std::uint64_t max_live_{};
    std::map<std::string, std::uint64_t> started_;       // reason:particle -> count
    std::map<std::string, std::uint64_t> start_failed_;  // reason:particle -> count
    std::map<std::string, std::uint64_t> skipped_;       // reason:particle -> ended before a frame reached it
    std::string failure_;
    bool released_{};
};

} // namespace eawr::presentation::godot_backend
