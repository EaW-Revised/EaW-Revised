#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// UI-07 (#304, docs/ui/ui-layer.md §3.1 and rules UI-C1/UI-C2): the one path from player input
// to the simulation. HUD buttons, hotkeys and world clicks all build a TacticalIntent and hand
// it to a CommandSink; nothing in presentation writes simulation state. The standard sink
// stamps each intent as a PlayerCommand for the next open tick; the session's stepping thread
// (the live session bridge, #80) takes and submits them at the tick boundary, so they enter the
// replay like any recorded command.
namespace eawr::presentation::ui {

enum class TacticalVerb : std::uint8_t {
    stop = 0,
    move = 1,
    attack = 2,
    face = 3,    // turn to face a point; not in tactical rules v1
    ability = 4, // a unit ability (#76); a targeted special ability has no command yet
    attack_move = 5, // #452: move to a point, engaging what comes into range
    guard = 6,       // #452: follow and escort a unit, or hold a point
    // #530 (space-purchasing PU-10 to PU-34): the local player's economy. A buy lists the station,
    // a cancel and a reinforce no unit.
    buy = 7,
    cancel = 8,
    reinforce = 9,
    pad_build = 10, // WBP-09/10
    pad_sell = 11, // WBP-30
    intentional_quit = 12, // WBF-43/48
    repair_hardpoint = 13, // WSL-40: station in units, hardpoint index, local payer
    special_fire = 14, // WSW-31: one source and a player-admitted object target
    beacon = 15, // HCB-12: player-owned point placement, no selection.
};

// Where an intent came from. Presentation-only: it never enters a command, so a HUD button and
// the equivalent world click give the same command stream (UI-C1).
enum class CommandOrigin : std::uint8_t { hud_button = 0, hotkey = 1, world_click = 2, minimap = 3 };

struct TacticalIntent {
    TacticalVerb verb{TacticalVerb::stop};
    // The units to order, in any order; the scheduler sorts them and drops duplicates.
    std::vector<sim::EntityId> units;
    // move, face, attack-move and guard: the point; ability: the target point when the ability
    // takes one.
    sim::math::Vec3 destination{};
    // attack: the target; guard: the guarded unit, or none to hold the point; ability: the target
    // unit when the ability takes one.
    sim::EntityId target{sim::invalid_entity_id};
    // ability: the ability's name CRC (the retail SpecialAbility key); zero otherwise.
    std::uint32_t ability{};
    CommandOrigin origin{CommandOrigin::world_click};
    // ability (#76, docs/behaviour/space-abilities.md AB-50): a modelled unit ability and what to
    // do with it. With none the intent is a targeted special ability, which has no command yet.
    sim::tactical::AbilityKind unit_ability{sim::tactical::AbilityKind::none};
    sim::tactical::AbilityAction ability_action{sim::tactical::AbilityAction::activate};
    // attack (#531, docs/behaviour/space-orders.md OR-20): the index in the target type's
    // HardPoints list of the hardpoint to attack, or attack_hull for the unit.
    std::uint32_t hardpoint{sim::tactical::attack_hull};
    // #530: buy and reinforce: the type; cancel: the queue and its entry; reinforce: the point is
    // `destination`.
    sim::tactical::TypeId type{};
    sim::tactical::BuildQueue queue{sim::tactical::BuildQueue::units};
    std::uint32_t index{};
    std::uint64_t queue_entry_id{}; // PU-17: identity from the displayed queue entry.
    bool through_hazards{}; // WHZ-08a: explicit double-click movement
    std::optional<sim::math::Fixed> reinforcement_facing{}; // WR-X01: local battle deployment choice
    std::uint64_t reinforcement_token{}; // WGM-48: published persistent reserve identity
    bool append{}; // HCB-57: queued point movement.
    friend bool operator==(const TacticalIntent&, const TacticalIntent&) = default;
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_intent = "EAWR-UI-0751";     // no units, a zero ID, too many units
inline constexpr std::string_view unsupported_intent = "EAWR-UI-0752"; // the verb has no sim command yet
} // namespace diagnostic_codes

// Maps an intent to the sim payload it becomes. Fails for face, and for an ability intent without a
// modelled unit ability, until the tactical rules gain those commands; the caller then shows "cannot" feedback and records nothing.
[[nodiscard]] core::Result<sim::tactical::CommandPayload> command_payload(const TacticalIntent& intent);

// The interface UI and world input call. Implementations must not touch simulation state
// synchronously: an accepted intent only becomes a command at the next tick boundary.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    // Validates and queues. A failure leaves the sink unchanged.
    [[nodiscard]] virtual core::Result<void> issue(const TacticalIntent& intent) = 0;

protected:
    CommandSink() = default;
    CommandSink(const CommandSink&) = default;
    CommandSink& operator=(const CommandSink&) = default;
};

// Pending UI reservations survive transport handoff until their effects reach presentation.
struct PendingCommand {
    std::uint64_t tick{}, sequence{};
    TacticalIntent intent;
    bool handed_off{};
    std::optional<std::uint64_t> execution_tick{};
};
using PendingBuildOption = std::function<const sim::tactical::BuildOption*(sim::EntityId, sim::tactical::TypeId)>;
[[nodiscard]] sim::tactical::EconomyView pending_economy(sim::tactical::EconomyView snapshot,
    std::span<const PendingCommand> pending, const PendingBuildOption& option_of,
    std::optional<std::uint64_t> completed_tick = std::nullopt);
[[nodiscard]] sim::tactical::ProductionCounts pending_production_counts(
    sim::tactical::ProductionCounts base, const sim::tactical::EconomyView& ledger,
    std::uint64_t completed_tick, std::span<const PendingCommand> pending,
    const PendingBuildOption& option_of, sim::tactical::TypeId type);

// The standard sink, and the hand-off of one local player's orders from the presentation
// thread to the thread that steps the session. Each side has one owner:
// - issue() is the producer side, called on the Godot main thread (through OrderInput). It
//   validates the intent and stamps it at once as a command (open tick, player, sequence): the
//   sequence is the issue order, and the open tick is the earliest tick not yet taken.
// - take() is the consumer side, called on the thread that steps the session (the simulation
//   thread of the live session, #80) at the tick boundary, before it steps `next_tick`.
// A mutex guards the queue and the open tick, and take() closes its tick inside it, so every
// command reaches exactly one take() and none is stamped for a tick that already ran. Within a
// tick commands keep issue order, so the order never depends on thread timing; only the tick an
// order lands in depends on when the player gave it, as with any live input, and the replay
// records that tick. Sequences increase across ticks and never repeat, so every batch follows
// the player's previous submission (TacticalSession::submit). One per local player.
class CommandScheduler final : public CommandSink {
    struct EpochState {
        std::mutex mutex;
        std::optional<std::uint64_t> epoch;
    };
public:
    // Producer-thread scopes nest around decoding one input. The shared state
    // keeps restoration safe even if a callback releases its scheduler.
    class IntentEpochScope final {
    public:
        IntentEpochScope() = default;
        IntentEpochScope(IntentEpochScope&& other) noexcept;
        IntentEpochScope(const IntentEpochScope&) = delete;
        IntentEpochScope& operator=(const IntentEpochScope&) = delete;
        IntentEpochScope& operator=(IntentEpochScope&&) = delete;
        ~IntentEpochScope();
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
        [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    private:
        friend class CommandScheduler;
        IntentEpochScope(std::shared_ptr<EpochState> state, std::optional<std::uint64_t> previous,
            std::uint64_t epoch) : state_(std::move(state)), previous_(previous), epoch_(epoch) {}
        std::shared_ptr<EpochState> state_;
        std::optional<std::uint64_t> previous_;
        std::uint64_t epoch_{};
    };
    // A nonempty epoch provider selects LAN intent admission: issue reads its live
    // frame, take drains every queued intent while retaining that producer epoch,
    // and the lockstep handoff assigns the final execution key. The provider is
    // called by issue/open_tick and capture_intent_epoch under the epoch mutex;
    // it must be thread-safe and must not call back into this scheduler.
    explicit CommandScheduler(sim::tactical::PlayerId player, std::uint64_t first_sequence = 0,
        std::uint64_t first_tick = 0, std::function<std::uint64_t()> intent_epoch = {});

    // Sample and reserve under the same lock observed by LAN sealing. Decoding
    // callbacks must not block: a held epoch can prevent simulation progress.
    [[nodiscard]] IntentEpochScope capture_intent_epoch();
    // Earliest input still being decoded or queued. LAN seals below its target,
    // then releases the reservation when take hands the encoded intent over.
    [[nodiscard]] std::optional<std::uint64_t> pending_intent_epoch() const;

    // Producer side. Thread-safe; a failure queues nothing and uses no sequence.
    [[nodiscard]] core::Result<void> issue(const TacticalIntent& intent) override;

    // Enables snapshot reservations. Retire strictly after the assigned execution tick;
    // untaken inputs and LAN producer epochs alone never acknowledge execution.
    void present_snapshot(std::uint64_t completed_tick);
    void present_result(std::uint64_t sequence);
    void note_execution(std::uint64_t sequence, std::uint64_t tick);
    void present_event_gap(std::uint64_t lost_through);
    [[nodiscard]] std::vector<PendingCommand> reservations() const;
    [[nodiscard]] bool consumed(const TacticalIntent& intent) const;
    [[nodiscard]] std::uint64_t reservation_revision() const;

    // Consumer side: removes and returns, in sequence order, the commands for `next_tick`, the
    // session's next tick to execute (its completed_tick()), and closes that tick: an intent
    // issued after this returns is stamped for a later tick. A command stamped for an earlier
    // tick (one the consumer never took) is restamped to `next_tick`. Returns nothing when no
    // command is due. With an intent epoch provider, drains all current intents
    // and preserves their producer epochs for LAN admission. Thread-safe.
    [[nodiscard]] std::vector<sim::tactical::PlayerCommand> take(std::uint64_t next_tick);

    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] sim::tactical::PlayerId player() const noexcept { return player_; }
    [[nodiscard]] std::uint64_t next_sequence() const;
    // The tick the next issued intent is stamped for.
    [[nodiscard]] std::uint64_t open_tick() const;
    // Optional acceptance instrumentation, called after a successful issue.
    void set_issue_observer(std::function<void(std::uint64_t, std::uint64_t)> observer) {
        const std::lock_guard lock(mutex_); issue_observer_ = std::move(observer);
    }

private:
    const sim::tactical::PlayerId player_;
    mutable std::mutex mutex_;
    std::uint64_t next_sequence_;
    std::uint64_t open_tick_;
    // LAN owns the future execution stamp. Its producer records the live epoch,
    // and a consumer drains intents even when that epoch has already been visited.
    std::function<std::uint64_t()> intent_epoch_;
    std::shared_ptr<EpochState> epoch_state_;
    [[nodiscard]] std::uint64_t producer_epoch() const;
    // Stamped commands in sequence order; their ticks never decrease.
    std::vector<sim::tactical::PlayerCommand> queue_;
    std::function<void(std::uint64_t, std::uint64_t)> issue_observer_;
    bool reservations_enabled_{};
    std::vector<PendingCommand> reservations_;
    std::uint64_t reservation_revision_{};
};

// Presentation-local order input (UI-C2): the selection and the HUD's pending order mode. It
// turns HUD buttons, hotkeys and world clicks into intents on one sink, so every path yields
// the same commands. None of this state enters a command, the replay or a hash. Selection
// gestures themselves (click, box, type, group) belong to #82, which sets the selection here.
// #452 adds the command bar's attack-move and guard modes (docs/behaviour/space-orders.md OR-01).
enum class OrderMode : std::uint8_t { none = 0, move = 1, attack = 2, attack_move = 3, guard = 4, waypoint = 5 };

// What the pointer is over when a world click lands: the point on the play plane and, when
// the pick hit a unit, that unit, whether it is hostile to the local player, whether the local
// player owns it and whether it is selected.
struct WorldPick {
    sim::math::Vec3 point{};
    sim::EntityId entity{sim::invalid_entity_id};
    bool hostile{};
    bool own{};
    bool selected{};
    // #531 (OR-20): the hardpoint reticle under the pointer when `entity` is its unit, as its
    // index in the unit type's HardPoints list, else attack_hull.
    std::uint32_t hardpoint{sim::tactical::attack_hull};
};

// The modifier keys held with a world click (OR-01): Ctrl attack-moves, Ctrl and Alt guard.
struct OrderModifiers {
    bool ctrl{};
    bool alt{};
    bool through_hazards{};
};

class OrderInput final {
public:
    explicit OrderInput(CommandSink& sink) noexcept : sink_(&sink) {}

    void set_selection(std::vector<sim::EntityId> units);
    [[nodiscard]] const std::vector<sim::EntityId>& selection() const noexcept { return selection_; }
    [[nodiscard]] OrderMode mode() const noexcept { return mode_; }

    // HUD buttons and hotkeys. Stop issues at once; move and attack arm a mode that the next
    // world command consumes (the retail command bar's move and attack modes).
    [[nodiscard]] core::Result<void> stop(CommandOrigin origin);
    void arm(OrderMode mode) noexcept { mode_ = mode; }
    void cancel_mode() noexcept { mode_ = OrderMode::none; }
    [[nodiscard]] core::Result<void> ability(std::uint32_t ability_crc, const WorldPick& pick, CommandOrigin origin);

    // The world order click (right click, or the left click that completes an armed mode).
    // No mode: a hostile unit is attacked, anything else is a move to the pick point; with Ctrl
    // held that move is an attack-move, and with Ctrl and Alt held an own unit that is not
    // selected is guarded and anything else but a hostile unit is a guard of the point (OR-01).
    // Move mode: always a move. Attack mode: only a hostile unit gives a command; anything else
    // gives none and keeps the mode. Attack-move and guard modes act as their modifiers do. An
    // issued command disarms the mode, except Waypoint remains armed for another point. With no selection nothing is issued. Returns whether an
    // intent was issued.
    [[nodiscard]] core::Result<bool> world_command(const WorldPick& pick, CommandOrigin origin = CommandOrigin::world_click,
        OrderModifiers modifiers = {});

private:
    [[nodiscard]] core::Result<void> send(TacticalIntent intent);
    CommandSink* sink_;
    std::vector<sim::EntityId> selection_;
    OrderMode mode_{OrderMode::none};
};

} // namespace eawr::presentation::ui
