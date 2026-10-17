#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/snapshot.hpp"

#include <array>
#include "eawr/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::sim::tactical {

// The object turn (#3644, docs/simulation.md "Phase map"): the point inside tactical tick t where an
// object's own script runs in its turn, after the ability service has closed the damage-rate window
// and the special-ability handlers have run, and before tracking, late impacts and the commands. A
// provider (the object-script host) is installed with TacticalSession::set_object_turn_service. With
// none installed the tick is unchanged.

// One unit as the turn sees it: an immutable copy staged at the seam, in ascending unit ID.
struct ObjectTurnRow {
    UnitState state;                 // position after this tick's movement, the order, owner
    bool has_durability{};
    math::Fixed hull{};              // after the early impacts, before the late ones
    math::Fixed shields{};
    math::Fixed damage_rate{};       // the window rate as closed this tick when t is a close tick
    std::vector<AbilitySlot> slots;  // after this tick's expiry and recharge; empty without abilities
    EntityId squadron_parent{};      // the squadron container of a craft, else zero
    TacticalInstance published;     // the same projection used at a completed barrier
    std::optional<math::Fixed> time_till_dead;
    EntityId attack_target{}, formation_target{};
    bool moving{}, formation_moving{}, container{}, arriving{};
    std::vector<EntityId> members;
    std::optional<std::array<math::Vec3, 2>> proximity_bounds;
    friend bool operator==(const ObjectTurnRow&, const ObjectTurnRow&) = default;
};

struct ObjectTurnView {
    std::uint64_t tick{};
    std::span<const ObjectTurnRow> rows;
};

// One ability use an object script asked for: `unit` holds the ability and `sequence` orders the
// uses of one unit within the turn. The world applies the uses of tick t at the seam in ascending
// (unit, sequence) order, through the same activation paths the ability commands use.
struct ObjectTurnUse {
    EntityId unit{};
    std::uint64_t sequence{};
    AbilityKind ability{AbilityKind::none};
    AbilityAction action{AbilityAction::activate};
    EntityId target{};
    std::optional<math::Vec3> position{};
    friend bool operator==(const ObjectTurnUse&, const ObjectTurnUse&) = default;
};

// The replay form of a use: the tick that applied it. Replays carry them in strictly increasing
// (tick, unit, sequence) order, so a replay without a provider applies the same uses at the same seam.
struct ObjectTurnRecord {
    std::uint64_t tick{};
    ObjectTurnUse use;
    friend bool operator==(const ObjectTurnRecord&, const ObjectTurnRecord&) = default;
};

// What the seam did with one use, reported on TacticalTick (diagnostics, never hashed).
struct ObjectTurnOutcome {
    EntityId unit{};
    std::uint64_t sequence{};
    AbilityKind ability{AbilityKind::none};
    AbilityAction action{AbilityAction::activate};
    RejectReason reason{RejectReason::none}; // none: applied; a refused gate names its reason
    friend bool operator==(const ObjectTurnOutcome&, const ObjectTurnOutcome&) = default;
};

// Ordinary command handlers not yet admitted at this seam must diagnose at
// the Lua boundary, rather than silently consume a ready ability request.
[[nodiscard]] bool object_turn_supports(AbilityKind kind) noexcept;

// The provider. The session calls it from the stepping thread inside step():
// - `due(tick)` is cheap and answered from the provider's own precomputed due set; the session
//   builds the rows only for a tick it reports due;
// - `run` reads the immutable view and returns the uses, in any order (the world sorts them by
//   (unit, sequence) and rejects a repeated key). It may use the executor for its own partitioned
//   phases. Every public TacticalSession mutator fails while either call runs.
// The provider's state is not rolled back when a later phase of the same step fails: the session's
// tick is atomic, the provider's turn is not, so a caller discards or restores both after a failed step.
class ObjectTurnService {
public:
    virtual ~ObjectTurnService() = default;
    [[nodiscard]] virtual bool due(std::uint64_t tick) = 0;
    [[nodiscard]] virtual core::Result<std::vector<ObjectTurnUse>> run(
        const PartitionExecutor& executor, const ObjectTurnView& view) = 0;
};

} // namespace eawr::sim::tactical
