#pragma once

#include "eawr/presentation/space/live_units.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/tactical/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

// Breakoff props (#391, docs/behaviour/battle-presentation.md BP-30 to BP-36): the piece of a
// ship that a destroyed hardpoint throws off when its XML names a Death_Breakoff_Prop. The prop
// is a SpaceProp with SpaceBehavior DEBRIS. It appears at the hardpoint's attachment point with
// its ship's facing, then every logical frame it moves by Debris_Movement_Vector in world axes
// and its facing triple grows by Debris_Facing_Rotate_Vector degrees. After a lifetime of whole
// seconds drawn in [Debris_Min_Lifetime_Seconds, Debris_Max_Lifetime_Seconds] it dies. The
// viewer draws it; the simulation never sees it. Pure functions, no engine types.
namespace eawr::presentation::space {

// A DEBRIS type's motion. The facing triple is FoC's (x roll, y pitch, z yaw) in degrees, as
// the R-ROT-01 transform takes it.
struct DebrisMotion {
    std::array<double, 3> movement{};  // Debris_Movement_Vector: source units per logical frame
    std::array<double, 3> rotation{};  // Debris_Facing_Rotate_Vector: degrees per logical frame
    std::int64_t min_lifetime_seconds{};  // Debris_Min_Lifetime_Seconds, whole seconds
    std::int64_t max_lifetime_seconds{};  // Debris_Max_Lifetime_Seconds, whole seconds
};

// A Debris_*_Lifetime_Seconds value as the game keeps it: the authored float cut to whole
// seconds (BP-32). Nullopt when the text is no number.
[[nodiscard]] std::optional<std::int64_t> debris_seconds(std::string_view text);

// Parses a three-number vector such as "0.2, 0.0, -0.5". Nullopt unless it has three finite
// numbers.
[[nodiscard]] std::optional<std::array<double, 3>> debris_vector(std::string_view text);

// How many logical frames a prop lives: whole seconds drawn in [min, max] (both ends
// included; min and max swap when reversed) times `frames_per_second`. Retail draws the
// seconds from the synchronized random generator; the view keys the draw on the event (unit,
// hardpoint, tick) instead, so every run shows the same one. Nullopt when the prop never
// expires: a maximum of zero or less, or a draw below one second.
[[nodiscard]] std::optional<std::uint64_t> debris_lifetime_frames(const DebrisMotion& motion, sim::EntityId unit,
                                                                 std::uint32_t hardpoint, std::uint64_t tick,
                                                                 std::uint32_t frames_per_second);

// Where a prop is and how it faces: source position and the facing triple in degrees, each
// angle wrapped to [0, 360).
struct DebrisPose {
    std::array<double, 3> position{};
    std::array<double, 3> facing_degrees{};  // x roll, y pitch, z yaw
};

// The prop at its spawn: its ship's position and facing (yaw about +Z, bank roll about the
// forward axis, pitch zero as the tactical session has none), with the hardpoint's attachment
// point given in the ship's unit frame (scaled and turned by the +90 model turn, +X forward)
// placed by that facing.
[[nodiscard]] DebrisPose debris_spawn(const std::array<double, 3>& ship_position, double ship_yaw_degrees,
                                      double ship_roll_degrees, const std::array<double, 3>& attachment);

// The prop `frames` logical frames after its spawn (fractional between frames): the spawn
// pose plus `frames` times the motion.
[[nodiscard]] DebrisPose debris_pose(const DebrisPose& spawn, const DebrisMotion& motion, double frames);

// The rest are remake rules of the view (docs/behaviour/battle-presentation.md), not FoC
// behaviour.

// The ship whose hardpoint died at a tick, as the local player `viewer` saw it at that tick:
// from that tick's snapshot, none when the ship was hidden then. With no snapshot (it has left
// the history, #401 review 1) what the player saw at that tick is unknown and the prop is not
// thrown: the least visible choice, never a later frame's visibility. The pose points into
// `snapshot`. `reveal` (--eawr-live-reveal) draws the ship regardless of visibility.
[[nodiscard]] std::optional<LiveUnitPose> debris_ship_at(const sim::tactical::TacticalSnapshot* snapshot,
                                                         sim::EntityId unit, sim::tactical::PlayerId viewer,
                                                         bool reveal = false);

// The presented tick the props' effect clock starts at, on the first frame (#401 review 2):
// that frame's presented tick, or earlier at the birth (tick - 1) of the oldest tick the frame
// reaches, as the battle effects' clock does, so an effect born before the first frame keeps
// its age.
[[nodiscard]] double debris_clock_start(double presented_tick, std::optional<std::uint64_t> oldest_reached_tick);

// Whether a particle object born at clock sample `born` with a lifetime of `lifetime` samples
// has already ended by sample `due`, so a frame reaching its birth late does not start it at
// all (#401 review 2). A lifetime of zero is not authored and never counts as ended.
[[nodiscard]] bool debris_effect_ended(std::uint64_t born, std::uint32_t lifetime, std::uint64_t due);

// A prop in flight: `prop` indexes the caller's props; it is born at presented tick `tick - 1`
// (its hardpoint's destruction tick, as the battle effects count it) at `spawn` and lives
// `lifetime` logical frames (none: never expires).
struct DebrisFlight {
    std::size_t prop{};
    std::uint64_t tick{};
    DebrisPose spawn{};
    std::optional<std::uint64_t> lifetime;
};

// The presented tick a flight dies at, tick - 1 + lifetime; none when it never expires.
[[nodiscard]] std::optional<double> debris_death_tick(const DebrisFlight& flight);

// The props in flight, by serial (launch order). One prop flies at most once at a time. A frame
// after a stall reaches many ticks at once; their events launch oldest first, and each first
// ends the flights that ran out by its birth, so a prop whose earlier flight ended before its
// hardpoint died again is thrown again (#401 review 3).
class DebrisFlights {
public:
    struct Ended {
        std::uint64_t serial{};
        DebrisFlight flight{};
        double death_tick{};  // presented tick
    };

    // Ends every flight that has run out at presented tick `presented_tick`, appending them to
    // `ended` in serial order.
    void expire(double presented_tick, std::vector<Ended>& ended);
    // The breakoff event of `flight.prop` at `flight.tick`: ends the flights that ran out by
    // its birth (appended to `ended`), then launches it unless that prop still flies. The new
    // serial, or none when the prop is busy.
    [[nodiscard]] std::optional<std::uint64_t> launch(const DebrisFlight& flight, std::vector<Ended>& ended);
    [[nodiscard]] const std::map<std::uint64_t, DebrisFlight>& flights() const noexcept { return flights_; }
    void clear() noexcept { flights_.clear(); }

private:
    std::map<std::uint64_t, DebrisFlight> flights_;
    std::uint64_t next_serial_{1};
};

} // namespace eawr::presentation::space
