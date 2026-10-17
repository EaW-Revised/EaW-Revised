#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/campaign/conflict.hpp"
#include "eawr/campaign/invasion_offer.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/campaign/story_notices.hpp"
#include "eawr/platform/tactical_feed.hpp"
#include "eawr/platform/campaign_tactical_view.hpp"
#include "eawr/platform/campaign_loading_view.hpp"
#include "eawr/platform/campaign_end_view.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::campaign { class CampaignRoot; }

// ADR-019: presentation consumes values and immutable snapshots, never the owning session.
namespace eawr::platform {

struct CampaignServiceConfiguration;
struct PreparedCampaignBattle;
class PreparedLiveRestore;

// #3035: the authored campaign a save belongs to, stored in unhashed checkpoint section 0x8002
// so a front end can prepare that campaign before restoring. Names come from the data, never a
// fixed faction or campaign list.
struct LiveSaveCampaign {
    std::string campaign;     // authored campaign entry
    std::string campaign_set; // its Campaign_Set
    std::string faction;      // the saving seat's playable faction
    std::string difficulty;
    std::uint32_t seat{};     // the saving seat
    bool multiplayer{};
    std::optional<campaign::CampaignCustomOptions> custom{};
    friend bool operator==(const LiveSaveCampaign&, const LiveSaveCampaign&) = default;
};
struct LiveSaveCampaignSource {
    std::string campaign, campaign_set, difficulty;
    bool multiplayer{};
    struct Faction { sim::galactic::PlayerId player{}; std::string name; };
    std::vector<Faction> factions; // each save resolves its seat's player here
    std::optional<campaign::CampaignCustomOptions> custom{};
};
struct LiveSaveOptions {
    // UTF-8 filesystem directory, decoded by the save repository.
    std::string directory;
    // Nonzero currently refuses capture: mod-list participation is a format seam.
    std::array<std::uint8_t, 32> mod_list_identity{};
    // Filled from a prepared authored campaign when absent; synthetic galaxies save without one.
    std::optional<LiveSaveCampaignSource> campaign;
};
struct LiveSaveRequest {
    sim::galactic::SeatId seat{};
    std::string name;
    bool overwrite{}, quick{}, autosave{}, prebattle{};
    std::vector<std::uint8_t> presentation_hints;
};
enum class LiveSavePhase { requested, rejected, captured, completed, failed };
struct LiveSaveMenuFacts { bool pending_battle{}, tactical{}; };
struct LiveSaveOutcome {
    std::uint64_t token{}, completed_tick{};
    std::string name;
    LiveSavePhase phase{LiveSavePhase::requested};
    std::optional<core::Diagnostic> diagnostic;
};
struct LiveSaveSlot {
    std::string name;
    // Repository facts, separate from localized labels and checkpoint ticks.
    std::string description;
    std::optional<std::int64_t> last_write_seconds;
    std::uint64_t completed_tick{}, galactic_frame{}, bytes{};
    bool child{};
    std::optional<LiveSaveCampaign> campaign; // absent: the save cannot start its own campaign
    std::optional<core::Diagnostic> diagnostic;
};
// Session-free: one slot's repository facts and campaign identity, without a restore.
[[nodiscard]] core::Result<LiveSaveSlot> read_save_slot(std::string_view directory, std::string_view name);

// Validated private owner; no startup pass or live thread runs while loading.
class PreparedLiveRestore final {
public:
    ~PreparedLiveRestore();
    [[nodiscard]] const sim::galactic::GalacticSetup& layout() const noexcept;
    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& presentation_hints() const noexcept;
    // Restored state digest after the player-load prologue: equal for every route into one save.
    [[nodiscard]] const std::string& state_sha256() const noexcept;
private:
    friend class LiveGameSession;
    class Impl;
    explicit PreparedLiveRestore(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

// WGS-03/68, ADR-019: opaque initialized campaign owner for presentation startup.
// The plan/layout and snapshot are read-only values; transfer starts its live thread.
class PreparedLiveCampaign final {
public:
    // Native GC AI installation is opt-in until its compute budget is met.
    // Off skips native AI preparation too; loading native AI state requires opt-in.
    [[nodiscard]] static core::Result<std::unique_ptr<PreparedLiveCampaign>> create(
        std::shared_ptr<const vfs::Vfs>, const data::Catalog&, const data::galactic::GalacticData&,
        const units::GalacticTypeTable&, std::string_view campaign,
        campaign::CampaignSetupOptions, std::string_view difficulty, bool install_native_ai = false,
        std::array<std::uint64_t, 6> ai_service_periods = {300, 1, 3, 3, 300, 1});
    ~PreparedLiveCampaign();
    [[nodiscard]] const campaign::CampaignSetupPlan& plan() const noexcept;
    [[nodiscard]] const sim::galactic::GalacticSetup& layout() const noexcept;
    [[nodiscard]] std::shared_ptr<const sim::galactic::GalacticSnapshot> snapshot() const;
    // Confirmed startup fact from the prepared owner, before its live thread starts.
    [[nodiscard]] bool native_ai_installed() const noexcept;
    // The AI players the runtime actually selected (player and authored AI player type).
    struct NativeAiPlayer { sim::galactic::PlayerId player{}; std::string player_type; };
    [[nodiscard]] const std::vector<NativeAiPlayer>& native_ai_players() const noexcept;
private:
    friend class LiveGameSession;
    class Impl;
    explicit PreparedLiveCampaign(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

struct LiveGameOrder {
    sim::galactic::SeatId seat{};
    sim::galactic::CommandPayload payload;
    // Without a tick, stamp on the simulation thread at its next command boundary.
    // Future orders claim their seat's sequence only when due.
    std::optional<std::uint64_t> tick;
    // This intent replaces payload; simulation owner records native queue effects.
    std::optional<sim::EntityId> accept_invasion{};
};

// Presentation publications contain confirmed values only. Solver/application
// services and their owning-session headers stay inside the platform producer.
struct LiveAutoResolveRecordEntry {
    std::string original_type;
    std::uint32_t owner{};
};
struct LiveAutoResolveOutcome {
    std::uint64_t planet{};
    std::uint32_t invader{}, defender{};
    bool land{};
    campaign::BattleResult result;
    std::vector<LiveAutoResolveRecordEntry> record;
};
struct LiveAutoResolveProgress {
    std::uint64_t conflict{};
    double elapsed_seconds{}, display_seconds{};
    struct Combatant {
        std::uint32_t owner{};
        std::string original_type, current_type;
    };
    std::array<Combatant, 2> combatants;
    struct SideRoster {
        std::uint32_t owner{};
        std::optional<Combatant> current, escort;
        std::vector<Combatant> queue;
    };
    std::optional<std::array<SideRoster, 2>> roster;
    bool initiated{}, withdraw_admitted{};
    std::uint32_t retreating_player{};
};
struct LiveBattleChoiceResponse {
    std::uint64_t conflict{}, revision{};
    sim::SeatId seat{};
    std::string refusal;
};

struct LiveGameFrame {
    std::shared_ptr<const sim::galactic::GalacticSnapshot> previous;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> latest;
    std::chrono::steady_clock::time_point latest_published{};
    // Session time advances while the galaxy's own frame is frozen in battle.
    std::uint64_t session_tick{};
    std::shared_ptr<const campaign::ConflictRecord> conflict;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> battle;
    std::optional<campaign::BattleResult> result;
    std::weak_ptr<const void> battle_lifetime;
    std::shared_ptr<const campaign::PendingConflictView> pending_conflict{};
    std::shared_ptr<const LiveAutoResolveProgress> auto_resolve_progress{};
    std::shared_ptr<const LiveAutoResolveProgress> auto_resolve_observer_progress{};
    std::shared_ptr<const LiveAutoResolveOutcome> auto_resolve_outcome{};
    std::vector<LiveBattleChoiceResponse> battle_choice_responses{};
    std::shared_ptr<const PreparedCampaignBattle> battle_presentation{};
    bool tactical_available{};
    std::string tactical_refusal{};
    std::shared_ptr<const campaign::ConflictRecord> result_conflict{};
    std::string tactical_refusal_code{};
    std::shared_ptr<const campaign::ObservedInvasionOffer> invasion_offer;
    std::optional<CampaignRetreatPolicy> retreat_policy{};
    std::shared_ptr<const std::vector<campaign::GameStoryNotice>> story_notices{}; // immutable observer publication
    std::shared_ptr<const CampaignEndView> campaign_end{};
};
[[nodiscard]] std::optional<CampaignTacticalView> campaign_tactical_view_for_observer(
    const LiveGameFrame&, sim::galactic::SeatId);
// Recheck the immutable publication when a delayed dialog callback fires.
// The ordinary session command boundary remains the final authority.
[[nodiscard]] std::optional<LiveGameOrder> battle_choice_order(const LiveGameFrame&,
    sim::galactic::SeatId, sim::EntityId conflict, bool tactical);

// WGM-176: only a published owner deadline may inject the zero resolve preference.
[[nodiscard]] std::optional<LiveGameOrder> pending_battle_timeout_order(const LiveGameFrame&,
    sim::galactic::SeatId);
[[nodiscard]] std::optional<LiveGameOrder> auto_resolve_withdraw_order(const LiveGameFrame&,
    sim::galactic::SeatId, sim::EntityId conflict);
[[nodiscard]] std::shared_ptr<const LiveAutoResolveProgress> auto_resolve_progress_for_observer(
    const LiveGameFrame&, sim::galactic::SeatId);

// Presentation-only retained event cursor. Prior ownership is captured at the
// event boundary, so consumers never infer losses by polling galaxy internals.
struct LiveGalacticEvent {
    std::uint64_t cursor{};
    sim::galactic::GalacticEvent event;
    std::optional<sim::galactic::PlayerId> previous_owner;
    struct PendingAlert {
        sim::galactic::PlayerId invader{}, defender{};
        bool land{};
    };
    std::optional<PendingAlert> pending_alert{};
    bool new_owner_playable{}; // WGM-194: committed existing player fact, not a faction guess
    // SND-68/WGEP-130: qualified local tech-setter request. For these rows
    // `event` is unused; the recipient is retained independently of snapshots.
    std::optional<sim::galactic::PlayerId> construction_options_player{};
    std::optional<sim::galactic::GalacticFeedbackNotice> feedback{};
};
struct LiveGalacticEvents {
    std::uint64_t through{};
    bool incomplete{}; // reader fell behind the bounded retention window
    std::vector<LiveGalacticEvent> events;
};

// WGUI-114: transient per-player wall clock; never checkpointed or hashed.
class GalacticApproachThrottle final {
public:
    [[nodiscard]] bool admit(sim::galactic::PlayerId player, std::uint64_t now_ms) {
        if (!player) return false;
        const auto at = last_.find(player);
        const auto last = at == last_.end() ? 0 : at->second;
        if (now_ms < last || now_ms - last < 5000) return false;
        last_[player] = now_ms;
        return true;
    }
private:
    std::map<sim::galactic::PlayerId, std::uint64_t> last_;
};

// The live pacer's wall-clock schedule (ADR-013: speed changes only the pacing). Step n after a
// rebase is due at start + n * interval, so wake-up latency and tick cost do not add to every
// period and a 4x setting really runs four times as many ticks per real second as long as the
// ticks fit the interval. A pacer that falls too far behind drops the missed time instead of
// spiralling. Pure arithmetic over caller-supplied time points; no clock is read here.
class LivePaceSchedule final {
public:
    using Clock = std::chrono::steady_clock;

    // Starts a fresh interval: the next step is due one interval after `now`.
    void rebase(const Clock::time_point now, const std::chrono::nanoseconds interval) noexcept {
        start_ = now;
        interval_ = interval;
        steps_ = 0;
    }
    [[nodiscard]] std::chrono::nanoseconds interval() const noexcept { return interval_; }
    [[nodiscard]] Clock::time_point next_due() const noexcept {
        return start_ + interval_ * static_cast<std::int64_t>(steps_ + 1U);
    }
    // One step ran (paced or forced by input); the next due time moves one interval on.
    void stepped() noexcept { ++steps_; }
    // Steps due at `now` and not yet run (1 when only the next one is due).
    [[nodiscard]] std::uint64_t behind(const Clock::time_point now) const noexcept {
        if (now < next_due()) return 0U;
        const auto due = static_cast<std::uint64_t>((now - start_) / interval_);
        return due > steps_ ? due - steps_ : 0U;
    }
    // When more than `max_catch_up` steps are due at once, forget the missed time (the next step is
    // due now). Returns whether it did.
    bool drop_backlog(const Clock::time_point now, const std::uint64_t max_catch_up) noexcept {
        if (behind(now) <= max_catch_up) return false;
        start_ = now - interval_;
        steps_ = 0;
        return true;
    }
private:
    Clock::time_point start_{};
    std::chrono::nanoseconds interval_{std::chrono::nanoseconds(1)};
    std::uint64_t steps_{};
};

class LiveGameSession final {
public:
    enum class Pacing { real_time, driven };
    struct Options {
        std::size_t workers{1};
        Pacing pacing{Pacing::real_time};
        // Presentation failure diagnostic only; an admitted player's pause has no timeout.
        std::chrono::milliseconds readiness_timeout{std::chrono::minutes(5)};
        // Called on the simulation thread before a step; stamped commands keep their keys.
        // Captures must outlive stop(). Exceptions fail only this live session.
        std::function<std::vector<sim::galactic::GameCommand>(std::uint64_t)> command_source{};
        // Deterministic conflict-service inputs, until the fleet entry service is wired.
        struct Entry {
            std::uint64_t tick{};
            campaign::ConflictRecord conflict;
            sim::tactical::TacticalContent content;
        };
        std::vector<Entry> entries{};
        // Opaque engine-owned startup services. Presentation cannot access the owning
        // campaign session; configuration refusal starts no simulation thread.
        std::shared_ptr<const CampaignServiceConfiguration> services{};
        LiveSaveOptions saves{};
        bool await_presentation{};
        // Real-time pacing: ticks that may be overdue before the pacer drops the missed wall time
        // (the tactical pacer's LiveSession::Options::max_catch_up, same value).
        std::uint64_t max_catch_up{5};
    };
    // The newest tick_cost_history parent steps are kept for the performance overlay.
    static constexpr std::size_t tick_cost_history = 4096;

    [[nodiscard]] static core::Result<std::unique_ptr<LiveGameSession>> start(
        const sim::galactic::GalacticSetup& setup, Options options);
    // WGS-03/68: retain the complete initialized root and its existing game.
    // This transfer creates no second GameSession and does not rerun startup.
    [[nodiscard]] static core::Result<std::unique_ptr<LiveGameSession>> start(
        campaign::CampaignRoot root, Options options);
    [[nodiscard]] static core::Result<std::unique_ptr<LiveGameSession>> start(
        std::unique_ptr<PreparedLiveCampaign>, Options options);
    [[nodiscard]] static core::Result<std::unique_ptr<LiveGameSession>> start(
        std::unique_ptr<PreparedLiveRestore>, Options options);
    // ADR-013 / WGM-U02: speed is only a wall-clock factor over 30 logical frames/s.
    // Integer nanoseconds, clamped to at least one; no simulation state reads the clock.
    [[nodiscard]] static std::chrono::nanoseconds tick_interval(sim::math::Fixed speed) noexcept;

    ~LiveGameSession();
    LiveGameSession(const LiveGameSession&) = delete;
    LiveGameSession& operator=(const LiveGameSession&) = delete;

    // Thread-safe input. Refusals are returned by rejected_orders(). Untimed input and
    // commands for the current tick drive one step even when the galaxy is paused.
    // Future input waits for its tick; paused sessions never generate empty ticks to reach it.
    void submit(LiveGameOrder order);
    void submit(sim::galactic::GameCommand command);
    // Admission/capture runs on the simulation thread; file completion is separate.
    [[nodiscard]] std::uint64_t request_save(LiveSaveRequest);
    [[nodiscard]] std::vector<LiveSaveOutcome> save_outcomes() const;
    [[nodiscard]] bool wait_save(std::uint64_t token, std::chrono::milliseconds timeout) const;
    [[nodiscard]] core::Result<std::vector<LiveSaveSlot>> save_slots() const;
    [[nodiscard]] core::Result<void> delete_save(std::string_view name);
    [[nodiscard]] core::Result<std::unique_ptr<PreparedLiveRestore>> prepare_load(
        std::string_view name, bool player_load = true) const;
    // Driven mode's upper tick target. A paused galaxy still needs a due command to step.
    void advance_to(std::uint64_t tick);
    [[nodiscard]] bool wait_for(std::uint64_t tick, std::chrono::milliseconds timeout) const;
    [[nodiscard]] LiveGameFrame frame() const;
    // WAR-115/121: filter roster/type facts at the observer boundary before presentation.
    [[nodiscard]] LiveGameFrame frame(sim::galactic::SeatId observer) const;
    // WGP-162: admitted human participants only; no unrestricted loading facts.
    [[nodiscard]] std::shared_ptr<const CampaignLoadingView> loading(sim::galactic::SeatId) const;
    // Allocation-free menu polling; terminal save results are copied once.
    [[nodiscard]] LiveSaveMenuFacts save_menu_facts() const;
    [[nodiscard]] std::optional<LiveSaveOutcome> terminal_save_outcome(std::uint64_t token) const;
    [[nodiscard]] LiveGalacticEvents galactic_events_after(std::uint64_t cursor) const;
    // Returns a conflict-scoped feed. Stale feeds cannot start or command a later child.
    // Readiness belongs to the live pacer; it never enters GameSession's state or replay.
    [[nodiscard]] std::shared_ptr<TacticalFeed> battle_feed(sim::EntityId conflict, sim::galactic::SeatId seat);
    void battle_ready(sim::EntityId conflict);
    // Optional live startup barrier for a validated restored first frame/HUD.
    void presentation_ready();
    [[nodiscard]] bool battle_paused(sim::EntityId conflict) const;
    [[nodiscard]] bool battle_is_ready(sim::EntityId conflict) const;
    [[nodiscard]] std::uint64_t completed_tick() const;
    // Wall-clock cost of each parent step after `after` (a session tick), oldest first:
    // `command-source` (the callback, which runs the GC AI) and `session-step`. Never part of a
    // snapshot, hash or replay.
    [[nodiscard]] std::vector<LiveTickCost> tick_costs_after(std::uint64_t after) const;
    [[nodiscard]] std::optional<core::Diagnostic> failure() const;
    [[nodiscard]] std::vector<core::Diagnostic> rejected_orders() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    // Lifetime observation for teardown contracts; does not expose mutable state.
    [[nodiscard]] std::weak_ptr<const void> lifetime() const noexcept;
    // Joins the simulation thread, idempotently. Last published snapshots remain readable.
    void stop();
    // WGS-82/176/177: outer presentation owner supplies its current admission
    // gates. Copies end facts, then joins and releases solo AI/scripts before
    // publishing. Ordinary multiplayer only retains this local quit intent.
    [[nodiscard]] core::Result<std::shared_ptr<const CampaignEndView>> consume_campaign_result(
        sim::galactic::SeatId observer, bool dialog_active, bool movie_playing);
    // After stop(): every completed tick's hash and the executed command stream.
    [[nodiscard]] std::vector<std::string> tick_hashes() const;
    [[nodiscard]] sim::galactic::GalacticReplay record() const;

private:
    class Impl;
    explicit LiveGameSession(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace eawr::platform
