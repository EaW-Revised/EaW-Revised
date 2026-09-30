#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {
namespace {

struct StableId {
    EntityId value{};
};

struct Identity {
    TypeId type_id{};
    PlayerId owner{};
};

struct Placement {
    math::Vec3 position{};
    math::Quat rotation{};
};

struct CurrentOrder {
    Order value{};
};

// Present only on units whose type has a durability profile.
struct Health {
    DurabilityState value;
};

// Present only on units whose type has a motion profile (#70).
struct Motion {
    MotionState value;
    math::Fixed speed{};
    math::Fixed roll{}; // degrees about the forward axis (#351, BK-01)
};

// Present only on units whose type has a combat profile (#73).
struct Combat {
    CombatState value;
};

// A ship of a group move that plans later (#344, FM-08, FM-10), or whose search runs in slices
// (`sliced`, PC-08, #520): at `frame` it plans towards `destination` with at most `max_speed`
// (a sliced search's plan lands then); until then it keeps its current plan, predicted as
// holding where that plan puts it at `frame`. Waiting ships due in one frame plan in the order
// of their command, then `rank` (the group's planning order).
struct FormationWait {
    std::uint64_t frame{};
    math::Vec3 destination{};
    math::Fixed max_speed{};
    CommandKey order{};
    std::uint32_t rank{};
    bool sliced{};
};

// Present only on waiting group members.
struct Waiting {
    FormationWait value;
};

// Present only on units whose type has abilities (#76).
struct Abilities {
    AbilityState value;
};

// An attack, attack-move or guard order's last approach mapping (#452, OR-05 to OR-07): the
// frame its target prediction is for. Present only after a mapping planned an approach.
struct Approach {
    std::uint64_t prediction_frame{};
};

struct Approaching {
    Approach value;
};

// Present only on ion-stunned units (#561, IS-03), until the stun is serviced away.
struct IonStunned {
    IonStunState value;
};

// A unit as the step stages it: its public state and, when durable, its health; when it has a
// motion profile, its movement, its speed and its roll at the staged tick; when it has a
// combat profile, its targets and fire cycle; when a group move delays its plan, that plan.
struct LiveUnit {
    UnitState state;
    std::optional<DurabilityState> durability;
    std::optional<MotionState> motion;
    math::Fixed speed{};
    math::Fixed roll{};
    std::optional<CombatState> combat;
    std::optional<FormationWait> formation{};
    std::optional<AbilityState> abilities{}; // #76: a type with abilities
    std::optional<Approach> approach{};
    std::optional<IonStunState> ion_stun{}; // #561: while an ion stun runs (IS-03)
};

constexpr std::array<std::uint8_t, 8> state_magic{'E', 'A', 'W', 'R', 'T', 'S', 'T', 0};
// Tags of the optional state blocks that follow the units (#271, #274).
constexpr std::array<std::uint8_t, 4> squadron_tag{'S', 'Q', 'D', 'N'};
constexpr std::array<std::uint8_t, 4> fog_tag{'F', 'O', 'G', 'C'};
// The tracking layers' window anchors (#71); only with avoidance rules.
constexpr std::array<std::uint8_t, 4> tracking_tag{'T', 'R', 'A', 'K'};
constexpr std::array<std::uint8_t, 4> roll_tag{'R', 'O', 'L', 'L'};
// Projectiles in flight (#74); only with damage rules.
constexpr std::array<std::uint8_t, 4> projectile_tag{'P', 'R', 'O', 'J'};
// Group members waiting to plan (#344); only while any waits.
constexpr std::array<std::uint8_t, 4> formation_tag{'F', 'R', 'M', 'N'};
// Ships waiting for a sliced search to land (PC-08, #520); only while any waits.
constexpr std::array<std::uint8_t, 4> sliced_tag{'S', 'L', 'C', 'E'};
// Approach mappings (#452); only while any unit has one.
constexpr std::array<std::uint8_t, 4> approach_tag{'A', 'P', 'P', 'R'};
// Squadron flight, orders and hangars (#75); only with a squadron table that names them.
constexpr std::array<std::uint8_t, 4> craft_tag{'C', 'R', 'F', 'T'};
constexpr std::array<std::uint8_t, 4> squadron_state_tag{'S', 'Q', 'S', 'T'};
constexpr std::array<std::uint8_t, 4> hangar_tag{'H', 'N', 'G', 'R'};
// The target scans' collection trees (#469, space-targeting CO rules), once a unit with a combat
// profile has stepped.
constexpr std::array<std::uint8_t, 4> collection_tag{'C', 'U', 'L', 'L'};
// The decided battle (#77), in the state and the snapshot; only once decided. The block carries
// its own format version so an outcome can change shape without touching undecided hashes.
constexpr std::array<std::uint8_t, 4> victory_tag{'V', 'I', 'C', 'T'};
// Unit abilities (#76); only when a live unit's type has abilities.
constexpr std::array<std::uint8_t, 4> ability_tag{'A', 'B', 'I', 'L'};
// Ion stuns (#561); only while a live unit has one.
constexpr std::array<std::uint8_t, 4> ion_stun_tag{'I', 'O', 'N', 'S'};
constexpr std::uint32_t victory_block_version = 1;
// Killed craft spinning away (#447), in the state and the snapshot; only while any spins.
constexpr std::array<std::uint8_t, 4> spin_tag{'S', 'P', 'I', 'N'};
constexpr std::array<std::uint8_t, 8> snapshot_magic{'E', 'A', 'W', 'R', 'T', 'S', 'N', 0};

[[nodiscard]] std::string command_context(const CommandKey& key) {
    return "command (tick " + std::to_string(key.tick) + ", player " + std::to_string(key.player_id)
        + ", sequence " + std::to_string(key.sequence) + ")";
}

[[nodiscard]] Event destruction_event(
    const std::uint64_t tick, const EventKind kind, const UnitState& unit, const std::uint32_t hardpoint = 0) {
    return Event{
        .tick = tick,
        .kind = kind,
        .player = unit.owner,
        .sequence = 0,
        .unit = unit.entity_id,
        .order = OrderKind::none,
        .reason = RejectReason::none,
        .hardpoint = static_cast<std::uint8_t>(hardpoint),
    };
}

[[nodiscard]] InstanceDurability durability_status(
    const DurabilityProfile& profile, const DurabilityTable& table, const DurabilityState& state) {
    const auto& rules = table.rules;
    InstanceDurability status;
    if (table.damage && profile.max_shields.raw() > 0) {
        status.shields = state.shields;
        status.max_shields = profile.max_shields;
    }
    status.hull = state.hull;
    status.max_hull = profile.max_hull;
    status.max_speed_factor = max_speed_factor(profile, rules, state);
    if (profile.max_speed) {
        // validate_durability bounds the speed and keeps the factor at most 1, so this fits.
        status.max_speed = math::multiply(*profile.max_speed, status.max_speed_factor).value();
    }
    status.engines_online = engines_online(profile, state);
    status.shields_online = shields_online(profile, state);
    status.launch_ready = launch_ready(profile, state);
    status.hardpoints.reserve(profile.hardpoints.size());
    for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
        status.hardpoints.push_back(HardpointStatus{
            profile.hardpoints[index].role,
            hardpoint_state(profile, rules, state, index),
            !hardpoint_destroyed(profile, state, index),
            state.hardpoints[index],
        });
    }
    return status;
}

void append_outcome(std::vector<std::uint8_t>& bytes, const BattleOutcome& outcome) {
    bytes.insert(bytes.end(), victory_tag.begin(), victory_tag.end());
    sim::detail::append_u32(bytes, victory_block_version);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(outcome.condition));
    sim::detail::append_u32(bytes, outcome.winner);
    sim::detail::append_u32(bytes, outcome.winner_team);
    sim::detail::append_u64(bytes, outcome.decided_tick);
    sim::detail::append_u64(bytes, outcome.deciding_unit);
    sim::detail::append_u64(bytes, outcome.end_tick);
}

void append_fixed(std::vector<std::uint8_t>& bytes, const math::Fixed value) {
    sim::detail::append_i64(bytes, value.raw());
}

// floor((low + high) / 2) of two raw values with low <= high, without overflow.
[[nodiscard]] constexpr std::int64_t midpoint(const std::int64_t low, const std::int64_t high) noexcept {
    const auto half = (static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low)) / 2U;
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + half);
}

// A squadron's orders as its craft read them in the craft phase (#75): its leader, what it
// attacks or the point it holds.
struct SquadronFrame {
    const SquadronState* state{};
    const SquadronProfile* profile{};
    EntityId leader{};
    bool attacking{};
    math::Vec3 target_position{};
    math::Fixed target_radius{};
    bool target_craft{};
    bool approach{}; // FA-07
    DogfightFlight dogfight{DogfightFlight::none}; // #457, FD-01 to FD-03
    CombatCell cell{};
    math::Vec3 hold{};
    bool moving{};
    std::optional<LaneFlight> lane{}; // FO-10, FO-11 (#599)
    // FT-02, FO-05, FO-06: whether the squadron scans this frame, and from where (the point it
    // holds, or its leader on an attack-move's way).
    bool scans{};
    bool scan_from_leader{};
};

void append_vec3(std::vector<std::uint8_t>& bytes, const math::Vec3& value) {
    append_fixed(bytes, value.x);
    append_fixed(bytes, value.y);
    append_fixed(bytes, value.z);
}

// Canonical craft record (#75): ID, roll, pitch, yaw, velocity and the flip flag, then (#457) a
// mask of the dogfight state it carries: bit 0 a chase (FD-05, FD-06), bit 1 an avoidance
// relaxation (FD-10). A craft without either hashes as before #457.
void append_craft(std::vector<std::uint8_t>& bytes, const EntityId id, const CraftState& craft) {
    sim::detail::append_u64(bytes, id);
    append_fixed(bytes, craft.roll);
    append_fixed(bytes, craft.pitch);
    append_fixed(bytes, craft.yaw);
    append_vec3(bytes, craft.velocity);
    sim::detail::append_u32(bytes, craft.flipping ? 1U : 0U);
    const bool chasing = craft.chase != invalid_entity_id || craft.chase_until != 0;
    sim::detail::append_u32(bytes, (chasing ? 1U : 0U) | (craft.relaxation != 0 ? 2U : 0U));
    if (chasing) {
        sim::detail::append_u64(bytes, craft.chase);
        sim::detail::append_u64(bytes, craft.chase_until);
    }
    if (craft.relaxation != 0) sim::detail::append_u32(bytes, craft.relaxation);
}

// Canonical spinning craft record (#447).
void append_spin(std::vector<std::uint8_t>& bytes, const DeathSpin& spin) {
    sim::detail::append_u64(bytes, spin.unit);
    sim::detail::append_u64(bytes, spin.type);
    sim::detail::append_u32(bytes, spin.owner);
    sim::detail::append_u32(bytes, spin.path ? 1U : 0U);
    append_vec3(bytes, spin.position);
    append_fixed(bytes, spin.roll);
    append_fixed(bytes, spin.pitch);
    append_fixed(bytes, spin.yaw);
    append_vec3(bytes, spin.velocity);
    append_fixed(bytes, spin.spin_roll);
    for (const auto& point : spin.points) {
        append_vec3(bytes, point);
    }
    for (const auto length : spin.lengths) {
        append_fixed(bytes, length);
    }
    append_fixed(bytes, spin.travelled);
}

// Canonical squadron order record (#75).
void append_squadron_state(std::vector<std::uint8_t>& bytes, const SquadronState& state) {
    sim::detail::append_u64(bytes, state.container);
    sim::detail::append_u64(bytes, state.squadron_type);
    sim::detail::append_u64(bytes, state.spawner);
    sim::detail::append_u32(bytes, state.entry);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.mode));
    sim::detail::append_u64(bytes, state.escorted);
    append_vec3(bytes, state.anchor);
    sim::detail::append_u64(bytes, state.target);
    sim::detail::append_u64(bytes, state.next_scan_frame);
    // Bit 0 the FA-07 approach; #457: bit 1 a recorded combat cell, bit 2 joined (WU-25).
    sim::detail::append_u32(bytes, (state.approach ? 1U : 0U) | (state.cell ? 2U : 0U) | (state.joined ? 4U : 0U));
    if (state.cell) {
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.cell->x));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.cell->y));
    }
    sim::detail::append_u64(bytes, state.roster.size());
    for (const auto member : state.roster) {
        sim::detail::append_u64(bytes, member);
    }
    // FO-01 (#424): only a squadron on a player move carries its origin, so a session without
    // one hashes as before.
    if (state.mode == SquadronMode::move) {
        append_vec3(bytes, state.move_origin);
    }
    // FO-05, FO-06 (#452): only a squadron on a player attack-move or guard carries its diversion.
    if (state.diversion != SquadronDiversion::idle) {
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.diversion));
    }
    // #687 (FM-23): only a squadron holding an idle cell carries it, so a session where none does
    // hashes as before.
    if (state.idle_cell) {
        sim::detail::append_u32(bytes, 0x1d1eU);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.idle_cell->x));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.idle_cell->y));
    }
    // FO-10 (#599): only a squadron on a group move carries its lane.
    if (state.lane) {
        sim::detail::append_u64(bytes, state.lane->formation);
        append_vec3(bytes, state.lane->origin);
        append_vec3(bytes, state.lane->direction);
        append_fixed(bytes, state.lane->ahead);
        append_fixed(bytes, state.lane->aside);
    }
}

// Canonical hangar record (#75).
void append_spawner(std::vector<std::uint8_t>& bytes, const EntityId id, const SpawnerState& state) {
    sim::detail::append_u64(bytes, id);
    sim::detail::append_u64(bytes, state.next_service_frame);
    sim::detail::append_u64(bytes, state.next_spawn_frame);
    sim::detail::append_u32(bytes, state.ready ? 1U : 0U);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.entries.size()));
    for (const auto& entry : state.entries) {
        sim::detail::append_i64(bytes, entry.alive);
        sim::detail::append_i64(bytes, entry.remaining);
    }
}

// A squadron as the squadron phase stages it (#271).
struct SquadronOutcome {
    bool container_live{};
    std::vector<EntityId> members; // live craft, ascending
    math::Vec3 centre{};
    math::Mat3x4 transform{};
    std::optional<core::Diagnostic> error;
};

// FO-01, FO-03 (#424, docs/behaviour/space-fighters.md): a player order given to a squadron's
// team container. A move flies to the destination at the craft's layer height and drops the
// target and any escort; a stop holds where the squadron is; an attack makes the unit its target
// (FT-01 keeps it) and holds where the squadron is once it is gone. A face order changes nothing.
// FO-05, FO-06 (#452): an attack-move or guard of a point flies there as a move, of a unit
// follows it as an escort; either drops the target and sets the ranges the squadron diverts within.
void apply_squadron_order(SquadronState& state, const CommandPayload& payload, const math::Vec3& position,
    const SquadronTable& table) {
    math::Fixed layer_z{};
    if (const auto* squadron = table.find_squadron(state.squadron_type); squadron != nullptr && !squadron->members.empty()) {
        if (const auto* craft = table.find_craft(squadron->members.front())) layer_z = craft->layer_z;
    }
    const math::Vec3 here{position.x, position.y, layer_z};
    state.lane.reset(); // FO-10: a new order leaves the formation
    // FM-25 (#687, walk 1 WSQ-11, WSQ-39): every order but a face gives the idle cell up; a stop
    // holds its point without one until the squadron next reverts to idle.
    if (!std::holds_alternative<FacePayload>(payload)) state.idle_cell.reset();
    const auto fly_to = [&](const math::Vec3& destination) {
        state.mode = SquadronMode::move;
        state.move_origin = here;
        state.anchor = {destination.x, destination.y, layer_z};
        state.escorted = invalid_entity_id;
        state.target = invalid_entity_id;
    };
    const auto follow = [&](const EntityId unit) {
        state.mode = SquadronMode::escort;
        state.move_origin = {};
        state.anchor = here; // the squadron frame moves it to the unit (FL-07)
        state.escorted = unit;
        state.target = invalid_entity_id;
    };
    if (const auto* move = std::get_if<MovePayload>(&payload)) {
        fly_to(move->destination);
        state.diversion = SquadronDiversion::idle;
    } else if (std::holds_alternative<StopPayload>(payload)) {
        state.mode = SquadronMode::idle;
        state.move_origin = {};
        state.anchor = here;
        state.escorted = invalid_entity_id;
        state.target = invalid_entity_id;
        state.diversion = SquadronDiversion::idle;
    } else if (const auto* attack = std::get_if<AttackPayload>(&payload)) {
        state.mode = SquadronMode::idle;
        state.move_origin = {};
        state.anchor = here;
        state.escorted = invalid_entity_id;
        state.target = attack->target;
        // FA-07 (#497): the attack order plans the squadron's move to the target, so it starts
        // the approach even when the target is the one the squadron already had.
        state.approach = attack->target != invalid_entity_id;
        state.diversion = SquadronDiversion::idle;
    } else if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) {
        if (attack_move->target != invalid_entity_id) {
            follow(attack_move->target);
        } else {
            fly_to(attack_move->destination);
        }
        state.approach = false;
        state.diversion = SquadronDiversion::attack_move;
    } else if (const auto* guard = std::get_if<GuardPayload>(&payload)) {
        if (guard->target != invalid_entity_id) {
            follow(guard->target);
        } else {
            fly_to(guard->destination);
        }
        state.approach = false;
        state.diversion = SquadronDiversion::guard;
    }
}

[[nodiscard]] Order order_for(const CommandPayload& payload, const std::uint64_t tick) {
    Order order;
    order.kind = order_kind(payload);
    order.issued_tick = tick;
    if (const auto* move = std::get_if<MovePayload>(&payload)) {
        order.destination = move->destination;
    } else if (const auto* face = std::get_if<FacePayload>(&payload)) {
        order.destination = face->target;
    } else if (const auto* attack = std::get_if<AttackPayload>(&payload)) {
        order.target = attack->target;
    } else if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) {
        // OP-01: an order that names a unit keeps no point.
        order.target = attack_move->target;
        if (order.target == invalid_entity_id) order.destination = attack_move->destination;
    } else if (const auto* guard = std::get_if<GuardPayload>(&payload)) {
        order.target = guard->target;
        if (order.target == invalid_entity_id) order.destination = guard->destination;
    }
    return order;
}

// The point a move-like order sends its units to (#452, space-orders OR-11, OR-16): a move, or an
// attack-move or guard that names no unit.
[[nodiscard]] std::optional<math::Vec3> point_move(const CommandPayload& payload) {
    if (const auto* move = std::get_if<MovePayload>(&payload)) return move->destination;
    if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload);
        attack_move != nullptr && attack_move->target == invalid_entity_id) {
        return attack_move->destination;
    }
    if (const auto* guard = std::get_if<GuardPayload>(&payload); guard != nullptr && guard->target == invalid_entity_id) {
        return guard->destination;
    }
    return std::nullopt;
}

// The unit an order sends its units towards (#452, OR-02, OR-12, OR-14): an attack's target, or
// an attack-move's or guard's unit; zero for every other order.
[[nodiscard]] EntityId approach_target_of(const CommandPayload& payload) {
    if (const auto* attack = std::get_if<AttackPayload>(&payload)) return attack->target;
    if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) return attack_move->target;
    if (const auto* guard = std::get_if<GuardPayload>(&payload)) return guard->target;
    return invalid_entity_id;
}

// Canonical motion record (docs/replay-format.md): kind, zero, start tick, start position, start
// yaw, start speed and target; zero apart from the kind while at rest.
void append_motion(std::vector<std::uint8_t>& bytes, const MotionState& motion) {
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(motion.kind));
    sim::detail::append_u32(bytes, 0);
    sim::detail::append_u64(bytes, motion.start_tick);
    append_fixed(bytes, motion.start_position.x);
    append_fixed(bytes, motion.start_position.y);
    append_fixed(bytes, motion.start_position.z);
    append_fixed(bytes, motion.start_yaw);
    append_fixed(bytes, motion.start_speed);
    append_fixed(bytes, motion.target.x);
    append_fixed(bytes, motion.target.y);
    append_fixed(bytes, motion.target.z);
}

// With the path finder (#71) a plan's nodes depend on the other ships' predictions, so the
// record carries them: node count, then per node frame, position, yaw and speed.
void append_nodes(std::vector<std::uint8_t>& bytes, const MotionState& motion) {
    sim::detail::append_u64(bytes, motion.nodes.size());
    for (const auto& node : motion.nodes) {
        append_fixed(bytes, node.frame);
        append_fixed(bytes, node.position.x);
        append_fixed(bytes, node.position.y);
        append_fixed(bytes, node.position.z);
        append_fixed(bytes, node.yaw);
        append_fixed(bytes, node.speed);
    }
}

// A unit the tracking system holds (AV-01, research E71-12, E71-17): a ship with a motion
// profile in a space layer, or a SPACE_OBSTACLE type.
[[nodiscard]] const Footprint* tracked_footprint(const MotionTable& table, const TypeId type) noexcept {
    const auto* footprint = table.footprint(type);
    if (footprint == nullptr || footprint->layer == SpaceLayer::none) return nullptr;
    if (!footprint->obstacle && table.find(type) == nullptr) return nullptr;
    return footprint;
}

// Per-unit predictions at the window boundaries of the tick's two possible layer starts
// (AV-02, AV-03): `current` from the layer's rolled anchor, `fresh` from this frame (a layer
// rebuilt by a plan this tick). Empty for a unit at rest.
struct TrackSamples {
    std::vector<Prediction> current;
    std::vector<Prediction> fresh;
};

// The tracking state of one step (#71): which dynamic layers a submission rebuilt this
// tick, the samples of every tracked unit and the layer views built from them.
struct Tracking {
    std::uint64_t frame{};
    std::uint32_t interval{};
    std::uint32_t windows{};
    std::array<std::uint64_t, 4> current_start{};
    std::map<EntityId, TrackSamples> samples;
    std::array<std::optional<TrackingLayerView>, 4> views;
    std::optional<std::vector<TrackedLeaf>> statics;
};

// The first frame of the window that holds `frame` when windows roll every `interval` frames
// from `anchor` (the object tracking layer's service step, research E71-13).
[[nodiscard]] constexpr std::uint64_t rolled_start(
    const std::uint64_t anchor, const std::uint64_t frame, const std::uint32_t interval) noexcept {
    return frame <= anchor ? anchor : anchor + (frame - anchor) / interval * interval;
}

// `clip`: a waiting group member's plan frame; later frames predict the unit held where its plan
// puts it then (Clip_Object_Predictions, FM-10, research E344-13).
[[nodiscard]] core::Result<std::vector<Prediction>> sample_windows(const MotionState& motion, const std::uint64_t start,
    const std::uint32_t interval, const std::uint32_t windows, const math::Vec3 position, const math::Fixed yaw,
    const std::optional<std::uint64_t> clip = std::nullopt) {
    std::vector<Prediction> result;
    result.reserve(windows + 1U);
    for (std::uint32_t index = 0; index <= windows; ++index) {
        auto frame = start + std::uint64_t{index} * interval;
        if (clip) frame = std::min(frame, *clip);
        auto at = predict(motion, frame, position, yaw);
        if (!at) return core::Result<std::vector<Prediction>>::failure(at.error());
        result.push_back(at.value());
    }
    return core::Result<std::vector<Prediction>>::success(std::move(result));
}

// PC-09 (#613): a ship whose sliced search has ended before its landing is predicted along
// `current` (at rest when null) up to the `landing` frame and along the search's `plan` from
// then on: what FoC's layer holds for a ship that has planned (PC-02).
[[nodiscard]] core::Result<std::vector<Prediction>> sample_ahead(const MotionState* current, const std::uint64_t landing,
    const MotionState& plan, const std::uint64_t start, const std::uint32_t interval, const std::uint32_t windows,
    const math::Vec3 position, const math::Fixed yaw) {
    std::vector<Prediction> result;
    result.reserve(windows + 1U);
    for (std::uint32_t index = 0; index <= windows; ++index) {
        const auto frame = start + std::uint64_t{index} * interval;
        core::Result<Prediction> at = core::Result<Prediction>::success(Prediction{position, yaw});
        if (frame >= landing) {
            at = predict(plan, frame, position, yaw);
        } else if (current != nullptr) {
            at = predict(*current, frame, position, yaw);
        }
        if (!at) return core::Result<std::vector<Prediction>>::failure(at.error());
        result.push_back(at.value());
    }
    return core::Result<std::vector<Prediction>>::success(std::move(result));
}

// One tracked unit in one window (the tracking tree's leaf initialisation, research
// E71-19): an obstacle is a square of its radius facing +X; a ship is the rectangle of its
// half extents, facing along its motion (or its predicted facing when it holds still), static
// when it does not move in the window.
[[nodiscard]] TrackedLeaf leaf_for(motion_detail::Calc& calc, const EntityId id, const Footprint& footprint,
    const Prediction& from, const Prediction& to) {
    TrackedLeaf leaf;
    leaf.entity = id;
    leaf.start = {from.position.x, from.position.y};
    leaf.end = {to.position.x, to.position.y};
    if (footprint.obstacle) {
        leaf.facing = {math::Fixed::from_raw(math::Fixed::scale), math::Fixed{}};
        leaf.x_extent = footprint.radius;
        leaf.y_extent = footprint.radius;
        leaf.reach = footprint.radius;
    } else {
        leaf.x_extent = footprint.x_extent;
        leaf.y_extent = footprint.y_extent;
        leaf.reach = std::max(footprint.x_extent, footprint.y_extent);
        if (leaf.start != leaf.end) {
            const auto dx = calc.sub(leaf.end.x, leaf.start.x);
            const auto dy = calc.sub(leaf.end.y, leaf.start.y);
            const auto length = calc.length(dx, dy);
            leaf.facing = {calc.div(dx, length), calc.div(dy, length)};
        } else {
            const auto heading = motion_detail::heading(calc, to.yaw);
            leaf.facing = {heading.x, heading.y};
        }
    }
    leaf.collision = leaf.start == leaf.end ? collision_static : collision_moving;
    return leaf;
}

// Tracked units in the dynamic layers, as (layer, ID) in ascending order: a change marks the
// layer for rebuilding like a submission (Remove_Object, research E71-12).
template <typename Units>
[[nodiscard]] std::vector<std::pair<std::size_t, EntityId>> layer_members(const MotionTable& table, const Units& units) {
    std::vector<std::pair<std::size_t, EntityId>> members;
    for (const auto& unit : units) {
        if (const auto* footprint = tracked_footprint(table, unit.state.type_id)) {
            if (const auto index = dynamic_layer_index(footprint->layer)) {
                members.emplace_back(*index, unit.state.entity_id);
            }
        }
    }
    std::sort(members.begin(), members.end());
    return members;
}

// Builds the view of dynamic layer `index` from the staged units and their samples: windows
// from this frame when a submission rebuilt the layer this tick, else from its rolled anchor.
[[nodiscard]] core::Result<void> ensure_view(Tracking& tracking, const MotionTable& table,
    const std::map<EntityId, LiveUnit>& staged, const std::array<bool, 4>& rebuilt, const std::size_t index) {
    if (tracking.views[index]) return core::Result<void>::success();
    motion_detail::Calc calc;
    TrackingLayerView view;
    view.start_frame = rebuilt[index] ? tracking.frame : tracking.current_start[index];
    view.windows.resize(tracking.windows);
    for (const auto& [id, unit] : staged) {
        const auto* footprint = tracked_footprint(table, unit.state.type_id);
        if (footprint == nullptr || dynamic_layer_index(footprint->layer) != index) continue;
        const std::vector<Prediction>* samples = nullptr;
        if (const auto found = tracking.samples.find(id); found != tracking.samples.end()) {
            const bool fresh = rebuilt[index] && !found->second.fresh.empty();
            samples = fresh ? &found->second.fresh : &found->second.current;
            if (samples->empty()) samples = nullptr;
        }
        auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const Prediction still{unit.state.position, yaw.value()};
        for (std::uint32_t window = 0; window < tracking.windows; ++window) {
            const auto& from = samples != nullptr ? (*samples)[window] : still;
            const auto& to = samples != nullptr ? (*samples)[window + 1U] : still;
            view.windows[window].push_back(leaf_for(calc, id, *footprint, from, to));
        }
    }
    if (!calc.ok()) return core::Result<void>::failure(calc.error("tracking layer"));
    tracking.views[index] = std::move(view);
    return core::Result<void>::success();
}

// The static layer's objects (one window; research E71-13, E71-17).
[[nodiscard]] core::Result<void> ensure_statics(
    Tracking& tracking, const MotionTable& table, const std::map<EntityId, LiveUnit>& staged) {
    if (tracking.statics) return core::Result<void>::success();
    motion_detail::Calc calc;
    std::vector<TrackedLeaf> leaves;
    for (const auto& [id, unit] : staged) {
        const auto* footprint = tracked_footprint(table, unit.state.type_id);
        if (footprint == nullptr || footprint->layer != SpaceLayer::static_object) continue;
        auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const Prediction still{unit.state.position, yaw.value()};
        leaves.push_back(leaf_for(calc, id, *footprint, still, still));
    }
    if (!calc.ok()) return core::Result<void>::failure(calc.error("static layer"));
    tracking.statics = std::move(leaves);
    return core::Result<void>::success();
}

// One frame of a unit's movement (MV-30 to MV-33): the unit goes where its plan puts it at
// `tick`, level at the sampled yaw; a finished plan leaves it at rest. Its roll banks with the
// frame's turn while it follows a plan and levels at rest (BK-02 to BK-04).
[[nodiscard]] core::Result<void> advance(LiveUnit& unit, const MotionProfile* profile, const std::uint64_t tick) {
    if (!unit.motion || profile == nullptr) {
        return core::Result<void>::success();
    }
    if (unit.motion->kind == MotionKind::none) {
        unit.speed = math::Fixed{};
        auto roll = level_roll(*profile, unit.roll);
        if (!roll) {
            return core::Result<void>::failure(roll.error());
        }
        unit.roll = roll.value();
        return core::Result<void>::success();
    }
    const auto yaw = yaw_degrees(unit.state.rotation);
    if (!yaw) {
        return core::Result<void>::failure(yaw.error());
    }
    const auto sample = sample_motion(*unit.motion, tick, unit.state.position, yaw.value());
    if (!sample) {
        return core::Result<void>::failure(sample.error());
    }
    unit.state.position = sample.value().position;
    if (sample.value().yaw != yaw.value() || !sample.value().finished) {
        auto rotation = yaw_rotation(sample.value().yaw);
        if (!rotation) {
            return core::Result<void>::failure(rotation.error());
        }
        unit.state.rotation = rotation.value();
    }
    // BK-02: every frame a plan moves the unit banks it; the frame a path ends does not
    // (MV-32), the last frame of a turn in place does unless the turn took no time.
    const auto& nodes = unit.motion->nodes;
    const bool banks = !sample.value().finished
        || (unit.motion->kind == MotionKind::turn && nodes.size() >= 2 && nodes.front().frame < nodes.back().frame);
    if (banks) {
        auto roll = bank_roll(*profile, unit.roll, yaw.value(), sample.value().yaw);
        if (!roll) {
            return core::Result<void>::failure(roll.error());
        }
        unit.roll = roll.value();
    }
    unit.speed = sample.value().speed;
    if (sample.value().finished) {
        unit.motion = MotionState{};
        unit.speed = math::Fixed{};
    }
    return core::Result<void>::success();
}

// The unit's placement transform: its level rotation rolled by its bank (BK-05).
[[nodiscard]] core::Result<math::Mat3x4> unit_transform(const LiveUnit& unit) {
    const auto rotation = banked_rotation(unit.state.rotation, unit.roll);
    if (!rotation) {
        return core::Result<math::Mat3x4>::failure(rotation.error());
    }
    return math::to_matrix(rotation.value(), unit.state.position);
}

} // namespace

TacticalSnapshot::TacticalSnapshot(
    const std::uint64_t completed_tick,
    std::vector<SnapshotPlayer> players,
    std::vector<TacticalInstance> instances,
    std::vector<Event> events,
    std::vector<CombatEvent> combat_events,
    std::vector<Projectile> projectiles,
    std::optional<BattleOutcome> outcome,
    std::vector<SpinningCraft> spinning,
    std::vector<SquadronTarget> squadron_targets,
    std::vector<Squadron> squadrons)
    : completed_tick_(completed_tick), players_(std::move(players)), instances_(std::move(instances)),
      events_(std::move(events)), combat_events_(std::move(combat_events)), projectiles_(std::move(projectiles)),
      outcome_(outcome), spinning_(std::move(spinning)), squadron_targets_(std::move(squadron_targets)),
      squadrons_(std::move(squadrons)) {}

std::uint64_t TacticalSnapshot::completed_tick() const noexcept { return completed_tick_; }
std::span<const SnapshotPlayer> TacticalSnapshot::players() const noexcept { return players_; }
std::span<const TacticalInstance> TacticalSnapshot::instances() const noexcept { return instances_; }
std::span<const Event> TacticalSnapshot::events() const noexcept { return events_; }
std::span<const CombatEvent> TacticalSnapshot::combat_events() const noexcept { return combat_events_; }
std::span<const Projectile> TacticalSnapshot::projectiles() const noexcept { return projectiles_; }
const std::optional<BattleOutcome>& TacticalSnapshot::outcome() const noexcept { return outcome_; }
std::span<const SpinningCraft> TacticalSnapshot::spinning() const noexcept { return spinning_; }
std::span<const SquadronTarget> TacticalSnapshot::squadron_targets() const noexcept { return squadron_targets_; }
std::span<const Squadron> TacticalSnapshot::squadrons() const noexcept { return squadrons_; }

std::vector<EntityId> TacticalSnapshot::visible_entities(const PlayerId player) const {
    std::vector<EntityId> result;
    const auto found = std::find_if(players_.begin(), players_.end(),
        [&](const SnapshotPlayer& entry) { return entry.player_id == player; });
    if (found == players_.end()) {
        return result;
    }
    const auto bit = std::uint64_t{1} << static_cast<unsigned>(found - players_.begin());
    for (const auto& instance : instances_) {
        if ((instance.visible_to & bit) != 0U) {
            result.push_back(instance.entity_id);
        }
    }
    return result;
}

std::vector<std::uint8_t> TacticalSnapshot::canonical_bytes() const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + players_.size() * detail::snapshot_player_record_size
        + instances_.size() * detail::instance_record_size + events_.size() * detail::event_record_size);
    bytes.insert(bytes.end(), snapshot_magic.begin(), snapshot_magic.end());
    sim::detail::append_u32(bytes, tactical_snapshot_encoding_version);
    sim::detail::append_u64(bytes, completed_tick_);
    sim::detail::append_u64(bytes, players_.size());
    for (const auto& player : players_) {
        sim::detail::append_u32(bytes, player.player_id);
        sim::detail::append_u32(bytes, player.team_id);
    }
    sim::detail::append_u64(bytes, instances_.size());
    for (const auto& instance : instances_) {
        sim::detail::append_u64(bytes, instance.entity_id);
        sim::detail::append_u64(bytes, instance.type_id);
        sim::detail::append_u32(bytes, instance.owner);
        sim::detail::append_u32(bytes, instance.team);
        for (const auto& row : instance.fixed_transform.rows) {
            for (const auto value : row) {
                sim::detail::append_i64(bytes, value.raw());
            }
        }
        sim::detail::append_u64(bytes, instance.visible_to);
        sim::detail::append_u32(bytes, instance.reveal_range ? 1U : 0U);
        sim::detail::append_u32(bytes, 0);
        sim::detail::append_i64(bytes, instance.reveal_range ? instance.reveal_range->raw() : 0);
        const auto& durability = instance.durability;
        sim::detail::append_u32(bytes, durability ? 1U : 0U);
        sim::detail::append_u32(bytes, durability ? static_cast<std::uint32_t>(durability->hardpoints.size()) : 0U);
        if (!durability) {
            continue;
        }
        append_fixed(bytes, durability->hull);
        append_fixed(bytes, durability->max_hull);
        append_fixed(bytes, durability->max_speed_factor);
        append_fixed(bytes, durability->max_speed.value_or(math::Fixed{}));
        sim::detail::append_u32(bytes, (durability->engines_online ? 1U : 0U) | (durability->shields_online ? 2U : 0U)
                | (durability->launch_ready ? 4U : 0U) | (durability->max_speed ? 8U : 0U)
                | (durability->shields ? 16U : 0U));
        // A shielded instance of a session with damage rules (#74) appends its shield.
        if (durability->shields) {
            append_fixed(bytes, *durability->shields);
            append_fixed(bytes, durability->max_shields.value_or(math::Fixed{}));
        }
        sim::detail::append_u32(bytes, 0);
        for (const auto& hardpoint : durability->hardpoints) {
            append_fixed(bytes, hardpoint.health);
            bytes.push_back(static_cast<std::uint8_t>(hardpoint.role));
            bytes.push_back(static_cast<std::uint8_t>(hardpoint.state));
            bytes.push_back(hardpoint.enabled ? 1U : 0U);
            bytes.push_back(0);
            sim::detail::append_u32(bytes, 0);
        }
    }
    sim::detail::append_u64(bytes, events_.size());
    for (const auto& event : events_) {
        detail::append_event(bytes, event);
    }
    // A snapshot with combat events (#73) appends them, and one with projectiles in flight (#80)
    // the (possibly empty) combat events and then the projectiles; any other encodes exactly as
    // before.
    if (!combat_events_.empty() || !projectiles_.empty()) {
        sim::detail::append_u64(bytes, combat_events_.size());
        for (const auto& event : combat_events_) {
            detail::append_combat_event(bytes, event);
        }
    }
    if (!projectiles_.empty()) {
        sim::detail::append_u64(bytes, projectiles_.size());
        for (const auto& projectile : projectiles_) {
            detail::append_projectile(bytes, projectile);
        }
    }
    // A decided battle (#77) appends its outcome; an undecided one encodes exactly as before.
    if (outcome_) {
        append_outcome(bytes, *outcome_);
    }
    // Instances with abilities (#76), ascending ID, as a tagged block only when any has them.
    std::uint64_t able = 0;
    for (const auto& instance : instances_) able += instance.abilities.empty() ? 0 : 1;
    if (able != 0) {
        bytes.insert(bytes.end(), ability_tag.begin(), ability_tag.end());
        sim::detail::append_u64(bytes, able);
        for (const auto& instance : instances_) {
            if (instance.abilities.empty()) continue;
            sim::detail::append_u64(bytes, instance.entity_id);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(instance.abilities.size()));
            for (const auto& ability : instance.abilities) {
                bytes.push_back(static_cast<std::uint8_t>(ability.kind));
                bytes.push_back(static_cast<std::uint8_t>((ability.active ? 1U : 0U) | (ability.ready ? 2U : 0U)
                    | (ability.autofire ? 4U : 0U) | (ability.supports_autofire ? 8U : 0U)));
                sim::detail::append_u16(bytes, 0);
                sim::detail::append_u32(bytes, ability.remaining_frames);
                sim::detail::append_u32(bytes, ability.total_frames);
            }
        }
    }
    // Ion-stunned instances (#561), ascending ID, as a tagged block only when any is stunned.
    std::uint64_t stunned = 0;
    for (const auto& instance : instances_) stunned += instance.ion_stun_frames != 0 ? 1 : 0;
    if (stunned != 0) {
        bytes.insert(bytes.end(), ion_stun_tag.begin(), ion_stun_tag.end());
        sim::detail::append_u64(bytes, stunned);
        for (const auto& instance : instances_) {
            if (instance.ion_stun_frames == 0) continue;
            sim::detail::append_u64(bytes, instance.entity_id);
            sim::detail::append_u32(bytes, instance.ion_stun_frames);
        }
    }
    // Craft spinning away (#447) follow as a tagged block, only when there are any.
    if (!spinning_.empty()) {
        bytes.insert(bytes.end(), spin_tag.begin(), spin_tag.end());
        sim::detail::append_u64(bytes, spinning_.size());
        for (const auto& spin : spinning_) {
            sim::detail::append_u64(bytes, spin.entity_id);
            sim::detail::append_u64(bytes, spin.type_id);
            sim::detail::append_u32(bytes, spin.owner);
            sim::detail::append_u32(bytes, 0);
            for (const auto& row : spin.fixed_transform.rows) {
                for (const auto value : row) {
                    sim::detail::append_i64(bytes, value.raw());
                }
            }
            append_fixed(bytes, spin.roll);
            append_fixed(bytes, spin.pitch);
            append_fixed(bytes, spin.yaw);
            sim::detail::append_u64(bytes, spin.visible_to);
        }
    }
    return bytes;
}

std::string TacticalSnapshot::sha256() const {
    return sim::sha256_hex(canonical_bytes());
}

class TacticalSession::Impl final {
public:
    Impl(const TacticalSetup& source, const std::span<const SensorProfile> sensor_table, const DurabilityTable& table,
        const MotionTable& motion_table, const std::optional<FogRules>& fog_rules, const CombatTable& combat_table,
        const VictoryRules& victory_rules, const AbilityTable& ability_table)
        : setup(source), sensors(sensor_table.begin(), sensor_table.end()), durability(table), motion(motion_table),
          combat(combat_table), victory(victory_rules), abilities(ability_table), squadrons(source.squadrons),
          rng_state(source.seed) {
        for (const auto& player : source.players) {
            teams.emplace(player.player_id, player.team_id);
            snapshot_players.push_back(SnapshotPlayer{player.player_id, player.team_id});
        }
        for (const auto& unit : source.units) {
            add_starbase(unit);
        }
        if (source.units.empty()) {
            next_id = 1;
        } else if (source.units.back().entity_id == std::numeric_limits<EntityId>::max()) {
            next_id = invalid_entity_id;
        } else {
            next_id = source.units.back().entity_id + 1;
        }
        std::vector<LiveUnit> live;
        live.reserve(source.units.size());
        for (const auto& unit : source.units) {
            live.push_back(new_unit(unit, 0));
        }
        rebuild(live, false);
        bind_squadrons(live);
        // validate_setup and validate_sensors have already proved that the sensor field
        // builds and that every rotation converts.
        if (fog_rules) {
            // Tick zero marks every revealer's circle; there is no service before the first
            // step. The inline executor runs the same partitions a step does.
            const auto field = SensorField::build(source.players, source.units, sensors).value();
            fog.emplace(*fog_rules, source.players);
            static_cast<void>(fog->advance(0, false, revealers(field, source.units), InlineExecutor{}));
        }
        publish_staged(live, {}, {});
    }

    // A counted star base (#77, VT-03) joins the ascending-ID list the victory evaluation walks.
    // Units never change type or owner, so the list follows spawns, deaths and staged removals.
    void add_starbase(const UnitState& unit) {
        if (victory.condition == VictoryCondition::none
            || !std::binary_search(victory.starbase_types.begin(), victory.starbase_types.end(), unit.type_id)
            || !std::binary_search(victory.contenders.begin(), victory.contenders.end(), unit.owner)) {
            return;
        }
        const StarbaseEntry entry{unit.entity_id, unit.owner};
        starbases.insert(std::upper_bound(starbases.begin(), starbases.end(), entry,
                             [](const StarbaseEntry& left, const StarbaseEntry& right) { return left.unit < right.unit; }),
            entry);
    }

    // A unit entering the session at `frame`: full health, at rest, no target (#72, #70, #73).
    // With damage rules its shield and energy recharge on phases of its own (DG-13, EN-02): the
    // first at `frame` plus a keyed draw in [0, interval - 1], then every interval.
    [[nodiscard]] std::optional<DurabilityState> entering_durability(
        const DurabilityProfile* profile, const EntityId unit, const std::uint64_t frame) const {
        if (profile == nullptr) return std::nullopt;
        auto state = full_durability(*profile);
        if (durability.damage) {
            const auto& rules = *durability.damage;
            CombatRandom shield(setup.seed, frame, unit, shield_phase_slot);
            state.next_shield_frame = frame + shield.uniform(0, rules.shield_recharge_frames - 1U);
            if (has_energy_pool(*profile, rules)) {
                CombatRandom energy(setup.seed, frame, unit, energy_phase_slot);
                state.next_energy_frame = frame + energy.uniform(0, rules.energy_recharge_frames - 1U);
            }
        }
        return state;
    }

    [[nodiscard]] LiveUnit new_unit(const UnitState& unit, const std::uint64_t frame) const {
        const auto* profile = durability.find(unit.type_id);
        const auto* fighting = combat.find(unit.type_id);
        return LiveUnit{unit, entering_durability(profile, unit.entity_id, frame),
            motion.find(unit.type_id) != nullptr ? std::optional(MotionState{}) : std::nullopt, math::Fixed{},
            math::Fixed{},
            fighting != nullptr ? std::optional(initial_combat(*fighting, setup.seed, frame, unit.entity_id))
                                : std::nullopt,
            std::nullopt, entering_abilities(unit.type_id)};
    }

    // #76: a unit whose type has abilities enters with every one off and ready (AB-10).
    [[nodiscard]] std::optional<AbilityState> entering_abilities(const TypeId type) const {
        const auto* profile = abilities.find(type);
        return profile != nullptr ? std::optional(initial_abilities(*profile)) : std::nullopt;
    }

    // AB-14: what the unit's shield says about switching DEFEND on at `tick`.
    [[nodiscard]] AbilityGate ability_gate(const LiveUnit& unit, const std::uint64_t tick) const {
        AbilityGate gate;
        if (!unit.durability) return gate;
        const auto& profile = *durability.find(unit.state.type_id);
        gate.engines_online = engines_online(profile, *unit.durability);
        gate.ion_stunned = ion_stunned(unit.ion_stun, tick);
        if (!durability.damage) return gate;
        gate.shielded = profile.max_shields.raw() > 0;
        gate.shields_online = shields_online(profile, *unit.durability);
        gate.shield_depleted = shield_depleted(*durability.damage, *unit.durability, tick);
        return gate;
    }

    // AB-60 (#561): the unit's ION_CANNON_SHOT slot (the container's team slot, or a craft's own),
    // or null.
    [[nodiscard]] const AbilitySlot* ion_slot(const LiveUnit& unit) const {
        if (!unit.abilities) return nullptr;
        const auto slot = ability_slot(*abilities.find(unit.state.type_id), AbilityKind::ion_cannon_shot);
        return slot ? &unit.abilities->slots[*slot] : nullptr;
    }
    [[nodiscard]] AbilitySlot* ion_slot(LiveUnit& unit) const {
        return const_cast<AbilitySlot*>(ion_slot(static_cast<const LiveUnit&>(unit)));
    }

    // AB-62 (#561): whether an ION_CANNON_SHOT of `owner`'s squadron may lock onto `target`: a
    // live unit of another team that can be hit (a unit with a combat profile; a squadron's team
    // container has no model to aim at, its craft do).
    [[nodiscard]] bool ion_target_valid(const PlayerId owner, const LiveUnit* target) const {
        return target != nullptr && teams.at(target->state.owner) != teams.at(owner)
            && combat.find(target->state.type_id) != nullptr;
    }

    // AB-65 (#561): the squadron's ION_CANNON_SHOT ends: the container and every craft switch it
    // off; the container recharges only when at least one craft fired (a live craft no longer due).
    void finish_ion_shot(AbilitySlot& team, const std::vector<AbilitySlot*>& craft, const TypeId container_type,
        const std::uint64_t tick) const {
        std::size_t due = 0;
        for (auto* slot : craft) {
            due += slot->active ? 1 : 0;
            slot->active = false;
        }
        const auto& profile = *abilities.find(container_type);
        const auto& ability = profile.abilities[*ability_slot(profile, AbilityKind::ion_cannon_shot)];
        team.active = false;
        team.target = invalid_entity_id;
        team.target_hardpoint = no_hardpoint;
        if (due < craft.size()) team.ready_tick = tick + ability.recharge_frames;
    }

    // AB-20: the unit's multiplier of one kind from its active abilities; 1 without any.
    [[nodiscard]] math::Fixed ability_factor(const LiveUnit& unit, const AbilityModifier modifier) const {
        if (!unit.abilities) return math::Fixed::from_raw(math::Fixed::scale);
        return ability_multiplier(*abilities.find(unit.state.type_id), *unit.abilities, modifier);
    }

    // AB-43: DEFEND switched on brings the unit's next shield and energy recharges forward to the
    // next tick, from where they run at DEFEND's intervals.
    void defend_switched_on(LiveUnit& unit, const std::uint64_t tick) const {
        if (!unit.durability || !durability.damage) return;
        unit.durability->next_shield_frame = tick + 1;
        if (has_energy_pool(*durability.find(unit.state.type_id), *durability.damage)) {
            unit.durability->next_energy_frame = tick + 1;
        }
    }

    // AB-42: the hull and shield a hit took from a unit that runs the DEFEND stand-in.
    void track_damage(LiveUnit& unit, const math::Fixed hull_before, const math::Fixed shields_before) const {
        if (!unit.abilities || !unit.durability || !abilities.find(unit.state.type_id)->defend_script) return;
        const auto before = hull_before.raw() + shields_before.raw();
        const auto after = unit.durability->hull.raw() + unit.durability->shields.raw();
        if (before > after) {
            unit.abilities->window_damage = math::Fixed::from_raw(unit.abilities->window_damage.raw() + (before - after));
        }
    }

    // IS-01 to IS-04, IS-07 (#561): an ion shot stuns the unit it hit; the stun ends an active
    // DEFEND (early, AB-12). Speed changes wait for the next tick's ability phase (IS-05, AB-24).
    void ion_stun_unit(LiveUnit& unit, const IonStunShot& shot, const std::uint64_t tick) const {
        unit.ion_stun = ion_stun(unit.ion_stun, shot, tick);
        if (!unit.abilities) return;
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto slot = ability_slot(profile, AbilityKind::defend);
        if (!slot) return;
        const auto switched = deactivate_ability(profile.abilities[*slot], unit.abilities->slots[*slot], tick);
        unit.abilities->replan_due = unit.abilities->replan_due || switched.speed;
    }

    // IS-03: a unit's stun ends at its end frame. IS-05: neither its start nor its end plans a
    // move under way again; a move planned while it lasts keeps its cut speed.
    static void service_ion_stun(LiveUnit& unit, const std::uint64_t tick) {
        if (tick >= unit.ion_stun->end_frame) unit.ion_stun.reset();
    }

    // AB-17: a shield at zero ends DEFEND (early, AB-12); a speed change waits for the next tick's
    // ability phase (AB-24).
    void end_depleted_defend(LiveUnit& unit, const std::uint64_t tick) const {
        if (!unit.abilities || !unit.durability || unit.durability->shields.raw() > 0) return;
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto slot = ability_slot(profile, AbilityKind::defend);
        if (!slot) return;
        const auto switched = deactivate_ability(profile.abilities[*slot], unit.abilities->slots[*slot], tick);
        unit.abilities->replan_due = unit.abilities->replan_due || switched.speed;
    }

    // One unit's ability service at `tick` (the ability phase, AB-11, AB-16, AB-41, AB-42).
    // Returns whether its speed multiplier changed since its move was planned.
    [[nodiscard]] bool service_abilities(LiveUnit& unit, const std::uint64_t tick) const {
        auto& state = *unit.abilities;
        const auto& profile = *abilities.find(unit.state.type_id);
        bool speed = state.replan_due;
        state.replan_due = false;
        speed = expire_abilities(profile, state, tick).speed || speed;
        // AB-16: lost engines end TURBO and SPOILER_LOCK (HD-11).
        if (unit.durability) {
            if (const auto* hull = durability.find(unit.state.type_id); hull != nullptr && !engines_online(*hull, *unit.durability)) {
                for (std::size_t index = 0; index < profile.abilities.size(); ++index) {
                    const auto kind = profile.abilities[index].kind;
                    if (kind != AbilityKind::turbo && kind != AbilityKind::spoiler_lock) continue;
                    speed = deactivate_ability(profile.abilities[index], state.slots[index], tick).speed || speed;
                }
            }
        }
        // AB-41, AB-42: the object script's check runs when the damage-rate window closes.
        if (profile.defend_script && rate_window_closes(tick)) {
            close_rate_window(state);
            const auto slot = ability_slot(profile, AbilityKind::defend);
            const bool armed = slot && (!abilities.human(unit.state.owner) || state.slots[*slot].autofire);
            if (armed && state.damage_rate.raw() > defend_rate_threshold * math::Fixed::scale) {
                const auto switched =
                    activate_ability(profile.abilities[*slot], state.slots[*slot], ability_gate(unit, tick), tick);
                if (switched.changed) defend_switched_on(unit, tick);
                speed = switched.speed || speed;
            }
        }
        return speed;
    }

    // AB-50: the unit's abilities as the snapshot shows them, ready as of `tick`.
    [[nodiscard]] std::vector<AbilityStatus> ability_statuses(const LiveUnit& unit, const std::uint64_t tick) const {
        std::vector<AbilityStatus> result;
        if (!unit.abilities) return result;
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto gate = ability_gate(unit, tick);
        for (std::size_t index = 0; index < profile.abilities.size(); ++index) {
            const auto& ability = profile.abilities[index];
            const auto& slot = unit.abilities->slots[index];
            AbilityStatus status;
            status.kind = ability.kind;
            status.active = slot.active;
            status.ready = ability_ready(ability, slot, gate, tick);
            status.autofire = slot.autofire;
            status.supports_autofire = ability.supports_autofire;
            if (slot.active && slot.expires_tick > tick) {
                status.remaining_frames = static_cast<std::uint32_t>(slot.expires_tick - tick);
                status.total_frames = ability.expiration_frames;
            } else if (!slot.active && slot.ready_tick > tick) {
                status.remaining_frames = static_cast<std::uint32_t>(slot.ready_tick - tick);
                status.total_frames = ability.recharge_frames;
            }
            result.push_back(status);
        }
        return result;
    }

    // #75: the tick-zero squadrons whose type the squadron table knows hold their start position
    // (FM-20) and their craft fly by their craft profiles; every spawner starts its hangar.
    void bind_squadrons(const std::vector<LiveUnit>& live) {
        const auto& table = motion.squadrons;
        if (table.empty()) return;
        const auto unit_at = [&live](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(live.begin(), live.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != live.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        for (const auto& squadron : squadrons) {
            const auto* container = unit_at(squadron.container);
            if (container == nullptr || table.find_squadron(container->state.type_id) == nullptr) continue;
            SquadronState state;
            state.container = squadron.container;
            state.squadron_type = container->state.type_id;
            state.roster = squadron.members;
            state.anchor = container->state.position;
            minds.emplace(squadron.container, std::move(state));
            for (const auto member : squadron.members) {
                const auto* craft = unit_at(member);
                if (craft == nullptr || table.find_craft(craft->state.type_id) == nullptr) continue;
                CraftState flight;
                flight.yaw = yaw_degrees(craft->state.rotation).value();
                crafts.emplace(member, flight);
            }
        }
        for (const auto& unit : live) {
            if (table.find_spawner(unit.state.type_id) != nullptr) {
                spawners.emplace(unit.state.entity_id, initial_spawner(setup.seed, 1, unit.state.entity_id));
            }
        }
    }

    // FL-06, FL-07: one squadron of `decision` leaves `spawner`'s bay at `frame`: its craft at the
    // bay facing along the bay's spawn vector at full speed, then its team container; the squadron
    // escorts a mobile spawner and holds the end of the spawn vector otherwise.
    [[nodiscard]] core::Result<void> launch(const LiveUnit& spawner, const SpawnerProfile& profile,
        const SpawnDecision& decision, const std::uint64_t frame, EntityId& next, std::vector<LiveUnit>& survivors,
        std::vector<TacticalInstance>& instances, std::vector<Squadron>& squadron_list,
        std::map<EntityId, CraftState>& flights, std::map<EntityId, SquadronState>& orders) const {
        using Void = core::Result<void>;
        const auto& table = motion.squadrons;
        const auto* squadron = table.find_squadron(profile.entries[decision.entry].squadron);
        const auto& bay = profile.bays[decision.bay];
        const auto context = "tick " + std::to_string(frame) + " spawner " + std::to_string(spawner.state.entity_id) + ": ";
        const auto transform = math::to_matrix(spawner.state.rotation, spawner.state.position);
        if (!transform) return Void::failure(transform.error());
        const auto position = math::transform_point(transform.value(), bay.position);
        auto direction = math::transform_vector(transform.value(), bay.spawn_vector);
        if (!position || !direction) {
            return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, context + "bay point overflows"));
        }
        if (direction.value() == math::Vec3{}) {
            direction = math::transform_vector(transform.value(), {math::Fixed::from_raw(math::Fixed::scale), {}, {}});
        }
        const auto add = [&](const UnitState& state) -> Void {
            const auto placed = math::to_matrix(state.rotation, state.position);
            if (!placed) return Void::failure(placed.error());
            auto unit = new_unit(state, frame);
            instances.push_back(instance_for(unit, placed.value(), frame));
            survivors.push_back(std::move(unit));
            next = state.entity_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : state.entity_id + 1;
            return Void::success();
        };
        std::vector<EntityId> members;
        math::Fixed layer_z{};
        for (const auto type : squadron->members) {
            if (next == invalid_entity_id) {
                return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit, context + "stable ID space exhausted"));
            }
            const auto* craft = table.find_craft(type);
            auto flight = launch_state(direction.value(), craft->max_speed);
            if (!flight) return Void::failure(flight.error());
            auto rotation = craft_rotation(flight.value());
            if (!rotation) return Void::failure(rotation.error());
            if (members.empty()) layer_z = craft->layer_z;
            // space-movement LZ-01: a craft is created at the bay raised by its own Layer_Z_Adjust;
            // the team container is not (LZ-02).
            auto at = position.value();
            const auto raised = math::add(at.z, craft->layer_z);
            if (!raised) return Void::failure(raised.error());
            at.z = raised.value();
            const auto id = next;
            if (auto added = add(UnitState{id, type, spawner.state.owner, at, rotation.value(), {}}); !added) {
                return added;
            }
            flights[id] = flight.value();
            members.push_back(id);
        }
        if (next == invalid_entity_id) {
            return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit, context + "stable ID space exhausted"));
        }
        const auto container = next;
        if (auto added = add(UnitState{container, squadron->type_id, spawner.state.owner, position.value(),
                math::identity_quat(), {}}); !added) {
            return added;
        }
        squadron_list.push_back(Squadron{container, members});
        SquadronState order;
        order.container = container;
        order.squadron_type = squadron->type_id;
        order.spawner = spawner.state.entity_id;
        order.entry = decision.entry;
        order.roster = members;
        order.next_scan_frame = frame + logical_frames_per_second; // FT-06: a launched squadron idles first
        if (profile.mobile) {
            order.mode = SquadronMode::escort;
            order.escorted = spawner.state.entity_id;
            order.anchor = {spawner.state.position.x, spawner.state.position.y, layer_z};
        } else {
            const auto& at = position.value();
            const auto& along = direction.value();
            const auto x = math::add(at.x, along.x);
            const auto y = math::add(at.y, along.y);
            if (!x || !y) {
                return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, context + "bay point overflows"));
            }
            order.anchor = {x.value(), y.value(), layer_z};
        }
        orders.emplace(container, std::move(order));
        return Void::success();
    }

    // Publishes the snapshot of the staged units at the completed tick, with the given events.
    void publish_staged(const std::vector<LiveUnit>& live, std::vector<Event> events,
        std::vector<CombatEvent> combat_events) {
        std::vector<UnitState> states;
        states.reserve(live.size());
        for (const auto& unit : live) {
            states.push_back(unit.state);
        }
        const auto field = SensorField::build(setup.players, states, sensors).value();
        std::vector<TacticalInstance> instances;
        instances.reserve(live.size());
        for (const auto& unit : live) {
            auto instance = instance_for(unit, math::to_matrix(unit.state.rotation, unit.state.position).value(), completed_tick);
            instance.visible_to = visible_to(field, fog ? &*fog : nullptr, unit.state.owner, unit.state.position);
            instance.reveal_range = field.reveal_range(unit.state.type_id);
            instances.push_back(std::move(instance));
        }
        current_snapshot = std::make_shared<const TacticalSnapshot>(completed_tick, snapshot_players,
            std::move(instances), std::move(events), std::move(combat_events), projectiles, outcome,
            std::vector<SpinningCraft>{}, std::vector<SquadronTarget>{}, squadrons);
    }

    // Live units with a sensor profile, in the order of `units` (ascending ID).
    [[nodiscard]] static std::vector<FogRevealer> revealers(const SensorField& field, const std::span<const UnitState> units) {
        std::vector<FogRevealer> result;
        for (const auto& unit : units) {
            if (const auto range = field.reveal_range(unit.type_id)) {
                result.push_back(FogRevealer{unit.entity_id, unit.owner, unit.position, *range});
            }
        }
        return result;
    }

    // Own-team players always see a unit (V-04). Other players see it by the retail fog cells
    // when the session is bound to fog rules (V-11 to V-17), else by the exact range test.
    [[nodiscard]] std::uint64_t visible_to(const SensorField& field, const FogCells* cells, const PlayerId owner,
        const math::Vec3& position) const {
        if (cells == nullptr) {
            return field.visible_to(owner, position);
        }
        const auto team = teams.at(owner);
        std::uint64_t mask = 0;
        for (std::size_t index = 0; index < setup.players.size(); ++index) {
            if (setup.players[index].team_id == team || cells->revealed(index, position)) {
                mask |= std::uint64_t{1} << index;
            }
        }
        return mask;
    }

    // `now`: the tick the snapshot's readers act at (the completed tick it is published with).
    [[nodiscard]] TacticalInstance instance_for(const LiveUnit& unit, const math::Mat3x4& transform, const std::uint64_t now) const {
        TacticalInstance instance{unit.state.entity_id, unit.state.type_id, unit.state.owner,
            teams.at(unit.state.owner), transform, 0, {}, {}, {}};
        if (unit.durability) {
            instance.durability = durability_status(*durability.find(unit.state.type_id), durability, *unit.durability);
        }
        instance.abilities = ability_statuses(unit, now);
        if (ion_stunned(unit.ion_stun, now)) {
            instance.ion_stun_frames = static_cast<std::uint32_t>(unit.ion_stun->end_frame - now);
        }
        return instance;
    }

    [[nodiscard]] std::vector<LiveUnit> sorted_live() const {
        std::vector<LiveUnit> result;
        result.reserve(handles.size());
        for (const auto& [id, handle] : handles) {
            const auto& identity = registry.get<Identity>(handle);
            const auto& placement = registry.get<Placement>(handle);
            const auto* health = registry.try_get<Health>(handle);
            const auto* moving = registry.try_get<Motion>(handle);
            const auto* fighting = registry.try_get<Combat>(handle);
            const auto* waiting = registry.try_get<Waiting>(handle);
            const auto* able = registry.try_get<Abilities>(handle);
            const auto* approaching = registry.try_get<Approaching>(handle);
            const auto* stunned = registry.try_get<IonStunned>(handle);
            result.push_back(LiveUnit{
                UnitState{
                    id,
                    identity.type_id,
                    identity.owner,
                    placement.position,
                    placement.rotation,
                    registry.get<CurrentOrder>(handle).value,
                },
                health != nullptr ? std::optional(health->value) : std::nullopt,
                moving != nullptr ? std::optional(moving->value) : std::nullopt,
                moving != nullptr ? moving->speed : math::Fixed{},
                moving != nullptr ? moving->roll : math::Fixed{},
                fighting != nullptr ? std::optional(fighting->value) : std::nullopt,
                waiting != nullptr ? std::optional(waiting->value) : std::nullopt,
                able != nullptr ? std::optional(able->value) : std::nullopt,
                approaching != nullptr ? std::optional(approaching->value) : std::nullopt,
                stunned != nullptr ? std::optional(stunned->value) : std::nullopt,
            });
        }
        return result;
    }

    // Movement of an accepted order (MV-10, MV-20, MV-22): a move or face plans from the unit's
    // state at `start` (completed tick + 1), a stop ends any plan; other orders keep it.
    // With avoidance rules (#71) a move runs the path finder against `world` (AV-10 to AV-15).
    [[nodiscard]] core::Result<void> plan_order(LiveUnit& unit, const CommandPayload& payload, const std::uint64_t start,
        const CollisionWorld* world = nullptr) const {
        if (!unit.motion) {
            return core::Result<void>::success(); // MV-03: no motion profile, the unit stays
        }
        if (std::holds_alternative<StopPayload>(payload)) {
            unit.motion = MotionState{};
            unit.speed = math::Fixed{};
            return core::Result<void>::success();
        }
        const auto* move = std::get_if<MovePayload>(&payload);
        const auto* face = std::get_if<FacePayload>(&payload);
        if (move == nullptr && face == nullptr) {
            return core::Result<void>::success();
        }
        const auto& profile = *motion.find(unit.state.type_id);
        const auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) {
            return core::Result<void>::failure(yaw.error());
        }
        if (face != nullptr) {
            auto plan = plan_face(profile, start, unit.state.position, yaw.value(), face->target);
            if (!plan) {
                return core::Result<void>::failure(plan.error());
            }
            unit.motion = std::move(plan).value();
            unit.speed = math::Fixed{};
            return core::Result<void>::success();
        }
        return plan_path(unit, move->destination, start, world, std::nullopt, nullptr);
    }

    // HD-11: the unit's movement limits after lost engines scale its maximum speed,
    // acceleration and deceleration. The unit has a motion profile.
    [[nodiscard]] core::Result<MotionProfile> limits_of(const LiveUnit& unit) const {
        auto factor = math::Fixed::from_raw(math::Fixed::scale);
        if (unit.durability) {
            factor = max_speed_factor(*durability.find(unit.state.type_id), durability.rules, *unit.durability);
        }
        // AB-24: an active ability's speed multiplier scales them the same way.
        const auto speed = ability_factor(unit, AbilityModifier::speed);
        if (speed.raw() != math::Fixed::scale) {
            auto scaled = math::multiply(factor, speed);
            if (!scaled) return core::Result<MotionProfile>::failure(scaled.error());
            factor = scaled.value();
        }
        auto limits = scaled_profile(*motion.find(unit.state.type_id), factor);
        // IS-05: an ion stun cuts the maximum speed only (the ability phase clears an ended stun
        // before any plan of its tick).
        if (limits && unit.ion_stun) {
            const auto cut = math::Fixed::from_raw(math::Fixed::scale - unit.ion_stun->speed_reduction.raw());
            auto slowed = math::multiply(limits.value().max_speed, cut);
            if (!slowed) return core::Result<MotionProfile>::failure(slowed.error());
            auto profile = std::move(limits).value();
            profile.max_speed = slowed.value();
            return core::Result<MotionProfile>::success(std::move(profile));
        }
        return limits;
    }

    // A move's path from the unit's state at `start` towards `target`. `max_speed`: a group
    // move's planning speed (FM-09), which caps the unit's own; zero plans nothing. `stats`, when
    // given, receives the path search's work (PC-07).
    [[nodiscard]] core::Result<void> plan_path(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
        const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats) const {
        auto planned = plan_path_within(unit, target, start, world, max_speed, stats, std::nullopt);
        if (!planned) return core::Result<void>::failure(planned.error());
        return core::Result<void>::success();
    }

    // What plan_path plans from (PC-08): the unit's position, yaw and speed, and its limits
    // under a group move's `max_speed`.
    struct PathInputs {
        math::Vec3 position{};
        math::Fixed yaw{};
        math::Fixed speed{};
        MotionProfile limits{};
        friend bool operator==(const PathInputs&, const PathInputs&) noexcept = default;
    };
    [[nodiscard]] core::Result<PathInputs> path_inputs(const LiveUnit& unit, const std::optional<math::Fixed> max_speed) const {
        const auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<PathInputs>::failure(yaw.error());
        auto limits = limits_of(unit);
        if (!limits) return core::Result<PathInputs>::failure(limits.error());
        if (max_speed) limits.value().max_speed = std::min(limits.value().max_speed, *max_speed);
        return core::Result<PathInputs>::success(PathInputs{unit.state.position, yaw.value(), unit.speed, limits.value()});
    }

    // plan_path's last step: the unit takes `plan`.
    static void take_plan(LiveUnit& unit, MotionState plan) {
        unit.motion = std::move(plan);
        if (unit.motion->kind == MotionKind::none) {
            unit.speed = math::Fixed{};
        }
    }

    // plan_path, whose path search (a tracked unit with a world) is given up once it has counted
    // `budget` expansions (PC-08): false then, and the unit is unchanged.
    [[nodiscard]] core::Result<bool> plan_path_within(LiveUnit& unit, const math::Vec3 target, const std::uint64_t start,
        const CollisionWorld* world, const std::optional<math::Fixed> max_speed, PathSearchStats* const stats,
        const std::optional<std::uint64_t> budget) const {
        const auto inputs = path_inputs(unit, max_speed);
        if (!inputs) return core::Result<bool>::failure(inputs.error());
        const auto& in = inputs.value();
        if (max_speed && in.limits.max_speed.raw() <= 0) {
            unit.motion = MotionState{};
            unit.speed = math::Fixed{};
            return core::Result<bool>::success(true);
        }
        const auto* footprint = tracked_footprint(motion, unit.state.type_id);
        if (world != nullptr && footprint != nullptr && !footprint->obstacle) {
            if (budget) {
                auto within = plan_space_move_within(motion, in.limits, *footprint, *world, unit.state.entity_id, start,
                    in.position, in.yaw, in.speed, target, *budget, stats);
                if (!within) return core::Result<bool>::failure(within.error());
                if (!within.value()) return core::Result<bool>::success(false);
                take_plan(unit, std::move(*within.value()));
                return core::Result<bool>::success(true);
            }
            auto plan = plan_space_move(
                motion, in.limits, *footprint, *world, unit.state.entity_id, start, in.position, in.yaw, in.speed, target, stats);
            if (!plan) return core::Result<bool>::failure(plan.error());
            take_plan(unit, std::move(plan).value());
            return core::Result<bool>::success(true);
        }
        auto plan = plan_move(in.limits, motion.rules, start, in.position, in.yaw, in.speed, target);
        if (!plan) return core::Result<bool>::failure(plan.error());
        take_plan(unit, std::move(plan).value());
        return core::Result<bool>::success(true);
    }

    // A path search that runs in slices and lands at `frame` (PC-08, #520): the plan the unit
    // would make at `frame` towards `target`, from where its current plan puts it then, against
    // its layer and the static layer as `world` holds them now. `inputs` is what it plans from;
    // the landing takes the plan only while the unit still plans from exactly that.
    struct SlicedSearch {
        SlicedPathSearch search;
        std::uint64_t frame{};
        math::Vec3 target{};
        std::optional<math::Fixed> max_speed;
        PathInputs inputs;
        // PC-09: the plan, once the search has ended; its unit's layer predicts it from then on.
        std::optional<MotionState> plan;
    };
    [[nodiscard]] core::Result<SlicedSearch> start_sliced(const LiveUnit& unit, const math::Vec3 target,
        const std::optional<math::Fixed> max_speed, const std::uint64_t now, const std::uint64_t frame,
        const CollisionWorld& world) const {
        // The movement phase of each tick up to `frame` advances the unit along its plan.
        LiveUnit ahead = unit;
        for (auto tick = now + 1; tick <= frame; ++tick) {
            if (auto advanced = advance(ahead, motion.find(unit.state.type_id), tick); !advanced) {
                return core::Result<SlicedSearch>::failure(advanced.error());
            }
        }
        auto inputs = path_inputs(ahead, max_speed);
        if (!inputs) return core::Result<SlicedSearch>::failure(inputs.error());
        const auto& in = inputs.value();
        const auto* footprint = tracked_footprint(motion, unit.state.type_id);
        SlicedPathSearch search(motion, in.limits, *footprint, world, unit.state.entity_id, frame, in.position, in.yaw,
            in.speed, target);
        return core::Result<SlicedSearch>::success(SlicedSearch{std::move(search), frame, target, max_speed, in, std::nullopt});
    }

    // --- Approaching a unit (#452, docs/behaviour/space-orders.md OR-02 to OR-17) --------------

    struct ApproachPlan {
        math::Vec3 slot{};
        std::uint64_t prediction_frame{};
    };

    // The unit the order in force sends a unit towards (OR-08, OR-17): an attack's target while
    // it is still the unit's player-ordered target (T-01 drops it), an attack-move's or guard's
    // unit. Zero otherwise, and for a unit without a motion profile.
    [[nodiscard]] static EntityId approach_target(const LiveUnit& unit) noexcept {
        if (!unit.motion) return invalid_entity_id;
        const auto& order = unit.state.order;
        switch (order.kind) {
        case OrderKind::attack:
            if (unit.combat && !(unit.combat->direct && unit.combat->attack_target == order.target)) {
                return invalid_entity_id;
            }
            return order.target;
        case OrderKind::attack_move:
        case OrderKind::guard:
            return order.target;
        default:
            return invalid_entity_id;
        }
    }

    // OR-06: an approach is checked every reevaluation interval after the order's tick.
    [[nodiscard]] bool approach_due(const LiveUnit& unit, const std::uint64_t tick) const noexcept {
        const auto issued = unit.state.order.issued_tick;
        return approach_target(unit) != invalid_entity_id && tick > issued
            && (tick - issued) % motion.rules.reevaluation_frames == 0;
    }

    // OR-03, OR-04, OR-14: the approach distance of the unit's order, also its in-range test.
    [[nodiscard]] math::Fixed approach_range(const LiveUnit& unit, const bool guard) const noexcept {
        const auto* profile = combat.find(unit.state.type_id);
        return detail::approach_distance(
            guard, profile != nullptr ? profile->max_attack_distance : std::nullopt, motion.rules.guard_range);
    }

    // OR-04, OR-14: the point the in-range test measures to: a guarded unit's position, else the
    // aim point of A-04 seen from `from`.
    [[nodiscard]] static math::Vec3 approach_point(
        const bool guard, const LiveUnit& target, const detail::CombatUnit* target_view, const math::Vec3& from) {
        if (guard || target_view == nullptr) return target.state.position;
        return detail::ordered_aim_point(*target_view, from);
    }

    // Where the unit's current movement leaves it (OR-06): the tracked prediction of its path at
    // the path's end (where the last frame before the last node puts it, MV-32), else where it is.
    // #662: sampling the path after its end gives back the position passed in, the unit's current
    // one, so every check found the end out of range and planned the approach again.
    [[nodiscard]] static core::Result<math::Vec3> movement_end(const LiveUnit& unit) {
        if (!unit.motion || unit.motion->kind != MotionKind::path || unit.motion->nodes.size() < 2) {
            return core::Result<math::Vec3>::success(unit.state.position);
        }
        const auto yaw = yaw_degrees(unit.state.rotation);
        if (!yaw) return core::Result<math::Vec3>::failure(yaw.error());
        const auto last = std::max<std::int64_t>(unit.motion->nodes.back().frame.raw(), 0);
        const auto end_tick = static_cast<std::uint64_t>((last + math::Fixed::scale - 1) / math::Fixed::scale);
        auto end = predict(*unit.motion, std::max(end_tick, unit.motion->start_tick), unit.state.position, yaw.value());
        if (!end) return core::Result<math::Vec3>::failure(end.error());
        return core::Result<math::Vec3>::success(end.value().position);
    }

    // OR-07: where `target` will be at frame `at` on its own planned path, seen at `frame`.
    [[nodiscard]] static core::Result<math::Vec3> predicted_position(
        const LiveUnit& target, const std::uint64_t frame, const std::uint64_t at) {
        if (at <= frame || !target.motion || target.motion->kind != MotionKind::path) {
            return core::Result<math::Vec3>::success(target.state.position);
        }
        const auto yaw = yaw_degrees(target.state.rotation);
        if (!yaw) return core::Result<math::Vec3>::failure(yaw.error());
        auto sample = sample_motion(*target.motion, at, target.state.position, yaw.value());
        if (!sample) return core::Result<math::Vec3>::failure(sample.error());
        return core::Result<math::Vec3>::success(sample.value().position);
    }

    // OR-05, OR-07: a new approach towards `target` from where the unit is at `frame`.
    [[nodiscard]] core::Result<ApproachPlan> approach_mapping(
        const LiveUnit& unit, const LiveUnit& target, const math::Fixed range, const std::uint64_t frame) const {
        const auto limits = limits_of(unit);
        if (!limits) return core::Result<ApproachPlan>::failure(limits.error());
        const auto target_yaw = yaw_degrees(target.state.rotation);
        if (!target_yaw) return core::Result<ApproachPlan>::failure(target_yaw.error());
        // The target's top speed on its current path; zero at rest or turning in place.
        math::Fixed target_speed{};
        if (target.motion && target.motion->kind == MotionKind::path) {
            for (const auto& node : target.motion->nodes) target_speed = std::max(target_speed, node.speed);
        }
        ApproachPlan plan;
        plan.prediction_frame = detail::prediction_frame(frame, unit.state.position, limits.value().max_speed,
            target.state.position, target_yaw.value(), target_speed, range);
        const auto predicted = predicted_position(target, frame, plan.prediction_frame);
        if (!predicted) return core::Result<ApproachPlan>::failure(predicted.error());
        auto slot = detail::approach_slot(unit.state.position, predicted.value(), range);
        if (!slot) return core::Result<ApproachPlan>::failure(slot.error());
        plan.slot = slot.value();
        return core::Result<ApproachPlan>::success(plan);
    }

    // OR-06: nullopt while the unit keeps its movement (in range of the target now, or at the end
    // of its movement in range of the target's predicted position), else its new approach.
    [[nodiscard]] core::Result<std::optional<ApproachPlan>> approach_check(const LiveUnit& unit, const LiveUnit& target,
        const detail::CombatUnit* target_view, const std::uint64_t frame) const {
        using Checked = core::Result<std::optional<ApproachPlan>>;
        const bool guard = unit.state.order.kind == OrderKind::guard;
        const auto range = approach_range(unit, guard);
        const auto now = approach_point(guard, target, target_view, unit.state.position);
        if (within_range(unit.state.position, now, range, RangeMetric::planar)) return Checked::success(std::nullopt);
        const auto end = movement_end(unit);
        if (!end) return Checked::failure(end.error());
        const auto predicted =
            predicted_position(target, frame, unit.approach ? unit.approach->prediction_frame : std::uint64_t{0});
        if (!predicted) return Checked::failure(predicted.error());
        if (within_range(end.value(), predicted.value(), range, RangeMetric::planar)) {
            return Checked::success(std::nullopt);
        }
        auto plan = approach_mapping(unit, target, range, frame);
        if (!plan) return Checked::failure(plan.error());
        return Checked::success(plan.value());
    }

    [[nodiscard]] std::vector<UnitState> sorted_units() const {
        std::vector<UnitState> result;
        for (auto& unit : sorted_live()) {
            result.push_back(std::move(unit.state));
        }
        return result;
    }

    void rebuild(const std::vector<LiveUnit>& units, const bool reverse) {
        registry.clear();
        handles.clear();
        const auto insert = [this](const LiveUnit& unit) {
            const auto& state = unit.state;
            const auto handle = registry.create();
            registry.emplace<StableId>(handle, state.entity_id);
            registry.emplace<Identity>(handle, state.type_id, state.owner);
            registry.emplace<Placement>(handle, state.position, state.rotation);
            registry.emplace<CurrentOrder>(handle, state.order);
            if (unit.durability) {
                registry.emplace<Health>(handle, *unit.durability);
            }
            if (unit.motion) {
                registry.emplace<Motion>(handle, *unit.motion, unit.speed, unit.roll);
            }
            if (unit.combat) {
                registry.emplace<Combat>(handle, *unit.combat);
            }
            if (unit.formation) {
                registry.emplace<Waiting>(handle, *unit.formation);
            }
            if (unit.abilities) {
                registry.emplace<Abilities>(handle, *unit.abilities);
            }
            if (unit.approach) {
                registry.emplace<Approaching>(handle, *unit.approach);
            }
            if (unit.ion_stun) {
                registry.emplace<IonStunned>(handle, *unit.ion_stun);
            }
            handles.emplace(state.entity_id, handle);
        };
        if (reverse) {
            for (auto iterator = units.rbegin(); iterator != units.rend(); ++iterator) {
                insert(*iterator);
            }
        } else {
            for (const auto& unit : units) {
                insert(unit);
            }
        }
    }

    // The targeting phase's world view (#73): the moved units in ascending ID with their health,
    // combat state and the visibility of the last published snapshot, and one space index over
    // them. Built serially: one index over all units, like the sensor field.
    [[nodiscard]] core::Result<detail::CombatWorld> combat_world(const std::vector<LiveUnit>& units,
        const std::vector<std::pair<EntityId, math::Vec3>>& starts, const std::uint64_t frame) const {
        detail::CombatWorld world;
        world.players = setup.players;
        world.table = &combat;
        world.rules = &durability.rules;
        world.damage = durability.damage ? &*durability.damage : nullptr;
        world.seed = setup.seed;
        world.frame = frame;
        if (combat.profiles.empty()) {
            return core::Result<detail::CombatWorld>::success(std::move(world)); // no unit fights
        }
        // The broad phase of projectile flight (#74): no collision box corner lies further than
        // the sum of its largest coordinates from its unit's position, whatever the rotation.
        // #536: the box around its meshes (DG-36) counts as well. The craft sphere (DG-37) needs
        // the step to meet the collision box turned into the world, which lies within that reach.
        // #636: each type's own reach is the distance to the furthest corner of its boxes, plus a
        // margin of 1/1024 of it and one unit for the Q24 rounding of the exact tests, so the
        // per-candidate reject never drops a hit. With a DG-37 sphere it also covers that world
        // box: each world axis of it lies within the collision box corner's length of the unit
        // (the rotation's rows are unit vectors), so all of it within sqrt(3) times that length.
        // The sphere itself needs no reach: no step hits it without meeting the world box.
        std::vector<math::Fixed> reaches(combat.profiles.size());
        for (std::size_t profile_index = 0; profile_index < combat.profiles.size(); ++profile_index) {
            const auto& profile = combat.profiles[profile_index];
            if (!profile.collision) continue;
            std::int64_t own = 0;
            for (const auto* box : {&*profile.collision, profile.mesh_bounds ? &*profile.mesh_bounds : nullptr}) {
                if (box == nullptr) continue;
                std::int64_t reach = 0;
                std::array<math::Fixed, 3> corner{};
                for (const auto axis : {0, 1, 2}) {
                    const auto low = axis == 0 ? box->min.x : axis == 1 ? box->min.y : box->min.z;
                    const auto high = axis == 0 ? box->max.x : axis == 1 ? box->max.y : box->max.z;
                    corner[axis] = math::Fixed::from_raw(std::max(std::llabs(low.raw()), std::llabs(high.raw())));
                    reach += corner[axis].raw();
                }
                world.collision_reach = std::max(world.collision_reach, math::Fixed::from_raw(reach));
                const auto length = math::length(math::Vec3{corner[0], corner[1], corner[2]});
                const auto corner_length = length ? length.value().raw() : reach;
                own = std::max(own, corner_length);
                // sqrt(3) < 1.7321, rounded up.
                if (box == &*profile.collision && profile.sphere_modifier) {
                    own = std::max(own, corner_length / 10000 * 17321 + (corner_length % 10000 * 17321 + 9999) / 10000);
                }
            }
            reaches[profile_index] = math::Fixed::from_raw(own + own / 1024 + math::Fixed::scale);
        }
        world.units.reserve(units.size());
        std::vector<SpaceBody> bodies;
        bodies.reserve(units.size());
        const auto instances = current_snapshot->instances();
        auto published = instances.begin();
        for (const auto& unit : units) {
            const auto& state = unit.state;
            detail::CombatUnit entry;
            entry.id = state.entity_id;
            entry.type_id = state.type_id;
            entry.owner = state.owner;
            entry.team = teams.at(state.owner);
            const auto player = std::find_if(setup.players.begin(), setup.players.end(),
                [&](const Player& candidate) { return candidate.player_id == state.owner; });
            entry.player_index = static_cast<std::size_t>(player - setup.players.begin());
            entry.position = state.position;
            // W-10: where the unit stood before this frame's movement (a unit new this frame: here).
            const auto start = std::lower_bound(starts.begin(), starts.end(), state.entity_id,
                [](const std::pair<EntityId, math::Vec3>& item, const EntityId id) { return item.first < id; });
            entry.previous_position = start != starts.end() && start->first == state.entity_id ? start->second : state.position;
            auto transform = math::to_matrix(state.rotation, state.position);
            if (!transform) {
                return core::Result<detail::CombatWorld>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(frame) + " unit " + std::to_string(state.entity_id) + ": "
                        + transform.error().message));
            }
            entry.transform = transform.value();
            while (published != instances.end() && published->entity_id < state.entity_id) {
                ++published;
            }
            if (published != instances.end() && published->entity_id == state.entity_id) {
                entry.visible_to = published->visible_to;
            }
            entry.profile = combat.find(state.type_id);
            if (entry.profile != nullptr) {
                entry.collision_reach = reaches[static_cast<std::size_t>(entry.profile - combat.profiles.data())];
            }
            entry.durability_profile = durability.find(state.type_id);
            entry.durability = unit.durability ? &*unit.durability : nullptr;
            entry.combat = unit.combat ? &*unit.combat : nullptr;
            // A-04: only a unit at rest turns toward its target (FoC's Is_Moving_To).
            entry.can_turn = unit.motion && unit.motion->kind == MotionKind::none && !unit.formation
                && motion.find(state.type_id) != nullptr;
            entry.weapon_delay = ability_factor(unit, AbilityModifier::weapon_delay);
            entry.fire_rate = ion_fire_rate(unit.ion_stun, world.frame); // IS-06
            world.units.push_back(entry);
            bodies.push_back(SpaceBody{state.entity_id, state.owner, state.position});
        }
        auto index = SpaceIndex::build(bodies);
        if (!index) {
            return core::Result<detail::CombatWorld>::failure(index.error());
        }
        world.index = std::move(index).value();
        // #424: a player's attack on a squadron targets its team container, which its attackers
        // resolve to one of its live craft (FO-04).
        for (const auto& squadron : squadrons) {
            std::vector<EntityId> members;
            for (const auto member : squadron.members) {
                if (world.find(member) != nullptr) members.push_back(member);
            }
            world.teams.emplace_back(squadron.container, std::move(members));
        }
        // AB-66 (#561): the craft of a squadron whose ION_CANNON_SHOT is on hold every other shot.
        const auto live = [&](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(units.begin(), units.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != units.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        for (const auto& [container, members] : world.teams) {
            const auto* owner = live(container);
            const auto* slot = owner != nullptr ? ion_slot(*owner) : nullptr;
            if (slot == nullptr || !slot->active) continue;
            for (const auto member : members) {
                const auto* craft = live(member);
                auto entry = std::lower_bound(world.units.begin(), world.units.end(), member,
                    [](const detail::CombatUnit& unit, const EntityId value) { return unit.id < value; });
                if (craft == nullptr || entry == world.units.end() || entry->id != member) continue;
                const auto* own = ion_slot(*craft);
                entry->ion_held = true;
                entry->ion_armed = own != nullptr && own->active;
                entry->ion_target = slot->target;
                entry->ion_target_hardpoint = slot->target_hardpoint;
            }
        }
        return core::Result<detail::CombatWorld>::success(std::move(world));
    }

    // CO-01, CO-11: a live unit with a combat profile joins its owner's tree with the world box of
    // its collision box (FoC: its model's), or FoC's box for an object without a model: its
    // position to 0.1 beyond on each axis. None for a unit without a combat profile.
    [[nodiscard]] core::Result<std::optional<detail::CollectionTrees::Member>> collection_member(
        const LiveUnit& unit) const {
        using Member = std::optional<detail::CollectionTrees::Member>;
        const auto& state = unit.state;
        const auto* profile = combat.find(state.type_id);
        if (profile == nullptr) return core::Result<Member>::success(std::nullopt);
        detail::CullBox bounds;
        if (profile->collision) {
            auto transform = math::to_matrix(state.rotation, state.position);
            if (!transform) return core::Result<Member>::failure(transform.error());
            const auto& box = *profile->collision;
            for (int corner = 0; corner < 8; ++corner) {
                const math::Vec3 local{(corner & 1) != 0 ? box.max.x : box.min.x,
                    (corner & 2) != 0 ? box.max.y : box.min.y, (corner & 4) != 0 ? box.max.z : box.min.z};
                auto point = math::transform_point(transform.value(), local);
                if (!point) return core::Result<Member>::failure(point.error());
                const auto& p = point.value();
                if (corner == 0) {
                    bounds = detail::CullBox{p, p};
                    continue;
                }
                bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
                bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
            }
        } else {
            const auto tenth = math::Fixed::scale / 10;
            bounds = detail::CullBox{state.position, {math::Fixed::from_raw(state.position.x.raw() + tenth),
                math::Fixed::from_raw(state.position.y.raw() + tenth), math::Fixed::from_raw(state.position.z.raw() + tenth)}};
        }
        return core::Result<Member>::success(detail::CollectionTrees::Member{state.entity_id, state.owner, bounds});
    }

    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const {
        const auto units = sorted_live();
        std::vector<std::uint8_t> bytes;
        bytes.reserve(120 + setup.players.size() * detail::player_record_size
            + units.size() * (detail::unit_record_size + detail::order_record_size));
        bytes.insert(bytes.end(), state_magic.begin(), state_magic.end());
        sim::detail::append_u32(bytes, tactical_state_encoding_version);
        sim::detail::append_u32(bytes, tactical_rules_version);
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, math::Fixed::fractional_bits);
        sim::detail::append_u32(bytes, tick_numerator);
        sim::detail::append_u32(bytes, tick_denominator);
        sim::detail::append_u64(bytes, completed_tick);
        sim::detail::append_u64(bytes, next_id);
        sim::detail::append_u64(bytes, rng_state);
        bytes.insert(bytes.end(), setup.content_identity.begin(), setup.content_identity.end());
        sim::detail::append_u64(bytes, setup.players.size());
        for (const auto& player : setup.players) {
            detail::append_player(bytes, player);
        }
        sim::detail::append_u64(bytes, units.size());
        for (const auto& unit : units) {
            detail::append_unit_record(bytes, unit.state);
            detail::append_order(bytes, unit.state.order);
            // A durable unit appends its health; any other unit encodes exactly as before #72.
            if (unit.durability) {
                append_fixed(bytes, unit.durability->hull);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(unit.durability->hardpoints.size()));
                sim::detail::append_u32(bytes, 0);
                for (const auto health : unit.durability->hardpoints) {
                    append_fixed(bytes, health);
                }
                // With damage rules (#74): the shield and the depletion and last-hit frames; since
                // #361 the energy pool and the next shield and energy recharge frames.
                if (durability.damage) {
                    append_fixed(bytes, unit.durability->shields);
                    for (const auto& frame : {unit.durability->depleted_frame, unit.durability->last_hit_frame}) {
                        sim::detail::append_u32(bytes, frame ? 1U : 0U);
                        sim::detail::append_u64(bytes, frame.value_or(0));
                    }
                    append_fixed(bytes, unit.durability->energy);
                    sim::detail::append_u64(bytes, unit.durability->next_shield_frame);
                    sim::detail::append_u64(bytes, unit.durability->next_energy_frame);
                }
            }
            // A unit with a motion profile (#70) then appends its motion record, and with
            // avoidance rules (#71) its plan's nodes.
            if (unit.motion) {
                append_motion(bytes, *unit.motion);
                if (motion.avoidance) {
                    append_nodes(bytes, *unit.motion);
                }
            }
            // A unit with a combat profile (#73) then appends its combat record.
            if (unit.combat) {
                detail::append_combat(bytes, *unit.combat);
            }
        }
        // Squadron membership (#271) and fog cells (#274) follow as tagged blocks, only when
        // present, so a session without them hashes exactly as before.
        if (!squadrons.empty()) {
            bytes.insert(bytes.end(), squadron_tag.begin(), squadron_tag.end());
            sim::detail::append_u64(bytes, squadrons.size());
            for (const auto& squadron : squadrons) {
                sim::detail::append_u64(bytes, squadron.container);
                sim::detail::append_u64(bytes, squadron.members.size());
                for (const auto member : squadron.members) {
                    sim::detail::append_u64(bytes, member);
                }
            }
        }
        if (fog) {
            bytes.insert(bytes.end(), fog_tag.begin(), fog_tag.end());
            fog->append_state(bytes);
        }
        if (motion.avoidance) {
            bytes.insert(bytes.end(), tracking_tag.begin(), tracking_tag.end());
            for (const auto anchor : tracking_anchor) {
                sim::detail::append_u64(bytes, anchor);
            }
        }
        // Rolled units (#351): only those whose roll is not zero, so a session in which no unit
        // ever banked hashes as before.
        std::uint64_t rolled = 0;
        for (const auto& unit : units) {
            rolled += unit.roll.raw() != 0 ? 1 : 0;
        }
        if (rolled != 0) {
            bytes.insert(bytes.end(), roll_tag.begin(), roll_tag.end());
            sim::detail::append_u64(bytes, rolled);
            for (const auto& unit : units) {
                if (unit.roll.raw() != 0) {
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    append_fixed(bytes, unit.roll);
                }
            }
        }
        // Projectiles in flight (#74), with damage rules only.
        if (durability.damage) {
            bytes.insert(bytes.end(), projectile_tag.begin(), projectile_tag.end());
            sim::detail::append_u64(bytes, next_projectile);
            sim::detail::append_u64(bytes, projectiles.size());
            for (const auto& projectile : projectiles) {
                detail::append_projectile(bytes, projectile);
            }
        }
        // Waiting group members (#344), then ships waiting for a sliced search (PC-08), each
        // ascending ID, only while any wait.
        for (const bool sliced : {false, true}) {
            std::vector<std::pair<EntityId, const FormationWait*>> waits;
            for (const auto& unit : units) {
                if (unit.formation && unit.formation->sliced == sliced) waits.emplace_back(unit.state.entity_id, &*unit.formation);
            }
            if (waits.empty()) continue;
            const auto& tag = sliced ? sliced_tag : formation_tag;
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, waits.size());
            for (const auto& [id, wait] : waits) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, wait->frame);
                append_fixed(bytes, wait->destination.x);
                append_fixed(bytes, wait->destination.y);
                append_fixed(bytes, wait->destination.z);
                append_fixed(bytes, wait->max_speed);
                sim::detail::append_u64(bytes, wait->order.tick);
                sim::detail::append_u32(bytes, wait->order.player_id);
                sim::detail::append_u64(bytes, wait->order.sequence);
                sim::detail::append_u32(bytes, wait->rank);
            }
        }
        // Approach mappings (#452), ascending ID, only while any unit has one.
        std::vector<std::pair<EntityId, const Approach*>> approaches;
        for (const auto& unit : units) {
            if (unit.approach) approaches.emplace_back(unit.state.entity_id, &*unit.approach);
        }
        if (!approaches.empty()) {
            bytes.insert(bytes.end(), approach_tag.begin(), approach_tag.end());
            sim::detail::append_u64(bytes, approaches.size());
            for (const auto& [id, approach] : approaches) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, approach->prediction_frame);
            }
        }
        // Squadron flight, orders and hangars (#75), each only when present.
        if (!crafts.empty()) {
            bytes.insert(bytes.end(), craft_tag.begin(), craft_tag.end());
            sim::detail::append_u64(bytes, crafts.size());
            for (const auto& [id, craft] : crafts) {
                append_craft(bytes, id, craft);
            }
        }
        if (!minds.empty()) {
            bytes.insert(bytes.end(), squadron_state_tag.begin(), squadron_state_tag.end());
            sim::detail::append_u64(bytes, minds.size());
            for (const auto& [container, state] : minds) {
                static_cast<void>(container);
                append_squadron_state(bytes, state);
            }
        }
        if (!spawners.empty()) {
            bytes.insert(bytes.end(), hangar_tag.begin(), hangar_tag.end());
            sim::detail::append_u64(bytes, spawners.size());
            for (const auto& [id, state] : spawners) {
                append_spawner(bytes, id, state);
            }
        }
        if (!collection.empty()) {
            bytes.insert(bytes.end(), collection_tag.begin(), collection_tag.end());
            collection.append_state(bytes);
        }
        // The decided battle (#77); an undecided session hashes as before.
        if (outcome) {
            append_outcome(bytes, *outcome);
        }
        // Unit abilities (#76), ascending ID, only when a live unit's type has abilities.
        std::uint64_t able = 0;
        for (const auto& unit : units) able += unit.abilities ? 1 : 0;
        if (able != 0) {
            bytes.insert(bytes.end(), ability_tag.begin(), ability_tag.end());
            sim::detail::append_u64(bytes, able);
            for (const auto& unit : units) {
                if (!unit.abilities) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_abilities(bytes, *unit.abilities);
            }
        }
        // Ion stuns (#561), ascending ID, only while a live unit has one.
        std::uint64_t stunned = 0;
        for (const auto& unit : units) stunned += unit.ion_stun ? 1 : 0;
        if (stunned != 0) {
            bytes.insert(bytes.end(), ion_stun_tag.begin(), ion_stun_tag.end());
            sim::detail::append_u64(bytes, stunned);
            for (const auto& unit : units) {
                if (!unit.ion_stun) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_ion_stun(bytes, *unit.ion_stun);
            }
        }
        // Killed craft spinning away (#447); a session without any hashes as before.
        if (!spins.empty()) {
            bytes.insert(bytes.end(), spin_tag.begin(), spin_tag.end());
            sim::detail::append_u64(bytes, spins.size());
            for (const auto& spin : spins) {
                append_spin(bytes, spin);
            }
        }
        return bytes;
    }

    TacticalSetup setup;
    std::vector<SensorProfile> sensors;
    DurabilityTable durability;
    MotionTable motion;
    CombatTable combat;
    VictoryRules victory;
    AbilityTable abilities;
    std::vector<StarbaseEntry> starbases;   // counted star bases standing, ascending ID (#77)
    std::optional<BattleOutcome> outcome;   // hashed once decided (#77)
    std::vector<SnapshotPlayer> snapshot_players;
    std::map<PlayerId, TeamId> teams;
    std::vector<Squadron> squadrons; // live, ascending container ID (#271)
    std::optional<FogCells> fog;     // bound fog rules only (#274)
    // Per dynamic tracking layer (#71, AV-03): the frame of its last rebuild; its windows roll
    // every tracking interval from there.
    std::array<std::uint64_t, 4> tracking_anchor{};
    // Path searches running in slices (PC-08, #520), by unit; each unit waits for its search
    // with a sliced FormationWait of the same frame. A search's copied views and progress are a
    // function of the ticks before it, and its wait is hashed.
    std::map<EntityId, SlicedSearch> searches;
    std::vector<Projectile> projectiles; // in flight, ascending ID (#74)
    // #636: the projectile phase's candidate buffers, one per partition, kept from tick to tick
    // (not state: they only hold capacity and the tick's work counts).
    std::array<detail::ProjectileScratch, tick_partition_count> projectile_scratch;
    std::map<EntityId, CraftState> crafts;     // #75: squadron craft that fly by a craft profile
    std::map<EntityId, SquadronState> minds;   // #75: by container, squadrons of a known type
    std::map<EntityId, SpawnerState> spawners; // #75: hangars of SPAWN_SQUADRON units
    std::vector<DeathSpin> spins;              // #447: killed craft spinning away, ascending ID
    detail::CollectionTrees collection;        // #469: the candidate order of target scans
    std::uint64_t next_projectile{1};
    entt::registry registry;
    std::map<EntityId, entt::entity> handles;
    std::uint64_t completed_tick{};
    EntityId next_id{};
    std::uint64_t rng_state{};
    std::map<CommandKey, PlayerCommand> pending;
    std::map<PlayerId, std::pair<std::uint64_t, std::uint64_t>> last_submitted;
    std::vector<PlayerCommand> executed;
    std::shared_ptr<const TacticalSnapshot> current_snapshot;
};

TacticalSession::TacticalSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TacticalSession::TacticalSession(TacticalSession&&) noexcept = default;
TacticalSession& TacticalSession::operator=(TacticalSession&&) noexcept = default;
TacticalSession::~TacticalSession() = default;

core::Result<TacticalSession> TacticalSession::create(const TacticalSetup& setup,
    const std::span<const SensorProfile> sensors, const DurabilityTable& durability, const MotionTable& motion,
    const std::optional<FogRules>& fog, const CombatTable& combat, const VictoryRules& victory,
    const AbilityTable& abilities) {
    auto valid = validate_setup(setup);
    if (valid) {
        valid = validate_sensors(sensors);
    }
    if (valid) {
        valid = validate_durability(durability);
    }
    if (valid && !motion.profiles.empty()) {
        valid = validate_motion(motion);
    } else if (valid) {
        valid = validate_squadron_table(motion.squadrons);
    }
    if (valid && fog) {
        valid = validate_fog_rules(*fog);
    }
    if (valid) {
        valid = validate_combat(combat);
    }
    if (valid) {
        valid = validate_victory(victory, setup.players);
    }
    if (valid) {
        valid = validate_abilities(abilities);
    }
    if (!valid) {
        return core::Result<TacticalSession>::failure(valid.error());
    }
    return core::Result<TacticalSession>::success(
        TacticalSession(std::make_unique<Impl>(setup, sensors, durability, motion, fog, combat, victory, abilities)));
}

core::Result<TacticalSession> TacticalSession::from_replay(const TacticalReplay& replay,
    const std::span<const SensorProfile> sensors, const DurabilityTable& durability, const MotionTable& motion,
    const std::optional<FogRules>& fog, const CombatTable& combat, const VictoryRules& victory,
    const AbilityTable& abilities) {
    auto valid = validate_replay(replay);
    if (valid) {
        valid = validate_sensors(sensors);
    }
    if (valid) {
        valid = validate_durability(durability);
    }
    if (valid && !motion.profiles.empty()) {
        valid = validate_motion(motion);
    } else if (valid) {
        valid = validate_squadron_table(motion.squadrons);
    }
    if (valid && fog) {
        valid = validate_fog_rules(*fog);
    }
    if (valid) {
        valid = validate_combat(combat);
    }
    if (valid) {
        valid = validate_victory(victory, replay.setup.players);
    }
    if (valid) {
        valid = validate_abilities(abilities);
    }
    if (!valid) {
        return core::Result<TacticalSession>::failure(valid.error());
    }
    auto session = TacticalSession(
        std::make_unique<Impl>(replay.setup, sensors, durability, motion, fog, combat, victory, abilities));
    for (const auto& command : replay.commands) {
        const auto submitted = session.submit(command);
        if (!submitted) {
            return core::Result<TacticalSession>::failure(submitted.error());
        }
    }
    return core::Result<TacticalSession>::success(std::move(session));
}

core::Result<void> TacticalSession::submit(const PlayerCommand& command) {
    const auto context = command_context(command.key);
    const auto shape = detail::validate_command_shape(command, impl_->setup.players, context, {});
    if (!shape) {
        return shape;
    }
    if (command.key.tick >= max_ticks) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            context + ": tick exceeds the tactical tick limit"));
    }
    if (command.key.tick < impl_->completed_tick) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::late_command,
            context + ": tick " + std::to_string(command.key.tick)
                + " has already executed; the next tick to execute is "
                + std::to_string(impl_->completed_tick)));
    }
    const std::pair key{command.key.tick, command.key.sequence};
    const auto previous = impl_->last_submitted.find(command.key.player_id);
    if (previous != impl_->last_submitted.end() && !(previous->second < key)) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::order,
            context + ": does not follow the issuer's previous command (tick "
                + std::to_string(previous->second.first) + ", sequence "
                + std::to_string(previous->second.second) + ")"));
    }
    if (impl_->executed.size() + impl_->pending.size() >= max_commands) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            context + ": session command count limit reached"));
    }
    impl_->pending.emplace(command.key, command);
    impl_->last_submitted.insert_or_assign(command.key.player_id, key);
    return core::Result<void>::success();
}

core::Result<TacticalTick> TacticalSession::step(const PartitionExecutor& executor) {
    if (executor.worker_count() == 0) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(
            diagnostic_codes::worker_failure, "executor reports no workers"));
    }
    const auto tick = impl_->completed_tick;
    // Stopping at the replay tick limit keeps record() a valid replay v2.
    if (tick >= max_ticks) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "tick " + std::to_string(tick) + " reached the tactical tick limit"));
    }
    const auto& durability = impl_->durability;

    // Movement phase (#70): before this tick's commands, workers advance every unit that follows
    // a plan to completed tick + 1 in disjoint slots, so an order acts from the next frame (MV-02).
    auto moving = impl_->sorted_live();
    std::vector<std::pair<EntityId, math::Vec3>> starts;
    starts.reserve(moving.size());
    for (const auto& unit : moving) starts.emplace_back(unit.state.entity_id, unit.state.position);
    const auto& squadron_table = impl_->motion.squadrons;
    const auto live_unit = [](std::vector<LiveUnit>& units, const EntityId id) -> LiveUnit* {
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != units.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    // Craft phase inputs (#75): the start-of-tick flight of every squadron craft, and per squadron
    // its leader and order. Built serially like the combat world: an index over the craft and one
    // entry per squadron, which the workers only read.
    auto minds = impl_->minds;
    std::vector<CraftView> craft_views;
    craft_views.reserve(impl_->crafts.size());
    for (const auto& [id, flight] : impl_->crafts) {
        const auto* unit = live_unit(moving, id);
        if (unit == nullptr) continue;
        craft_views.push_back(CraftView{id, unit->state.position, flight, squadron_table.find_craft(unit->state.type_id)});
    }
    const auto craft_view = [&craft_views](const EntityId id) -> const CraftView* {
        const auto found = std::lower_bound(craft_views.begin(), craft_views.end(), id,
            [](const CraftView& view, const EntityId value) { return view.id < value; });
        return found != craft_views.end() && found->id == id ? &*found : nullptr;
    };
    // FM-24 (#687, walk 1 WSQ-09): a squadron reverting to idle at `desired` keeps the idle cell it
    // holds, else claims one and holds its point; with none free it holds `desired`. Serial, in
    // ascending container ID, since a claim changes what the next one sees.
    const auto claim_idle = [&minds](const EntityId container, SquadronState& state, const math::Vec3& desired) {
        if (state.idle_cell) return;
        std::vector<CombatCell> taken;
        for (const auto& [other, mind] : minds) {
            if (other != container && mind.idle_cell) taken.push_back(*mind.idle_cell);
        }
        if (const auto cell = idle_cell_claim(desired, taken)) {
            state.idle_cell = cell;
            state.anchor = idle_cell_point(*cell, desired.z);
        } else {
            state.anchor = desired;
        }
    };
    const auto idle_reach = math::Fixed::from_raw(idle_cell_size * math::Fixed::scale);
    std::map<EntityId, SquadronFrame> squadron_frames;  // by container
    std::map<EntityId, EntityId> craft_squadron;        // craft -> container
    for (const auto& squadron : impl_->squadrons) {
        const auto mind = minds.find(squadron.container);
        if (mind == minds.end()) continue;
        auto& state = mind->second;
        SquadronFrame frame;
        frame.state = &state;
        frame.profile = squadron_table.find_squadron(state.squadron_type);
        for (const auto member : squadron.members) {
            if (craft_view(member) == nullptr) continue;
            if (frame.leader == invalid_entity_id) frame.leader = member; // FM-10: the first live craft leads
            craft_squadron.emplace(member, squadron.container);
        }
        const auto* leader = craft_view(frame.leader);
        const math::Fixed layer_z = leader != nullptr ? leader->profile->layer_z : math::Fixed{};
        // FM-25 (#687, walk 1 WSQ-02, WSQ-49): a squadron on a move or with a target gives its idle
        // cell up.
        if (state.idle_cell && (state.mode == SquadronMode::move || state.target != invalid_entity_id)) {
            state.idle_cell.reset();
        }
        // FO-02 (#424): a move ends once the leader passes the line through the destination
        // square to the move (FoC's path-end half plane); the squadron then holds the destination,
        // or the idle cell it claims there (FM-24).
        if (state.mode == SquadronMode::move && leader != nullptr
            && squadron_move_arrived(state.move_origin, state.anchor, leader->position)) {
            state.mode = SquadronMode::idle;
            state.move_origin = {};
            state.lane.reset();
            claim_idle(squadron.container, state, state.anchor);
        }
        // FL-07: an escort holds its carrier's position at the craft's layer height. FO-05,
        // FO-06: a guard or attack-move of a unit ends with it, and the squadron holds there.
        if (state.mode == SquadronMode::escort) {
            if (const auto* escorted = live_unit(moving, state.escorted)) {
                const math::Vec3 carrier{escorted->state.position.x, escorted->state.position.y, layer_z};
                // FM-24 (#687, project): FoC's escort is a move to the unit that ends in an idle cell
                // (walk 1, FM-21). The remake's escort claims a cell around the unit once its leader
                // is within a cell's size of it, and gives the cell up when it takes a target or
                // the unit is more than a cell's size from the cell's point.
                if (state.idle_cell && !within_range(idle_cell_point(*state.idle_cell, layer_z), carrier, idle_reach, RangeMetric::planar)) {
                    state.idle_cell.reset();
                }
                if (!state.idle_cell && state.target == invalid_entity_id && leader != nullptr
                    && within_range(leader->position, carrier, idle_reach, RangeMetric::planar)) {
                    claim_idle(squadron.container, state, carrier);
                }
                if (!state.idle_cell) state.anchor = carrier;
            } else {
                state.mode = SquadronMode::idle;
                state.escorted = invalid_entity_id;
                state.diversion = SquadronDiversion::idle;
            }
        }
        frame.hold = state.anchor;
        frame.moving = state.mode == SquadronMode::move;
        // FO-01: a squadron on a move does not scan. FO-05: on an attack-move it scans from its
        // leader all the way. FO-06: on a guard's way it scans once the leader is within
        // `Guard_Chase_Range` of the guarded point.
        frame.scans = !frame.moving;
        if (frame.moving && state.diversion == SquadronDiversion::attack_move) {
            frame.scans = true;
            frame.scan_from_leader = true;
        } else if (frame.moving && state.diversion == SquadronDiversion::guard && leader != nullptr
            && frame.profile != nullptr) {
            frame.scans = within_range(leader->position, state.anchor, frame.profile->guard_chase_range, RangeMetric::planar);
        }
        squadron_frames.emplace(squadron.container, frame);
    }
    // FO-10, FO-11 (#599): the squadrons still on a group move fly their formation's path, each
    // steering toward its lane and keeping level with its row. Serial, per formation: every
    // member reads every other one's place (O(members^2), a few squadrons per formation).
    {
        std::map<EntityId, std::vector<EntityId>> formations;
        for (const auto& [container, frame] : squadron_frames) {
            const auto* leader = craft_view(frame.leader);
            if (frame.moving && frame.state->lane && leader != nullptr && leader->profile != nullptr) {
                formations[frame.state->lane->formation].push_back(container);
            }
        }
        for (const auto& [formation, containers] : formations) {
            std::vector<LaneMember> members;
            members.reserve(containers.size());
            for (const auto container : containers) {
                const auto& frame = squadron_frames.at(container);
                const auto* leader = craft_view(frame.leader);
                const auto* unit = live_unit(moving, container);
                LaneMember member;
                member.position = unit != nullptr ? unit->state.position : leader->position;
                member.lane = *frame.state->lane;
                member.max_speed = leader->profile->max_speed;
                member.min_speed = leader->profile->min_speed;
                members.push_back(member);
            }
            auto flights = formation_lane_flight(members, squadron_table.side_error_min, squadron_table.side_error_max);
            if (!flights) {
                return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " formation " + std::to_string(formation) + ": "
                        + flights.error().message));
            }
            for (std::size_t index = 0; index < containers.size(); ++index) {
                squadron_frames.at(containers[index]).lane = flights.value()[index];
            }
        }
    }

    // #457: the squadron behind a target: the target itself when it is a team container, else
    // the squadron of a craft (FO-04). Zero for any other unit.
    const auto squadron_of = [&](const EntityId target) -> EntityId {
        if (target == invalid_entity_id) return invalid_entity_id;
        for (const auto& squadron : impl_->squadrons) {
            if (squadron.container == target
                || std::binary_search(squadron.members.begin(), squadron.members.end(), target)) {
                return squadron.container;
            }
        }
        return invalid_entity_id;
    };
    const auto team_of = [&](const EntityId id) -> std::optional<TeamId> {
        const auto* unit = live_unit(moving, id);
        if (unit == nullptr) return std::nullopt;
        return impl_->teams.at(unit->state.owner);
    };
    // FT-05: hand a squadron's new target to its craft as their direct target, or take the old
    // one back.
    const auto hand_target = [&](const EntityId container, const EntityId previous, const EntityId target) {
        for (const auto& squadron : impl_->squadrons) {
            if (squadron.container != container) continue;
            for (const auto member : squadron.members) {
                auto* craft = live_unit(moving, member);
                if (craft == nullptr || !craft->combat) continue;
                if (target != invalid_entity_id) {
                    craft->combat->attack_target = target;
                    craft->combat->direct = true;
                } else if (craft->combat->direct && craft->combat->attack_target == previous) {
                    craft->combat->attack_target = invalid_entity_id;
                    craft->combat->direct = false;
                }
            }
        }
    };
    // FD-09: combat ends: the squadron leaves its cell and, holding a point, holds where its
    // leader will be this frame at its layer height, or, left without a target, the idle cell it
    // claims there (FM-24, walk 1 WSQ-14).
    const auto end_combat = [&](const EntityId container, SquadronState& state, const CraftView* leader) {
        state.cell.reset();
        state.joined = false;
        if (state.mode == SquadronMode::idle && leader != nullptr) {
            state.anchor = {math::Fixed::from_raw(leader->position.x.raw() + leader->state.velocity.x.raw()),
                math::Fixed::from_raw(leader->position.y.raw() + leader->state.velocity.y.raw()), leader->profile->layer_z};
            if (state.target == invalid_entity_id) claim_idle(container, state, state.anchor);
        }
    };
    // WU-25a: the cell a dogfight around `target` settles on.
    const auto search_cell = [&](const math::Vec3& target) {
        const CombatCell centre = combat_cell_of(target);
        const auto joined = [&](const CombatCell cell) {
            return std::any_of(minds.begin(), minds.end(),
                [&](const auto& entry) { return entry.second.joined && entry.second.cell == cell; });
        };
        CombatCell best_cell = centre;
        std::optional<math::detail::UInt192> best;
        for (std::int32_t y = centre.y - 1; y <= centre.y + 1; ++y) {
            for (std::int32_t x = centre.x - 1; x <= centre.x + 1; ++x) {
                const CombatCell cell{x, y};
                math::detail::UInt192 score = math::detail::from_u64(0);
                if (!joined(cell)) {
                    const auto point = combat_cell_point(cell, math::Fixed{});
                    const auto dx = math::detail::unsigned_magnitude(point.x.raw() - target.x.raw());
                    const auto dy = math::detail::unsigned_magnitude(point.y.raw() - target.y.raw());
                    score = math::detail::multiply_u64(dx, dx);
                    static_cast<void>(math::detail::add_magnitude(score, math::detail::multiply_u64(dy, dy)));
                }
                if (!best || math::detail::compare(score, *best) < 0) {
                    best = score;
                    best_cell = cell;
                }
            }
        }
        return best_cell;
    };
    // The squadrons' targets and dogfights (#457, FD-01 to FD-04, FD-09; foc-battle-world-ui
    // WU-25): serial in ascending container ID, the service order, since a squadron's cell and a
    // retaliation change what the squadrons after it see. O(squadrons) per frame with a short scan
    // of the joined squadrons each.
    for (auto& [container, frame] : squadron_frames) {
        auto& state = minds.at(container);
        const auto* leader = craft_view(frame.leader);
        // FD-09: a dead target gives way to the first enemy squadron joined in the recorded cell,
        // else combat ends.
        if (state.target != invalid_entity_id && live_unit(moving, state.target) == nullptr) {
            EntityId next = invalid_entity_id;
            const auto own_team = team_of(container);
            if (state.cell) {
                for (const auto& [other, other_state] : minds) {
                    if (other_state.joined && other_state.cell == state.cell && team_of(other) && own_team
                        && *team_of(other) != *own_team) {
                        next = other;
                        break;
                    }
                }
            }
            const auto previous = state.target;
            state.target = next;
            state.approach = next != invalid_entity_id;
            hand_target(container, previous, next);
            if (next == invalid_entity_id) end_combat(container, state, leader);
        }
        const auto* target = live_unit(moving, state.target);
        if (target == nullptr) {
            state.cell.reset();
            state.joined = false;
            continue;
        }
        frame.attacking = true;
        frame.target_position = target->state.position;
        const auto* footprint = impl_->motion.footprint(target->state.type_id);
        frame.target_radius = footprint != nullptr ? footprint->radius : math::Fixed{};
        // #424 (project, unverified): a squadron's team container counts as craft (FA-06).
        const EntityId target_squadron = squadron_of(state.target);
        frame.target_craft = squadron_table.find_craft(target->state.type_id) != nullptr
            || target_squadron != invalid_entity_id;
        // FA-01: whether the leader is within the strafe reach plus the target's radius (planar).
        bool in_reach = false;
        if (leader != nullptr) {
            in_reach = within_range(leader->position, target->state.position,
                math::Fixed::from_raw(std::max<std::int64_t>(0, leader->profile->strafe_distance.raw() + frame.target_radius.raw())),
                RangeMetric::planar);
        }
        // FA-07: the approach ends for good once the leader is within strafe reach (FA-01).
        if (state.approach && in_reach) state.approach = false;
        frame.approach = state.approach;
        // FD-01: a squadron fights a squadron in the combat grid while its leader is within reach or
        // while it records its target's cell; otherwise it leaves its cell (WU-25).
        const auto theirs = target_squadron != invalid_entity_id ? minds.find(target_squadron) : minds.end();
        const bool sharing = theirs != minds.end() && state.cell && theirs->second.cell && *state.cell == *theirs->second.cell;
        if (theirs == minds.end() || leader == nullptr || (!sharing && !in_reach)) {
            state.cell.reset();
            state.joined = false;
            continue;
        }
        if (!theirs->second.cell) {
            // FD-02: the target records no cell: record the searched one without joining it.
            state.cell = search_cell(target->state.position);
            state.joined = false;
            frame.dogfight = DogfightFlight::find_cell;
            continue;
        }
        // FD-03: join the target's cell.
        const CombatCell cell = *theirs->second.cell;
        state.cell = cell;
        state.joined = true;
        frame.dogfight = DogfightFlight::cell;
        frame.cell = cell;
        // FD-04: once a craft is near the cell point, a target squadron that is not on a move and
        // has no squadron target turns on this squadron.
        auto& other = theirs->second;
        bool near = false;
        for (const auto& squadron : impl_->squadrons) {
            if (squadron.container != container) continue;
            for (const auto member : squadron.members) {
                const auto* view = craft_view(member);
                near = near || (view != nullptr && within_combat_cell_reach(view->position, cell));
            }
        }
        if (near && other.mode != SquadronMode::move && squadron_of(other.target) == invalid_entity_id) {
            const auto previous = other.target;
            other.target = container;
            other.approach = true;
            hand_target(target_squadron, previous, container);
        }
    }
    const auto roster_offset = [](const SquadronFrame& frame, const EntityId craft) {
        const auto& roster = frame.state->roster;
        const auto slot = static_cast<std::size_t>(std::find(roster.begin(), roster.end(), craft) - roster.begin());
        return frame.profile != nullptr && slot < frame.profile->offsets.size() ? frame.profile->offsets[slot]
                                                                                : math::Vec3{};
    };

    // Chase pairing (#457, FD-05, FD-06). A run-out chase timer clears the chase, and a chased craft
    // that died is dropped (its timer runs on). Then every craft of a squadron fighting in a cell,
    // near the cell point and with its timer run out, looks for a craft to chase among the enemy
    // squadrons joined in its cell, in the cell's order (ascending container ID) and team order.
    // Workers list each such craft's followable candidates from the start-of-frame flight into
    // its own slot; the serial commit, in ascending craft ID (the service order), gives each craft
    // its first candidate whose timer has run out and restarts both timers, so a craft is chased
    // by one other at a time and a chased craft neither chases nor is taken for ten seconds.
    const auto frame_number = tick + 1;
    const auto squadron_frame_of = [&](const EntityId craft) -> const SquadronFrame* {
        const auto squadron = craft_squadron.find(craft);
        return squadron != craft_squadron.end() ? &squadron_frames.at(squadron->second) : nullptr;
    };
    std::vector<std::size_t> scanners;
    for (std::size_t index = 0; index < craft_views.size(); ++index) {
        auto& flight = craft_views[index].state;
        if (flight.chase_until != 0 && frame_number >= flight.chase_until) {
            flight.chase = invalid_entity_id;
            flight.chase_until = 0;
        }
        if (flight.chase != invalid_entity_id && craft_view(flight.chase) == nullptr) flight.chase = invalid_entity_id;
        const auto* frame = squadron_frame_of(craft_views[index].id);
        if (frame != nullptr && frame->dogfight == DogfightFlight::cell && flight.chase_until == 0
            && within_combat_cell_reach(craft_views[index].position, frame->cell)) {
            scanners.push_back(index);
        }
    }
    if (!scanners.empty()) {
        std::vector<std::vector<EntityId>> candidates(scanners.size());
        std::vector<std::optional<core::Diagnostic>> chase_errors(scanners.size());
        const auto listed = executor.execute_phase("dogfight-chases", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, scanners.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& self = craft_views[scanners[index]];
                const auto& cell = squadron_frame_of(self.id)->cell;
                const auto own = craft_squadron.at(self.id);
                const auto own_team = team_of(own);
                for (const auto& squadron : impl_->squadrons) {
                    const auto mind = minds.find(squadron.container);
                    if (mind == minds.end() || !mind->second.joined || mind->second.cell != cell) continue;
                    const auto team = team_of(squadron.container);
                    if (!team || !own_team || *team == *own_team) continue;
                    for (const auto member : mind->second.roster) {
                        const auto* view = craft_view(member);
                        if (view == nullptr) continue;
                        math::Fixed yaw;
                        math::Fixed pitch;
                        auto follows = in_follow_cone(self, view->position, yaw, pitch);
                        if (!follows) {
                            chase_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                                "tick " + std::to_string(tick) + ": " + follows.error().message);
                            break;
                        }
                        if (follows.value()) candidates[index].push_back(member);
                    }
                }
            }
        });
        if (!listed) {
            return core::Result<TacticalTick>::failure(listed.error());
        }
        for (auto& error : chase_errors) {
            if (error) {
                return core::Result<TacticalTick>::failure(std::move(*error));
            }
        }
        const auto mutable_view = [&craft_views](const EntityId id) -> CraftView* {
            const auto found = std::lower_bound(craft_views.begin(), craft_views.end(), id,
                [](const CraftView& view, const EntityId value) { return view.id < value; });
            return found != craft_views.end() && found->id == id ? &*found : nullptr;
        };
        for (std::size_t index = 0; index < scanners.size(); ++index) {
            auto& self = craft_views[scanners[index]].state;
            if (self.chase_until != 0) continue; // taken by an earlier craft this frame
            for (const auto candidate : candidates[index]) {
                auto* chased = mutable_view(candidate);
                if (chased->state.chase_until != 0) continue;
                self.chase = candidate;
                self.chase_until = frame_number + chase_frames;
                chased->state.chase = invalid_entity_id;
                chased->state.chase_until = frame_number + chase_frames;
                break;
            }
        }
    }
    // FD-10: the ships and static objects craft steer around, ascending ID, at the start of the frame.
    std::vector<CraftObstacle> obstacles;
    if (!craft_views.empty()) {
        for (const auto& unit : moving) {
            const auto* footprint = impl_->motion.footprint(unit.state.type_id);
            if (footprint == nullptr || footprint->layer == SpaceLayer::none || footprint->radius.raw() <= 0
                || craft_view(unit.state.entity_id) != nullptr || squadron_frames.contains(unit.state.entity_id)) {
                continue;
            }
            obstacles.push_back({unit.state.entity_id, unit.state.position, footprint->radius});
        }
    }

    std::vector<std::optional<CraftStep>> craft_steps(moving.size());
    std::vector<std::optional<core::Diagnostic>> move_errors(moving.size());
    const auto moved = executor.execute_phase("movement", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, moving.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& unit = moving[index];
            const auto squadron = craft_squadron.find(unit.state.entity_id);
            if (squadron != craft_squadron.end()) {
                // FM-01: a squadron craft flies by the fighter locomotor from the copied inputs.
                const auto& frame = squadron_frames.at(squadron->second);
                CraftFrame inputs;
                inputs.self = craft_view(unit.state.entity_id);
                inputs.leader = craft_view(frame.leader);
                inputs.own_offset = roster_offset(frame, unit.state.entity_id);
                inputs.leader_offset = roster_offset(frame, frame.leader);
                inputs.formation_tolerance = frame.profile != nullptr ? frame.profile->formation_tolerance : math::Fixed{};
                inputs.attacking = frame.attacking;
                inputs.target_position = frame.target_position;
                inputs.target_radius = frame.target_radius;
                inputs.target_craft = frame.target_craft;
                inputs.approach = frame.approach;
                inputs.hold = frame.hold;
                inputs.speed_factor = impl_->ability_factor(unit, AbilityModifier::speed);
                inputs.moving = frame.moving;
                inputs.lane = frame.lane;
                inputs.dogfight = frame.dogfight;
                inputs.cell = frame.cell;
                if (inputs.self->state.chase_until != 0) inputs.chase = craft_view(inputs.self->state.chase);
                inputs.obstacles = obstacles;
                auto stepped = step_craft(inputs);
                if (!stepped) {
                    move_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": "
                            + stepped.error().message);
                    continue;
                }
                unit.state.position = stepped.value().position;
                unit.state.rotation = stepped.value().rotation;
                craft_steps[index] = std::move(stepped).value();
                continue;
            }
            auto advanced = advance(unit, impl_->motion.find(unit.state.type_id), tick + 1);
            if (!advanced) {
                move_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id)
                        + ": " + advanced.error().message);
            }
        }
    });
    if (!moved) {
        return core::Result<TacticalTick>::failure(moved.error());
    }
    for (auto& error : move_errors) {
        if (error) {
            return core::Result<TacticalTick>::failure(std::move(*error));
        }
    }
    auto crafts = impl_->crafts;
    // DG-26: the defense each craft's locomotor set this frame, read by this frame's hits.
    std::map<EntityId, math::Fixed> craft_defense;
    // WU-25 (presentation, not hashed): the squadrons whose leader closes on its target (FA-01).
    std::set<EntityId> closing_squadrons;
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (craft_steps[index]) {
            crafts[moving[index].state.entity_id] = craft_steps[index]->state;
            if (craft_steps[index]->closing) {
                const auto squadron = craft_squadron.find(moving[index].state.entity_id);
                if (squadron != craft_squadron.end()
                    && squadron_frames.at(squadron->second).leader == moving[index].state.entity_id) {
                    closing_squadrons.insert(squadron->second);
                }
            }
            if (craft_steps[index]->defense.raw() != 0) {
                craft_defense.emplace(moving[index].state.entity_id, craft_steps[index]->defense);
            }
        }
    }

    // Targeting phase (#73): after movement and before this tick's commands, so an attack order
    // acts from the next frame like a move. Workers read one immutable world view (the moved
    // units, their health and the last published visibility) and write each unit's combat state
    // and events to its own slot; the serial commit below appends the events in ascending ID.
    auto world = impl_->combat_world(moving, starts, tick);
    if (!world) {
        return core::Result<TacticalTick>::failure(world.error());
    }
    // CO-11: workers compute each unit's box into its own slot; the collection trees then take the
    // boxes serially in ascending ID, as FoC's trees take each object's transform update in turn
    // (the tree's order is its history, so this commit is ordered by nature: O(units) with a short
    // tree walk each). The targeting phase only reads the trees.
    auto collection = impl_->collection;
    if (!impl_->combat.profiles.empty()) {
        std::vector<std::optional<detail::CollectionTrees::Member>> boxes(moving.size());
        std::vector<std::optional<core::Diagnostic>> box_errors(moving.size());
        const auto boxed = executor.execute_phase("collection-boxes", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto member = impl_->collection_member(moving[index]);
                if (!member) {
                    box_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(moving[index].state.entity_id) + ": "
                            + member.error().message);
                    continue;
                }
                boxes[index] = member.value();
            }
        });
        if (!boxed) {
            return core::Result<TacticalTick>::failure(boxed.error());
        }
        for (auto& error : box_errors) {
            if (error) {
                return core::Result<TacticalTick>::failure(std::move(*error));
            }
        }
        std::vector<detail::CollectionTrees::Member> members;
        members.reserve(moving.size());
        for (const auto& box : boxes) {
            if (box) members.push_back(*box);
        }
        collection.update(members, tick);
        world.value().collection = &collection;
    }
    const auto& view = world.value();

    // Squadron target phase (#75, FT-01 to FT-04): workers scan per squadron from the same
    // immutable world view into disjoint slots. The serial commit, in ascending container ID,
    // gives each squadron's craft its target as a direct target (FT-05) before they fire.
    if (!squadron_frames.empty() && !impl_->combat.profiles.empty()) {
        std::vector<std::pair<EntityId, SquadronFrame*>> scanning;
        for (auto& [container, frame] : squadron_frames) {
            // FO-01, FO-05, FO-06: whether a squadron on a player order looks for targets.
            if (frame.profile != nullptr && view.find(frame.leader) != nullptr && frame.scans) {
                scanning.emplace_back(container, &frame);
            }
        }
        std::vector<detail::SquadronScan> scans(scanning.size());
        const auto scanned = executor.execute_phase("squadron-targets", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, scanning.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& frame = *scanning[index].second;
                const auto& leader = *view.find(frame.leader);
                scans[index] = detail::squadron_target(
                    view, *frame.state, *frame.profile, leader, frame.scan_from_leader ? leader.position : frame.hold);
            }
        });
        if (!scanned) {
            return core::Result<TacticalTick>::failure(scanned.error());
        }
        for (std::size_t index = 0; index < scanning.size(); ++index) {
            auto& state = minds.at(scanning[index].first);
            const auto previous = state.target;
            state.target = scans[index].target;
            state.next_scan_frame = scans[index].next_scan_frame;
            // FA-07: a new target starts an approach; none ends it.
            if (state.target != previous) state.approach = state.target != invalid_entity_id;
            // FD-09: a target lost from sight ends combat.
            if (previous != invalid_entity_id && state.target != previous) {
                end_combat(scanning[index].first, state, craft_view(scanning[index].second->leader));
            }
            if (state.target == invalid_entity_id && previous == invalid_entity_id) continue;
            hand_target(scanning[index].first, previous, state.target);
        }
    }
    std::vector<std::optional<detail::CombatStep>> combat_steps(moving.size());
    std::vector<std::optional<core::Diagnostic>> combat_errors(moving.size());
    const auto targeted = executor.execute_phase("targeting", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, moving.size());
        for (auto index = range.begin; index < range.end; ++index) {
            if (!moving[index].combat) {
                continue;
            }
            // FT-07: a craft is idle while its squadron (committed above) has no target.
            auto unit = view.units[index];
            const auto squadron = craft_squadron.find(unit.id);
            unit.squadron_idle = squadron != craft_squadron.end() && minds.at(squadron->second).target == invalid_entity_id;
            auto stepped = detail::step_combat(view, unit);
            if (!stepped) {
                combat_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " unit " + std::to_string(moving[index].state.entity_id) + ": "
                        + stepped.error().message);
                continue;
            }
            combat_steps[index] = std::move(stepped).value();
        }
    });
    if (!targeted) {
        return core::Result<TacticalTick>::failure(targeted.error());
    }
    for (auto& error : combat_errors) {
        if (error) {
            return core::Result<TacticalTick>::failure(std::move(*error));
        }
    }
    std::vector<CombatEvent> combat_events;
    std::vector<math::Vec3> aim_offsets; // per weapon_fired event, in order (MS-03)
    // A-04 to A-07: the turns toward ordered targets, in ascending unit ID; planned below.
    std::vector<std::pair<EntityId, math::Vec3>> combat_turns;
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (!combat_steps[index]) {
            continue;
        }
        if (combat_steps[index]->face) {
            combat_turns.emplace_back(moving[index].state.entity_id, *combat_steps[index]->face);
        }
        moving[index].combat = std::move(combat_steps[index]->state);
        // EN-05: what the object weapon fired is paid from the pool it was checked against.
        if (combat_steps[index]->energy_spent.raw() > 0 && moving[index].durability) {
            auto& pool = moving[index].durability->energy;
            pool = math::Fixed::from_raw(pool.raw() - combat_steps[index]->energy_spent.raw());
        }
        auto& produced = combat_steps[index]->events;
        // AB-67 (#561): a craft that fired its ion shot at the target has fired it.
        for (const auto& event : produced) {
            if (event.kind != CombatEventKind::weapon_fired || (event.outcome & fired_ability_shot) == 0U) continue;
            if (auto* slot = impl_->ion_slot(moving[index])) slot->active = false;
        }
        combat_events.insert(combat_events.end(), produced.begin(), produced.end());
        const auto& offsets = combat_steps[index]->aim_offsets;
        aim_offsets.insert(aim_offsets.end(), offsets.begin(), offsets.end());
    }

    // AB-63 to AB-68 (#561): each squadron's ION_CANNON_SHOT, in container order. Serial because
    // it reads and writes a container and its craft together and orders the squadron, which the
    // tick does serially (FO-01); it visits only squadrons whose team type has the ability.
    const auto lock_ion_shot = [&](LiveUnit& container, AbilitySlot& team, const Squadron& squadron,
                                   SquadronState& mind, const EntityId target, const std::uint32_t hardpoint) {
        team.active = true;
        team.started_tick = tick;
        team.target = target;
        team.target_hardpoint = hardpoint;
        for (const auto member : squadron.members) {
            auto* craft = live_unit(moving, member);
            if (auto* slot = craft != nullptr ? impl_->ion_slot(*craft) : nullptr) slot->active = true;
        }
        // AB-63: the squadron attacks the target.
        const CommandPayload attack = AttackPayload{target};
        container.state.order = order_for(attack, tick);
        apply_squadron_order(mind, attack, container.state.position, squadron_table);
    };
    for (const auto& squadron : impl_->squadrons) {
        auto* container = live_unit(moving, squadron.container);
        auto* team = container != nullptr ? impl_->ion_slot(*container) : nullptr;
        const auto mind = minds.find(squadron.container);
        if (team == nullptr || mind == minds.end()) continue;
        std::vector<AbilitySlot*> craft;
        for (const auto member : squadron.members) {
            auto* live = live_unit(moving, member);
            if (auto* slot = live != nullptr ? impl_->ion_slot(*live) : nullptr) craft.push_back(slot);
        }
        if (team->active) {
            // AB-64, AB-65: it ends once no craft still has its shot due, or its target is gone.
            const bool due = std::any_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; });
            if (!due || !impl_->ion_target_valid(container->state.owner, live_unit(moving, team->target))) {
                impl_->finish_ion_shot(*team, craft, container->state.type_id, tick);
            } else if (mind->second.target != team->target) {
                // AB-63: while it is on, the squadron keeps attacking the target.
                lock_ion_shot(*container, *team, squadron, mind->second, team->target, team->target_hardpoint);
            }
            continue;
        }
        // AB-68: on autofire (always for a player the engine plays, AB-40), a ready ion shot locks
        // onto the squadron's attack target.
        const bool autofire = team->autofire || !impl_->abilities.human(container->state.owner);
        const auto target = mind->second.target;
        if (autofire && tick >= team->ready_tick && !craft.empty()
            && std::none_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; })
            && impl_->ion_target_valid(container->state.owner, live_unit(moving, target))) {
            lock_ion_shot(*container, *team, squadron, mind->second, target, no_hardpoint);
        }
    }

    // Orders phase (#452, docs/behaviour/space-orders.md OR-06): every reevaluation interval after
    // its order, a unit approaching another (attack, attack-move or guard on a unit) checks whether
    // it keeps its movement. Workers read the moved units with this tick's targets and write each
    // unit's new approach to its own slot; the paths plan serially in ascending ID after the
    // attack turns, like every plan with avoidance (AV-15).
    std::vector<std::optional<Impl::ApproachPlan>> approach_plans(moving.size());
    bool approaches_due = false;
    for (const auto& unit : moving) {
        approaches_due = approaches_due || impl_->approach_due(unit, tick);
    }
    if (approaches_due) {
        const auto find_live = [&moving](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(moving.begin(), moving.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != moving.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        std::vector<std::optional<core::Diagnostic>> approach_errors(moving.size());
        const auto approached = executor.execute_phase("orders", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = moving[index];
                if (!impl_->approach_due(unit, tick)) continue;
                const auto target_id = Impl::approach_target(unit);
                const auto* target = find_live(target_id);
                if (target == nullptr) continue;
                // A target fogged to the unit's owner is not followed (research E452-11).
                const auto* target_view = view.find(target_id);
                const auto* unit_view = view.find(unit.state.entity_id);
                if (target_view != nullptr && unit_view != nullptr && target_view->team != unit_view->team
                    && (target_view->visible_to & (std::uint64_t{1} << unit_view->player_index)) == 0U) {
                    continue;
                }
                auto checked = impl_->approach_check(unit, *target, target_view, tick + 1);
                if (!checked) {
                    approach_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, "tick "
                        + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": "
                        + checked.error().message);
                    continue;
                }
                approach_plans[index] = checked.value();
            }
        });
        if (!approached) {
            return core::Result<TacticalTick>::failure(approached.error());
        }
        for (auto& error : approach_errors) {
            if (error) {
                return core::Result<TacticalTick>::failure(std::move(*error));
            }
        }
    }
    std::vector<std::pair<EntityId, Impl::ApproachPlan>> approaches;
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (approach_plans[index]) approaches.emplace_back(moving[index].state.entity_id, *approach_plans[index]);
    }

    // Projectile phase (#74, DG-30): with damage rules, workers fly every projectile that was in
    // flight at the start of the tick one frame through the same immutable world view and write
    // its outcome to its own slot. The shots of this tick start flying next tick (DG-21).
    const auto* damage_rules = durability.damage ? &*durability.damage : nullptr;
    const auto& flying = impl_->projectiles;
    std::vector<std::optional<detail::ProjectileStep>> flights(flying.size());
    std::vector<std::optional<core::Diagnostic>> flight_errors(flying.size());
    std::uint64_t projectile_candidates = 0;
    std::uint64_t projectile_exact_tests = 0;
    if (!flying.empty()) {
        const auto flown = executor.execute_phase("projectiles", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, flying.size());
            auto& scratch = impl_->projectile_scratch[partition]; // #636: only this partition's
            scratch.candidate_count = 0;
            scratch.exact_count = 0;
            for (auto index = range.begin; index < range.end; ++index) {
                auto stepped = detail::step_projectile(view, flying[index], impl_->teams.at(flying[index].owner), scratch);
                if (!stepped) {
                    flight_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, "tick "
                        + std::to_string(tick) + " projectile " + std::to_string(flying[index].id) + ": "
                        + stepped.error().message);
                    continue;
                }
                flights[index] = std::move(stepped).value();
            }
        });
        if (!flown) {
            return core::Result<TacticalTick>::failure(flown.error());
        }
        for (auto& error : flight_errors) {
            if (error) {
                return core::Result<TacticalTick>::failure(std::move(*error));
            }
        }
        for (const auto& scratch : impl_->projectile_scratch) {
            projectile_candidates += scratch.candidate_count;
            projectile_exact_tests += scratch.exact_count;
        }
    }
    // This tick's shots become projectiles in event order (ascending shooter, then weapon).
    std::vector<Projectile> launched;
    auto next_projectile = impl_->next_projectile;
    if (damage_rules != nullptr) {
        std::size_t shot_index = 0;
        for (const auto& event : combat_events) {
            if (event.kind != CombatEventKind::weapon_fired) {
                continue;
            }
            const auto offset = shot_index < aim_offsets.size() ? aim_offsets[shot_index] : math::Vec3{};
            ++shot_index;
            const auto* shooter = view.find(event.shooter);
            const auto& weapons = shooter->profile->weapons;
            const auto weapon = std::find_if(weapons.begin(), weapons.end(),
                [&](const WeaponProfile& entry) { return entry.hardpoint == event.weapon; });
            // AB-66 (#561): an ion shot flies the weapon's ability shot.
            if (weapon == weapons.end()) continue;
            const auto& shot = (event.outcome & fired_ability_shot) != 0U ? weapon->ability_shot : weapon->shot;
            if (!shot) continue;
            // DG-05: the shooter's mode flag, on by default and only off without a durability
            // profile's data saying otherwise; no modelled ability changes it (space-abilities AB-25).
            const bool allow_diminishing_firepower =
                shooter->durability_profile == nullptr || shooter->durability_profile->allow_diminishing_firepower;
            auto projectile = detail::launch_projectile(
                event, *shot, shooter->owner, allow_diminishing_firepower, next_projectile++, offset);
            if (!projectile) {
                return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " unit " + std::to_string(event.shooter) + ": "
                        + projectile.error().message));
            }
            launched.push_back(projectile.value());
        }
    }
    // Ability phase (#76, AB-11, AB-16, AB-41, AB-42): workers service each unit's abilities in its
    // own slot: time limits, lost engines, the damage-rate window and the DEFEND stand-in. The
    // serial commit lists, ascending, the moving ships whose speed multiplier changed: their move
    // plans again this tick, before the commands (AB-24).
    // IS-03, IS-05 (#561): the same phase ends each ion stun at its end frame.
    std::vector<EntityId> replans;
    const bool any_stunned = std::any_of(moving.begin(), moving.end(), [](const LiveUnit& unit) { return unit.ion_stun.has_value(); });
    if (!impl_->abilities.profiles.empty() || any_stunned) {
        std::vector<std::uint8_t> speed_changed(moving.size());
        const auto serviced_abilities = executor.execute_phase("abilities", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                bool speed = false;
                if (moving[index].abilities) speed = impl_->service_abilities(moving[index], tick);
                if (moving[index].ion_stun) Impl::service_ion_stun(moving[index], tick);
                speed_changed[index] = speed ? 1U : 0U;
            }
        });
        if (!serviced_abilities) {
            return core::Result<TacticalTick>::failure(serviced_abilities.error());
        }
        for (std::size_t index = 0; index < moving.size(); ++index) {
            const auto& unit = moving[index];
            if (speed_changed[index] != 0U && unit.motion && unit.motion->kind == MotionKind::path && !unit.formation) {
                replans.push_back(unit.state.entity_id);
            }
        }
    }
    // Tracking phase (#71, AV-02, AV-03): with avoidance rules and a move due this tick,
    // workers sample each tracked moving unit's prediction at the window boundaries of its
    // layer, from the layer's rolled anchor and from this frame, into disjoint slots. The
    // planning below stays serial in command order (AV-15).
    const auto& table = impl_->motion;
    const std::uint64_t frame = tick + 1;
    std::array<bool, 4> rebuilt{};
    std::optional<Tracking> tracking;
    std::vector<std::pair<std::size_t, EntityId>> members_before;
    if (table.avoidance) {
        members_before = layer_members(table, moving);
        // A speed change replans a move (AB-24): a due ability command may cause one.
        bool moves_due = !replans.empty();
        for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end() && iterator->first.tick == tick;
             ++iterator) {
            const auto& payload = iterator->second.payload;
            moves_due = moves_due || point_move(payload).has_value() || approach_target_of(payload) != invalid_entity_id
                || std::holds_alternative<AbilityPayload>(payload);
        }
        for (const auto& unit : moving) {
            moves_due = moves_due || (unit.formation && unit.formation->frame <= frame);
        }
        moves_due = moves_due || !approaches.empty();
        if (moves_due) {
            tracking.emplace();
            tracking->frame = frame;
            tracking->interval = table.avoidance->tracking_interval;
            tracking->windows = table.avoidance->tracking_windows;
            for (std::size_t index = 0; index < 4; ++index) {
                tracking->current_start[index] = rolled_start(impl_->tracking_anchor[index], frame, tracking->interval);
            }
            std::vector<TrackSamples> sampled(moving.size());
            std::vector<std::optional<core::Diagnostic>> sample_errors(moving.size());
            // PC-09: the plans of sliced searches that have ended, by unit, while the unit waits for them.
            std::vector<const Impl::SlicedSearch*> published(moving.size());
            for (std::size_t index = 0; index < moving.size(); ++index) {
                const auto& unit = moving[index];
                if (!unit.formation || !unit.formation->sliced) continue;
                const auto found = impl_->searches.find(unit.state.entity_id);
                if (found != impl_->searches.end() && found->second.plan) published[index] = &found->second;
            }
            const auto tracked = executor.execute_phase("tracking", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, moving.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto& unit = moving[index];
                    const auto* footprint = tracked_footprint(table, unit.state.type_id);
                    const bool moves = unit.motion && unit.motion->kind != MotionKind::none;
                    if (footprint == nullptr || footprint->obstacle || (!moves && published[index] == nullptr)) {
                        continue;
                    }
                    const auto layer = dynamic_layer_index(footprint->layer);
                    if (!layer) continue;
                    const auto fail = [&](const core::Diagnostic& error) {
                        sample_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, "tick "
                            + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": " + error.message);
                    };
                    const auto yaw = yaw_degrees(unit.state.rotation);
                    if (!yaw) {
                        fail(yaw.error());
                        continue;
                    }
                    const auto start = tracking->current_start[*layer];
                    // A waiting group member is predicted up to its plan frame (FM-10); a ship
                    // whose sliced search has ended, along that search's plan after it (PC-09).
                    const auto clip = unit.formation ? std::optional(unit.formation->frame) : std::nullopt;
                    const auto* ahead = published[index];
                    const auto sample = [&](const std::uint64_t from) {
                        if (ahead != nullptr) {
                            return sample_ahead(moves ? &*unit.motion : nullptr, ahead->frame, *ahead->plan, from,
                                tracking->interval, tracking->windows, unit.state.position, yaw.value());
                        }
                        return sample_windows(
                            *unit.motion, from, tracking->interval, tracking->windows, unit.state.position, yaw.value(), clip);
                    };
                    auto current = sample(start);
                    if (!current) {
                        fail(current.error());
                        continue;
                    }
                    sampled[index].current = std::move(current).value();
                    if (start != frame) {
                        auto fresh = sample(frame);
                        if (!fresh) {
                            fail(fresh.error());
                            continue;
                        }
                        sampled[index].fresh = std::move(fresh).value();
                    }
                }
            });
            if (!tracked) {
                return core::Result<TacticalTick>::failure(tracked.error());
            }
            for (std::size_t index = 0; index < moving.size(); ++index) {
                if (sample_errors[index]) {
                    return core::Result<TacticalTick>::failure(std::move(*sample_errors[index]));
                }
                if (!sampled[index].current.empty()) {
                    tracking->samples.emplace(moving[index].state.entity_id, std::move(sampled[index]));
                }
            }
        }
    }

    std::map<EntityId, LiveUnit> staged;
    for (auto& unit : moving) {
        const auto id = unit.state.entity_id;
        staged.emplace(id, std::move(unit));
    }

    // Serial, in ascending projectile ID: each hit applies to the staged unit it reached (DG-01 to
    // DG-11). A unit killed by an earlier hit leaves at once; a later projectile that reached it
    // is spent without effect.
    std::vector<Event> events;
    // #447: every unit killed this tick as it stood when it died, in destruction order.
    std::vector<UnitState> killed;
    std::vector<Projectile> projectiles;
    projectiles.reserve(flights.size() + launched.size());
    for (auto& flight : flights) {
        if (!flight->hit) {
            if (!flight->expired) {
                projectiles.push_back(flight->projectile);
            }
            continue;
        }
        const auto target = staged.find(*flight->hit);
        if (target == staged.end() || !target->second.durability) {
            continue;
        }
        const auto& projectile = flight->projectile;
        const auto& profile = *durability.find(target->second.state.type_id);
        // DG-11: with meshes, the hardpoint whose collision mesh the projectile met takes it (#536),
        // or for a shot aimed at a hardpoint of the unit it reached, the hardpoint its aim names (DG-39);
        // with the box alone, the hardpoint the shot aimed at when it reached that unit.
        auto hardpoint = hull_target;
        if (flight->meshed) {
            if (flight->mesh_hardpoint != no_hardpoint && damage_target_valid(profile, flight->mesh_hardpoint)) {
                hardpoint = flight->mesh_hardpoint;
            }
        } else if (*flight->hit == projectile.target && projectile.target_hardpoint != no_hardpoint
            && damage_target_valid(profile, projectile.target_hardpoint)) {
            hardpoint = projectile.target_hardpoint;
        }
        const auto defense = craft_defense.find(*flight->hit);
        const Hit hit{projectile.damage, projectile.damage_type, true, projectile.shield_damage,
            projectile.hitpoint_damage, hardpoint, projectile.allow_diminishing_firepower,
            projectile.internal_damage_misc, defense != craft_defense.end() ? defense->second : math::Fixed{},
            projectile.energy_damage};
        const auto hull_before = target->second.durability->hull;
        const auto shields_before = target->second.durability->shields;
        auto outcome = apply_hit(profile, *damage_rules, *target->second.durability, hit, tick);
        if (!outcome) {
            return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                "tick " + std::to_string(tick) + " projectile " + std::to_string(projectile.id) + ": "
                    + outcome.error().message));
        }
        impl_->track_damage(target->second, hull_before, shields_before);
        impl_->end_depleted_defend(target->second, tick);
        // IS-01, IS-02: an ion shot stuns a unit whose type has the ion-stun behaviour.
        if (projectile.ion_stun && profile.ion_stun_effect && !outcome.value().damage.unit_destroyed) {
            impl_->ion_stun_unit(target->second, *projectile.ion_stun, tick);
        }
        // How the hit was taken, for the impact effect only (#80, docs/behaviour/battle-presentation.md).
        std::uint32_t taken = 0;
        if (outcome.value().shield_absorbed) {
            taken |= hit_outcome_shield_absorbed;
        } else if (outcome.value().armor_multiplier <= armor_reduced_limit) {
            taken |= hit_outcome_armor_reduced;
        }
        combat_events.push_back(CombatEvent{tick, CombatEventKind::projectile_hit, projectile.shooter,
            projectile.weapon, *flight->hit, hardpoint == hull_target ? no_hardpoint : hardpoint, flight->from,
            flight->contact, taken});
        const auto& result = outcome.value().damage;
        if (result.destroyed_hardpoint) {
            events.push_back(destruction_event(
                tick, EventKind::hardpoint_destroyed, target->second.state, *result.destroyed_hardpoint));
        }
        if (result.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, target->second.state));
            killed.push_back(target->second.state);
            staged.erase(target);
        }
    }
    projectiles.insert(projectiles.end(), launched.begin(), launched.end());

    // A plan submits the unit's new prediction: its layer rebuilds from this frame (AV-02,
    // AV-15). `clip`: a waiting group member's plan frame, the end of its prediction (FM-10).
    const auto submit = [&](const EntityId unit_id, const LiveUnit& live, const std::size_t layer,
                            const std::optional<std::uint64_t> clip) -> core::Result<void> {
        rebuilt[layer] = true;
        if (!tracking) return core::Result<void>::success();
        tracking->views[layer].reset();
        tracking->samples.erase(unit_id);
        if (live.motion->kind != MotionKind::none) {
            const auto yaw = yaw_degrees(live.state.rotation);
            if (!yaw) return core::Result<void>::failure(yaw.error());
            auto fresh = sample_windows(*live.motion, frame, tracking->interval, tracking->windows,
                live.state.position, yaw.value(), clip);
            if (!fresh) return core::Result<void>::failure(fresh.error());
            TrackSamples samples;
            samples.current = fresh.value();
            samples.fresh = std::move(fresh).value();
            tracking->samples.emplace(unit_id, std::move(samples));
        }
        return core::Result<void>::success();
    };
    // PC-09: `live` waits for `search`, which has ended: its layer predicts the search's plan
    // from the landing on (sample_ahead). The layer's windows keep their anchor: the landing,
    // not the search's end, is the unit's submission (PC-08), so when the search ends (which
    // depends on the slice size) changes only what a later search reads before the landing.
    const auto publish = [&](const EntityId unit_id, const LiveUnit& live, const Impl::SlicedSearch& search) -> core::Result<void> {
        if (!tracking) return core::Result<void>::success();
        const auto* footprint = tracked_footprint(table, live.state.type_id);
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        if (!layer) return core::Result<void>::success();
        tracking->views[*layer].reset();
        tracking->samples.erase(unit_id);
        const auto yaw = yaw_degrees(live.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const MotionState* current = live.motion && live.motion->kind != MotionKind::none ? &*live.motion : nullptr;
        const auto sample = [&](const std::uint64_t from) {
            return sample_ahead(current, search.frame, *search.plan, from, tracking->interval, tracking->windows,
                live.state.position, yaw.value());
        };
        const auto start = tracking->current_start[*layer];
        auto from_anchor = sample(start);
        if (!from_anchor) return core::Result<void>::failure(from_anchor.error());
        TrackSamples samples;
        samples.current = std::move(from_anchor).value();
        if (start != frame) {
            auto fresh = sample(frame);
            if (!fresh) return core::Result<void>::failure(fresh.error());
            samples.fresh = std::move(fresh).value();
        }
        tracking->samples.emplace(unit_id, std::move(samples));
        return core::Result<void>::success();
    };
    // The collision world a move in dynamic layer `layer` plans against (AV-01).
    const auto world_for = [&](const std::size_t layer, CollisionWorld& collisions) -> core::Result<void> {
        if (auto ensured = ensure_view(*tracking, table, staged, rebuilt, layer); !ensured) return ensured;
        if (auto statics = ensure_statics(*tracking, table, staged); !statics) return statics;
        collisions.interval = tracking->interval;
        collisions.layers[layer] = &*tracking->views[layer];
        collisions.statics = &*tracking->statics;
        return core::Result<void>::success();
    };
    const auto unit_failure = [&](const EntityId unit_id, const core::Diagnostic& error) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "tick " + std::to_string(tick) + " unit " + std::to_string(unit_id) + ": " + error.message));
    };

    // The tick's path searches (AV-15, FM-07, FM-12; PC-05, PC-07, PC-08): queued in their
    // planning order and planned when something else is about to read or change a layer (a
    // group's slot mapping, a face, stop or other plan, a non-move command) and after the
    // commands. A search reads only its own layer and the static layer (AV-01), so each dynamic
    // layer is a lane: its jobs run in order, each submitting before the next, and the lanes run
    // side by side, the next search of every lane together in the partitioned `plan-searches`
    // phase. That is exactly the serial result. A job is one of three:
    // - a sliced search landing this frame (`landing`): the unit takes its plan (PC-08);
    // - a search, while its layer has spent fewer expansions this tick than the avoidance rules'
    //   search_budget (PC-07): it may spend the rest of the budget, and a search that reaches it
    //   is given up and starts again sliced;
    // - once the budget is spent, a sliced search from the start: it lands search_delay frames
    //   later, and the unit keeps its current plan until then (PC-04), predicted up to that frame.
    // The first failed job in planning order answers.
    struct PlanJob {
        EntityId unit{};
        math::Vec3 destination{};
        std::optional<math::Fixed> max_speed;
        std::size_t layer{};
        CommandKey order{};
        std::uint32_t rank{};
        bool landing{};
    };
    std::vector<PlanJob> pending;
    std::array<std::uint64_t, 4> spent{};
    // PC-08: `job`'s search runs in slices from now on; its unit waits for the landing.
    const auto start_sliced = [&](const PlanJob& job) -> core::Result<void> {
        auto& live = staged.at(job.unit);
        CollisionWorld world;
        if (auto built = world_for(job.layer, world); !built) return built;
        const auto landing = frame + table.avoidance->search_delay;
        auto sliced = impl_->start_sliced(live, job.destination, job.max_speed, frame, landing, world);
        if (!sliced) return core::Result<void>::failure(sliced.error());
        // The wait's maximum speed is hashed only: a unit's own caps nothing it would not cap itself.
        auto max_speed = job.max_speed;
        if (!max_speed) max_speed = sliced.value().inputs.limits.max_speed;
        impl_->searches.insert_or_assign(job.unit, std::move(sliced).value());
        live.formation = FormationWait{landing, job.destination, *max_speed, job.order, job.rank, true};
        return submit(job.unit, live, job.layer, landing);
    };
    // PC-08: the unit takes its sliced search's plan, while it still plans from what the search
    // planned from; otherwise (a change the prediction could not see) it plans now.
    const auto land = [&](const PlanJob& job) -> core::Result<void> {
        auto& live = staged.at(job.unit);
        auto found = impl_->searches.find(job.unit);
        if (found == impl_->searches.end()) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure, "no sliced search to land"));
        }
        const auto inputs = impl_->path_inputs(live, found->second.max_speed);
        if (!inputs) return core::Result<void>::failure(inputs.error());
        if (inputs.value() == found->second.inputs && found->second.search.ended()) {
            auto plan = found->second.search.result();
            if (!plan) return core::Result<void>::failure(plan.error());
            Impl::take_plan(live, std::move(plan).value());
        } else {
            CollisionWorld world;
            if (auto built = world_for(job.layer, world); !built) return built;
            if (auto planned = impl_->plan_path(live, found->second.target, frame, &world, found->second.max_speed, nullptr);
                !planned) {
                return planned;
            }
        }
        impl_->searches.erase(found);
        return submit(job.unit, live, job.layer, std::nullopt);
    };
    const auto plan_pending = [&]() -> std::optional<std::pair<EntityId, core::Diagnostic>> {
        const auto budget = table.avoidance->search_budget;
        std::array<std::vector<std::size_t>, 4> lanes;
        for (std::size_t index = 0; index < pending.size(); ++index) lanes[pending[index].layer].push_back(index);
        std::array<std::size_t, 4> next{};
        while (true) {
            std::vector<std::size_t> round;
            for (std::size_t layer = 0; layer < lanes.size(); ++layer) {
                if (next[layer] < lanes[layer].size()) round.push_back(lanes[layer][next[layer]++]);
            }
            if (round.empty()) break;
            std::sort(round.begin(), round.end());
            const std::size_t count = round.size();
            // Which jobs search now, each within the rest of its layer's budget.
            std::vector<char> searches(count);
            std::vector<std::size_t> searching;
            for (std::size_t index = 0; index < count; ++index) {
                const auto& job = pending[round[index]];
                searches[index] = !job.landing && spent[job.layer] < budget ? 1 : 0;
                if (searches[index] != 0) searching.push_back(index);
            }
            std::vector<CollisionWorld> worlds(count);
            std::vector<LiveUnit*> units(count);
            std::vector<PathSearchStats> work(count);
            std::vector<char> planned(count);
            std::vector<std::optional<core::Diagnostic>> errors(count);
            for (const auto index : searching) {
                const auto& job = pending[round[index]];
                units[index] = &staged.at(job.unit);
                if (auto built = world_for(job.layer, worlds[index]); !built) errors[index] = built.error();
            }
            const auto search = [&](const std::size_t index) {
                if (errors[index]) return;
                const auto& job = pending[round[index]];
                auto result = impl_->plan_path_within(*units[index], job.destination, frame, &worlds[index], job.max_speed,
                    &work[index], budget - spent[job.layer]);
                if (!result) {
                    errors[index] = result.error();
                    return;
                }
                planned[index] = result.value() ? 1 : 0;
            };
            if (searching.size() == 1) {
                search(searching.front());
            } else if (!searching.empty()) {
                const auto searched = executor.execute_phase("plan-searches", tick_partition_count, [&](const std::size_t partition) {
                    const auto range = partition_range(partition, searching.size());
                    for (auto index = range.begin; index < range.end; ++index) search(searching[index]);
                });
                if (!searched) return std::pair{pending[round[searching.front()]].unit, searched.error()};
            }
            for (std::size_t index = 0; index < count; ++index) {
                const auto& job = pending[round[index]];
                if (errors[index]) return std::pair{job.unit, std::move(*errors[index])};
                if (job.landing) {
                    if (auto landed = land(job); !landed) return std::pair{job.unit, landed.error()};
                    continue;
                }
                if (searches[index] != 0) {
                    spent[job.layer] += work[index].expansions;
                    if (planned[index] != 0) {
                        if (auto submitted = submit(job.unit, *units[index], job.layer, std::nullopt); !submitted) {
                            return std::pair{job.unit, submitted.error()};
                        }
                        continue;
                    }
                }
                if (auto sliced = start_sliced(job); !sliced) return std::pair{job.unit, sliced.error()};
            }
        }
        pending.clear();
        return std::nullopt;
    };

    // FO-07 to FO-10 (#552, #599): squadrons one command sends to the same point take their
    // formation's slots around it, each at its own layer height, and its lanes. Serial, once per
    // command.
    const auto spread_squadrons = [&](const std::vector<EntityId>& squadrons) -> core::Result<void> {
        std::vector<GroupSquadron> members;
        members.reserve(squadrons.size());
        for (const auto id : squadrons) {
            const auto& state = minds.at(id);
            GroupSquadron member;
            member.position = staged.at(id).state.position;
            member.type = state.squadron_type;
            if (const auto* squadron = squadron_table.find_squadron(state.squadron_type);
                squadron != nullptr && !squadron->members.empty()) {
                if (const auto* craft = squadron_table.find_craft(squadron->members.front())) {
                    member.max_speed = craft->max_speed;
                    member.min_speed = craft->min_speed;
                    member.attack_distance = craft->attack_distance;
                }
                // FO-08: the farthest slot plus the craft's soft radius.
                math::Fixed farthest{};
                for (const auto& offset : squadron->offsets) {
                    auto reach = math::length(offset);
                    if (!reach) return core::Result<void>::failure(reach.error());
                    farthest = std::max(farthest, reach.value());
                }
                const auto* footprint = impl_->motion.footprint(squadron->members.front());
                member.radius = math::Fixed::from_raw(farthest.raw() + (footprint != nullptr ? footprint->radius.raw() : 0));
            }
            members.push_back(member);
        }
        // Every squadron of the command flies to the same point; the slots keep its x and y.
        const auto destination = minds.at(squadrons.front()).anchor;
        auto slots = squadron_group_slots(members, destination);
        if (!slots) return core::Result<void>::failure(slots.error());
        for (std::size_t index = 0; index < squadrons.size(); ++index) {
            auto& state = minds.at(squadrons[index]);
            const auto& slot = slots.value()[index];
            state.anchor = {slot.point.x, slot.point.y, state.anchor.z};
            if (slot.lane) {
                // FO-10: the squadron flies its formation's path and its move ends at the line
                // through its slot square to that path (FO-02).
                state.lane = *slot.lane;
                state.lane->formation = squadrons[static_cast<std::size_t>(slot.lane->formation)];
                state.move_origin = {math::Fixed::from_raw(state.anchor.x.raw() - slot.lane->direction.x.raw()),
                    math::Fixed::from_raw(state.anchor.y.raw() - slot.lane->direction.y.raw()), state.anchor.z};
            }
        }
        return core::Result<void>::success();
    };

    // FM-01 to FM-10: the tracked ships of one move command (in command order). One plans as a
    // single move; two or more move as a group: each gets its slot and planning speed, the
    // front ship of each layer plans now and the others wait, their predictions clipped.
    const auto plan_group = [&](const PlayerCommand& command, const std::vector<EntityId>& group) -> core::Result<void> {
        const auto destination = *point_move(command.payload);
        const auto layer_of = [&](const LiveUnit& live) {
            return *dynamic_layer_index(tracked_footprint(table, live.state.type_id)->layer);
        };
        if (group.size() == 1) {
            pending.push_back({group.front(), destination, std::nullopt, layer_of(staged.at(group.front())), command.key, 0,
                false});
            return core::Result<void>::success();
        }
        // The slot mapping reads the layers: the queued searches plan first.
        if (auto failed = plan_pending()) return core::Result<void>::failure(std::move(failed->second));
        std::vector<FormationMember> members;
        members.reserve(group.size());
        CollisionWorld mapping;
        for (const auto unit_id : group) {
            auto& live = staged.at(unit_id);
            const auto* footprint = tracked_footprint(table, live.state.type_id);
            const auto limits = impl_->limits_of(live);
            if (!limits) return core::Result<void>::failure(limits.error());
            if (limits.value().max_speed.raw() <= 0) {
                // FM-09a: no slot for a ship with no speed. Its order is accepted (MV-03) and
                // its old path cleared, so it stays as a stationary obstacle.
                if (auto planned = impl_->plan_path(live, destination, frame, nullptr, math::Fixed{}, nullptr); !planned) {
                    return planned;
                }
                if (auto submitted = submit(unit_id, live, layer_of(live), std::nullopt); !submitted) {
                    return submitted;
                }
                continue;
            }
            const auto yaw = yaw_degrees(live.state.rotation);
            if (!yaw) return core::Result<void>::failure(yaw.error());
            const auto radius = occupation_radius(
                *footprint, limits.value().max_speed, limits.value().rate_of_turn, *table.avoidance);
            if (!radius) return core::Result<void>::failure(radius.error());
            members.push_back(FormationMember{unit_id, live.state.position, yaw.value(), footprint->layer,
                radius.value(), limits.value().max_speed, limits.value().rate_of_turn, footprint->radius});
            if (auto built = world_for(layer_of(live), mapping); !built) return built;
        }
        const auto slots = map_group_move(members, destination, mapping, frame, *table.avoidance);
        if (!slots) return core::Result<void>::failure(slots.error());
        for (std::size_t rank = 0; rank < slots.value().size(); ++rank) {
            const auto& slot = slots.value()[rank];
            if (slot.delay == 0) continue;
            auto& live = staged.at(slot.entity);
            live.formation = FormationWait{frame + slot.delay, slot.destination, slot.max_speed, command.key,
                static_cast<std::uint32_t>(rank), false};
            if (auto submitted = submit(slot.entity, live, layer_of(live), live.formation->frame); !submitted) {
                return submitted;
            }
        }
        for (std::size_t rank = 0; rank < slots.value().size(); ++rank) {
            const auto& slot = slots.value()[rank];
            if (slot.delay != 0) continue;
            pending.push_back({slot.entity, slot.destination, slot.max_speed, layer_of(staged.at(slot.entity)), command.key,
                static_cast<std::uint32_t>(rank), false});
        }
        return core::Result<void>::success();
    };

    // A-04: a unit that turns toward its ordered target plans a turn in place as a face order of
    // this tick would, before this tick's commands (which may replace it). A unit a hit
    // destroyed this tick is gone.
    for (const auto& [unit_id, point] : combat_turns) {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) continue;
        auto& live = found->second;
        if (auto planned = impl_->plan_order(live, FacePayload{point}, frame); !planned) {
            return unit_failure(unit_id, planned.error());
        }
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        if (layer && live.motion) {
            if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) {
                return unit_failure(unit_id, submitted.error());
            }
        }
    }

    // OR-05, OR-06: a unit plans towards the approach slot the orders phase mapped, serially in
    // ascending ID. A unit a hit destroyed this tick is gone.
    // Plans `live` towards the slot from this frame and keeps the mapping's prediction frame. A
    // tracked unit's search is a job of its lane (PC-07, PC-08), queued in the same order.
    const auto plan_approach = [&](const EntityId unit_id, LiveUnit& live, const Impl::ApproachPlan& plan) -> core::Result<void> {
        live.formation.reset();
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        std::optional<std::size_t> layer;
        if (footprint != nullptr) layer = dynamic_layer_index(footprint->layer);
        live.approach = Approach{plan.prediction_frame};
        if (tracking && layer) {
            pending.push_back({unit_id, plan.slot, std::nullopt, layer.value_or(0), CommandKey{}, 0, false});
            return core::Result<void>::success();
        }
        if (auto planned = impl_->plan_path(live, plan.slot, frame, nullptr, std::nullopt, nullptr); !planned) return planned;
        if (layer && live.motion) return submit(unit_id, live, *layer, std::nullopt);
        return core::Result<void>::success();
    };
    for (const auto& [unit_id, plan] : approaches) {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) continue;
        if (auto planned = plan_approach(unit_id, found->second, plan); !planned) {
            return unit_failure(unit_id, planned.error());
        }
    }

    // PC-08: every sliced search runs one slice, side by side in the `plan-searches` phase; a
    // search that lands this frame runs to its end.
    if (!impl_->searches.empty()) {
        std::vector<Impl::SlicedSearch*> running;
        for (auto& entry : impl_->searches) running.push_back(&entry.second);
        const auto slice = table.avoidance->search_slice;
        const auto sliced = executor.execute_phase("plan-searches", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, running.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& search = *running[index];
                static_cast<void>(search.search.run(search.frame <= frame ? std::numeric_limits<std::uint64_t>::max() : slice));
            }
        });
        if (!sliced) return core::Result<TacticalTick>::failure(sliced.error());
        // PC-09 (#613): a search that has ended publishes its plan to its unit's layer at once,
        // as FoC's plan would be there from the frame it was made (PC-02); its unit still takes
        // it at the landing. In ascending unit ID.
        for (auto& [unit_id, search] : impl_->searches) {
            if (search.plan || !search.search.ended()) continue;
            const auto found = staged.find(unit_id);
            if (found == staged.end() || !found->second.formation || !found->second.formation->sliced) continue;
            auto plan = search.search.result();
            if (!plan) continue; // the landing reports it
            search.plan = std::move(plan).value();
            if (auto published = publish(unit_id, found->second, search); !published) {
                return unit_failure(unit_id, published.error());
            }
        }
    }

    // FM-08, FM-10, PC-08: waiting ships whose plan is due (staggered group members, and sliced
    // searches that land) plan first, in the order of their commands and then each group's
    // planning order.
    if (tracking) {
        std::vector<std::tuple<CommandKey, std::uint32_t, EntityId>> due;
        for (const auto& [id, live] : staged) {
            if (live.formation && live.formation->frame <= frame) due.emplace_back(live.formation->order, live.formation->rank, id);
        }
        std::sort(due.begin(), due.end());
        for (const auto& entry : due) {
            auto& live = staged.at(std::get<2>(entry));
            const auto wait = *live.formation;
            live.formation.reset();
            const auto sliced = wait.sliced ? impl_->searches.find(std::get<2>(entry)) : impl_->searches.end();
            const bool landing = sliced != impl_->searches.end() && sliced->second.frame == frame;
            pending.push_back({std::get<2>(entry), wait.destination, wait.max_speed,
                *dynamic_layer_index(tracked_footprint(table, live.state.type_id)->layer), wait.order, wait.rank, landing});
        }
    }

    // AB-24: a ship whose speed multiplier changed plans its move again from this frame, towards
    // the same point, as a move order would (a group member still waiting keeps its wait).
    const auto replan = [&](const EntityId unit_id) -> core::Result<void> {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) return core::Result<void>::success();
        auto& live = found->second;
        if (!live.motion || live.motion->kind != MotionKind::path || live.formation) return core::Result<void>::success();
        const auto target = live.motion->target;
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        const bool layered = layer.has_value();
        const auto layer_index = layer.value_or(0);
        // A tracked unit's search is a job of its lane (PC-07, PC-08).
        if (tracking && layered) {
            pending.push_back({unit_id, target, std::nullopt, layer_index, CommandKey{}, 0, false});
            return core::Result<void>::success();
        }
        if (auto planned = impl_->plan_path(live, target, frame, nullptr, std::nullopt, nullptr); !planned) return planned;
        if (layered) return submit(unit_id, live, layer_index, std::nullopt);
        return core::Result<void>::success();
    };
    // The replans read the layers and the due waits' new plans: the queued searches plan first.
    if (!replans.empty() && !pending.empty()) {
        if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
    }
    for (const auto unit_id : replans) {
        if (auto planned = replan(unit_id); !planned) return unit_failure(unit_id, planned.error());
    }

    // An ability command on one listed unit (AB-10 to AB-15, AB-40): the unit itself, or for a
    // squadron container its live craft that have the ability. Every holder must be ready and
    // off for an activation to act (AB-15); otherwise nothing changes.
    const auto apply_ability = [&](const EntityId unit_id, const AbilityPayload& payload) -> core::Result<RejectReason> {
        // AB-61 to AB-65 (#561): ION_CANNON_SHOT acts on the squadron's team container itself.
        if (payload.ability == AbilityKind::ion_cannon_shot) {
            auto& container = staged.at(unit_id);
            auto* team = impl_->ion_slot(container);
            const auto squadron = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
                [&](const Squadron& entry) { return entry.container == unit_id; });
            const auto mind = minds.find(unit_id);
            if (team == nullptr || squadron == impl_->squadrons.end() || mind == minds.end()) {
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
            std::vector<AbilitySlot*> craft;
            for (const auto member : squadron->members) {
                const auto found = staged.find(member);
                if (auto* slot = found != staged.end() ? impl_->ion_slot(found->second) : nullptr) craft.push_back(slot);
            }
            const auto& profile = *impl_->abilities.find(container.state.type_id);
            const auto& ability = profile.abilities[*ability_slot(profile, AbilityKind::ion_cannon_shot)];
            switch (payload.action) {
            case AbilityAction::activate: {
                // AB-62: ready, off, with a craft to fire it and none still due; a valid target.
                const bool due = std::any_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; });
                if (team->active || tick < team->ready_tick || craft.empty() || due) {
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                }
                const auto target = staged.find(payload.target);
                if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
                if (!impl_->ion_target_valid(container.state.owner, &target->second)) {
                    return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
                }
                const auto* hit = durability.find(target->second.state.type_id);
                const auto hardpoint = hit != nullptr && payload.target_hardpoint != no_hardpoint
                        && damage_target_valid(*hit, payload.target_hardpoint)
                    ? payload.target_hardpoint
                    : no_hardpoint;
                team->active = true;
                team->started_tick = tick;
                team->target = payload.target;
                team->target_hardpoint = hardpoint;
                for (auto* slot : craft) slot->active = true;
                // AB-63: the squadron attacks the target.
                const CommandPayload attack = AttackPayload{payload.target};
                container.state.order = order_for(attack, tick);
                apply_squadron_order(mind->second, attack, container.state.position, squadron_table);
                break;
            }
            case AbilityAction::deactivate:
                if (team->active) impl_->finish_ion_shot(*team, craft, container.state.type_id, tick);
                break;
            case AbilityAction::autofire_on:
            case AbilityAction::autofire_off:
                if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                team->autofire = payload.action == AbilityAction::autofire_on;
                break;
            }
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        std::vector<LiveUnit*> holders;
        const auto holds = [&](LiveUnit& live) -> std::optional<std::size_t> {
            if (!live.abilities) return std::nullopt;
            return ability_slot(*impl_->abilities.find(live.state.type_id), payload.ability);
        };
        const auto squadron = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
            [&](const Squadron& entry) { return entry.container == unit_id; });
        if (squadron != impl_->squadrons.end()) {
            for (const auto member : squadron->members) {
                const auto found = staged.find(member);
                if (found != staged.end() && holds(found->second)) holders.push_back(&found->second);
            }
        } else if (auto& own = staged.at(unit_id); holds(own)) {
            holders.push_back(&own);
        }
        if (holders.empty()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        const auto profile_of = [&](const LiveUnit& live) -> const AbilityProfile& {
            const auto& profile = *impl_->abilities.find(live.state.type_id);
            return profile.abilities[*ability_slot(profile, payload.ability)];
        };
        const auto slot_of = [&](LiveUnit& live) -> AbilitySlot& { return live.abilities->slots[*holds(live)]; };
        std::vector<EntityId> changed_speed;
        switch (payload.action) {
        case AbilityAction::activate:
            for (auto* live : holders) {
                const auto& slot = slot_of(*live);
                if (slot.active || !ability_ready(profile_of(*live), slot, impl_->ability_gate(*live, tick), tick)) {
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                }
            }
            for (auto* live : holders) {
                const auto switched = activate_ability(profile_of(*live), slot_of(*live), impl_->ability_gate(*live, tick), tick);
                if (switched.changed && payload.ability == AbilityKind::defend) impl_->defend_switched_on(*live, tick);
                if (switched.speed) changed_speed.push_back(live->state.entity_id);
            }
            break;
        case AbilityAction::deactivate:
            for (auto* live : holders) {
                if (deactivate_ability(profile_of(*live), slot_of(*live), tick).speed) changed_speed.push_back(live->state.entity_id);
            }
            break;
        case AbilityAction::autofire_on:
        case AbilityAction::autofire_off:
            for (auto* live : holders) {
                if (!profile_of(*live).supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
            for (auto* live : holders) slot_of(*live).autofire = payload.action == AbilityAction::autofire_on;
            break;
        }
        for (const auto id : changed_speed) {
            if (auto planned = replan(id); !planned) return core::Result<RejectReason>::failure(planned.error());
        }
        return core::Result<RejectReason>::success(RejectReason::none);
    };

    // Commands phase: serial, in canonical (tick, player, sequence) order.
    std::vector<core::Diagnostic> diagnostics;
    auto due_end = impl_->pending.begin();
    for (; due_end != impl_->pending.end() && due_end->first.tick == tick; ++due_end) {
        const auto& command = due_end->second;
        // A command other than a move may read or change what the queued searches read.
        if (!std::holds_alternative<MovePayload>(command.payload) && !pending.empty()) {
            if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
        }
        const auto issuer = command.key.player_id;
        const auto issuer_team = impl_->teams.at(issuer);
        const auto* damage = std::get_if<DamagePayload>(&command.payload);
        auto command_reason = RejectReason::none;
        // The unit an attack, attack-move or guard sends its units towards (#452).
        const auto approach = approach_target_of(command.payload);
        if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
            const auto target = staged.find(attack->target);
            if (target == staged.end()) {
                command_reason = RejectReason::target_not_live;
            } else if (impl_->teams.at(target->second.state.owner) == issuer_team) {
                command_reason = RejectReason::target_not_hostile;
            }
        } else if (approach != invalid_entity_id && staged.find(approach) == staged.end()) {
            command_reason = RejectReason::target_not_live;
        }
        std::size_t rejected = 0;
        auto first_reason = RejectReason::none;
        // FM-01: the tracked ships a move command accepts, in command order; two or more move
        // as a group once every unit of the command is accepted.
        std::vector<EntityId> group;
        // FO-07 (#552): the squadrons the command sends flying to a point, in command order.
        std::vector<EntityId> squadron_group;
        for (const auto unit_id : command.units) {
            auto reason = command_reason;
            const auto unit = staged.find(unit_id);
            const DurabilityProfile* profile = nullptr;
            if (reason == RejectReason::none) {
                if (unit == staged.end()) {
                    reason = RejectReason::unit_not_live;
                } else if (damage != nullptr) {
                    // Scripted damage (HD-30) may hit any live unit, whoever owns it.
                    profile = unit->second.durability ? durability.find(unit->second.state.type_id) : nullptr;
                    if (profile == nullptr) {
                        reason = RejectReason::not_damageable;
                    } else if (!damage_target_valid(*profile, damage->hardpoint)) {
                        reason = RejectReason::hardpoint_invalid;
                    }
                } else if (unit->second.state.owner != issuer) {
                    reason = RejectReason::unit_not_owned;
                } else if (const auto* ability = std::get_if<AbilityPayload>(&command.payload)) {
                    auto applied = apply_ability(unit_id, *ability);
                    if (!applied) {
                        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                            command_context(command.key) + " unit " + std::to_string(unit_id) + ": "
                                + applied.error().message));
                    }
                    reason = applied.value();
                } else if (approach != invalid_entity_id && unit_id == approach) {
                    reason = RejectReason::target_is_unit;
                }
            }
            Event event{
                .tick = tick,
                .kind = EventKind::order_accepted,
                .player = issuer,
                .sequence = command.key.sequence,
                .unit = unit_id,
                .order = order_kind(command.payload),
                .reason = reason,
                .hardpoint = 0,
            };
            if (reason != RejectReason::none) {
                event.kind = EventKind::order_rejected;
                if (rejected++ == 0) {
                    first_reason = reason;
                }
                events.push_back(event);
                continue;
            }
            events.push_back(event);
            // An ability command acted above; it never becomes the unit's order (AB-10).
            if (std::holds_alternative<AbilityPayload>(command.payload)) continue;
            if (damage == nullptr) {
                auto& live = unit->second;
                live.state.order = order_for(command.payload, tick);
                // FO-01, FO-03 (#424): a squadron takes the order as one unit through its team
                // container; its craft fly it in the next craft phase. Serial, in command order.
                if (const auto mind = minds.find(unit_id); mind != minds.end()) {
                    apply_squadron_order(mind->second, command.payload, live.state.position, squadron_table);
                    if (mind->second.mode == SquadronMode::move) squadron_group.push_back(unit_id);
                    continue;
                }
                // An attack order gives the unit and its hardpoints that target (#73); any other
                // order ends a previous attack order.
                if (live.combat) {
                    if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
                        live.combat->attack_target = attack->target;
                        live.combat->direct = true;
                    } else if (live.combat->direct) {
                        live.combat->attack_target = invalid_entity_id;
                        live.combat->direct = false;
                    }
                }
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                std::optional<std::size_t> layer;
                if (footprint != nullptr) layer = dynamic_layer_index(footprint->layer);
                const auto failed = [&](const core::Diagnostic& error) {
                    return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                        command_context(command.key) + " unit " + std::to_string(unit_id) + ": " + error.message));
                };
                const bool moves = point_move(command.payload).has_value();
                // A move, face or stop submits the unit's new prediction and replaces a group
                // move's wait (FM-10), and so does an order that approaches a unit (#452); other
                // orders keep both. Every order ends the previous approach mapping.
                const bool submits = moves || std::holds_alternative<FacePayload>(command.payload)
                    || std::holds_alternative<StopPayload>(command.payload) || approach != invalid_entity_id;
                if (submits) live.formation.reset();
                live.approach.reset();
                if (tracking && layer && moves && live.motion) {
                    group.push_back(unit_id);
                    continue;
                }
                if (layer && (moves || submits) && !pending.empty()) {
                    if (auto queued = plan_pending()) return unit_failure(queued->first, queued->second);
                }
                if (approach != invalid_entity_id) {
                    // OR-02, OR-05: the first mapping. A unit in range of the target holds where
                    // it is; otherwise it plans towards its approach slot.
                    if (!live.motion) continue;
                    const auto& target = staged.at(approach);
                    const bool guard = std::holds_alternative<GuardPayload>(command.payload);
                    const auto range = impl_->approach_range(live, guard);
                    // The targeting view's health and combat pointers name the units before
                    // staging moved them; point the target's entry at its staged health.
                    std::optional<detail::CombatUnit> target_view;
                    if (const auto* entry = view.find(approach)) {
                        target_view = *entry;
                        target_view->durability = target.durability ? &*target.durability : nullptr;
                        target_view->combat = target.combat ? &*target.combat : nullptr;
                    }
                    const auto point = Impl::approach_point(
                        guard, target, target_view ? &*target_view : nullptr, live.state.position);
                    if (within_range(live.state.position, point, range, RangeMetric::planar)) {
                        // A path is dropped; a turn in place under way (A-04's) carries on (OR-05).
                        if (live.motion->kind != MotionKind::path) continue;
                        if (auto held = impl_->plan_order(live, StopPayload{}, tick + 1); !held) return failed(held.error());
                        if (layer) {
                            if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) {
                                return failed(submitted.error());
                            }
                        }
                        continue;
                    }
                    auto mapped = impl_->approach_mapping(live, target, range, tick + 1);
                    if (!mapped) return failed(mapped.error());
                    if (auto planned = plan_approach(unit_id, live, mapped.value()); !planned) return failed(planned.error());
                    continue;
                }
                CollisionWorld collisions;
                const CollisionWorld* planning = nullptr;
                if (tracking && layer && moves) {
                    if (auto built = world_for(*layer, collisions); !built) return failed(built.error());
                    planning = &collisions;
                }
                // An attack-move or guard of a point plans as the move it is for a ship (OR-11, OR-16).
                const CommandPayload plan_payload =
                    moves ? CommandPayload{MovePayload{*point_move(command.payload)}} : command.payload;
                auto planned = impl_->plan_order(live, plan_payload, tick + 1, planning);
                if (!planned) {
                    return failed(planned.error());
                }
                if (layer && live.motion && submits) {
                    if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) return failed(submitted.error());
                }
                continue;
            }
            // With damage rules the shield absorbs scripted damage first (DG-20); without, it is raw.
            auto outcome = DamageOutcome{};
            if (damage_rules != nullptr) {
                const Hit hit{damage->amount, no_type_index, false, true, true, damage->hardpoint};
                const auto hull_before = unit->second.durability->hull;
                const auto shields_before = unit->second.durability->shields;
                auto applied = apply_hit(*profile, *damage_rules, *unit->second.durability, hit, tick);
                if (!applied) {
                    return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                        command_context(command.key) + " unit " + std::to_string(unit_id) + ": "
                            + applied.error().message));
                }
                impl_->track_damage(unit->second, hull_before, shields_before);
                impl_->end_depleted_defend(unit->second, tick);
                outcome = applied.value().damage;
            } else {
                outcome = apply_damage(*profile, *unit->second.durability, damage->hardpoint, damage->amount);
            }
            if (outcome.destroyed_hardpoint) {
                events.push_back(destruction_event(
                    tick, EventKind::hardpoint_destroyed, unit->second.state, *outcome.destroyed_hardpoint));
            }
            if (outcome.unit_destroyed) {
                events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit->second.state));
                killed.push_back(unit->second.state);
                // A tracked unit leaving its layer rebuilds it (AV-03): a later order this tick
                // plans without it (AV-15).
                if (const auto* footprint = table.avoidance ? tracked_footprint(table, unit->second.state.type_id) : nullptr) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        rebuilt[*layer] = true;
                        if (tracking) tracking->views[*layer].reset();
                    } else if (footprint->layer == SpaceLayer::static_object && tracking) {
                        tracking->statics.reset();
                    }
                    if (tracking) tracking->samples.erase(unit_id);
                }
                staged.erase(unit);
            }
        }
        if (squadron_group.size() > 1) {
            if (auto spread = spread_squadrons(squadron_group); !spread) {
                return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    command_context(command.key) + ": " + spread.error().message));
            }
        }
        if (!group.empty()) {
            const auto planned = plan_group(command, group);
            if (!planned) {
                return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    command_context(command.key) + ": " + planned.error().message));
            }
        }
        if (rejected != 0) {
            diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                command_context(command.key) + ": " + std::to_string(rejected) + " of "
                    + std::to_string(command.units.size()) + " unit orders rejected (first: "
                    + std::string(to_string(first_reason)) + ")",
                {}, core::Severity::warning));
        }
    }

    if (!pending.empty()) {
        if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
    }
    // PC-08: a sliced search whose unit is gone or no longer waits for it (a new move, face or
    // stop, FM-10) is dropped with its copied views.
    for (auto iterator = impl_->searches.begin(); iterator != impl_->searches.end();) {
        const auto unit = staged.find(iterator->first);
        const bool waits = unit != staged.end() && unit->second.formation && unit->second.formation->sliced
            && unit->second.formation->frame == iterator->second.frame;
        iterator = waits ? std::next(iterator) : impl_->searches.erase(iterator);
    }

    // Systems phase: workers read the copied ascending-ID units and fill disjoint slots. Each
    // durable unit's hull and hardpoints are serviced (HS-01) before its instance is built.
    std::vector<LiveUnit> inputs;
    inputs.reserve(staged.size());
    for (auto& [id, unit] : staged) {
        static_cast<void>(id);
        inputs.push_back(std::move(unit));
    }
    std::vector<TacticalInstance> instances(inputs.size());
    std::vector<ServiceOutcome> serviced(inputs.size());
    std::vector<std::optional<core::Diagnostic>> errors(inputs.size());
    const auto executed = executor.execute_phase("unit-systems", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, inputs.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& unit = inputs[index];
            const auto context = "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": ";
            if (unit.durability) {
                const auto& profile = *durability.find(unit.state.type_id);
                auto outcome = service_durability(profile, durability.rules, *unit.durability);
                if (!outcome) {
                    errors[index] = detail::diagnostic(
                        diagnostic_codes::worker_failure, context + outcome.error().message);
                    continue;
                }
                serviced[index] = std::move(outcome).value();
                // With damage rules (#74): a lost last shield generator drops the shield (DG-17); the
                // pool (EN-02), then the shield (DG-13), recharge on the unit's own recharge frames.
                if (damage_rules != nullptr) {
                    auto& state = *unit.durability;
                    shield_generators_lost(profile, *damage_rules, state, tick);
                    auto recharged = core::Result<void>::success();
                    // AB-22, AB-23: an active ability scales the recharge intervals and amounts.
                    const auto factor = [&](const AbilityModifier modifier) { return impl_->ability_factor(unit, modifier); };
                    if (has_energy_pool(profile, *damage_rules) && tick >= state.next_energy_frame) {
                        state.next_energy_frame =
                            tick + scaled_interval(damage_rules->energy_recharge_frames, factor(AbilityModifier::energy_regen_interval));
                        recharged = recharge_energy(profile, *damage_rules, state, factor(AbilityModifier::energy_regen));
                    }
                    if (recharged && tick >= state.next_shield_frame) {
                        state.next_shield_frame =
                            tick + scaled_interval(damage_rules->shield_recharge_frames, factor(AbilityModifier::shield_regen_interval));
                        recharged = recharge_shields(profile, *damage_rules, state, tick, factor(AbilityModifier::shield_regen));
                    }
                    if (recharged) impl_->end_depleted_defend(unit, tick);
                    if (!recharged) {
                        errors[index] = detail::diagnostic(
                            diagnostic_codes::worker_failure, context + recharged.error().message);
                        continue;
                    }
                }
            }
            const auto transform = unit_transform(unit);
            if (!transform) {
                errors[index] = detail::diagnostic(
                    diagnostic_codes::worker_failure, context + transform.error().message);
                continue;
            }
            instances[index] = impl_->instance_for(unit, transform.value(), tick + 1);
        }
    });
    if (!executed) {
        return core::Result<TacticalTick>::failure(executed.error());
    }
    for (auto& error : errors) {
        if (error) {
            return core::Result<TacticalTick>::failure(std::move(*error));
        }
    }

    // Serial, in ascending ID: the service's destruction events; dead units leave (HD-20).
    std::vector<LiveUnit> survivors;
    std::vector<TacticalInstance> surviving_instances;
    survivors.reserve(inputs.size());
    surviving_instances.reserve(inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        for (const auto hardpoint : serviced[index].destroyed_hardpoints) {
            events.push_back(destruction_event(tick, EventKind::hardpoint_destroyed, inputs[index].state, hardpoint));
        }
        if (serviced[index].unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, inputs[index].state));
            killed.push_back(inputs[index].state);
            continue;
        }
        survivors.push_back(std::move(inputs[index]));
        surviving_instances.push_back(std::move(instances[index]));
    }
    instances = std::move(surviving_instances);

    // Spin phase (#447, space-fighter-deaths SP-04 to SP-08): workers service each spinning
    // craft's copy in its own slot; the serial commit in ascending ID drops those that ended and
    // publishes their spin_away_ended. The craft killed this tick then draw whether they spin away
    // (SP-02, SP-03), serially in destruction order: one keyed draw per death, so this walks the
    // tick's deaths, never every unit, and their order is the events' order.
    auto spins = impl_->spins;
    if (!spins.empty()) {
        std::vector<std::uint8_t> ended(spins.size());
        std::vector<std::optional<core::Diagnostic>> spin_errors(spins.size());
        const auto spun = executor.execute_phase("spins", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, spins.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto* craft = squadron_table.find_craft(spins[index].type);
                if (craft == nullptr || !craft->spin_away) {
                    ended[index] = 1;
                    continue;
                }
                const auto stepped = step_spin(spins[index], craft->rate_of_turn, *craft->spin_away,
                    impl_->setup.seed, frame);
                if (!stepped) {
                    spin_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + ": " + stepped.error().message);
                    continue;
                }
                ended[index] = stepped.value() ? 1 : 0;
            }
        });
        if (!spun) {
            return core::Result<TacticalTick>::failure(spun.error());
        }
        std::vector<DeathSpin> running;
        for (std::size_t index = 0; index < spins.size(); ++index) {
            if (spin_errors[index]) {
                return core::Result<TacticalTick>::failure(std::move(*spin_errors[index]));
            }
            if (ended[index] != 0) {
                events.push_back(Event{tick, EventKind::spin_away_ended, spins[index].owner, 0, spins[index].unit});
                continue;
            }
            running.push_back(std::move(spins[index]));
        }
        spins = std::move(running);
    }
    for (const auto& state : killed) {
        const auto* craft = squadron_table.find_craft(state.type_id);
        if (craft == nullptr || !craft->spin_away
            || !spins_away(*craft->spin_away, impl_->setup.seed, frame, state.entity_id)) {
            continue;
        }
        // SP-03: a craft without a flight state spins from rest.
        const auto flight = crafts.find(state.entity_id);
        spins.push_back(start_spin(state.entity_id, state.type_id, state.owner, state.position,
            flight != crafts.end() ? flight->second : CraftState{}));
        events.push_back(Event{tick, EventKind::spin_away_started, state.owner, 0, state.entity_id});
    }
    std::sort(spins.begin(), spins.end(), [](const DeathSpin& left, const DeathSpin& right) { return left.unit < right.unit; });

    // Squadron phase (#271, V-03): workers read the survivors and fill one slot per squadron
    // with its live craft and their bounding-box centre. The serial commit then moves each
    // container there, or removes a container whose last craft has died.
    auto squadrons = impl_->squadrons;
    if (!squadrons.empty()) {
        const auto find = [&survivors](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        std::vector<SquadronOutcome> outcomes(squadrons.size());
        const auto grouped = executor.execute_phase("squadrons", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, squadrons.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& outcome = outcomes[index];
                const auto* container = find(squadrons[index].container);
                outcome.container_live = container != nullptr;
                math::Vec3 low{};
                math::Vec3 high{};
                for (const auto member : squadrons[index].members) {
                    const auto* craft = find(member);
                    if (craft == nullptr) {
                        continue;
                    }
                    const auto& at = craft->state.position;
                    if (outcome.members.empty()) {
                        low = at;
                        high = at;
                    } else {
                        low = {std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
                        high = {std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
                    }
                    outcome.members.push_back(member);
                }
                if (container == nullptr || outcome.members.empty()) {
                    continue;
                }
                outcome.centre = {math::Fixed::from_raw(midpoint(low.x.raw(), high.x.raw())),
                    math::Fixed::from_raw(midpoint(low.y.raw(), high.y.raw())),
                    math::Fixed::from_raw(midpoint(low.z.raw(), high.z.raw()))};
                // FM-26 (#687, walk 1 WSQ-50): a squadron holding an idle cell has its container
                // on the cell's point.
                if (const auto mind = minds.find(squadrons[index].container); mind != minds.end() && mind->second.idle_cell) {
                    outcome.centre = mind->second.anchor;
                }
                const auto transform = math::to_matrix(container->state.rotation, outcome.centre);
                if (!transform) {
                    outcome.error = detail::diagnostic(diagnostic_codes::worker_failure, "tick " + std::to_string(tick)
                        + " squadron " + std::to_string(squadrons[index].container) + ": " + transform.error().message);
                    continue;
                }
                outcome.transform = transform.value();
            }
        });
        if (!grouped) {
            return core::Result<TacticalTick>::failure(grouped.error());
        }
        std::vector<EntityId> emptied;
        std::vector<Squadron> kept;
        for (std::size_t index = 0; index < squadrons.size(); ++index) {
            auto& outcome = outcomes[index];
            if (outcome.error) {
                return core::Result<TacticalTick>::failure(std::move(*outcome.error));
            }
            if (!outcome.container_live) {
                continue; // the container itself died: its craft fly on without a sensor
            }
            const auto container = squadrons[index].container;
            if (outcome.members.empty()) {
                emptied.push_back(container);
                continue;
            }
            const auto slot = static_cast<std::size_t>(find(container) - survivors.data());
            survivors[slot].state.position = outcome.centre;
            instances[slot].fixed_transform = outcome.transform;
            kept.push_back(Squadron{container, std::move(outcome.members)});
        }
        // Containers without craft leave the session. They have no hull, so no event is
        // published; presentation sees them leave the snapshot.
        if (!emptied.empty()) {
            std::vector<LiveUnit> remaining;
            std::vector<TacticalInstance> remaining_instances;
            for (std::size_t index = 0; index < survivors.size(); ++index) {
                if (std::binary_search(emptied.begin(), emptied.end(), survivors[index].state.entity_id)) {
                    continue;
                }
                remaining.push_back(std::move(survivors[index]));
                remaining_instances.push_back(std::move(instances[index]));
            }
            survivors = std::move(remaining);
            instances = std::move(remaining_instances);
        }
        squadrons = std::move(kept);
    }

    // #75: a squadron that left releases its spawner entry (FL-08); dead craft, squadrons and
    // spawners drop their state. Serial, in ascending ID.
    const auto survivor = [&survivors](const EntityId id) -> const LiveUnit* {
        const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    auto spawners = impl_->spawners;
    for (auto iterator = minds.begin(); iterator != minds.end();) {
        const auto container = iterator->first;
        const bool kept = std::any_of(squadrons.begin(), squadrons.end(),
            [container](const Squadron& squadron) { return squadron.container == container; });
        if (kept) {
            ++iterator;
            continue;
        }
        const auto& state = iterator->second;
        const auto hangar = spawners.find(state.spawner);
        const auto* spawner = survivor(state.spawner);
        if (hangar != spawners.end() && spawner != nullptr) {
            squadron_lost(*squadron_table.find_spawner(spawner->state.type_id), hangar->second, state.entry, frame);
        }
        iterator = minds.erase(iterator);
    }
    std::erase_if(crafts, [&](const auto& entry) { return survivor(entry.first) == nullptr; });
    std::erase_if(spawners, [&](const auto& entry) { return survivor(entry.first) == nullptr; });

    // Hangar phase (#75, FL-01 to FL-06): workers service each spawner's hangar copy in its own
    // slot; the serial commit launches the squadrons in ascending spawner ID with the next stable
    // IDs, craft first and their team container last (FL-06).
    auto next_id = impl_->next_id;
    if (!spawners.empty()) {
        std::vector<std::pair<EntityId, SpawnerState>> hangars(spawners.begin(), spawners.end());
        std::vector<std::optional<SpawnDecision>> decisions(hangars.size());
        const auto serviced_hangars = executor.execute_phase("hangars", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, hangars.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = *survivor(hangars[index].first);
                const auto& profile = *squadron_table.find_spawner(unit.state.type_id);
                std::vector<bool> intact;
                const auto* hull = durability.find(unit.state.type_id);
                for (const auto& bay : profile.bays) {
                    intact.push_back(!unit.durability || hull == nullptr || bay.hardpoint >= hull->hardpoints.size()
                        || !hardpoint_destroyed(*hull, *unit.durability, bay.hardpoint));
                }
                decisions[index] = service_spawner(profile, hangars[index].second, impl_->setup.seed, frame,
                    hangars[index].first, intact);
            }
        });
        if (!serviced_hangars) {
            return core::Result<TacticalTick>::failure(serviced_hangars.error());
        }
        for (std::size_t index = 0; index < hangars.size(); ++index) {
            spawners[hangars[index].first] = hangars[index].second;
            if (!decisions[index]) continue;
            const auto spawner = *survivor(hangars[index].first); // copied: launches grow `survivors`
            const auto& profile = *squadron_table.find_spawner(spawner.state.type_id);
            const auto flown_out = impl_->launch(spawner, profile, *decisions[index], frame, next_id, survivors, instances,
                squadrons, crafts, minds);
            if (!flown_out) {
                return core::Result<TacticalTick>::failure(flown_out.error());
            }
        }
    }

    std::vector<UnitState> positions;
    positions.reserve(survivors.size());
    for (const auto& unit : survivors) {
        positions.push_back(unit.state);
    }

    // Visibility phase: after every per-unit system, workers read one sensor field built
    // from the tick's final positions and fill the same disjoint slots.
    auto field = SensorField::build(impl_->setup.players, positions, impl_->sensors);
    if (!field) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "tick " + std::to_string(tick) + ": " + field.error().message));
    }
    const auto& sensing = field.value();
    // Fog cells (#274): the grids due this tick are serviced, then revealers release and mark
    // their circles, from the same final positions.
    std::optional<FogCells> fog = impl_->fog;
    if (fog) {
        // V-19 (#495): each shot of a revealing unit shows its shooter to the owner of the unit it
        // fired at, from where it fired. One entry per shot in event order, read serially: the
        // shots are few and their order is the commit order.
        std::vector<FogFlash> flashes;
        for (const auto& event : combat_events) {
            if (event.kind != CombatEventKind::weapon_fired) {
                continue;
            }
            const auto* shooter = view.find(event.shooter);
            const auto* target = view.find(event.target);
            if (shooter == nullptr || target == nullptr || !sensing.reveal_range(shooter->type_id)) {
                continue;
            }
            flashes.push_back(FogFlash{target->owner, shooter->position});
        }
        const auto advanced = fog->advance(tick, true, Impl::revealers(sensing, positions), executor, flashes);
        if (!advanced) {
            return core::Result<TacticalTick>::failure(advanced.error());
        }
    }
    const FogCells* cells = fog ? &*fog : nullptr;
    // Spinning craft (#447) are seen like their owner's units at their spin positions.
    std::vector<SpinningCraft> spinning(spins.size());
    std::vector<std::optional<core::Diagnostic>> spin_pose_errors(spins.size());
    const auto sensed = executor.execute_phase("visibility", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, positions.size());
        for (auto index = range.begin; index < range.end; ++index) {
            instances[index].visible_to = impl_->visible_to(sensing, cells, positions[index].owner, positions[index].position);
            instances[index].reveal_range = sensing.reveal_range(positions[index].type_id);
        }
        const auto spun = partition_range(partition, spins.size());
        for (auto index = spun.begin; index < spun.end; ++index) {
            const auto& spin = spins[index];
            auto rotation = spin_rotation(spin);
            if (!rotation) {
                spin_pose_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " spinning craft " + std::to_string(spin.unit) + ": "
                        + rotation.error().message);
                continue;
            }
            auto transform = math::to_matrix(rotation.value(), spin.position);
            if (!transform) {
                spin_pose_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " spinning craft " + std::to_string(spin.unit) + ": "
                        + transform.error().message);
                continue;
            }
            spinning[index] = SpinningCraft{spin.unit, spin.type, spin.owner, transform.value(), spin.roll, spin.pitch,
                spin.yaw, impl_->visible_to(sensing, cells, spin.owner, spin.position)};
        }
    });
    if (!sensed) {
        return core::Result<TacticalTick>::failure(sensed.error());
    }
    for (auto& error : spin_pose_errors) {
        if (error) {
            return core::Result<TacticalTick>::failure(std::move(*error));
        }
    }

    // A layer that a submission or a member change rebuilt this tick rolls from this frame.
    auto anchors = impl_->tracking_anchor;
    if (table.avoidance) {
        const auto members_after = layer_members(table, survivors);
        const auto in_layer = [](const std::vector<std::pair<std::size_t, EntityId>>& members, const std::size_t index) {
            std::vector<EntityId> ids;
            for (const auto& [layer, id] : members) {
                if (layer == index) ids.push_back(id);
            }
            return ids;
        };
        for (std::size_t index = 0; index < anchors.size(); ++index) {
            if (rebuilt[index] || in_layer(members_before, index) != in_layer(members_after, index)) anchors[index] = frame;
        }
    }

    // Victory (#77, VT-04 to VT-10): serial, after every destruction of the tick. It walks the
    // tick's unit_destroyed events in event order, the order retail destroys objects in, and
    // judges each counted star base's loss against the star bases standing at that moment (the
    // later losses of the same tick still stand). It visits only this tick's events and the few
    // star bases, never every unit, so it is no per-entity loop. The first winner decides.
    auto starbases = impl_->starbases;
    auto outcome = impl_->outcome;
    if (impl_->victory.condition != VictoryCondition::none) {
        std::optional<Event> decided;
        for (const auto& event : events) {
            if (event.kind != EventKind::unit_destroyed) {
                continue;
            }
            const auto base = std::find_if(starbases.begin(), starbases.end(),
                [&](const StarbaseEntry& entry) { return entry.unit == event.unit; });
            if (base == starbases.end()) {
                continue;
            }
            const auto owner = base->owner;
            starbases.erase(base);
            if (outcome) {
                continue;
            }
            const auto winner = starbase_destroyed_winner(impl_->victory, impl_->setup.players, owner, starbases);
            if (winner) {
                outcome = BattleOutcome{impl_->victory.condition, *winner, impl_->teams.at(*winner), tick, event.unit,
                    tick + impl_->victory.countdown_frames};
                decided = Event{tick, EventKind::victory, *winner, 0, event.unit};
            }
        }
        if (decided) {
            events.push_back(*decided);
        }
    }

    // Commit in stable-ID order; nothing above mutated the session.
    impl_->rebuild(survivors, false);
    impl_->starbases = std::move(starbases);
    impl_->outcome = outcome;
    impl_->tracking_anchor = anchors;
    impl_->squadrons = std::move(squadrons);
    impl_->crafts = std::move(crafts);
    impl_->minds = std::move(minds);
    impl_->spawners = std::move(spawners);
    impl_->spins = std::move(spins);
    impl_->collection = std::move(collection);
    impl_->next_id = next_id;
    impl_->fog = std::move(fog);
    impl_->projectiles = std::move(projectiles);
    impl_->next_projectile = next_projectile;
    for (auto iterator = impl_->pending.begin(); iterator != due_end; ++iterator) {
        impl_->executed.push_back(std::move(iterator->second));
    }
    impl_->pending.erase(impl_->pending.begin(), due_end);
    ++impl_->completed_tick;
    auto hash = state_sha256();
    // #424: the squadrons' targets for the world UI (presentation only, not hashed).
    std::vector<SquadronTarget> squadron_targets;
    for (const auto& [container, mind] : impl_->minds) {
        if (mind.target != invalid_entity_id) {
            squadron_targets.push_back({container, mind.target, closing_squadrons.contains(container), mind.cell.has_value(), mind.joined,
                mind.cell ? mind.cell->x : 0, mind.cell ? mind.cell->y : 0});
        }
    }
    impl_->current_snapshot = std::make_shared<const TacticalSnapshot>(impl_->completed_tick, impl_->snapshot_players,
        std::move(instances), std::move(events), std::move(combat_events), impl_->projectiles, impl_->outcome,
        std::move(spinning), std::move(squadron_targets), impl_->squadrons);
    return core::Result<TacticalTick>::success(TacticalTick{
        impl_->completed_tick,
        std::move(hash),
        impl_->current_snapshot,
        std::move(diagnostics),
        projectile_candidates,
        projectile_exact_tests,
    });
}

std::uint64_t TacticalSession::completed_tick() const noexcept { return impl_->completed_tick; }
EntityId TacticalSession::next_entity_id() const noexcept { return impl_->next_id; }
std::uint64_t TacticalSession::rng_state() const noexcept { return impl_->rng_state; }
std::span<const Player> TacticalSession::players() const noexcept { return impl_->setup.players; }
std::span<const SensorProfile> TacticalSession::sensors() const noexcept { return impl_->sensors; }
const DurabilityTable& TacticalSession::durability() const noexcept { return impl_->durability; }
const AbilityTable& TacticalSession::abilities() const noexcept { return impl_->abilities; }
const MotionTable& TacticalSession::motion() const noexcept { return impl_->motion; }
const CombatTable& TacticalSession::combat() const noexcept { return impl_->combat; }
const VictoryRules& TacticalSession::victory() const noexcept { return impl_->victory; }
const std::optional<BattleOutcome>& TacticalSession::outcome() const noexcept { return impl_->outcome; }
std::vector<UnitState> TacticalSession::units() const { return impl_->sorted_units(); }
std::span<const Squadron> TacticalSession::squadrons() const noexcept { return impl_->squadrons; }
const FogCells* TacticalSession::fog_cells() const noexcept { return impl_->fog ? &*impl_->fog : nullptr; }
std::span<const Projectile> TacticalSession::projectiles() const noexcept { return impl_->projectiles; }

std::optional<DurabilityState> TacticalSession::durability_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* health = impl_->registry.try_get<Health>(found->second);
    return health != nullptr ? std::optional(health->value) : std::nullopt;
}

std::optional<AbilityState> TacticalSession::ability_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* able = impl_->registry.try_get<Abilities>(found->second);
    return able != nullptr ? std::optional(able->value) : std::nullopt;
}

TacticalSession::SlicedSearchUse TacticalSession::sliced_searches() const noexcept {
    SlicedSearchUse use;
    for (const auto& entry : impl_->searches) {
        ++use.searches;
        use.bytes += entry.second.search.held_bytes();
    }
    return use;
}

std::optional<MotionState> TacticalSession::motion_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* moving = impl_->registry.try_get<Motion>(found->second);
    return moving != nullptr ? std::optional(moving->value) : std::nullopt;
}

std::optional<math::Fixed> TacticalSession::roll_degrees(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* moving = impl_->registry.try_get<Motion>(found->second);
    return moving != nullptr ? std::optional(moving->roll) : std::nullopt;
}

std::optional<CraftState> TacticalSession::craft_state(const EntityId craft) const {
    const auto found = impl_->crafts.find(craft);
    return found != impl_->crafts.end() ? std::optional(found->second) : std::nullopt;
}

std::optional<SquadronState> TacticalSession::squadron_state(const EntityId container) const {
    const auto found = impl_->minds.find(container);
    return found != impl_->minds.end() ? std::optional(found->second) : std::nullopt;
}

std::optional<CombatState> TacticalSession::combat_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* fighting = impl_->registry.try_get<Combat>(found->second);
    return fighting != nullptr ? std::optional(fighting->value) : std::nullopt;
}

std::size_t TacticalSession::pending_command_count() const noexcept { return impl_->pending.size(); }
std::vector<std::uint8_t> TacticalSession::canonical_state_bytes() const { return impl_->canonical_bytes(); }

std::string TacticalSession::state_sha256() const {
    return sim::sha256_hex(impl_->canonical_bytes());
}

std::shared_ptr<const TacticalSnapshot> TacticalSession::snapshot() const noexcept {
    return impl_->current_snapshot;
}

TacticalReplay TacticalSession::record() const {
    return TacticalReplay{impl_->setup, impl_->completed_tick, impl_->executed};
}

TacticalReplay TacticalSession::record_through_next_tick() const {
    TacticalReplay replay{impl_->setup, impl_->completed_tick + 1, impl_->executed};
    // The next step executes the commands keyed to the completed tick, in the queue's canonical order.
    for (const auto& [key, command] : impl_->pending) {
        if (key.tick > impl_->completed_tick) break;
        replay.commands.push_back(command);
    }
    return replay;
}

void TacticalSession::scramble_storage_for_testing() {
    const auto units = impl_->sorted_live();
    impl_->rebuild(units, true);
}

core::Result<EntityId> TacticalSession::stage_spawn(const UnitState& unit) {
    if (!impl_->teams.contains(unit.owner)) {
        return core::Result<EntityId>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "staged unit owner " + std::to_string(unit.owner) + " is not a declared player"));
    }
    if (impl_->next_id == invalid_entity_id) {
        return core::Result<EntityId>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "stable ID space exhausted"));
    }
    const auto transform = math::to_matrix(unit.rotation, unit.position);
    if (!transform) {
        return core::Result<EntityId>::failure(transform.error());
    }
    auto state = unit;
    state.entity_id = impl_->next_id;
    state.order = Order{};
    auto live = impl_->sorted_live();
    live.push_back(impl_->new_unit(state, impl_->completed_tick));
    impl_->rebuild(live, false);
    if (impl_->motion.squadrons.find_spawner(state.type_id) != nullptr) {
        impl_->spawners.emplace(state.entity_id,
            initial_spawner(impl_->setup.seed, impl_->completed_tick + 1, state.entity_id));
    }
    impl_->add_starbase(state);
    impl_->next_id = state.entity_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : state.entity_id + 1;
    const auto& published = *impl_->current_snapshot;
    impl_->publish_staged(live, {published.events().begin(), published.events().end()},
        {published.combat_events().begin(), published.combat_events().end()});
    return core::Result<EntityId>::success(state.entity_id);
}

core::Result<void> TacticalSession::stage_remove(const EntityId unit) {
    auto live = impl_->sorted_live();
    const auto found = std::find_if(
        live.begin(), live.end(), [&](const LiveUnit& entry) { return entry.state.entity_id == unit; });
    if (found == live.end()) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "staged removal of unit " + std::to_string(unit) + ", which is not live"));
    }
    for (const auto& squadron : impl_->squadrons) {
        if (squadron.container == unit
            || std::find(squadron.members.begin(), squadron.members.end(), unit) != squadron.members.end()) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
                "staged removal of unit " + std::to_string(unit) + ", which belongs to a squadron"));
        }
    }
    live.erase(found);
    impl_->rebuild(live, false);
    impl_->spawners.erase(unit);
    // A staged removal is the recorder's deletion, not a destruction: it decides nothing (VT-12).
    std::erase_if(impl_->starbases, [&](const StarbaseEntry& entry) { return entry.unit == unit; });
    const auto& published = *impl_->current_snapshot;
    impl_->publish_staged(live, {published.events().begin(), published.events().end()},
        {published.combat_events().begin(), published.combat_events().end()});
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
