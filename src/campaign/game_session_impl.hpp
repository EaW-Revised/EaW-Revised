#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/sim/tactical/admission.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::campaign {
namespace galactic = sim::galactic;
namespace tactical = sim::tactical;
namespace authoritative = script::authoritative;
namespace game_session_detail {
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, std::size_t size);
void overwrite(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value, std::size_t size);
void put_digest(std::vector<std::uint8_t>& bytes, const std::string& digest);
core::Result<std::string> conflict_hash(const ConflictRecord& record);
std::string combined_hash(const std::string& galaxy, const std::string& world,
    const std::string& scripts, const std::string& context);
} // namespace game_session_detail
using game_session_detail::put;
using game_session_detail::overwrite;
using game_session_detail::put_digest;
using game_session_detail::conflict_hash;
using game_session_detail::combined_hash;

class GameSession::Impl {
public:
    explicit Impl(galactic::GalacticSession session);
    struct ChildTimelinePublication {
        ModeTimelineSnapshot snapshot;
        bool retired{};
    };
    struct TimelinePublication {
        ModeTimelineSnapshot galaxy{{SessionMode::galaxy, 0}, 0, 0};
        std::optional<ModeTimelineSnapshot> child, historical;
        // Process-local stable child slots, allocated at entry/restore only.
        std::shared_ptr<ChildTimelinePublication> child_slot, historical_slot;
    };
    std::shared_ptr<TimelinePublication> timelines = std::make_shared<TimelinePublication>();
    ChildTimelineTech child_tech;
    core::Result<void> update_galaxy_timeline(const std::optional<galactic::PlayerEconomySetup>& inputs);
    core::Result<void> update_child_timeline();
    static std::string with_timeline(const std::string& base, const TimelinePublication& publication, bool ai_sync = false);
    galactic::GalacticSession galaxy;
    std::optional<authoritative::ScriptedGalacticSession> scripts;
    galactic::GalacticSession::ControlEffectsHandler control_observer;
    StoryNoticeHandler story_handler;
    bool reconstructing_native_notices{};
    std::map<galactic::PlanetId, std::string> story_planets;
    std::map<galactic::PlayerId, std::string> story_factions;
    std::vector<GameStoryNotice> story_notices; // current publication only, never saved/hashed
    core::Result<void> story_notice(GameStoryNotice notice);
    // #683: the live plot engine; absent when the campaign names no plots. Fed by story_notice and the
    // assigned-technology notice, serviced once per galaxy step (GFS-15).
    std::optional<galactic::story::Engine> story_engine;
    std::vector<galactic::story::Triggered> story_triggers; // current publication only
    galactic::GalacticSession::TechnologyNoticeHandler technology_handler; // caller's, run after the story saw the notice
    std::vector<std::string> story_notes; // published once as diagnostics of the next step
    std::vector<std::string> entry_notes; // passengers an entry left out; same publication, never saved or hashed
    StoryRewardApplier story_rewards;
    const sim::PartitionExecutor* executor{}; // the running step's, for rewards applied inside it
    std::set<std::string> story_reported;     // refusal texts already published (diagnostics only, never saved)
    core::Result<void> apply_story_rewards();
    // WST-20 (3)/55: the engine's Lua callbacks, forwarded to the story scripts before the rewards are applied.
    std::shared_ptr<const GalacticStoryScripts> story_scripts;
    std::uint64_t story_call_sequence{};
    core::Result<void> forward_story_calls();
    core::Result<void> tech_assigned(galactic::PlayerId player, std::int64_t level);
    core::Result<void> service_story();
    static std::string with_story(const std::string& base, const std::string& story);
    core::Result<void> control_notices(std::span<const galactic::PoliticalControlEffect> effects,
        std::span<const galactic::Planet> planets);
    core::Result<void> battle_story(galactic::PlanetId planet, BattleLayer layer,
        galactic::PlayerId winner, galactic::PlayerId loser);
    GalacticAiServices native_ai;
    core::Result<void> battle_notice(GalacticAiBattleKind kind, const ConflictRecord& conflict,
        const BattleResult* result = nullptr, bool automatic = false);
    mutable std::optional<std::uint64_t> native_oracle_session;
    core::Result<std::string> native_hash() const;
    static std::string with_native(const std::string& base, const std::string& native);
    const galactic::GalacticSession& world() const;
    galactic::GalacticSession& world();
    core::Result<std::string> galaxy_hash();
    std::uint64_t tick{};
    AutoResolveOutcomeServices outcome_services;
    std::array<std::optional<AutoResolveOutcomeApplication>, 8> outcome_history{};
    std::size_t outcome_record_index{};
    std::map<galactic::CommandOrderKey, galactic::GameCommand> pending;
    std::map<galactic::SeatId, std::pair<std::uint64_t, std::uint64_t>> last;
    std::vector<galactic::GameCommand> executed;
    // #4058: history before this boundary is not kept; the next boundary (a save or load happened) is applied at the
    // start of the next step so a capture or recapture right after either still sees the window it belongs to.
    HistoryBase history;
    std::optional<HistoryBoundary> scheduled_base;
    void rebase_history();
    struct Child {
        ConflictRecord conflict;
        authoritative::ScriptedTacticalSession session;
        std::map<tactical::PlayerId, std::uint64_t> sequences;
        std::map<sim::EntityId, tactical::PlayerId> killers;
        std::string galaxy_hash, script_hash, conflict_hash;
        sim::StateHash world_hash;
        // Observation only: never enters deterministic state or recording.
        std::shared_ptr<const void> lifetime{std::make_shared<const int>(0)};
        bool owns_pause{};
        std::string context_hash() const;
    };
    struct Layer {
        ConflictLayerServices services;
        bool installed{};
        std::shared_ptr<GalacticParentQueryFactory> parent_queries = std::make_shared<GalacticParentQueryFactory>();
        bool queries_installed{};
    };
    std::array<Layer, 2> layers;
    Layer& layer(BattleLayer value);
    const Layer& layer(BattleLayer value) const;
    bool assembling_layers();
    ConflictLayerServices& services(const ConflictRecord& value);
    core::Result<bool> tactical_selection(const ConflictRecord&, bool requested, bool forced) const;
    core::Result<authoritative::ScriptScheduler> scripts_for(const ConflictRecord& record,
        ScriptRequirement requirement = ScriptRequirement::required);
    std::optional<Child> child, staged;
    bool restoring_child{}; // inactive reconstruction only; suppress tactical service
    std::optional<tactical::TacticalReplay> last_battle;
    std::set<sim::EntityId> ended;
    std::shared_ptr<sim::StateHasher> hasher;
    HeroDefeatResolver hero_resolver;
    galactic::HeroPlacement hero_placement;
    galactic::HeroStructureResolver hero_structures;
    LiveHeroReturns live_hero_returns;
    TacticalReturnState return_state;
    std::optional<BattleResult> latest_result;
    std::optional<ConflictRecord> latest_context;
    AutoResolver auto_resolver;
    AutoResolveStateWriter auto_state_writer;
    AutoResolveStateReader auto_state_reader;
    AutoResolveProgressReader auto_progress;
    AutoResolveWithdrawal auto_withdraw;
    std::shared_ptr<const AutoResolveOutcomeApplication> latest_auto_outcome;
    VoteProgress vote_progress;
    PendingBattleAlertReader pending_alert;
    ConflictGate conflict_gate;
    bool conflict_services{};
    std::optional<RecordedAdmission> admission;
    std::vector<RecordedAdmission> queued_admissions, waiting_admissions;
    CorruptionMissionService corruption_mission_service;
    core::Result<void> queue_conflict(ConflictRecord, Arbitration);
    std::vector<RecordedAdmission> admissions;
    std::multimap<std::uint64_t, RecordedAdmission> replay_admissions;
    std::map<galactic::PlayerId, bool> votes;
    std::optional<std::uint64_t> multiplayer_vote_timeout, vote_started;
    galactic::PlayerId withdrawing_player{};
    bool conflict_pause{};
    galactic::SeatId interactive_seat{};
    sim::EntityId summary_conflict{};
    void refresh_system_hold();
    core::Result<void> release_summary();
    core::Result<void> hold_summary(sim::EntityId conflict, galactic::PlayerId invader, galactic::PlayerId defender);
    static std::string with_summary(const std::string& base, galactic::SeatId observer, sim::EntityId conflict);
    std::optional<core::Diagnostic> failure;
    std::vector<RecordedConflict> conflicts;
    std::vector<galactic::GameCommand> load_fixups;
    std::multimap<std::uint64_t, galactic::GameCommand> replay_fixups;
    std::map<std::uint64_t, ConflictRecord> replay_conflicts;
    std::map<sim::EntityId, RecordedConflict> replay_selected;
    const galactic::Seat* seat(const galactic::SeatId id) const;
    bool human_side(const galactic::PlayerId owner) const;
    tactical::PlayerId player(const galactic::SeatId id) const;
    tactical::CampaignReserveAdmission reserve_admission(const tactical::PlayerId owner) const;
    core::Result<void> refresh_battle_seats();
    bool role_pending() const;
    std::string queued_context(std::string digest) const;
    std::string child_context_hash() const;
    core::Result<std::string> base_hash();
    core::Result<std::string> hash();
    BattleResult result() const;
};

} // namespace eawr::campaign
