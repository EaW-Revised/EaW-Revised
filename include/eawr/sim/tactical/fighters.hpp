#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/math/trig.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

// Fighter squadrons (P2-12, #75; docs/behaviour/space-fighters.md): launch and replenishment
// from carriers and stations (FL rules), the retail fighter locomotor (FM rules), attack runs
// (FA rules) and the squadron's target (FT rules). Everything here is a pure function of Q24
// values and keyed draws: no clock, thread or host input.
namespace eawr::sim::tactical {

// Spin_Away_On_Death (#447, docs/behaviour/space-fighter-deaths.md SP-01): the chance that a
// killed craft spins away before it explodes, and for how long.
struct SpinAwayProfile {
    math::Fixed chance{}; // Spin_Away_On_Death_Chance, in [0, 1]
    math::Fixed time{};   // Spin_Away_On_Death_Time, seconds
    friend constexpr bool operator==(const SpinAwayProfile&, const SpinAwayProfile&) noexcept = default;
};

// One craft type's flight (FM-01): speeds per frame, turn, lift and roll in degrees per frame.
// Max speed, min speed, turn, lift and roll include Object_Max_Speed_Multiplier_Space; thrust
// (the speed change per frame) and the strafe distance do not.
struct CraftProfile {
    TypeId type_id{};
    math::Fixed max_speed{};
    math::Fixed min_speed{};
    math::Fixed rate_of_turn{};
    math::Fixed lift{};      // Max_Lift: pitch change per frame
    math::Fixed thrust{};    // Max_Thrust
    math::Fixed roll_rate{}; // Max_Rate_Of_Roll
    math::Fixed bank_angle{};
    math::Fixed strafe_distance{};
    math::Fixed layer_z{};   // Layer_Z_Adjust: the height a squadron settles at
    std::optional<SpinAwayProfile> spin_away; // a type with Spin_Away_On_Death (SP-01)
    // Out_Of_Combat_Defense_Adjustment: out of combat a hit does 1 - this times its damage (DG-26).
    math::Fixed out_of_combat_defense{};
    // #457 (FD-05, FD-06): Minimum_Follow_Distance and Targeting_Max_Attack_Distance, not scaled.
    math::Fixed follow_distance{};
    math::Fixed attack_distance{};
    friend constexpr bool operator==(const CraftProfile&, const CraftProfile&) noexcept = default;
};

// One squadron type (Squadron_Units, Squadron_Offsets): its craft in member order, each with its
// formation offset, and the ranges its formation diverts to attack within (FT-02).
struct SquadronProfile {
    TypeId type_id{};                    // also the type of its team container
    std::vector<TypeId> members;
    std::vector<math::Vec3> offsets;     // one per member; zero where the data has none
    math::Fixed guard_chase_range{};     // Guard_Chase_Range
    math::Fixed idle_chase_range{};      // Idle_Chase_Range
    math::Fixed attack_move_response_range{}; // Attack_Move_Response_Range (#452, FO-05)
    math::Fixed formation_tolerance{};   // Squadron_Formation_Error_Tolerance
    friend bool operator==(const SquadronProfile&, const SquadronProfile&) = default;
};

// A reserve of this size never runs out (FL-02).
inline constexpr std::int32_t unlimited_reserve = -1;

struct SpawnEntryProfile {
    TypeId squadron{};
    std::int32_t starting{}; // Starting_Spawned_Units_Tech_0 count: the most alive at once
    std::int32_t reserve{};  // Reserve_Spawned_Units_Tech_0 count, or unlimited_reserve
    friend constexpr bool operator==(const SpawnEntryProfile&, const SpawnEntryProfile&) noexcept = default;
};

// A HARD_POINT_FIGHTER_BAY hardpoint (FL-05): its attachment bone in model space (scaled, with
// FoC's model turn) and the bone's X axis times Fighter_Bay_Flyout_Distance.
struct BayProfile {
    std::uint32_t hardpoint{}; // index in the spawner's HardPoints list
    math::Vec3 position{};
    math::Vec3 spawn_vector{};
    friend constexpr bool operator==(const BayProfile&, const BayProfile&) noexcept = default;
};

// A SPAWN_SQUADRON type (FL-01 to FL-09).
struct SpawnerProfile {
    TypeId type_id{};
    std::vector<SpawnEntryProfile> entries; // Starting_Spawned_Units_Tech_0 order
    std::uint32_t delay_frames{};           // trunc(Spawned_Squadron_Delay_Seconds x 30)
    std::vector<BayProfile> bays;           // HardPoints order
    bool mobile{};                          // it has a locomotor: its squadrons escort it (FL-07)
    friend bool operator==(const SpawnerProfile&, const SpawnerProfile&) = default;
};

// The #75 content of a MotionTable. Empty: nothing launches and craft never move, as before #75.
struct SquadronTable {
    std::vector<CraftProfile> craft;        // strictly increasing type_id
    std::vector<SquadronProfile> squadrons; // strictly increasing type_id
    std::vector<SpawnerProfile> spawners;   // strictly increasing type_id
    // FO-11 (#599): gameconstants.xml FormationMinimumSideError and FormationMaximumSideError, the
    // side error a squadron of a group move ignores and the one its lane steer divides by. A
    // maximum of zero (a table without the constants) turns the lane steer off.
    math::Fixed side_error_min{};
    math::Fixed side_error_max{};
    [[nodiscard]] const CraftProfile* find_craft(TypeId type_id) const noexcept;
    [[nodiscard]] const SquadronProfile* find_squadron(TypeId type_id) const noexcept;
    [[nodiscard]] const SpawnerProfile* find_spawner(TypeId type_id) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return craft.empty() && squadrons.empty() && spawners.empty(); }
    friend bool operator==(const SquadronTable&, const SquadronTable&) = default;
};

// The longest Spin_Away_On_Death_Time a table may give (FoC's M2 craft give 2 s).
inline constexpr std::int64_t max_spin_seconds = 60;

// Fails with EAWR-SIM-0305 unless type IDs strictly increase in each list, speeds, rates and
// distances and the side errors are in [0, max_motion_rate] (max speed, turn and thrust positive), a spin-away
// chance is in [0, 1] and its time in [0, max_spin_seconds], a squadron lists
// 1 to 64 members with one offset each, a spawner has at most 64 entries and 255 bays, counts are
// nonnegative (a reserve may be unlimited_reserve), finite starting plus reserve fits in int32,
// and points lie within max_motion_coordinate.
[[nodiscard]] core::Result<void> validate_squadron_table(const SquadronTable& table);

// --- State ---------------------------------------------------------------------------------------

// The retail facing of a craft (FM-02): roll (X), pitch (Y, positive lowers the nose) and yaw
// (Z) in degrees, its velocity per frame, and whether it is flipping (FM-06). Hashed state.
struct CraftState {
    math::Fixed roll{};
    math::Fixed pitch{};
    math::Fixed yaw{};
    math::Vec3 velocity{};
    bool flipping{};
    // #457: the craft it chases (FD-05) and the frame its chase timer runs out (FD-06); zero for
    // none and for a timer that has run out.
    EntityId chase{};
    std::uint64_t chase_until{};
    // FD-10: frames the craft still flies straight on after steering away from a ship.
    std::uint32_t relaxation{};
    friend constexpr bool operator==(const CraftState&, const CraftState&) noexcept = default;
};

enum class SquadronMode : std::uint8_t {
    idle = 0,   // hold `anchor`
    escort = 1, // guard `escorted` (FL-07); holds its last position once that unit is gone
    move = 2,   // FO-01 (#424): a player move to `anchor` from `move_origin`, then idle there
};

// The player order a squadron's diversion range comes from (#452, FO-05, FO-06): FoC's escort
// and attack-on-path movement types. Independent of the flight mode above.
enum class SquadronDiversion : std::uint8_t {
    idle = 0,        // Idle_Chase_Range, or Guard_Chase_Range while escorting (FT-02)
    guard = 1,       // a guard: Guard_Chase_Range
    attack_move = 2, // an attack-move: Attack_Move_Response_Range, scanning on the move too
};

// A cell of the fighter combat grid (#457, FD-02; foc-battle-world-ui WU-25): 400-unit cells from
// the world origin, every odd row shifted by half a cell along x.
struct CombatCell {
    std::int32_t x{};
    std::int32_t y{};
    friend constexpr bool operator==(const CombatCell&, const CombatCell&) noexcept = default;
};

inline constexpr std::int64_t combat_cell_size = 400;

// WU-25: the cell a point lies in, and a cell's point (its centre, odd rows half a cell further
// along x) at height `z`.
[[nodiscard]] CombatCell combat_cell_of(const math::Vec3& point) noexcept;
[[nodiscard]] math::Vec3 combat_cell_point(CombatCell cell, math::Fixed z) noexcept;
// FD-03: whether `point` lies within 400 / sqrt(2) of the cell point in the plane.
[[nodiscard]] bool within_combat_cell_reach(const math::Vec3& point, CombatCell cell) noexcept;

// FO-09, FO-10 (#599): a squadron's place in the formation of a group move. The formation flies
// one straight path from `origin` along `direction` (a unit vector in the plane); the squadron's
// slot lies `ahead` along it and `aside` to its left of the formation's centre line. `formation`
// is the team container of the squadron that started the formation (FO-07).
struct SquadronLane {
    EntityId formation{};
    math::Vec3 origin{};
    math::Vec3 direction{};
    math::Fixed ahead{};
    math::Fixed aside{};
    friend constexpr bool operator==(const SquadronLane&, const SquadronLane&) noexcept = default;
};

// FM-23 (#687, debug build; walk 1 WSQ-08): the idle grid, 120-unit cells laid out as the combat
// grid (from the world origin, WU-27), each held by at most one squadron of any side.
inline constexpr std::int64_t idle_cell_size = 120;
// FM-24 (walk 1 WSQ-09): at most this many cells are examined for a free one.
inline constexpr std::uint32_t idle_cell_search_limit = 64;

[[nodiscard]] CombatCell idle_cell_of(const math::Vec3& point) noexcept;
[[nodiscard]] math::Vec3 idle_cell_point(CombatCell cell, math::Fixed z) noexcept;
// FM-24: the idle cell a squadron reverting to idle at `desired` claims when it holds none: the
// cell under `desired` when free, else the free cell nearest `desired` (squared, in the plane) in
// the first ring around it that has one, rings searched row by row from the low corner and the
// first strictly nearest kept. None after `idle_cell_search_limit` cells without a free one.
// `taken` lists the cells other squadrons hold.
[[nodiscard]] std::optional<CombatCell> idle_cell_claim(const math::Vec3& desired, std::span<const CombatCell> taken);

// WMV-17/18: a team can retain a completed position formation before any player order.
// An autonomous split copies completion history and may replace that base position.
struct SquadronFormationState {
    math::Vec3 base_position{};
    EntityId base_target{};
    bool complete{};
    bool has_reached_done{};
    bool attack_override{};
    friend bool operator==(const SquadronFormationState&, const SquadronFormationState&) = default;
};

// A squadron's orders and target (FT rules). Hashed state.
struct SquadronState {
    EntityId container{};
    TypeId squadron_type{};
    EntityId spawner{};        // the unit that launched it; zero for a tick-zero squadron
    std::uint32_t entry{};     // the spawner entry it counts against (FL-08)
    std::vector<EntityId> roster; // launch history; the live roster determines formation slots
    SquadronMode mode{SquadronMode::idle};
    EntityId escorted{};
    math::Vec3 anchor{};       // idle: the point it holds
    EntityId target{};         // the formation's attack target, zero for none
    // #531: the hardpoint of `target` a player attack order named (attack_hull for none); hashed
    // only when set, so sessions without a hardpoint order keep their hashes.
    std::uint32_t target_hardpoint{attack_hull};
    std::uint64_t next_scan_frame{};
    // FO-01: where the move started; hashed only in move mode, so sessions without a squadron
    // move keep their hashes.
    math::Vec3 move_origin{};
    bool approach{};           // FA-07: still on the approach to `target`; ends once within strafe reach
    // FO-05, FO-06 (#452): hashed only when not idle, so sessions without a squadron attack-move
    // or guard keep their hashes.
    SquadronDiversion diversion{SquadronDiversion::idle};
    // #457 (FD-01 to FD-03, WU-25): the combat cell the squadron records, and whether it joined it.
    std::optional<CombatCell> cell{};
    bool joined{};
    // #687 (FM-23 to FM-26): the idle cell it holds, if any; its point is then `anchor`.
    std::optional<CombatCell> idle_cell{};
    // WSQ-10/WSQ-17, FO-10: the move's current straight segment and formation slot;
    // a squadron moved alone has zero slot offsets. Hashed only while set.
    std::optional<SquadronLane> lane{};
    std::optional<SquadronFormationState> formation{}; // WMV-17/18, independent of FA-07
    friend bool operator==(const SquadronState&, const SquadronState&) = default;
};

struct SpawnEntryState {
    std::int32_t alive{};     // squadrons of this entry alive now
    std::int32_t remaining{}; // squadrons it may still launch; unlimited_reserve for no limit
    friend constexpr bool operator==(const SpawnEntryState&, const SpawnEntryState&) noexcept = default;
};

// A spawner's hangar (FL-01): hashed state.
struct SpawnerState {
    std::uint64_t next_service_frame{}; // the SPAWN_SQUADRON behaviour's next service
    std::uint64_t next_spawn_frame{};
    bool ready{};                       // the first service built the entries (FL-02)
    std::vector<SpawnEntryState> entries;
    friend bool operator==(const SpawnerState&, const SpawnerState&) = default;
};

// The first service of a spawner entering the session at `frame` (FL-01): the frame plus a
// keyed draw of 0 to 29.
[[nodiscard]] SpawnerState initial_spawner(std::uint64_t seed, std::uint64_t frame, EntityId unit);

// What a service decided: the entry and bay of a squadron to launch, if any.
struct SpawnDecision {
    std::uint32_t entry{};
    std::uint32_t bay{}; // index in SpawnerProfile::bays
};

// One service of a spawner at `frame` (FL-02 to FL-06). `bay_intact` says, per bay, whether its
// hardpoint still stands. Updates the state; returns the launch, if any. Frames that are not the
// service frame change nothing.
[[nodiscard]] std::optional<SpawnDecision> service_spawner(const SpawnerProfile& profile, SpawnerState& state,
    std::uint64_t seed, std::uint64_t frame, EntityId unit, const std::vector<bool>& bay_intact,
    bool suspended = false);

// A launched squadron of `entry` left the session at `frame` (FL-08).
void squadron_lost(const SpawnerProfile& profile, SpawnerState& state, std::uint32_t entry, std::uint64_t frame);

// --- The fighter locomotor (FM, FA rules) --------------------------------------------------------

// Where a squadron member is and flies, as the craft phase reads it.
struct CraftView {
    EntityId id{};
    math::Vec3 position{};
    CraftState state{};
    const CraftProfile* profile{};
    math::TrigCache* trig_cache{}; // scratch owned by this craft
};

// FD-06 broad phase (#893): rows arrive in container/roster order after timer expiry.
// Rows with running chase timers cannot be chosen and are excluded. Spatial buckets
// only reject positions outside the attack-distance box; queries restore row order.
// The views must outlive the index and stay fixed while workers query it.
struct ChaseCandidate {
    CombatCell joined_cell{};
    TeamId team{};
    const CraftView* craft{};
};

// One phase partition owns these buffers. Clear between scanners, retain capacity
// between ticks; the returned span is valid until that partition's next query.
struct ChaseQueryScratch {
    std::vector<std::size_t> rows;
    std::vector<const CraftView*> result;
};

class ChaseCandidateIndex final {
public:
    explicit ChaseCandidateIndex(std::vector<ChaseCandidate> candidates);
    [[nodiscard]] std::span<const CraftView* const> query(const CraftView& self, CombatCell joined_cell,
        TeamId team, ChaseQueryScratch& scratch) const;

private:
    struct Cell {
        std::vector<std::size_t> rows;
        std::map<std::array<std::int64_t, 2>, std::vector<std::size_t>> positions;
    };
    std::vector<ChaseCandidate> candidates_;
    std::map<std::array<std::int32_t, 2>, Cell> cells_;
};

// FD-10: a ship or static object fighters steer around (a unit with a space layer), as a sphere
// of its soft radius (project: FoC tests the hull's collision shape).
struct CraftObstacle {
    EntityId id{};
    math::Vec3 position{};
    math::Fixed radius{};
};

// How a craft flies a dogfight this frame (#457, FD-01 to FD-07).
enum class DogfightFlight : std::uint8_t {
    none = 0,      // no dogfight: FA rules
    find_cell = 1, // FD-02: the target squadron records no cell yet
    cell = 2,      // FD-03 to FD-07: fight over `cell`
};

// FO-10 (#599): how a squadron of a group move flies this frame: along `direction` (a unit vector
// in the plane), its leader aiming `shift` units to the left of its look-ahead point, every craft
// forming up at `speed`.
struct LaneFlight {
    math::Vec3 direction{};
    math::Fixed shift{};
    math::Fixed speed{};
    bool individual_speed{}; // one member: use each craft's current maximum (AB-24)
};

// What one craft's frame reads (copied before the phase, never written by it).
struct CraftFrame {
    const CraftView* self{};
    const CraftView* leader{};        // the squadron's first live member (FM-10)
    math::Vec3 own_offset{};          // formation offsets of the craft and the leader
    math::Vec3 leader_offset{};
    math::Fixed formation_tolerance{};
    // The squadron's order: attack `target_position` (a unit of `target_radius`, `target_craft`
    // when it is a fighter), or hold `hold` (FM-20, FM-21).
    bool attacking{};
    math::Vec3 target_position{};
    math::Fixed target_radius{};
    bool target_craft{};
    bool approach{};                  // FA-07: the squadron still flies its approach in formation
    math::Vec3 hold{};
    // AB-24: the craft's speed multiplier from its active abilities (1 without).
    math::Fixed speed_factor{math::Fixed::from_raw(math::Fixed::scale)};
    // FO-01: a player move toward `hold` (at full speed, in formation) instead of holding it.
    bool moving{};
    // WSQ-17, FO-10: the path direction, leader's sideways lane steer and the speed
    // every craft forms up at, including on a move alone.
    std::optional<LaneFlight> lane{};
    // #457: the squadron's dogfight, its combat cell, and the craft this one chases while its
    // chase timer runs (FD-05), null for none.
    DogfightFlight dogfight{DogfightFlight::none};
    CombatCell cell{};
    const CraftView* chase{};
    // FD-10: the ships to steer around, ascending ID.
    std::span<const CraftObstacle> obstacles{};
    math::TrigCache* trig_cache{}; // optional; the locomotor owns a local cache otherwise
};

struct CraftStep {
    math::Vec3 position{};
    CraftState state{};
    math::Quat rotation{};
    math::Fixed defense{}; // DG-26: the craft's defense adjustment this frame (out of combat), else 0
    bool closing{};        // FA-01: it closes on its target beyond the strafe reach (presentation, WU-25)
};

// The most an out-of-combat adjustment may lower a craft's defense: hits do at most 1 + this times
// their damage (FoC authors -1 on every craft that sets it; damage.cpp's Q24 bound assumes 1).
inline constexpr std::int64_t max_out_of_combat_defense = 1;

// FD-05: whether `self` can follow a craft at `target`: within its attack distance and within 90
// degrees of yaw and of pitch of its nose. Sets the yaw and pitch toward it (FM-02).
[[nodiscard]] core::Result<bool> in_follow_cone(const CraftView& self, const math::Vec3& target, math::Fixed& yaw,
    math::Fixed& pitch);

// FD-06: a chase lasts ten seconds.
inline constexpr std::uint64_t chase_frames = 300;

// One frame of a craft's locomotor (FM-01 to FM-23, FA-01 to FA-06, FD-01 to FD-10).
[[nodiscard]] core::Result<CraftStep> step_craft(const CraftFrame& frame);

// FO-02 (#424): whether a squadron moving from `origin` to `destination` has arrived: its leader
// stands on or past the line through the destination square to the move in the plane (FoC's
// path-end half plane). Exact in raw Q24 units; a move of zero length has arrived.
[[nodiscard]] bool squadron_move_arrived(const math::Vec3& origin, const math::Vec3& destination,
    const math::Vec3& leader) noexcept;

// A squadron of a group move (#552, FO-07): its team container's position, its formation radius
// (FO-08: its farthest `Squadron_Offsets` slot plus its craft's soft radius), its craft's
// `Max_Speed` and `Min_Speed`, and its type with its craft's `Targeting_Max_Attack_Distance`
// (FO-09's row order).
struct GroupSquadron {
    math::Vec3 position{};
    math::Fixed radius{};
    math::Fixed max_speed{};
    math::Fixed min_speed{};
    TypeId type{};
    math::Fixed attack_distance{};
};

// Where one squadron of a group move flies: its slot, and its lane when it shares a formation
// with others (`formation` then holds the input index of the squadron that started it).
struct GroupSlot {
    math::Vec3 point{};
    std::optional<SquadronLane> lane{};
};

// FO-07 to FO-09 (#552, #599): where each squadron of one move to `destination` flies: squadrons
// moved together share a formation and take its rows' slots around the destination; a squadron
// alone flies to the destination. One slot per squadron, in input order, at the destination's
// height.
[[nodiscard]] core::Result<std::vector<GroupSlot>> squadron_group_slots(
    std::span<const GroupSquadron> squadrons, const math::Vec3& destination);

// A squadron of one formation on its move this frame (FO-10, FO-11): its team container's
// position, its lane, its craft's `Max_Speed` and `Min_Speed`.
struct LaneMember {
    math::Vec3 position{};
    SquadronLane lane{};
    math::Fixed max_speed{};
    math::Fixed min_speed{};
};

// FO-10, FO-11 (#599): how each squadron of one formation flies this frame: its leader's sideways
// steer toward its lane and the speed that keeps it level with its row, from every member's
// place along the path. One result per member, in input order.
[[nodiscard]] core::Result<std::vector<LaneFlight>> formation_lane_flight(std::span<const LaneMember> members,
    math::Fixed side_error_min, math::Fixed side_error_max);

// The facing of a unit launched along `direction` at `speed` (FL-06): yaw and pitch of the
// direction, no roll, velocity direction x speed.
[[nodiscard]] core::Result<CraftState> launch_state(math::Vec3 direction, math::Fixed speed);
// The rotation of a craft facing (FM-02).
[[nodiscard]] core::Result<math::Quat> craft_rotation(const CraftState& state);

// --- Spin-away deaths (#447, docs/behaviour/space-fighter-deaths.md SP rules) ---------------------

// A killed craft spinning away (SP-03 to SP-08). The craft itself left the session when it was
// killed; this is the dead copy that flies on until it explodes. Hashed state.
struct DeathSpin {
    EntityId unit{};           // the killed craft's ID
    TypeId type{};
    PlayerId owner{};          // the killed craft's owner (for visibility only)
    math::Vec3 position{};
    math::Fixed roll{};        // facing in degrees (FM-02)
    math::Fixed pitch{};
    math::Fixed yaw{};
    math::Vec3 velocity{};     // per frame, taken over from the craft (SP-03)
    math::Fixed spin_roll{};   // the accumulated roll; zero until a path is built (SP-04)
    bool path{};               // a path was built (a craft at rest builds none)
    std::array<math::Vec3, 4> points{}; // the path's control points (SP-05)
    std::array<math::Fixed, 3> lengths{}; // each segment's length
    math::Fixed travelled{};   // distance along the path
    friend bool operator==(const DeathSpin&, const DeathSpin&) = default;
};

// Whether a craft of `profile` killed at `frame` spins away (SP-02): one keyed draw in [0, 1]
// at most the chance.
[[nodiscard]] bool spins_away(const SpinAwayProfile& profile, std::uint64_t seed, std::uint64_t frame, EntityId unit) noexcept;

// The spin of a craft killed at `position` with flight `state` (SP-03): its pose and velocity,
// no path yet.
[[nodiscard]] DeathSpin start_spin(EntityId unit, TypeId type, PlayerId owner, math::Vec3 position, const CraftState& state);

// One service of a spin at `frame` (SP-04 to SP-08) for a craft type of `rate_of_turn` degrees
// per frame and `profile`. True when the spin ended this frame: the craft explodes where it is
// and leaves.
[[nodiscard]] core::Result<bool> step_spin(DeathSpin& spin, math::Fixed rate_of_turn, const SpinAwayProfile& profile,
    std::uint64_t seed, std::uint64_t frame);

// The rotation of a spin's facing (FM-02).
[[nodiscard]] core::Result<math::Quat> spin_rotation(const DeathSpin& spin);

} // namespace eawr::sim::tactical
