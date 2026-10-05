#pragma once

#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Presentation of a live tactical session's projectiles beyond the laser batches (#456,
// docs/behaviour/battle-presentation.md BP-60 to BP-65): where a model projectile is drawn and
// how it faces, which projectile a hit event ended, and the presentation draw that picks one of
// a target type's hit particles. It reads immutable snapshots and events only and never writes
// sim state; no draw here touches the simulation's random streams.
namespace eawr::presentation::space {

// BP-68: the unit camera-plane axis whose perspective projection follows the flight
// tangent at `centre`. Eye, centre and direction use the same coordinate basis;
// `forward` is a unit view direction. Nullopt at/behind the eye or for radial/zero motion.
[[nodiscard]] std::optional<std::array<double, 3>> projectile_screen_axis(
    const std::array<double, 3>& eye, const std::array<double, 3>& forward,
    const std::array<double, 3>& centre, const std::array<double, 3>& direction) noexcept;

// A model projectile as drawn: source-basis position and its facing, yaw in degrees in [0, 360)
// (0 along source +X, counter-clockwise about +Z) and pitch in degrees, positive downward (the
// facing triple's y; R-ROT-01 draws it as Rz(yaw) Ry(pitch)).
struct ProjectilePose {
    std::array<double, 3> position{};
    double yaw_degrees{};
    double pitch_degrees{};
};

// BP-61: a projectile's facing. A homing projectile carries its own (MS-02 to MS-05); any other
// flies along its step, so its facing is the step's heading and negated elevation. Nullopt for
// a non-homing projectile with no step.
[[nodiscard]] std::optional<std::array<double, 2>> projectile_facing(const sim::tactical::Projectile& projectile) noexcept;

// BP-61: the pose `alpha` (clamped to [0, 1]) of the way from `before` (the same projectile a
// tick earlier, or null when it is new) to `latest`: the position straight between, the yaw the
// short way round, the pitch between. Nullopt when `latest` has no facing.
[[nodiscard]] std::optional<ProjectilePose> interpolate_projectile(const sim::tactical::Projectile* before,
                                                                  const sim::tactical::Projectile& latest,
                                                                  double alpha) noexcept;

// BP-64: the projectile a projectile_hit event ended, among the projectiles in flight at the end
// of the tick before the hit (ascending ID): the one from the event's shooter and weapon whose
// position is the event's origin (the flight's start that tick). Nullopt when none matches (a
// projectile launched and spent within one tick, or a tick that left the snapshot history).
[[nodiscard]] std::optional<std::uint64_t> hit_projectile(std::span<const sim::tactical::Projectile> before,
                                                          const sim::tactical::CombatEvent& event) noexcept;

// BP-64: a stand-in key for a hit whose projectile is unknown: a mix of the event's tick,
// shooter, weapon, target and origin. The same event always gives the same key.
[[nodiscard]] std::uint64_t hit_event_key(const sim::tactical::CombatEvent& event) noexcept;

// BP-63, BP-64: which of a target type's hit particle lists a draw is for.
enum class HitParticleList : std::uint8_t { damage = 1, shield = 2 };

// BP-64: the presentation's uniform draw of one entry among `count` (at least 1) for the hit of
// projectile `key`: a fixed mix of the key and the list, so every run and every viewer shows the
// same entry for the same projectile. Never the simulation's random streams.
[[nodiscard]] std::size_t hit_particle_pick(std::uint64_t key, HitParticleList list, std::size_t count) noexcept;

// BP-62: a fixed pool of model slots for the projectiles of one type in flight. A projectile
// keeps its slot for its whole flight; a slot its projectile left rests for one bind() (one
// frame) before another takes it, so whatever ran on it (its particle proxies) stops before the
// slot is shown again elsewhere. A projectile that finds no free slot is refused and counted.
class ProjectileModelSlots final {
public:
    explicit ProjectileModelSlots(std::size_t count);

    // Binds the projectiles in flight this frame (`ids` ascending, no repeats) and returns, per
    // entry, its slot or nullopt when the pool is full. A projectile bound before keeps its
    // slot; a new one takes the lowest free slot that is not resting.
    [[nodiscard]] std::vector<std::optional<std::size_t>> bind(std::span<const std::uint64_t> ids);
    // The projectile slot `slot` holds now.
    [[nodiscard]] std::optional<std::uint64_t> bound(std::size_t slot) const noexcept;
    // The projectile slot `slot` held just before the most recent bind() call (nullopt before any
    // bind() has run). bind() runs once per presented frame, so this is the binding every
    // historical catch-up tick that frame samples actually saw: a slot's current occupant (from
    // this frame's own bind()) only takes effect from this frame's own presented tick on.
    [[nodiscard]] std::optional<std::uint64_t> bound_before(std::size_t slot) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }
    [[nodiscard]] std::uint64_t bindings() const noexcept { return bindings_; }
    [[nodiscard]] std::size_t max_bound() const noexcept { return max_bound_; }

private:
    struct Slot final {
        std::optional<std::uint64_t> projectile;
        bool resting{};
    };
    std::vector<Slot> slots_;
    // Each slot's `projectile` as of just before the most recent bind() call (bound_before()).
    std::vector<std::optional<std::uint64_t>> before_bind_;
    std::uint64_t refused_{};
    std::uint64_t bindings_{};
    std::size_t max_bound_{};
};

} // namespace eawr::presentation::space
