#pragma once

// WGA-01: the shared AI engine reads a completed barrier through a mode host.
// Economy/event records retain the existing value schema; no session or snapshot
// is exposed. A host without a capability supplies empty records or rejects it.
#include "ai_data.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <set>

namespace eawr::script::foc::detail {
namespace tactical = sim::tactical;
namespace math = sim::math;
using numeric::LuaNumber;

// ---- The completed tick as the bindings read it -------------------------------------------

struct ViewUnit {
    sim::EntityId id{};
    tactical::TypeId type{};
    tactical::PlayerId owner{};
    tactical::TeamId team{};
    math::Vec3 position{};
    math::Quat rotation{math::identity_quat()};
    std::optional<math::Fixed> hull;     // durability hull and maximum, when it has any
    std::optional<math::Fixed> max_hull;
    std::optional<math::Fixed> max_shields;
    std::uint64_t visible_to{};          // snapshot player bits
    sim::EntityId attack_target{};       // weapon target: combat eligibility and in-range events
    sim::EntityId formation_target{};    // commanded formation target: Lua object queries
    bool moving{};                       // a planned path or turn
    bool formation_moving{};             // formation point travel for Lua order queries
    tactical::OrderKind order{tactical::OrderKind::none};
    sim::EntityId order_target{};
    bool container{};                    // a squadron's team container
    bool craft{};                        // a squadron member
    // #449 perception inputs.
    LuaNumber health{LuaNumber(1)};      // Get_Health_Percent: hull over maximum, 1 without durability
    LuaNumber shield{};                  // Get_Shield_Percent: shield over maximum, 0 without shields
    bool engines_online{true};
    std::vector<bool> hardpoint_destroyed; // HardPoints order
    // WTA-30, WTA-32 (#733): a DAMAGE_TRACKING unit's rate of damage taken per second (WCC-82)
    // and its estimated time to death (WCC-15; none: never). Zero and none without tracking.
    math::Fixed damage_rate{};
    std::optional<math::Fixed> time_till_dead;
    sim::EntityId squadron{};            // a craft's team container
    std::vector<sim::EntityId> members;  // a container's live craft
    std::optional<std::array<math::Vec3, 2>> proximity_bounds; // post-movement collection box in an object turn
    std::vector<tactical::AbilityStatus> abilities; // #76: the snapshot's (none on a container)
    tactical::TypeId purchase_type{}; // SAE-11/WHE-49: logical identity; combat still reads type
    std::uint64_t purchase_token{}; // SAE-11: distinguish concurrent arrivals of the same type
    std::optional<tactical::TacticalInstance::PlanetaryWeaponView> planetary_weapon;
    bool can_move{}; // completed SPACE formation eligibility, including active movement locks
};

struct ViewPlayer {
    tactical::PlayerId id{};
    tactical::TeamId team{};
    std::size_t snapshot_index{};
};

struct WorldView {
    std::uint64_t tick{};
    std::vector<ViewUnit> units; // ascending ID
    std::vector<ViewPlayer> players; // ascending ID
    std::vector<tactical::CaptureCandidate> capture_candidates; // prepared with the unit rows

    [[nodiscard]] const ViewUnit* find(sim::EntityId id) const {
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const ViewUnit& unit, sim::EntityId key) { return unit.id < key; });
        return found != units.end() && found->id == id ? &*found : nullptr;
    }
    [[nodiscard]] const ViewPlayer* player(tactical::PlayerId id) const {
        for (const ViewPlayer& entry : players) {
            if (entry.id == id) return &entry;
        }
        return nullptr;
    }
};

// WNO-11/13: forced object queries consume requester-specific raw fog, including AI.
inline bool object_fogged(const WorldView& view, const ViewUnit& unit, tactical::PlayerId player) {
    const ViewPlayer* viewer = view.player(player);
    return viewer == nullptr || viewer->snapshot_index >= 64 ||
        (unit.visible_to & (std::uint64_t{1} << viewer->snapshot_index)) == 0;
}

} // namespace eawr::script::foc::detail

namespace eawr::script::foc::ai {
class Engine;
struct Context;
struct Target;
struct PlayerAi;

struct ReinforcementResult {
    std::uint32_t next_attempt{};
    std::uint32_t candidates{};
    std::uint64_t nanoseconds{};
};

class World {
public:
    virtual ~World() = default;
    [[nodiscard]] virtual std::shared_ptr<const detail::WorldView> view() const = 0;
    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t completed_tick() const noexcept = 0;
    [[nodiscard]] virtual std::optional<Real> perception(const Engine&, const Equation&, const Context&, AiRandom&) const = 0;
    [[nodiscard]] virtual Context goal_context(const PlayerAi&, const Target*) const = 0;
    [[nodiscard]] virtual bool target_matches(sim::tactical::PlayerId, const std::set<std::string>&, const Target*) const = 0;
    [[nodiscard]] virtual std::vector<Target> targets(const Engine&, const PlayerAi&) const = 0;
    [[nodiscard]] virtual std::vector<Target> new_targets(const Engine&) const = 0;
    [[nodiscard]] virtual bool target_alive(const Target&) const = 0;
    [[nodiscard]] virtual std::string_view goal_mode() const noexcept = 0;
    [[nodiscard]] virtual std::span<const sim::tactical::Player> players() const noexcept = 0;
    [[nodiscard]] virtual const sim::tactical::EconomyRules& economy() const noexcept = 0;
    [[nodiscard]] virtual const sim::tactical::EconomyView* account(sim::tactical::PlayerId) const = 0;
    [[nodiscard]] virtual const std::map<sim::EntityId, sim::tactical::PadState>& pads() const noexcept = 0;
    [[nodiscard]] virtual bool arriving(sim::EntityId) const = 0;
    [[nodiscard]] virtual bool build_allowed(sim::tactical::PlayerId, sim::EntityId, sim::tactical::TypeId) const = 0;
    [[nodiscard]] virtual const sim::tactical::BuildOption* build_option(sim::tactical::PlayerId, sim::EntityId, sim::tactical::TypeId) const = 0;
    [[nodiscard]] virtual sim::tactical::ProductionCounts production_counts(sim::tactical::PlayerId, sim::tactical::TypeId) const = 0;
    [[nodiscard]] virtual bool construction_allowed(sim::tactical::PlayerId, sim::EntityId) const = 0;
    [[nodiscard]] virtual bool constructing_for(sim::EntityId, sim::tactical::PlayerId) const = 0;
    [[nodiscard]] virtual sim::EntityId next_entity_id() const noexcept = 0;
    [[nodiscard]] virtual std::optional<ReinforcementResult> reinforcement_search_result(sim::tactical::PlayerId, std::uint64_t) const = 0;
    [[nodiscard]] virtual std::span<const sim::tactical::Event> events() const noexcept = 0;
    [[nodiscard]] virtual std::span<const sim::tactical::CombatEvent> combat_events() const noexcept = 0;
};
} // namespace eawr::script::foc::ai
