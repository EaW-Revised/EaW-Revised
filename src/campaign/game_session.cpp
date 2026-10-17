#include "eawr/campaign/game_session.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/campaign/invasion_offer.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "checkpoint_owner_internal.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <span>
#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <utility>

namespace eawr::campaign {
namespace galactic = sim::galactic;
namespace tactical = sim::tactical;
namespace authoritative = script::authoritative;
namespace {
template <class A, class T> void checkpoint_game_owner(A& a, T& v) {
    a(v.tick, v.pending, v.last, v.executed, v.ended, v.last_battle,
        v.conflict_services, v.admission, v.queued_admissions, v.waiting_admissions,
        v.admissions, v.replay_admissions, v.votes, v.withdrawing_player, v.conflict_pause,
        v.conflicts, v.load_fixups, v.replay_fixups, v.replay_conflicts, v.replay_selected,
        v.timelines->galaxy, v.timelines->child, v.timelines->historical);
}
template <typename T>
core::Result<T> fail(std::string message, std::string_view code = galactic::diagnostic_codes::invalid_command) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, const std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) { bytes.push_back(static_cast<std::uint8_t>(value & 255U)); value >>= 8U; }
}
void overwrite(std::vector<std::uint8_t>& bytes, const std::size_t offset, std::uint64_t value, const std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) { bytes[offset + i] = static_cast<std::uint8_t>(value & 255U); value >>= 8U; }
}
void put_digest(std::vector<std::uint8_t>& bytes, const std::string& digest) {
    const auto nibble = [](const char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < digest.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>((nibble(digest[i]) << 4) | nibble(digest[i + 1])));
}
core::Result<std::string> conflict_hash(const ConflictRecord& record) {
    tactical::TacticalReplay replay; replay.setup = record.tactical;
    auto setup = tactical::write_replay(replay);
    if (!setup) return core::Result<std::string>::failure(setup.error());
    std::vector<std::uint8_t> bytes;
    const auto scalar = [&](const auto value) { put(bytes, value, 8); };
    const auto flag = [&](const bool value) { put(bytes, value ? 1 : 0, 1); };
    const auto blob = [&](const auto& value) {
        scalar(value.size()); bytes.insert(bytes.end(), value.begin(), value.end());
    };
    scalar(record.id); flag(record.layer == BattleLayer::land); scalar(record.planet);
    scalar(record.invader); scalar(record.defender);
    flag(record.raid); flag(record.corruption_mission); flag(record.tutorial); flag(record.ai_tactical_credits); scalar(record.corruption_request);
    scalar(record.forces.size());
    for (const auto& force : record.forces) {
        scalar(force.id); scalar(force.type); scalar(force.owner); scalar(force.parent);
        scalar(static_cast<unsigned>(force.role)); scalar(force.tactical_type); scalar(force.carrier); scalar(force.purchase_type);
        flag(force.destroy_after_tactical); flag(force.hero);
    }
    scalar(record.seats.size());
    for (const auto& seat : record.seats) { scalar(seat.seat.value); scalar(seat.player); }
    scalar(record.station); scalar(record.superweapon); scalar(record.station_owner); scalar(record.station_type);
    scalar(record.station_level); scalar(record.terrain_index);
    scalar(record.structure_markers.size());
    for (const auto& [id, marker] : record.structure_markers) { scalar(id); scalar(marker); }
    scalar(record.upgrades.size());
    for (const auto& row : record.upgrades) { scalar(row.type); scalar(row.holder); scalar(row.owner); }
    blob(record.map_identity); blob(record.story_plot); blob(record.linked_mission_options); blob(setup.value());
    return core::Result<std::string>::success(core::sha256_hex(bytes));
}
std::string corruption_name(galactic::corruption_fixture::Type type) {
    const std::array<std::string_view, 9> names{"CORRUPTION_NONE", "CORRUPTION_INTIMIDATION", "CORRUPTION_PIRACY", "CORRUPTION_KIDNAPPING", "CORRUPTION_RACKETEERING", "CORRUPTION_CORRUPT_MILITIA", "CORRUPTION_BRIBERY", "CORRUPTION_BLACK_MARKET", "CORRUPTION_BONDED_CITIZENS"};
    return std::string(names[static_cast<std::size_t>(type)]);
}
std::string combined_hash(const std::string& galaxy, const std::string& world,
    const std::string& scripts, const std::string& context) {
    // ADR-014: coordinator-reserved domain followed by four raw digests.
    constexpr std::string_view domain = "eawr-game-state-v1";
    std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
    for (const auto* digest : {&galaxy, &world, &scripts, &context}) put_digest(bytes, *digest);
    return core::sha256_hex(bytes);
}
core::Result<tactical::PlayerCommand> decode_order(const galactic::TacticalLayerCommand& input,
    const tactical::TacticalSetup& setup, const sim::CommandKey key) {
    // Reuse every ordinary tactical opcode's shape checks, wrapping only the small player
    // table around the body. Never copy the world's entities/content for command routing.
    if (input.body.size() < 32 || input.body.size() > 16384 || input.body[20] != input.opcode)
        return fail<tactical::PlayerCommand>("invalid tactical command body or opcode");
    tactical::TacticalReplay wrapper;
    wrapper.setup.players = setup.players;
    wrapper.setup.queue_identities = setup.queue_identities;
    wrapper.final_tick_count = 1;
    auto encoded = tactical::write_replay(wrapper);
    if (!encoded) return core::Result<tactical::PlayerCommand>::failure(encoded.error());
    auto bytes = std::move(encoded).value();
    overwrite(bytes, 64, 1, 8); // header command count
    auto body = input.body;
    // The untrusted common key is replaced by the authenticated seat-derived key.
    overwrite(body, 0, 0, 8);
    overwrite(body, 8, key.player_id, 4);
    overwrite(body, 12, key.sequence, 8);
    put(bytes, body.size(), 4);
    bytes.insert(bytes.end(), body.begin(), body.end());
    auto decoded = tactical::parse_replay(bytes);
    if (!decoded) return core::Result<tactical::PlayerCommand>::failure(decoded.error());
    auto command = std::move(decoded.value().commands.front());
    command.key = key;
    return core::Result<tactical::PlayerCommand>::success(std::move(command));
}
} // namespace

std::uint64_t child_seed(const std::uint64_t seed, const sim::EntityId conflict) {
    std::vector<std::uint8_t> bytes;
    put(bytes, seed, 8); put(bytes, conflict, 8);
    const auto digest = core::sha256(bytes);
    std::uint64_t result{};
    for (std::size_t i = 0; i < 8; ++i) result |= static_cast<std::uint64_t>(digest[i]) << (8U * i);
    return result;
}
class GameSession::Impl {
public:
    explicit Impl(galactic::GalacticSession session) : galaxy(std::move(session)) { galaxy.defer_victory_tail(galaxy.setup().victory.has_value()); }
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
    core::Result<void> update_galaxy_timeline(const std::optional<galactic::PlayerEconomySetup>& inputs) {
        // Missing tech owner is a named seam, never an invented zero input list.
        if (!inputs) return core::Result<void>::success();
        std::int64_t value{};
        for (const auto& row : inputs->players) value = std::max(value, row.current_tech);
        if (value > 5) return fail<void>("WGA-378: timeline maximum outside 0..5");
        timelines->galaxy.value = static_cast<std::int32_t>(value);
        timelines->galaxy.completed_tick = tick + 1;
        return core::Result<void>::success();
    }
    core::Result<void> update_child_timeline() {
        if (!child || !timelines->child) return fail<void>("WGA-380: missing active child mode");
        timelines->child->completed_tick = tick + 1;
        const auto global_snapshot = world().snapshot();
        if (!child_tech) {
            // WGA-390..399: this is the shared canonical player-tech owner,
            // not the child purchasing roster or the parent's timeline scalar.
            // Missing tech ownership remains a named seam, never a zero row.
            const auto& tech = global_snapshot->player_economy();
            if (!tech) {
                timelines->child_slot->snapshot = *timelines->child;
                return core::Result<void>::success();
            }
            const auto players = global_snapshot->players();
            if (tech->players.size() != players.size()) return fail<void>("WGA-399: incomplete shared player tech");
            std::int64_t value{};
            for (std::size_t i = 0; i < players.size(); ++i) {
                if (tech->players[i].player != players[i].player_id) return fail<void>("WGA-391: shared tech index mismatch");
                value = std::max(value, tech->players[i].current_tech);
            }
            if (value > 5) return fail<void>("WGA-378: child timeline maximum outside 0..5");
            timelines->child->value = static_cast<std::int32_t>(value);
            timelines->child_slot->snapshot = *timelines->child;
            return core::Result<void>::success();
        }
        const auto players = global_snapshot->players();
        auto inputs = child_tech(timelines->child->mode, players);
        if (!inputs) return core::Result<void>::failure(inputs.error());
        if (inputs.value().size() != players.size())
            return fail<void>("WGA-377: incomplete global current player tech list");
        std::int64_t value{};
        for (const auto tech : inputs.value()) value = std::max(value, tech);
        if (value > 5) return fail<void>("WGA-378: child timeline maximum outside 0..5");
        timelines->child->value = static_cast<std::int32_t>(value);
        timelines->child->completed_tick = tick + 1;
        timelines->child_slot->snapshot = *timelines->child;
        return core::Result<void>::success();
    }
    static std::string with_timeline(const std::string& base, const TimelinePublication& publication, bool ai_sync = false) {
        constexpr std::string_view domain = "eawr-mode-timeline-v1";
        std::vector<std::uint8_t> bytes(domain.begin(), domain.end()); put_digest(bytes, base);
        put(bytes, static_cast<std::uint32_t>(publication.galaxy.value), 4);
        const auto append = [&](const auto& row) {
            put(bytes, row ? 1 : 0, 1);
            if (row) { put(bytes, row->mode.conflict, 8); put(bytes, row->completed_tick, 8); put(bytes, static_cast<std::uint32_t>(row->value), 4); }
        };
        append(publication.child); append(publication.historical);
        auto digest = core::sha256_hex(bytes);
        // Fixed 64 lowercase hex characters: EA/galactic domain 47, sync ABI 0101.
        if (ai_sync) digest.replace(0, 8, "ea470101");
        return digest;
    }
    galactic::GalacticSession galaxy;
    std::optional<authoritative::ScriptedGalacticSession> scripts;
    galactic::GalacticSession::ControlEffectsHandler control_observer;
    StoryNoticeHandler story_handler;
    bool reconstructing_native_notices{};
    std::map<galactic::PlanetId, std::string> story_planets;
    std::map<galactic::PlayerId, std::string> story_factions;
    std::vector<GameStoryNotice> story_notices; // current publication only, never saved/hashed
    core::Result<void> story_notice(GameStoryNotice notice) {
        story_notices.push_back(std::move(notice));
        return story_handler && !reconstructing_native_notices ? story_handler(story_notices.back()) : core::Result<void>::success();
    }
    core::Result<void> control_notices(std::span<const galactic::PoliticalControlEffect> effects,
        std::span<const galactic::Planet> planets) {
        for (const auto& effect : effects) if (effect.owner_changed) {
            const auto at = story_planets.find(effect.planet);
            const auto name = at == story_planets.end() ? std::string{} : at->second;
            // WGM-192: both broadcasts follow caps, before the ordinary reward.
            if (auto delivered = story_notice({{}, galactic::story::ConquestNotice{
                name, effect.new_owner, effect.old_owner, effect.planet}, effect.old_owner, effect.new_owner}); !delivered) return delivered;
            if (auto delivered = story_notice({{}, galactic::story::ConquestCountNotice{
                name, effect.planet}, effect.old_owner, effect.new_owner}); !delivered) return delivered;
        }
        return control_observer ? control_observer(effects, planets) : core::Result<void>::success();
    }
    core::Result<void> battle_story(galactic::PlanetId planet, BattleLayer layer,
        galactic::PlayerId winner, galactic::PlayerId loser) {
        const auto at = story_planets.find(planet);
        const auto name = at == story_planets.end() ? std::string{} : at->second;
        const auto base = layer == BattleLayer::land ? galactic::story::BaseKind::ground : galactic::story::BaseKind::space;
        for (const auto& [player, kind] : {std::pair{winner, galactic::story::Battle::won},
            std::pair{loser, galactic::story::Battle::lost}}) {
            const auto faction = story_factions.find(player);
            if (auto delivered = story_notice({player, galactic::story::BattleNotice{kind, name,
                faction == story_factions.end() ? std::string{} : faction->second, {}, base, player, {}, planet},
                0, 0}); !delivered) return delivered;
        }
        return core::Result<void>::success();
    }
    GalacticAiServices native_ai;
    core::Result<void> battle_notice(GalacticAiBattleKind kind, const ConflictRecord& conflict,
        const BattleResult* result = nullptr, bool automatic = false) {
        if (!native_ai.battle) return core::Result<void>::success();
        const auto snapshot = world().snapshot();
        return native_ai.battle({kind, tick, conflict, *snapshot, result, automatic});
    }
    mutable std::optional<std::uint64_t> native_oracle_session;
    core::Result<std::string> native_hash() const {
        if (!native_ai.state) return core::Result<std::string>::success({});
        // Contributors may cache their canonical digest across unchanged
        // barriers. Do not copy every goal/cache/ledger on every session tick.
        auto digest = native_ai.state->sync_hash();
        if (!digest) return digest;
        if (digest.value().size() != 64 || digest.value().find_first_not_of("0123456789abcdef") != std::string::npos)
            return fail<std::string>("native AI contributor returned an invalid synchronization digest");
        // Opt-in correctness evidence, deliberately separate from profiling.
        // Captures full canonical state, so a changed sync format can be checked
        // against its parent at every tick and across a fresh restore owner.
        const auto oracle_path = [] {
#ifdef _MSC_VER
            char* option{}; std::size_t length{};
            if (_dupenv_s(&option, &length, "EAWR_GALACTIC_NATIVE_ORACLE") != 0 || !option) return std::string{};
            std::string path(option); std::free(option); return path;
#else
            const auto* option = std::getenv("EAWR_GALACTIC_NATIVE_ORACLE"); return option ? std::string(option) : std::string{};
#endif
        }();
        if (!oracle_path.empty()) {
            static std::atomic<std::uint64_t> sessions{};
            if (!native_oracle_session) native_oracle_session = sessions.fetch_add(1, std::memory_order_relaxed);
            auto captured = native_ai.state->capture(); if (!captured) return core::Result<std::string>::failure(captured.error());
            auto full = script::foc::galactic_ai::native_state_hash(captured.value()); if (!full) return full;
            const auto goals = captured.value().components.find("galactic-goals");
            std::vector<std::uint8_t> events;
            for (const auto& event : world().snapshot()->events()) {
                put(events, event.tick, 8); put(events, static_cast<std::uint64_t>(event.kind), 8);
                put(events, event.producer.value, 8); put(events, event.subject, 8); put(events, event.sequence, 8);
                put(events, event.value, 8); put(events, static_cast<std::uint64_t>(event.reason), 8);
            }
            std::ofstream trace(oracle_path, std::ios::app);
            trace << *native_oracle_session << ',' << tick << ',' << world().completed_tick() << ',' << full.value() << ',' << world().state_sha256()
                << ',' << (goals == captured.value().components.end() ? std::string{} : core::sha256_hex(goals->second))
                << ',' << core::sha256_hex(events) << ',' << digest.value() << '\n';
            if (!trace) return fail<std::string>("native oracle trace could not be written");
        }
        return digest;
    }
    static std::string with_native(const std::string& base, const std::string& native) {
        if (native.empty()) return base;
        constexpr std::string_view domain = "eawr-game-native-ai-v1";
        std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
        put_digest(bytes, base); put_digest(bytes, native); return core::sha256_hex(bytes);
    }
    const galactic::GalacticSession& world() const { return scripts ? scripts->world() : galaxy; }
    galactic::GalacticSession& world() { return scripts ? scripts->parent_world() : galaxy; }
    core::Result<std::string> galaxy_hash() {
        auto digest = core::Result<std::string>::success(world().cached_state_hash().get());
        if (scripts) {
            auto script_digest = scripts->uses_sync_hash() ? scripts->script_sync_hash() : scripts->script_state_hash();
            if (!script_digest) return script_digest;
            digest = core::Result<std::string>::success(authoritative::authoritative_state_sha256(
                world().completed_tick(), digest.value(), script_digest.value()));
        }
        if (conflict_services) {
            constexpr std::string_view domain = "eawr-conflict-service-v1";
            std::vector<std::uint8_t> bytes(domain.begin(), domain.end()); put_digest(bytes, digest.value());
            return core::Result<std::string>::success(core::sha256_hex(bytes));
        }
        return digest;
    }
    std::uint64_t tick{};
    AutoResolveOutcomeServices outcome_services;
    std::array<std::optional<AutoResolveOutcomeApplication>, 8> outcome_history{};
    std::size_t outcome_record_index{};
    std::map<galactic::CommandOrderKey, galactic::GameCommand> pending;
    std::map<galactic::SeatId, std::pair<std::uint64_t, std::uint64_t>> last;
    std::vector<galactic::GameCommand> executed;
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
        std::string context_hash() const {
            std::vector<std::uint8_t> bytes;
            put_digest(bytes, conflict_hash);
            put(bytes, sequences.size(), 8);
            for (const auto& [player, sequence] : sequences) { put(bytes, player, 8); put(bytes, sequence, 8); }
            put(bytes, killers.size(), 8);
            for (const auto& [id, killer] : killers) { put(bytes, id, 8); put(bytes, killer, 8); }
            if (owns_pause) put(bytes, 1, 1);
            return core::sha256_hex(bytes);
        }
    };
    std::optional<Child> child, staged;
    bool restoring_child{}; // inactive reconstruction only; suppress tactical service
    std::optional<tactical::TacticalReplay> last_battle;
    std::set<sim::EntityId> ended;
    std::shared_ptr<sim::StateHasher> hasher;
    ResultApplier applier;
    ResultDecorator decorator;
    HeroDefeatResolver hero_resolver;
    galactic::HeroPlacement hero_placement;
    galactic::HeroStructureResolver hero_structures;
    LiveHeroReturns live_hero_returns;
    TacticalReturnState return_state;
    std::optional<BattleResult> latest_result;
    std::optional<ConflictRecord> latest_context;
    ChildConfigurator configure;
    ConflictService conflict_service;
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
    NativeConflictService native_conflict_service;
    CorruptionMissionService corruption_mission_service;
    core::Result<void> queue_conflict(ConflictRecord, Arbitration);
    std::vector<RecordedAdmission> admissions;
    std::multimap<std::uint64_t, RecordedAdmission> replay_admissions;
    std::map<galactic::PlayerId, bool> votes;
    galactic::PlayerId withdrawing_player{};
    bool conflict_pause{};
    std::optional<core::Diagnostic> failure;
    std::vector<RecordedConflict> conflicts;
    std::vector<galactic::GameCommand> load_fixups;
    std::multimap<std::uint64_t, galactic::GameCommand> replay_fixups;
    std::map<std::uint64_t, ConflictRecord> replay_conflicts;
    std::map<sim::EntityId, RecordedConflict> replay_selected;
    ContentResolver content_resolver;
    EntryResolver entry_resolver;
    ScriptFactory script_factory;
    const galactic::Seat* seat(const galactic::SeatId id) const {
        const auto seats = world().seats();
        const auto found = std::lower_bound(seats.begin(), seats.end(), id,
            [](const auto& value, const auto key) { return value.seat_id < key; });
        return found == seats.end() || found->seat_id != id ? nullptr : &*found;
    }
    tactical::PlayerId player(const galactic::SeatId id) const {
        if (!child) return 0;
        const auto& seats = child->conflict.seats;
        const auto found = std::lower_bound(seats.begin(), seats.end(), id,
            [](const auto& value, const auto key) { return value.seat < key; });
        return found == seats.end() || found->seat != id ? 0 : found->player;
    }
    tactical::CampaignReserveAdmission reserve_admission(const tactical::PlayerId owner) const {
        if (!child || !child->conflict.tactical.campaign || owner == 0) return {};
        const auto snapshot = child->session.world().snapshot();
        const auto accounts = snapshot->economy();
        const auto account = std::find_if(accounts.begin(), accounts.end(),
            [&](const auto& row) { return row.player == owner; });
        if (account == accounts.end()) return {};
        // These views belong to the active trusted child and its completed snapshot.
        // Neither the human envelope nor a standalone tactical route creates them.
        return {true, owner, child->conflict.tactical.campaign->reserves, account->pool_tokens};
    }
    core::Result<void> refresh_battle_seats() {
        if (!child || !world().seat_control()) return core::Result<void>::success();
        auto& record = child->conflict;
        record.seats.clear();
        const auto& state = *world().seat_control();
        for (const auto& row : world().seats()) {
            if (!row.active) continue;
            tactical::PlayerId player{};
            if (row.player == record.invader || row.player == record.defender) player = row.player;
            if (row.kind == galactic::SeatKind::human && state.battle && row.human_side_id == state.battle->choosing_side) {
                const auto& role = *state.battle;
                player = role.role == galactic::BattleTeamRole::ai_side ? role.ai_player
                    : role.role == galactic::BattleTeamRole::ally ? role.human_player : 0;
            }
            if (row.kind == galactic::SeatKind::ai) {
                if (state.battle && state.battle->role == galactic::BattleTeamRole::ai_side
                    && row.player == state.battle->ai_player
                    && std::any_of(world().seats().begin(), world().seats().end(), [&](const auto& seat) {
                        return seat.active && seat.kind == galactic::SeatKind::human && seat.human_side_id == state.battle->choosing_side;
                    })) continue;
                if (player == 0) continue;
            }
            if (row.kind == galactic::SeatKind::script && player == 0) continue;
            record.seats.push_back({row.seat_id, player});
        }
        auto digest = conflict_hash(record);
        if (!digest) return core::Result<void>::failure(digest.error());
        child->conflict_hash = std::move(digest).value();
        return core::Result<void>::success();
    }
    bool role_pending() const {
        const auto& state = world().seat_control();
        return state && state->battle && state->battle->role == galactic::BattleTeamRole::pending;
    }
    std::string queued_context(std::string digest) const {
        if (waiting_admissions.empty() && return_state == TacticalReturnState{}) return digest;
        std::vector<std::uint8_t> bytes; put_digest(bytes, digest); put(bytes, waiting_admissions.size(), 8);
        const auto returned = encode_tactical_return_state(return_state);
        put(bytes, returned.size(), 8); bytes.insert(bytes.end(), returned.begin(), returned.end());
        for (const auto& entry : waiting_admissions) {
            put_digest(bytes, campaign::conflict_hash(entry.conflict).value());
            put(bytes, entry.entry_tick, 8); put(bytes, static_cast<std::uint8_t>(entry.arbitration.route), 1);
            put(bytes, entry.arbitration.forced_tactical ? 1 : 0, 1); put(bytes, entry.arbitration.overrun ? 1 : 0, 1);
        }
        return core::sha256_hex(bytes);
    }
    std::string child_context_hash() const {
        if (!world().seat_control()) return queued_context(child->context_hash());
        std::vector<std::uint8_t> bytes;
        put_digest(bytes, child->context_hash()); put(bytes, tick, 8);
        return queued_context(core::sha256_hex(bytes));
    }
    core::Result<std::string> base_hash() {
        if (!child) {
            auto galaxy_digest = galaxy_hash();
            if (!galaxy_digest) return galaxy_digest;
            if (!admission) return core::Result<std::string>::success(queued_context(galaxy_digest.value()));
            auto record_digest = campaign::conflict_hash(admission->conflict);
            if (!record_digest) return record_digest;
            std::vector<std::uint8_t> bytes;
            constexpr std::string_view domain = "eawr-pending-conflict-v2";
            bytes.insert(bytes.end(), domain.begin(), domain.end());
            put_digest(bytes, galaxy_digest.value()); put_digest(bytes, record_digest.value());
            put(bytes, static_cast<std::uint8_t>(admission->arbitration.route), 1);
            put(bytes, admission->arbitration.forced_tactical ? 1 : 0, 1);
            put(bytes, admission->arbitration.overrun ? 1 : 0, 1);
            put(bytes, admission->entry_tick, 8); put(bytes, conflict_pause ? 1 : 0, 1);
            // Withdrawal is command-reconstructed on restore; include its owner fact
            // while the aggregate solver remains outside the galaxy hash.
            put(bytes, withdrawing_player, 4);
            put(bytes, votes.size(), 8);
            for (const auto& [owner, yes] : votes) { put(bytes, owner, 4); put(bytes, yes ? 1 : 0, 1); }
            return core::Result<std::string>::success(queued_context(core::sha256_hex(bytes)));
        }
        return core::Result<std::string>::success(combined_hash(child->galaxy_hash, child->world_hash.get(), child->script_hash, child_context_hash()));
    }
    core::Result<std::string> hash() {
        auto base = base_hash(); if (!base) return base;
        auto native = native_hash(); if (!native) return native;
        return core::Result<std::string>::success(with_timeline(with_native(base.value(), native.value()), *timelines, bool(native_ai.state)));
    }
    BattleResult result() const {
        BattleResult out;
        const auto& world = child->session.world();
        const auto& conflict = child->conflict;
        out.conflict = conflict.id; out.winner = world.outcome()->winner;
        out.station_level = conflict.station_level;
        out.tactical_frames = world.completed_tick();
        const auto units = world.units();
        for (const auto& force : conflict.forces) {
            const bool survived = (force.role == ForceRole::reinforcement && !child->killers.contains(force.id))
                || tactical_force_alive(force, units);
            ForceResult row;
            row.id = force.id;
            row.disposition = force.destroy_after_tactical ? ForceDisposition::destroy_after_tactical
                : (survived ? ForceDisposition::survived : ForceDisposition::destroyed);
            if (const auto killer = child->killers.find(force.id); killer != child->killers.end()) row.final_blow_player = killer->second;
            row.hero_defeated = force.hero && !survived;
            if (force.id == conflict.station && !survived) out.station_level = 0;
            if (force.id == conflict.superweapon && !survived) {
                out.superweapon_destroyed = true; out.superweapon_killer = row.final_blow_player;
            }
            out.forces.push_back(row);
            if (!survived) out.casualties.push_back({force.id, force.tactical_type ? force.tactical_type : force.type,
                force.type, force.owner, row.final_blow_player});
        }
        if (world.retreat_state().phase == tactical::RetreatPhase::escaped) {
            out.retreat_player = world.retreat_state().player;
            for (std::size_t i = 0; i < out.forces.size(); ++i)
                if (conflict.forces[i].owner == *out.retreat_player && out.forces[i].disposition == ForceDisposition::survived)
                    out.forces[i].disposition = ForceDisposition::retreated;
        }
        if (conflict.tactical.campaign) for (const auto& ledger : world.ledgers()) out.credits.push_back({ledger.player, ledger.credits});
        return out;
    }
};

GameSession::GameSession(galactic::GalacticSession galaxy) : impl_(std::make_unique<Impl>(std::move(galaxy))) {
    if (const auto* host = impl_->world().ability_state()) for (const auto& row : host->planet_entitlements)
        for (const auto* values : {&row.automatic_upgrades, &row.tactical_upgrades}) for (const auto type : *values)
            impl_->return_state.upgrades.push_back({row.planet, {type, row.planet, row.player}});
    auto& initial_upgrades = impl_->return_state.upgrades;
    const auto upgrade_key = [](const auto& row) { return std::tuple{row.planet, row.upgrade.owner, row.upgrade.type, row.upgrade.holder}; };
    std::sort(initial_upgrades.begin(), initial_upgrades.end(), [&](const auto& a, const auto& b) { return upgrade_key(a) < upgrade_key(b); });
    initial_upgrades.erase(std::unique(initial_upgrades.begin(), initial_upgrades.end(), [&](const auto& a, const auto& b) { return upgrade_key(a) == upgrade_key(b); }), initial_upgrades.end());
    // WGCH-70/GFS-C-04: one saved return owner for tactical death and ability consumption.
    impl_->world().set_ability_consumption_handler([owner = impl_.get()](const galactic::HeroDefeat& defeat) -> core::Result<void> {
        const auto snapshot = owner->world().snapshot();
        auto scheduled = galactic::schedule_hero_return(owner->return_state.heroes, defeat,
            owner->world().clock().galactic_frame, snapshot->planets());
        if (!scheduled) return core::Result<void>::failure(scheduled.error());
        auto removed = scheduled.value().destroyed_companions;
        removed.insert(removed.end(), defeat.removed_objects.begin(), defeat.removed_objects.end());
        removed.push_back(defeat.id); std::sort(removed.begin(), removed.end());
        removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
        // Bounded action commit reuses canonical loss cleanup; never an idle/tick scan.
        // WGP-98: the defeated hero is credited to its recorded killer.
        const std::array killers{galactic::BattleKiller{defeat.id, defeat.killer}};
        return owner->world().apply_battle_losses(removed, defeat.killer ? std::span<const galactic::BattleKiller>(killers)
            : std::span<const galactic::BattleKiller>());
    });
    impl_->world().set_corruption_mission_handler([owner = impl_.get()](const auto& mission, const auto& choice) -> core::Result<bool> {
        using R = core::Result<bool>;
        const auto entries = owner->replay_admissions.equal_range(owner->tick);
        for (auto at = entries.first; at != entries.second; ++at)
            if (at->second.conflict.corruption_request == mission.request && at->second.conflict.corruption_mission)
                return R::success(true); // the recorded tail owns admission and allocator reservation
        if (!owner->corruption_mission_service || choice.mode != 2) return R::success(false); // M4 land/map builder seam
        auto built = owner->corruption_mission_service(*owner->world().snapshot(), mission, choice, owner->world().next_entity_id());
        if (!built) return R::failure(built.error());
        if (!built.value()) return R::success(false);
        auto admission = std::move(*built.value());
        if (!admission.conflict.corruption_mission || admission.conflict.planet != mission.planet
            || admission.conflict.invader != mission.player || admission.conflict.layer != BattleLayer::space
            || admission.conflict.map_identity != choice.map || admission.arbitration.route != ConflictRoute::tactical
            || !admission.arbitration.forced_tactical) return R::success(false);
        admission.conflict.corruption_request = mission.request;
        auto queued = owner->queue_conflict(std::move(admission.conflict), admission.arbitration);
        return queued ? R::success(true) : R::success(false); // WGCH-08: prepaid queue refusal, not campaign failure
    });
    impl_->world().set_corruption_completion_handler([owner = impl_.get()](galactic::PlanetId planet, galactic::corruption_fixture::Type old, galactic::corruption_fixture::Type next, galactic::CorruptionCompletionPhase phase, std::span<const galactic::PlanetAbilityEntitlements> entitlements) -> core::Result<void> {
        if (phase == galactic::CorruptionCompletionPhase::state_committed) {
            const auto name = owner->story_planets.find(planet);
            return owner->story_notice({std::nullopt, galactic::story::CorruptionNotice{galactic::story::Corruption::changed,
                name == owner->story_planets.end() ? std::string{} : name->second, corruption_name(next)}, 0, 0});
        }
        // WGCH-162/163: planet-held entries, independent of global player upgrades.
        auto& upgrades = owner->return_state.upgrades;
        for (const auto& row : entitlements) {
            if (row.planet != planet) continue;
            std::vector<galactic::TypeId> replaced;
            const auto& inputs = *owner->world().setup().ability_host;
            for (const auto& player : inputs.corruption_players) if (player.player == row.player)
                replaced.push_back(player.automatic_upgrades[static_cast<std::size_t>(old)]);
            if (phase == galactic::CorruptionCompletionPhase::rewards) for (const auto& location : inputs.corruption_planets) if (location.planet == planet)
                for (const auto& slot : location.slots) if (slot.type == old) replaced.push_back(slot.tactical_upgrade);
            replaced.insert(replaced.end(), row.automatic_upgrades.begin(), row.automatic_upgrades.end());
            replaced.insert(replaced.end(), row.tactical_upgrades.begin(), row.tactical_upgrades.end());
            upgrades.erase(std::remove_if(upgrades.begin(), upgrades.end(), [&](const auto& entry) {
                return entry.planet == planet && entry.upgrade.owner == row.player && entry.upgrade.holder == planet
                    && std::find(replaced.begin(), replaced.end(), entry.upgrade.type) != replaced.end();
            }), upgrades.end());
            for (const auto* values : {&row.automatic_upgrades, &row.tactical_upgrades}) for (const auto type : *values)
                upgrades.push_back({planet, {type, planet, row.player}});
        }
        std::sort(upgrades.begin(), upgrades.end(), [](const auto& a, const auto& b) {
            return std::tie(a.planet, a.upgrade.owner, a.upgrade.type, a.upgrade.holder) < std::tie(b.planet, b.upgrade.owner, b.upgrade.type, b.upgrade.holder);
        });
        upgrades.erase(std::unique(upgrades.begin(), upgrades.end()), upgrades.end());
        return core::Result<void>::success();
    });
    impl_->world().set_corruption_result_handler([owner = impl_.get()](const galactic::LiveCorruptionMission& mission) -> core::Result<void> {
        if (mission.state == galactic::CorruptionMissionState::cancelled) return core::Result<void>::success();
        const auto name = owner->story_planets.find(mission.planet);
        return owner->story_notice({mission.player, galactic::story::CorruptionNotice{
            mission.state == galactic::CorruptionMissionState::succeeded ? galactic::story::Corruption::complete : galactic::story::Corruption::failed,
            name == owner->story_planets.end() ? std::string{} : name->second, corruption_name(mission.choice.type)}, 0, 0, mission.planet});
    });
    // WGCH-140: the shared ruins owner announces the destroyed planet before any cleanup or flag write.
    impl_->world().set_planet_ruins_story_handler([owner = impl_.get()](galactic::PlanetId planet) -> core::Result<void> {
        const auto name = owner->story_planets.find(planet);
        galactic::story::TravelNotice notice;
        notice.kind = galactic::story::Travel::planet_destroyed;
        notice.planet = name == owner->story_planets.end() ? std::string{} : name->second;
        return owner->story_notice({std::nullopt, std::move(notice), 0, 0, planet});
    });
    // The Impl address survives moves and script ownership transfers.
    impl_->world().set_control_effects_handler([owner = impl_.get()](const auto effects, const auto planets) {
        return owner->control_notices(effects, planets);
    });
}
GameSession::GameSession(GameSession&&) noexcept = default;
GameSession& GameSession::operator=(GameSession&&) noexcept = default;
GameSession::~GameSession() = default;
core::Result<GameSession> GameSession::create(const galactic::GalacticSetup& setup) {
    for (const auto& entry : setup.outcomes) {
        auto application = decode_auto_resolve_outcome(entry.payload);
        if (!application || application.value().result.conflict != entry.conflict)
            return fail<GameSession>("unknown or malformed campaign outcome input");
    }
    auto galaxy = galactic::GalacticSession::create(setup);
    if (!galaxy) return core::Result<GameSession>::failure(galaxy.error());
    return core::Result<GameSession>::success(GameSession(std::move(galaxy).value()));
}
core::Result<GameSession> GameSession::from_replay(const galactic::GalacticReplay& replay) {
    if (auto valid = galactic::validate_replay(replay); !valid) return core::Result<GameSession>::failure(valid.error());
    auto created = create(replay.setup);
    if (!created) return created;
    for (const auto& command : replay.commands)
        if (auto submitted = created.value().submit(command); !submitted) return core::Result<GameSession>::failure(submitted.error());
    if (auto staged = created.value().impl_->world().stage_native_replay(replay.native_effects, replay.final_tick_count); !staged)
        return core::Result<GameSession>::failure(staged.error());
    return created;
}
core::Result<GameSession> GameSession::from_recording(const GameRecording& recording,
    ContentResolver content, ScriptFactory scripts) {
    if (auto valid = galactic::validate_replay(recording.commands); !valid) return core::Result<GameSession>::failure(valid.error());
    auto created = create(recording.commands.setup);
    if (!created) return created;
    auto& impl = *created.value().impl_;
    if (auto staged = impl.world().stage_native_replay(recording.commands.native_effects, recording.commands.final_tick_count); !staged)
        return core::Result<GameSession>::failure(staged.error());
    impl.conflict_services = recording.conflict_services || !recording.admissions.empty();
    std::set<sim::EntityId> ids;
    std::optional<std::uint64_t> previous;
    for (const auto& entry : recording.conflicts) {
        if (!content || entry.entry_tick >= recording.commands.final_tick_count
            || (previous && entry.entry_tick <= *previous) || entry.conflict.id == 0 || !ids.insert(entry.conflict.id).second)
            return fail<GameSession>("recorded conflicts need a resolver, unique IDs and increasing in-range ticks");
        previous = entry.entry_tick;
        impl.replay_conflicts.emplace(entry.entry_tick, entry.conflict);
    }
    previous.reset();
    std::set<sim::EntityId> admitted;
    for (const auto& entry : recording.admissions) {
        if (!content || entry.entry_tick >= recording.commands.final_tick_count
            || (previous && entry.entry_tick < *previous) || !entry.conflict.id
            || !admitted.insert(entry.conflict.id).second
            || static_cast<std::uint8_t>(entry.arbitration.route) > static_cast<std::uint8_t>(ConflictRoute::tactical))
            return fail<GameSession>("invalid recorded conflict admission");
        previous = entry.entry_tick;
        ids.insert(entry.conflict.id);
        impl.replay_admissions.emplace(entry.entry_tick, entry);
    }
    // Selected children are recreated by their recorded choice, not by a second entry.
    std::erase_if(impl.replay_conflicts, [&](const auto& row) {
        if (!admitted.contains(row.second.id)) return false;
        impl.replay_selected.emplace(row.second.id, RecordedConflict{row.first, row.second}); return true;
    });
    impl.world().enable_conflict_requests(impl.conflict_services);
    for (const auto& command : recording.commands.commands) {
        if (const auto* tactical_input = std::get_if<galactic::TacticalLayerCommand>(&command.payload);
            tactical_input && !ids.contains(tactical_input->conflict))
            return fail<GameSession>("recorded tactical command names an absent conflict entry");
        impl.pending.emplace(command.key, command);
        impl.last.insert_or_assign(command.key.seat, std::pair{command.key.tick, command.key.sequence});
    }
    impl.content_resolver = std::move(content); impl.script_factory = std::move(scripts);
    for (const auto& fixup : recording.load_fixups) {
        if (!std::holds_alternative<galactic::PostLoadFixup>(fixup.payload) || fixup.key.tick > recording.commands.final_tick_count
            || impl.seat(fixup.key.seat) == nullptr
            || galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::post_load_fixup,
                galactic::encode_payload(fixup.payload)) != std::optional(fixup.payload)
            || (!impl.replay_fixups.empty() && fixup.key.tick < impl.replay_fixups.rbegin()->first))
            return fail<GameSession>("invalid load prologue");
        impl.replay_fixups.emplace(fixup.key.tick, fixup);
    }
    return created;
}
core::Result<std::vector<std::uint8_t>> GameSession::save_checkpoint_state() const {
    using namespace galactic::checkpoint_detail;
    try {
        if (impl_->failure || impl_->restoring_child) throw Invalid("game is not at a healthy save barrier");
        auto world = impl_->world().save_checkpoint_state();
        if (!world) return core::Result<std::vector<std::uint8_t>>::failure(world.error());
        const auto child = [](auto& owner) -> std::optional<ChildOwner> {
            if (!owner) return {};
            auto state = owner->session.world().save_checkpoint_state();
            auto scripts = owner->session.save_scripts();
            if (!state) throw Invalid(state.error().message);
            if (!scripts) throw Invalid(scripts.error().message);
            return ChildOwner{owner->conflict, std::move(state).value(), std::move(scripts).value(),
                owner->session.record(), owner->sequences, owner->killers, owner->owns_pause};
        };
        Writer out; out(world.value()); checkpoint_game_owner(out, *impl_);
        out(child(impl_->child), child(impl_->staged));
        std::optional<std::vector<std::uint8_t>> resolver;
        if (auto_resolve_needs_service() && impl_->auto_state_writer) {
            auto saved = impl_->auto_state_writer();
            if (!saved) throw Invalid(saved.error().message);
            resolver = std::move(saved).value();
        } else if (const auto progress = auto_resolve_progress(); progress && progress->initiated) {
            throw Invalid("stateful auto-resolve needs checkpoint services");
        }
        out(resolver);
        return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
    } catch (const Invalid& error) { return fail<std::vector<std::uint8_t>>(error.what()); }
}

core::Result<void> GameSession::load_checkpoint_state(std::span<const std::uint8_t> bytes) {
    using namespace galactic::checkpoint_detail;
    try {
        if (impl_->tick || impl_->failure || impl_->scripts || impl_->child || impl_->staged)
            throw Invalid("direct restore requires a fresh private game");
        Reader in(bytes); std::vector<std::uint8_t> world;
        in(world);
        if (auto restored = impl_->world().load_checkpoint_state(world); !restored) return restored;
        checkpoint_game_owner(in, *impl_);
        std::optional<ChildOwner> child, staged; in(child, staged);
        std::optional<std::vector<std::uint8_t>> resolver; in(resolver);
        if (in.remaining() || impl_->tick > galactic::galactic_max_ticks || impl_->world().completed_tick() > impl_->tick
            || (!child && impl_->world().completed_tick() != impl_->tick)
            || child.has_value() != impl_->timelines->child.has_value())
            throw Invalid("inconsistent direct game barrier");
        const auto valid_timeline = [&](const ModeTimelineSnapshot& row, SessionMode mode) {
            return row.mode.mode == mode && row.completed_tick <= impl_->tick && row.value >= 0 && row.value <= 5
                && (mode == SessionMode::galaxy ? row.mode.conflict == 0 : row.mode.conflict != 0);
        };
        if (!valid_timeline(impl_->timelines->galaxy, SessionMode::galaxy)
            || (impl_->timelines->historical && !valid_timeline(*impl_->timelines->historical, SessionMode::battle))
            || (impl_->timelines->child && (!valid_timeline(*impl_->timelines->child, SessionMode::battle)
                || impl_->timelines->child->mode.conflict != child->conflict.id)))
            throw Invalid("invalid direct mode timeline");
        impl_->world().enable_conflict_requests(impl_->conflict_services);
        impl_->timelines->child_slot = impl_->timelines->child
            ? std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl_->timelines->child, false}) : nullptr;
        impl_->timelines->historical_slot = impl_->timelines->historical
            ? std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl_->timelines->historical, true}) : nullptr;
        const auto restore_child = [&](std::optional<ChildOwner>& saved, std::optional<Impl::Child>& owner) -> core::Result<void> {
            if (!saved) return core::Result<void>::success();
            if (!impl_->content_resolver || saved->log.setup != saved->conflict.tactical
                || (owner.has_value()) || saved->scripts.completed_tick > impl_->tick)
                return fail<void>("inconsistent direct child barrier");
            // Parent cursors count attempted envelopes, including refusals. Script
            // keys in the child log use another cursor, and current seat roles need
            // not be their historical roles. Bound the total by recorded attempts.
            std::uint64_t attempts{};
            for (const auto& command : impl_->executed)
                if (const auto* order = std::get_if<galactic::TacticalLayerCommand>(&command.payload);
                    order && order->conflict == saved->conflict.id) ++attempts;
            for (const auto& [player, sequence] : saved->sequences) {
                if (std::none_of(saved->log.setup.players.begin(), saved->log.setup.players.end(),
                    [&](const auto& row) { return row.player_id == player; }) || sequence == 0 || sequence > attempts)
                    return fail<void>("child command cursors differ from recording");
                attempts -= sequence;
            }
            for (const auto& [id, killer] : saved->killers)
                if (id == 0 || (killer != 0 && std::none_of(saved->log.setup.players.begin(), saved->log.setup.players.end(),
                    [&](const auto& row) { return row.player_id == killer; })))
                    return fail<void>("invalid child death attribution");
            auto content = impl_->content_resolver(saved->conflict);
            if (!content) return core::Result<void>::failure(content.error());
            auto child_world = tactical::TacticalSession::from_replay(saved->log, content.value());
            if (!child_world) return core::Result<void>::failure(child_world.error());
            if (auto loaded = child_world.value().load_checkpoint_state(saved->state); !loaded) return loaded;
            if (&owner == &impl_->child) {
                const auto entry = std::find_if(impl_->conflicts.begin(), impl_->conflicts.end(),
                    [&](const auto& row) { return row.conflict.id == saved->conflict.id; });
                // Save-only/paused service may lag the parent, but cannot precede
                // the selected child's birth barrier or its completed local work.
                if (entry == impl_->conflicts.end() || entry->entry_tick >= impl_->tick || !impl_->timelines->child
                    || impl_->timelines->child->completed_tick < entry->entry_tick + 1
                    || impl_->timelines->child->completed_tick < child_world.value().completed_tick()
                    || impl_->timelines->child->completed_tick > impl_->tick)
                    return fail<void>("WGA-378: invalid saved child timeline tick");
            }
            auto schedulers = [&]() -> core::Result<authoritative::ScriptScheduler> {
                if (impl_->script_factory) return impl_->script_factory(saved->conflict);
                authoritative::SessionConfig config; config.seed = child_seed(impl_->world().setup().seed, saved->conflict.id);
                return authoritative::ScriptScheduler::create(config, {});
            }();
            if (!schedulers) return core::Result<void>::failure(schedulers.error());
            auto hosted = authoritative::ScriptedTacticalSession::restore(std::move(child_world).value(), std::move(schedulers).value(), saved->scripts);
            if (!hosted) return core::Result<void>::failure(hosted.error());
            if (impl_->configure) if (auto configured = impl_->configure(hosted.value()); !configured) return configured;
            hosted.value().set_state_hasher(impl_->hasher); hosted.value().set_authoritative_hash(true);
            hosted.value().set_shared_mode_service([context = impl_.get()] { return context->update_child_timeline(); });
            auto scripts = hosted.value().script_state_hash();
            auto conflict = conflict_hash(saved->conflict);
            auto galaxy = impl_->galaxy_hash();
            if (!scripts) return core::Result<void>::failure(scripts.error());
            if (!conflict) return core::Result<void>::failure(conflict.error());
            if (!galaxy) return core::Result<void>::failure(galaxy.error());
            const auto hash = sim::StateHash(hosted.value().world().state_sha256());
            owner.emplace(Impl::Child{std::move(saved->conflict), std::move(hosted).value(),
                std::move(saved->sequences), std::move(saved->killers), std::move(galaxy).value(),
                std::move(scripts).value(), std::move(conflict).value(), hash});
            owner->owns_pause = saved->owns_pause;
            return core::Result<void>::success();
        };
        if (auto restored = restore_child(child, impl_->child); !restored) return restored;
        if (auto restored = restore_child(staged, impl_->staged); !restored) return restored;
        if (resolver) {
            if (!impl_->auto_state_reader || !auto_resolve_needs_service()) throw Invalid("missing saved auto-resolve owner");
            if (auto restored = impl_->auto_state_reader(*resolver, impl_->admission->conflict, *impl_->world().snapshot()); !restored) return restored;
        }
        auto canonical = save_checkpoint_state();
        if (!canonical || !std::equal(bytes.begin(), bytes.end(), canonical.value().begin(), canonical.value().end()))
            throw Invalid("noncanonical direct game barrier");
        return core::Result<void>::success();
    } catch (const Invalid& error) { return fail<void>(error.what()); }
}

core::Result<void> GameSession::stage_battle(ConflictRecord record, const tactical::TacticalContent& content) {
    authoritative::SessionConfig config;
    config.seed = child_seed(impl_->world().setup().seed, record.id);
    auto scripts = authoritative::ScriptScheduler::create(config, {});
    if (!scripts) return core::Result<void>::failure(scripts.error());
    return stage_battle(std::move(record), content, std::move(scripts).value());
}
core::Result<void> GameSession::stage_battle(ConflictRecord record, const tactical::TacticalContent& content,
    authoritative::ScriptScheduler scripts) {
    if (impl_->failure || impl_->child || impl_->staged || record.id == 0 || impl_->ended.contains(record.id))
        return fail<void>("a conflict needs a fresh ID and an idle, healthy game session");
    if (record.forces.size() > galactic::galactic_max_objects || record.seats.size() > galactic::galactic_max_seats)
        return fail<void>("conflict record exceeds the session resource limits", galactic::diagnostic_codes::resource_limit);
    const auto snapshot = impl_->world().snapshot();
    if (record.invader == record.defender || record.invader == 0 || record.defender == 0
        || std::none_of(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& p) { return p.id == record.planet; }))
        return fail<void>("conflict needs two different owners and an existing planet");
    const bool reserved_admission = impl_->admission && impl_->admission->conflict.id == record.id;
    if (reserved_admission && record.tactical.first_free_entity_id)
        record.tactical.first_free_entity_id = std::max(*record.tactical.first_free_entity_id, impl_->world().next_entity_id());
    const auto first = record.tactical.first_free_entity_id;
    if (!first || *first <= record.id || impl_->world().next_entity_id() == sim::invalid_entity_id
        || (!reserved_admission && record.id < impl_->world().next_entity_id()))
        return fail<void>("conflict setup must reserve its ID and the shared allocator boundary");
    record.tactical.seed = child_seed(impl_->world().setup().seed, record.id);
    if (auto valid = tactical::validate_setup(record.tactical); !valid) return valid;
    if (content.retreat) tactical::resolve_cinematic_seats(record.tactical, content.economy.players);
    for (const auto owner : {record.invader, record.defender}) {
        if (std::none_of(snapshot->players().begin(), snapshot->players().end(), [owner](const auto& p) { return p.player_id == owner; })
            || std::none_of(record.tactical.players.begin(), record.tactical.players.end(), [owner](const auto& p) { return p.player_id == owner && p.commandable(); }))
            return fail<void>("both conflict owners must be known commandable tactical players");
    }
    sim::EntityId previous{};
    for (const auto& force : record.forces) {
        if (force.id <= previous || force.id >= *first || force.id == record.id || force.type == 0
            || static_cast<unsigned>(force.role) > static_cast<unsigned>(ForceRole::orbital)
            || (force.owner != record.invader && force.owner != record.defender && (force.role != ForceRole::orbital
                || std::none_of(snapshot->players().begin(), snapshot->players().end(), [&](const auto& p) { return p.player_id == force.owner; }))))
            return fail<void>("persistent force IDs and owners are invalid");
        previous = force.id;
        if (force.role == ForceRole::orbital) {
            bool matched{};
            if (snapshot->production()) for (const auto& planet : snapshot->production()->planets)
                if (planet.planet == record.planet) for (const auto& object : planet.objects)
                    if (object.id == force.parent) matched = true;
            if (!matched || !force.parent || force.destroy_after_tactical)
                return fail<void>("orbital copy must retain its live source structure");
        } else if (force.role != ForceRole::escort) {
            const auto fleet = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& f) { return f.id == force.parent && f.owner == force.owner; });
            std::vector<galactic::FleetMember> members;
            if (fleet != snapshot->fleets().end()) members = fleet->members;
            std::set<sim::EntityId> visited;
            bool matched = false;
            while (!members.empty() && visited.size() <= galactic::galactic_max_objects) {
                const auto member = members.back(); members.pop_back();
                if (!visited.insert(member.id).second) continue;
                if (member.id == force.id && member.type == force.type) { matched = true; break; }
                if (const auto* movement = snapshot->movement()) {
                    const auto contents = std::find_if(movement->contents.begin(), movement->contents.end(),
                        [&](const auto& row) { return row.container == member.id; });
                    if (contents != movement->contents.end())
                        members.insert(members.end(), contents->children.begin(), contents->children.end());
                }
            }
            if (!matched)
                return fail<void>("persistent forces must match their galactic parent and owner");
        }
        const auto unit = std::lower_bound(record.tactical.units.begin(), record.tactical.units.end(), force.id,
            [](const auto& row, const auto id) { return row.entity_id < id; });
        const bool deployed = unit != record.tactical.units.end() && unit->entity_id == force.id;
        if ((!deployed && force.role != ForceRole::reinforcement && force.role != ForceRole::retained && force.role != ForceRole::rider)
            || (deployed && (unit->type_id != (force.tactical_type ? force.tactical_type : force.type) || unit->owner != force.owner))
            || (deployed && force.purchase_type && tactical::purchase_identity(*unit) != force.purchase_type)
            || (force.role == ForceRole::rider && !tactical_force_alive(force, record.tactical.units)))
            return fail<void>("deployed persistent force must retain its ID, type and owner");
    }
    galactic::SeatId previous_seat{};
    for (const auto& binding : record.seats) {
        const auto* issuer = impl_->seat(binding.seat);
        if (binding.seat <= previous_seat || issuer == nullptr || !issuer->active
            || (binding.player != 0 && (binding.player != issuer->player
                || std::none_of(record.tactical.players.begin(), record.tactical.players.end(), [&](const auto& p) { return p.player_id == binding.player && p.commandable(); }))))
            return fail<void>("battle seats must be active, ordered and retain their owner; temporary roles await participation policy");
        previous_seat = binding.seat;
    }
    auto world = tactical::TacticalSession::create(record.tactical, content);
    if (!world) return core::Result<void>::failure(world.error());
    auto child = authoritative::ScriptedTacticalSession::create(std::move(world).value(), std::move(scripts));
    if (!child) return core::Result<void>::failure(child.error());
    child.value().set_state_hasher(impl_->hasher);
    if (impl_->configure) {
        if (auto configured = impl_->configure(child.value()); !configured) return configured;
    }
    child.value().set_authoritative_hash(true);
    auto script_hash = child.value().script_state_hash();
    if (!script_hash) return core::Result<void>::failure(script_hash.error());
    const auto world_hash = sim::StateHash(child.value().world().state_sha256());
    auto record_hash = conflict_hash(record);
    if (!record_hash) return core::Result<void>::failure(record_hash.error());
    child.value().set_shared_mode_service([owner = impl_.get()] { return owner->update_child_timeline(); });
    impl_->staged.emplace(Impl::Child{std::move(record), std::move(child).value(), {}, {}, {}, std::move(script_hash).value(), std::move(record_hash).value(), world_hash});
    return core::Result<void>::success();
}
core::Result<void> GameSession::queue_conflict(ConflictRecord record, Arbitration arbitration) { return impl_->queue_conflict(std::move(record), arbitration); }
core::Result<void> GameSession::Impl::queue_conflict(ConflictRecord record, Arbitration arbitration) {
    auto& impl = *this;
    if (impl.failure || impl.child || impl.staged || impl.queued_admissions.size() + impl.waiting_admissions.size() >= galactic::galactic_max_objects
        || !impl.content_resolver || !impl.conflict_services
        || !record.id || impl.ended.contains(record.id)
        || record.id < impl.world().next_entity_id() || !record.tactical.first_free_entity_id
        || *record.tactical.first_free_entity_id <= record.id || record.invader == record.defender
        || !record.invader || !record.defender
        || static_cast<std::uint8_t>(arbitration.route) > static_cast<std::uint8_t>(ConflictRoute::tactical))
        return fail<void>("conflict admission needs a fresh identity, pinned content and an idle service");
    // The host's live request is the only way a result can settle; a flagged conflict without one would fail at its result.
    if (record.corruption_mission != (record.corruption_request != 0))
        return fail<void>("corruption mission conflicts carry exactly their live host request");
    if (auto valid = tactical::validate_setup(record.tactical); !valid) return valid;
    const auto snapshot = impl.world().snapshot();
    if (std::none_of(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& p) { return p.id == record.planet; }))
        return fail<void>("conflict planet is absent");
    for (const auto owner : {record.invader, record.defender})
        if (std::none_of(snapshot->players().begin(), snapshot->players().end(), [&](const auto& p) { return p.player_id == owner; }))
            return fail<void>("conflict owner is absent");
    sim::EntityId previous{};
    for (const auto& force : record.forces) {
        if (force.id <= previous || force.id >= *record.tactical.first_free_entity_id || force.id == record.id || !force.type
            || static_cast<unsigned>(force.role) > static_cast<unsigned>(ForceRole::orbital)
            || (force.owner != record.invader && force.owner != record.defender && (force.role != ForceRole::orbital
                || std::none_of(snapshot->players().begin(), snapshot->players().end(), [&](const auto& p) { return p.player_id == force.owner; }))))
            return fail<void>("invalid admitted force identities");
        previous = force.id;
    }
    if ((impl.admission && impl.admission->conflict.id == record.id)
        || (!impl.queued_admissions.empty() && record.id < *impl.queued_admissions.back().conflict.tactical.first_free_entity_id)
        || std::any_of(impl.queued_admissions.begin(), impl.queued_admissions.end(), [&](const auto& e) { return e.conflict.id == record.id; })
        || std::any_of(impl.waiting_admissions.begin(), impl.waiting_admissions.end(), [&](const auto& e) { return e.conflict.id == record.id; }))
        return fail<void>("duplicate admitted conflict identity");
    impl.queued_admissions.push_back(RecordedAdmission{impl.tick, std::move(record), arbitration});
    return core::Result<void>::success();
}
core::Result<void> GameSession::select_pending_battle(bool fight, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (!impl.admission) return fail<void>("no pending conflict");
    const auto& entry = *impl.admission;
    if (fight && entry.conflict.layer == BattleLayer::land) {
        // M3 scope (gc-tactical evaluation boundary): land uses the solver seam.
        // A sourced forcing story/tutorial restriction cannot be silently bypassed.
        if (entry.arbitration.forced_tactical) return fail<void>("forced land tactical selection requires the M4 entry service");
        fight = false;
    }
    if (fight) {
        auto conflict = entry.conflict;
        const auto selected = impl.replay_selected.find(conflict.id);
        const bool recorded_selection = selected != impl.replay_selected.end();
        if (recorded_selection) {
            if (selected->second.entry_tick != impl.tick) return fail<void>("recorded child selection tick differs");
            conflict = selected->second.conflict;
        }
        if (!recorded_selection) conflict.structure_markers = impl.return_state.structure_markers;
        if (!recorded_selection) {
            conflict.upgrades.clear();
            for (const auto& row : impl.return_state.upgrades) if (row.planet == conflict.planet) conflict.upgrades.push_back(row.upgrade);
        }
        auto prepared = [&]() -> core::Result<ResolvedConflictEntry> {
            if (!recorded_selection && !conflict.tactical.skirmish && impl.entry_resolver)
                return impl.entry_resolver(conflict, *impl.world().snapshot(), impl.world().next_entity_id());
            if (!impl.content_resolver) return fail<ResolvedConflictEntry>("tactical content provider is unavailable");
            auto content = impl.content_resolver(conflict);
            if (!content) return core::Result<ResolvedConflictEntry>::failure(content.error());
            return core::Result<ResolvedConflictEntry>::success({conflict, std::move(content).value()});
        }();
        if (!prepared) return core::Result<void>::failure(prepared.error());
        conflict = std::move(prepared.value().conflict);
        if (conflict.id != entry.conflict.id || conflict.layer != entry.conflict.layer
            || conflict.planet != entry.conflict.planet || conflict.invader != entry.conflict.invader
            || conflict.defender != entry.conflict.defender || conflict.seats != entry.conflict.seats)
            return fail<void>("entry resolver changed the admitted conflict identity or seats");
        auto staged = [&]() {
            if (!impl.script_factory) return stage_battle(conflict, prepared.value().content);
            auto scripts = impl.script_factory(conflict);
            if (!scripts) return core::Result<void>::failure(scripts.error());
            return stage_battle(conflict, prepared.value().content, std::move(scripts).value());
        }();
        if (!staged) return staged;
        if (recorded_selection) impl.replay_selected.erase(selected);
    } else {
        if (!impl.auto_resolver) return core::Result<void>::success(); // #2023/2024 solver/return seam
        auto result = impl.auto_resolver(entry.conflict, *impl.world().snapshot(), executor);
        if (!result) return core::Result<void>::failure(result.error());
        if (!result.value()) return core::Result<void>::success();
        const auto& application = *result.value();
        const auto& resolved = application.result;
        if (application.planet != entry.conflict.planet || application.invader != entry.conflict.invader
            || application.defender != entry.conflict.defender || application.land != (entry.conflict.layer == BattleLayer::land)
            || resolved.conflict != entry.conflict.id
            || (resolved.winner != entry.conflict.invader && resolved.winner != entry.conflict.defender)
            || resolved.forces.size() != entry.conflict.forces.size()) return fail<void>("invalid auto-resolve outcome");
        for (std::size_t i = 0; i < resolved.forces.size(); ++i)
            if (resolved.forces[i].id != entry.conflict.forces[i].id) return fail<void>("auto-resolve force identities differ");
        // WAR-49..63: one journaled return, including control, losses and retreat.
        // Tactical return adapters use the same application services.
        auto staged = stage_auto_resolve_outcome(application);
        if (!staged) return staged;
        impl.ended.insert(entry.conflict.id);
        // The auto-resolver stays in the parent tail. GalacticSession seals this
        // logical tick once; it must not take the child's frontier-advance route.
        if (impl.conflict_pause) { auto resumed = impl.world().adjust_conflict_pause(false); if (!resumed) return resumed; }
        impl.conflict_pause = false;
    }
    impl.admission.reset(); impl.votes.clear(); impl.withdrawing_player = 0;
    return core::Result<void>::success();
}
core::Result<void> GameSession::submit(const galactic::GameCommand& command) {
    if (std::holds_alternative<galactic::PostLoadFixup>(command.payload)) return fail<void>("load fixups are internal restore commands");
    if (impl_->failure) return core::Result<void>::failure(*impl_->failure);
    const auto* issuer = impl_->seat(command.key.seat);
    if (issuer == nullptr) return fail<void>("unknown game-command seat", galactic::diagnostic_codes::unknown_issuer);
    if (command.key.tick < impl_->tick) return fail<void>("session tick already executed", galactic::diagnostic_codes::late_command);
    if (command.key.tick >= galactic::galactic_max_ticks || impl_->pending.size() + impl_->executed.size() >= galactic::galactic_max_commands)
        return fail<void>("session command limit reached", galactic::diagnostic_codes::resource_limit);
    const std::pair order{command.key.tick, command.key.sequence};
    const auto old = impl_->last.find(command.key.seat);
    if (old != impl_->last.end() && !(old->second < order))
        return fail<void>("command does not follow this seat's previous submission", galactic::diagnostic_codes::command_order);
    // WGM-174/177: live alert admission is a submission refusal, not a
    // local-observer predicate replayed on every peer's deterministic step.
    if (const auto* choice = std::get_if<galactic::BattleChoice>(&command.payload)) {
        const auto published = pending_conflict_view();
        if (published && published->alert && published->alert->local == issuer->player
            && !(choice->tactical ? published->alert->begin_admitted : published->alert->resolve_admitted))
            return fail<void>("battle choice is unavailable in the current alert");
    }
    const auto layer = galactic::layer_of(command.payload);
    if (galactic::decode_payload(layer, galactic::opcode_of(command.payload), galactic::encode_payload(command.payload)) != std::optional(command.payload))
        return fail<void>("malformed game command");
    if (const auto* input = std::get_if<galactic::TacticalLayerCommand>(&command.payload)) {
        const auto player = impl_->player(command.key.seat);
        if (!impl_->child || impl_->role_pending() || impl_->child->conflict.id != input->conflict || player == 0 || !issuer->active)
            return fail<void>("no matching battle or this seat has no commandable battle side");
        auto decoded = decode_order(*input, impl_->child->conflict.tactical, {0, player, 1});
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (impl_->world().seat_control() && issuer->kind == galactic::SeatKind::human
            && !tactical::human_wire_payload(decoded.value().payload, impl_->reserve_admission(player))) return fail<void>("human battle seat cannot issue engine-only orders");
    } else if ((std::holds_alternative<galactic::DebugSetPlanetOwner>(command.payload)
        || std::holds_alternative<galactic::DebugSetFleetOrbit>(command.payload)) && !impl_->world().setup().debug_commands)
        return fail<void>("debug commands are off in this session");
    impl_->pending.emplace(command.key, command);
    impl_->last.insert_or_assign(command.key.seat, order);
    return core::Result<void>::success();
}
core::Result<void> GameSession::accept_invasion_offer(galactic::SeatId seat, sim::EntityId conflict) {
    auto& impl = *impl_;
    const auto* issuer = impl.seat(seat);
    if (!issuer || !issuer->active || issuer->kind != galactic::SeatKind::human || !impl.latest_result
        || !impl.latest_context || impl.latest_result->conflict != conflict || impl.child || impl.admission || impl.staged
        || !impl.waiting_admissions.empty()) return fail<void>("invasion offer is no longer available");
    const auto snapshot = impl.world().snapshot();
    auto offer = observe_invasion_offer(*impl.latest_result, *impl.latest_context, *snapshot,
        issuer->player, impl.world().native_invasion_available());
    if (!offer.accept_permitted) return fail<void>("invasion offer has no permitted surviving transports");
    // WGP-136: resolve missing identities first, cap the event before ownership
    // filtering, and never backfill filtered entries from beyond that cap.
    constexpr std::size_t accepted_limit = 127;
    std::vector<sim::EntityId> resolved;
    for (const auto& prepared : impl.latest_result->invasion_offers) for (const auto id : prepared.transports) {
        if (resolved.size() == accepted_limit) break;
        const bool exists = std::any_of(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) {
            return std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.id == id; });
        });
        if (exists) resolved.push_back(id);
    }
    bool admitted_any{};
    for (const auto& row : offer.candidates) {
        std::vector<sim::EntityId> selected;
        for (const auto id : resolved)
            if (std::find(row.transports.begin(), row.transports.end(), id) != row.transports.end()) selected.push_back(id);
        if (selected.empty()) continue;
        auto admitted = impl.world().admit_native_invasion(issuer->player, row.fleet, selected);
        if (!admitted) return core::Result<void>::failure(admitted.error());
        if (!admitted.value().admitted) return fail<void>("retained invasion admission refused");
        for (auto& prepared : impl.latest_result->invasion_offers)
            if (prepared.fleet == row.fleet && prepared.planet == row.planet) prepared.transports.clear();
        // No presentation reader polls the queue receipt. Its conflict owner keeps
        // pending requests alive until actual completion, then prunes them.
        if (auto released = impl.world().retire_native_invasion(admitted.value().request); !released) return released;
        admitted_any = true;
    }
    if (!admitted_any) return fail<void>("capped invasion selection has no eligible transports");
    for (auto& prepared : impl.latest_result->invasion_offers) prepared.transports.clear();
    return core::Result<void>::success();
}
core::Result<GameTick> GameSession::step(const sim::PartitionExecutor& executor) {
    try {
    auto& impl = *impl_;
    if (impl.failure) return core::Result<GameTick>::failure(*impl.failure);
    if (impl.tick >= galactic::galactic_max_ticks)
        return fail<GameTick>("session tick limit reached", galactic::diagnostic_codes::simulation_complete);
    GameTick out; out.mode = mode();
    impl.story_notices.clear();
    std::size_t published_conflict_events = 0;
    bool parent_mode_serviced = false;
    if (auto applied = apply_restore_prologue(); !applied) return core::Result<GameTick>::failure(applied.error());
    if (const auto entry = impl.replay_conflicts.find(impl.tick); entry != impl.replay_conflicts.end()) {
        auto content = impl.content_resolver(entry->second);
        if (!content) { impl.failure = content.error(); return core::Result<GameTick>::failure(*impl.failure); }
        auto staged = [&]() {
            if (!impl.script_factory) return stage_battle(entry->second, content.value());
            auto scripts = impl.script_factory(entry->second);
            if (!scripts) return core::Result<void>::failure(scripts.error());
            return stage_battle(entry->second, content.value(), std::move(scripts).value());
        }();
        if (!staged) { impl.failure = staged.error(); return core::Result<GameTick>::failure(*impl.failure); }
        impl.replay_conflicts.erase(entry);
    }
    sim::StateHash world_hash;
    std::vector<tactical::PlayerCommand> inputs;
    std::vector<galactic::GameCommand> galaxy_inputs;
    bool controls_applied{};
    bool has_tick_input{}, save_only = true;
    while (!impl.pending.empty() && impl.pending.begin()->first.tick == impl.tick) {
        auto command = std::move(impl.pending.begin()->second);
        impl.pending.erase(impl.pending.begin());
        has_tick_input = true;
        save_only = save_only && std::holds_alternative<galactic::SaveRequest>(command.payload);
        if (const auto* order = std::get_if<galactic::TacticalLayerCommand>(&command.payload)) {
            const auto player = impl.player(command.key.seat);
            const auto* issuer = impl.seat(command.key.seat);
            if (!impl.child || impl.role_pending() || !issuer || !issuer->active || order->conflict != impl.child->conflict.id || player == 0)
                out.diagnostics.push_back(fail<void>("queued command's battle ended before execution").error());
            else {
                auto decoded = decode_order(*order, impl.child->conflict.tactical,
                    {impl.child->session.completed_tick(), player, ++impl.child->sequences[player]});
                if (!decoded) out.diagnostics.push_back(decoded.error());
                else if (impl.world().seat_control() && issuer->kind == galactic::SeatKind::human
                    && !tactical::human_wire_payload(decoded.value().payload, impl.reserve_admission(player)))
                    out.diagnostics.push_back(fail<void>("human battle seat cannot issue engine-only orders").error());
                else inputs.push_back(std::move(decoded).value());
            }
        } else if (const auto* choice = std::get_if<galactic::BattleChoice>(&command.payload)) {
            const auto diagnostics_before = out.diagnostics.size();
            const auto* issuer = impl.seat(command.key.seat);
            if (!impl.admission || !impl.conflict_pause || choice->conflict != impl.admission->conflict.id || !issuer || !issuer->active
                || issuer->kind != galactic::SeatKind::human
                || (impl.admission->arbitration.route != ConflictRoute::vote
                    && impl.admission->arbitration.route != ConflictRoute::pending_tactical)
                || (issuer->player != impl.admission->conflict.invader && issuer->player != impl.admission->conflict.defender)
                || (!choice->tactical && impl.admission->arbitration.forced_tactical))
                out.diagnostics.push_back(fail<void>("battle choice does not belong to this pending conflict or side").error());
            else {
                impl.votes[issuer->player] = choice->tactical;
                const auto& conflict = impl.admission->conflict;
                const auto human_side = [&](auto player) {
                    return std::any_of(impl.world().setup().seats.begin(), impl.world().setup().seats.end(), [&](const auto& seat) {
                        return seat.active && seat.kind == galactic::SeatKind::human && seat.player == player;
                    });
                };
                std::optional<bool> selected;
                if (human_side(conflict.invader) && human_side(conflict.defender)) {
                    // The manager's vote progression/timeout owner decides; do not invent a
                    // majority rule or derive wall-clock time from logical frames (WGM-47).
                    if (impl.vote_progress) {
                        auto progress = impl.vote_progress(conflict, impl.votes);
                        if (!progress) { impl.failure = progress.error(); return core::Result<GameTick>::failure(*impl.failure); }
                        selected = progress.value();
                    }
                } else selected = choice->tactical;
                if (selected && !*selected && impl.admission->arbitration.forced_tactical) {
                    out.diagnostics.push_back(fail<void>("vote result cannot bypass forced tactical selection").error());
                    selected.reset();
                }
                if (selected) {
                    if (!*selected || (conflict.layer == BattleLayer::land && !impl.admission->arbitration.forced_tactical))
                        impl.admission->arbitration.route = ConflictRoute::instant_auto;
                    else {
                        auto selection = select_pending_battle(true, executor);
                        if (!selection) out.diagnostics.push_back(selection.error());
                    }
                }
            }
            out.battle_choice_responses.push_back({choice->conflict, command.key.seat,
                out.diagnostics.size() == diagnostics_before ? std::string{} : out.diagnostics.back().message});
        } else if (const auto* withdraw = std::get_if<galactic::AutoResolveWithdraw>(&command.payload)) {
            const auto* issuer = impl.seat(command.key.seat);
            const auto progress = auto_resolve_progress();
            if (!impl.admission || !impl.conflict_pause || !issuer || !issuer->active
                || issuer->kind != galactic::SeatKind::human || !progress
                || withdraw->conflict != impl.admission->conflict.id || progress->conflict != withdraw->conflict
                || impl.withdrawing_player || !impl.auto_withdraw || !impl.auto_withdraw(withdraw->conflict, issuer->player))
                out.diagnostics.push_back(fail<void>("withdrawal is unavailable for this conflict").error());
            else impl.withdrawing_player = issuer->player;
        } else if (impl.child) {
            if (std::holds_alternative<galactic::PauseGalaxy>(command.payload)
                || std::holds_alternative<galactic::ResumeGalaxy>(command.payload)
                || std::holds_alternative<galactic::SetSpeed>(command.payload)
                || std::holds_alternative<galactic::SaveRequest>(command.payload)
                || std::holds_alternative<galactic::SeatLeave>(command.payload)
                || std::holds_alternative<galactic::SeatRejoin>(command.payload)
                || std::holds_alternative<galactic::AssignBattleTeamRole>(command.payload)) {
                // Canonical seat order: a departure/choice takes effect before later inputs.
                auto controlled = impl.world().apply_session_controls(std::span(&command, 1), true, controls_applied);
                controls_applied = true;
                if (!controlled) { impl.failure = controlled.error(); return core::Result<GameTick>::failure(*impl.failure); }
                if (auto refreshed = impl.refresh_battle_seats(); !refreshed) { impl.failure = refreshed.error(); return core::Result<GameTick>::failure(*impl.failure); }
                impl.child->galaxy_hash = impl.galaxy_hash().value();
            }
            else out.diagnostics.push_back(fail<void>("galactic action is unavailable while a child is active").error());
        } else {
            auto local = command; local.key.tick = impl.world().completed_tick();
            if (impl.scripts) galaxy_inputs.push_back(local);
            else if (auto submitted = impl.world().submit(local); !submitted) out.diagnostics.push_back(submitted.error());
        }
        impl.executed.push_back(std::move(command));
    }
    if (impl.child) {
        if (controls_applied) for (const auto& event : impl.world().snapshot()->events())
            if (event.kind == galactic::GalacticEventKind::save_requested || event.kind == galactic::GalacticEventKind::save_rejected)
                out.save_events.push_back(event);
        if (impl.restoring_child || impl.role_pending() || (has_tick_input && save_only)) {
            // Recorded SaveRequest-only barriers reproduce paused-child saves without
            // advancing the child world or hosted Lua. Outer controls retain their tick.
            out.battle = impl.child->session.world().snapshot();
        } else {
        if (impl.world().seat_control()) impl.child->session.set_order_admission([this](const authoritative::TacticalOrder& order) {
            // WGS-P07: delegated/manual control suppresses AI gameplay orders, while
            // engine-only story/reward effects retain their separate authority.
            const bool engine_effect = tactical::payload_authority(order.payload) == tactical::PayloadAuthority::engine_only
                && !std::holds_alternative<tactical::AiReservationDebitPayload>(order.payload);
            return engine_effect || battle_controller(order.issuer) == galactic::FactionControl::ai;
        });
        auto stepped = impl.child->session.step(executor, inputs);
        if (!stepped) { impl.failure = stepped.error(); return core::Result<GameTick>::failure(*impl.failure); }
        out.battle = stepped.value().world.snapshot;
        out.asteroid_impacts = stepped.value().world.asteroid_impacts;
        world_hash = stepped.value().world.state_hash;
        impl.child->world_hash = world_hash;
        impl.child->script_hash = std::move(stepped.value().script_sha256);
        out.battle_state_hash = world_hash;
        out.diagnostics.insert(out.diagnostics.end(), stepped.value().refused_input.begin(), stepped.value().refused_input.end());
        out.diagnostics.insert(out.diagnostics.end(), stepped.value().world.diagnostics.begin(), stepped.value().world.diagnostics.end());
        for (const auto& [id, killer] : stepped.value().world.deaths) impl.child->killers[id] = killer;
        const auto& outcome = impl.child->session.world().outcome();
        if (outcome && stepped.value().world.completed_tick > outcome->end_tick) {
            out.result = impl.result();
            bool return_weapon_killed = false;
            const bool weapon_was_alive = !out.result->superweapon_destroyed;
            if (impl.world().victory() && impl.child->conflict.superweapon && weapon_was_alive) {
                const auto& conflict = impl.child->conflict;
                const auto weapon = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& force) { return force.id == conflict.superweapon; });
                if (weapon != conflict.forces.end() && weapon->owner != out.result->winner) {
                    const auto& killers = impl.world().victory()->context.superweapon_killers;
                    for (std::size_t i = 0; i < conflict.forces.size(); ++i)
                        if (conflict.forces[i].owner == out.result->winner && out.result->forces[i].disposition == ForceDisposition::survived
                            && std::find(killers.begin(), killers.end(), conflict.forces[i].type) != killers.end()) { return_weapon_killed = true; break; }
                    if (return_weapon_killed) {
                        // WGS-28: feed the weapon casualty into the existing return commit once.
                        const auto index = static_cast<std::size_t>(weapon - conflict.forces.begin());
                        out.result->forces[index].disposition = ForceDisposition::destroyed;
                        out.result->forces[index].final_blow_player = out.result->winner;
                        out.result->superweapon_destroyed = true; out.result->superweapon_killer = out.result->winner;
                    } else out.result->retreat_player = out.result->winner;
                }
            }
            const auto& context = impl.child->conflict;
            if (impl.decorator) {
                auto decorated = impl.decorator(context, impl.child->session.world(), *out.result);
                if (!decorated) { impl.failure = decorated.error(); return core::Result<GameTick>::failure(*impl.failure); }
            }
            auto planned = context.corruption_mission ? core::Result<TacticalReturnPlan>::success({*out.result, {}, {}, {}})
                : plan_tactical_return(context, *out.result, *impl.world().snapshot());
            if (!planned) { impl.failure = planned.error(); return core::Result<GameTick>::failure(*impl.failure); }
            out.result = planned.value().result;
            if (impl.hero_resolver && !context.corruption_mission) for (std::size_t i = 0; i < context.forces.size(); ++i) {
                if (!out.result->forces[i].hero_defeated) continue;
                auto defeat = impl.hero_resolver(context, context.forces[i], out.result->forces[i], *impl.world().snapshot());
                if (!defeat) { impl.failure = defeat.error(); return core::Result<GameTick>::failure(*impl.failure); }
                if (!defeat.value()) continue;
                for (auto& companion : defeat.value()->companions) {
                    companion.dead = std::binary_search(planned.value().destroyed.begin(), planned.value().destroyed.end(), companion.id);
                    companion.deletion_pending = companion.dead;
                }
                auto scheduled = schedule_hero_defeat(*defeat.value(), executor);
                if (!scheduled) { impl.failure = scheduled.error(); return core::Result<GameTick>::failure(*impl.failure); }
                planned.value().destroyed.insert(planned.value().destroyed.end(), scheduled.value().destroyed_companions.begin(), scheduled.value().destroyed_companions.end());
            }
            std::sort(planned.value().destroyed.begin(), planned.value().destroyed.end());
            planned.value().destroyed.erase(std::unique(planned.value().destroyed.begin(), planned.value().destroyed.end()), planned.value().destroyed.end());
            auto replay = impl.child->session.record();
            auto reconciled = impl.world().reconcile_battle(impl.child->session.world().next_entity_id(),
                impl.tick + 1, impl.child->session.completed_tick());
            if (reconciled && impl.world().setup().control && !impl.child->conflict.raid && !impl.child->conflict.corruption_mission) {
                const auto& conflict = impl.child->conflict;
                galactic::ControlBattleReturn returned;
                returned.planet = conflict.planet; returned.invader = conflict.invader; returned.defender = conflict.defender; returned.station_owner = conflict.station_owner;
                returned.winner = out.result->winner; returned.land = conflict.layer == BattleLayer::land;
                // WGM-57: a space return without a tactical station clears the stored base.
                returned.station_level = !returned.land && !conflict.station ? 0 : out.result->station_level;
                for (std::size_t i = 0; i < conflict.forces.size(); ++i) {
                    const auto& force = conflict.forces[i]; const auto& result = out.result->forces[i];
                    if (result.disposition == ForceDisposition::destroyed || result.disposition == ForceDisposition::destroy_after_tactical)
                        returned.destroyed.push_back(force.id);
                    else if (returned.land && force.owner == conflict.invader && result.disposition == ForceDisposition::survived) {
                        const auto& traits = impl.world().setup().fleet_control;
                        const auto transport = traits && std::any_of(traits->types.begin(), traits->types.end(),
                            [&](const auto& row) { return row.type == force.type && row.transport; });
                        if (transport) returned.surviving_invaders.push_back({force.id, force.type, force.owner});
                    }
                }
                reconciled = impl.world().apply_control_battle(returned);
            }
            if (reconciled && !context.corruption_mission && impl.world().victory() && impl.child->conflict.superweapon && weapon_was_alive) {
                const auto& conflict = impl.child->conflict;
                const auto weapon = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& force) { return force.id == conflict.superweapon; });
                if (weapon != conflict.forces.end()) {
                    const auto loser = out.result->winner == conflict.invader ? conflict.defender : conflict.invader;
                    reconciled = impl.world().apply_superweapon_victory_return(weapon->owner, out.result->winner, loser, return_weapon_killed);
                }
            }
            if (reconciled && context.corruption_mission) reconciled = impl.world().finish_corruption_mission(context.corruption_request,
                out.result->winner == context.invader ? galactic::CorruptionMissionState::succeeded : galactic::CorruptionMissionState::failed);
            if (reconciled && !context.corruption_mission) reconciled = apply_tactical_return(impl.world(), context, planned.value(), impl.return_state, executor);
            if (reconciled && !context.corruption_mission && impl.applier) reconciled = impl.applier(impl.world(), *out.result);
            if (reconciled && !context.corruption_mission) reconciled = impl.world().finish_native_invasion(context.id, out.result->winner);
            if (!reconciled) { impl.failure = reconciled.error(); return core::Result<GameTick>::failure(*impl.failure); }
            if (!context.corruption_mission) out.result->invasion_offers = tactical_invasion_offers(context, *out.result, *impl.world().snapshot());
            if (!context.corruption_mission) if (auto noticed = impl.battle_story(context.planet, context.layer, out.result->winner,
                out.result->winner == context.invader ? context.defender : context.invader); !noticed) {
                impl.failure = noticed.error(); return core::Result<GameTick>::failure(*impl.failure);
            }
            if (auto noticed = impl.battle_notice(GalacticAiBattleKind::returned, context, &*out.result); !noticed) {
                impl.failure = noticed.error(); return core::Result<GameTick>::failure(*impl.failure);
            }
            impl.latest_result = *out.result; impl.latest_context = context;
            if (!context.corruption_mission) impl.return_state.structure_markers = context.structure_markers;
            impl.last_battle = std::move(replay);
            impl.world().end_battle_roles();
            impl.timelines->historical = impl.timelines->child;
            impl.timelines->historical_slot = std::move(impl.timelines->child_slot);
            impl.timelines->historical_slot->retired = true;
            impl.timelines->child.reset();
            impl.ended.insert(impl.child->conflict.id); impl.child.reset();
            if (impl.conflict_pause) {
                auto resumed = impl.world().adjust_conflict_pause(false);
                if (!resumed) { impl.failure = resumed.error(); return core::Result<GameTick>::failure(*impl.failure); }
                impl.conflict_pause = false;
            }
        }
        }
    } else {
        struct TailReset {
            galactic::GalacticSession& world;
            ~TailReset() { world.set_conflict_tail_handler({}); }
        } tail_reset{impl.world()};
        parent_mode_serviced = true;
        impl.world().set_timeline_service([owner = &impl](const auto& inputs) { return owner->update_galaxy_timeline(inputs); });
        // GFS-15/GFD-26: the outer tail below owns the frame timer only for conflict services or a victory
        // monitor (the condition that advances it there, and the barrier lag hosted scripts see). A tail
        // installed only for pending hero returns leaves the timer with the galaxy step.
        const bool outer_timer = impl.conflict_services || impl.world().victory();
        if (impl.conflict_services || !impl.return_state.heroes.empty()) impl.world().set_conflict_tail_handler([&](galactic::GalacticSession&) -> core::Result<void> {
            auto placement = impl.hero_placement;
            auto structures = impl.hero_structures;
            if (impl.live_hero_returns.place) placement = [&](const auto& record, const auto planet) {
                return impl.live_hero_returns.place(impl.world(), record, planet, executor);
            };
            if (impl.live_hero_returns.structures) structures = [&](const auto& record) {
                return impl.live_hero_returns.structures(impl.world(), record, executor);
            };
            // Retain this snapshot while placement publishes replacement snapshots.
            const auto hero_snapshot = impl.world().snapshot();
            const auto hero_count = impl.return_state.heroes.size();
            if (auto serviced = galactic::service_hero_returns(impl.return_state.heroes, impl.world().clock().galactic_frame,
                impl.world().clock().pause_depth != 0, hero_snapshot->planets(), placement,
                structures); !serviced) return serviced;
            if (impl.return_state.heroes.size() != hero_count)
                impl.world().sync_victory_hero_returns(impl.return_state.heroes);
            if (!impl.conflict_services) return core::Result<void>::success();
            const auto tail_snapshot = impl.world().snapshot();
            // WGM-43 / GFD-18: the conflict tail receives the arrival in this frame,
            // announces it, and returns before the next eligible arbitration invocation.
            const auto commit_admissions = [&]() -> core::Result<void> {
                for (const auto& entry : impl.queued_admissions) {
                    auto reserved = impl.world().reserve_conflict_entities(*entry.conflict.tactical.first_free_entity_id);
                    if (!reserved) return reserved;
                    impl.admissions.push_back(entry); impl.waiting_admissions.push_back(entry);
                    for (const auto& force : entry.conflict.forces)
                        out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::combat_imminent, {},
                            force.id, 0, entry.conflict.id, galactic::GalacticRejectReason::none});
                    out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::planet_combat_begin, {},
                        entry.conflict.planet, 0, entry.conflict.id, galactic::GalacticRejectReason::none});
                }
                impl.queued_admissions.clear(); return core::Result<void>::success();
            };
            const auto replay_entries = impl.replay_admissions.equal_range(impl.tick);
            const bool replaying_admission = replay_entries.first != replay_entries.second;
            for (auto replay = replay_entries.first; replay != replay_entries.second; ++replay) {
                auto queued = queue_conflict(replay->second.conflict, replay->second.arbitration);
                if (queued) queued = commit_admissions();
                if (!queued) { impl.failure = queued.error(); return core::Result<void>::failure(*impl.failure); }
            }
            impl.replay_admissions.erase(replay_entries.first, replay_entries.second);
            if (auto committed = commit_admissions(); !committed) { impl.failure = committed.error(); return core::Result<void>::failure(*impl.failure); }
            if (impl.world().replaying_native_effects()) {
                if (auto replayed = impl.world().replay_native_invasion_service(); !replayed) return replayed;
            } else if (impl.native_conflict_service && !impl.admission && !impl.staged && impl.waiting_admissions.empty()) {
                const auto* invasion = impl.world().pending_native_invasion();
                if (invasion && invasion->state == galactic::GalacticSession::NativeInvasion::State::pending) {
                    const auto request = invasion->request;
                    auto candidate = impl.native_conflict_service(*impl.world().snapshot(), *invasion, impl.world().next_entity_id());
                    if (!candidate) return core::Result<void>::failure(candidate.error());
                    sim::EntityId conflict{};
                    if (candidate.value()) {
                        const auto& record = candidate.value()->conflict;
                        if (record.layer != BattleLayer::land || record.planet != invasion->planet || record.invader != invasion->invader || record.defender != invasion->defender)
                            return fail<void>("native invasion admission differs from its retained owners/location");
                        conflict = record.id;
                        if (auto queued = queue_conflict(record, candidate.value()->arbitration); !queued) return queued;
                        if (auto committed = commit_admissions(); !committed) return committed;
                    }
                    if (auto dispatched = impl.world().dispatch_native_invasion(request, conflict); !dispatched) return dispatched;
                }
            }
            if (!replaying_admission && impl.conflict_service && !impl.staged) {
                for (const auto& event : tail_snapshot->events()) {
                    if (event.kind != galactic::GalacticEventKind::movement_conflict
                        && event.kind != galactic::GalacticEventKind::land_conflict_requested) continue;
                    auto candidate = impl.conflict_service(*tail_snapshot, event, impl.world().next_entity_id());
                    if (!candidate) { impl.failure = candidate.error(); return core::Result<void>::failure(*impl.failure); }
                    if (!candidate.value()) continue;
                    auto queued = queue_conflict(candidate.value()->conflict, candidate.value()->arbitration);
                    if (queued) queued = commit_admissions();
                    if (!queued) { impl.failure = queued.error(); return core::Result<void>::failure(*impl.failure); }
                }
            }
            if (!impl.admission && !impl.waiting_admissions.empty()) {
                impl.admission = std::move(impl.waiting_admissions.front());
                impl.waiting_admissions.erase(impl.waiting_admissions.begin());
            }
            if (impl.admission && impl.tick > impl.admission->entry_tick && !impl.conflict_pause
                && (!impl.conflict_gate || impl.conflict_gate(*impl.world().snapshot()))) {
                auto paused = impl.world().adjust_conflict_pause(true);
                if (!paused) { impl.failure = paused.error(); return core::Result<void>::failure(*impl.failure); }
                impl.conflict_pause = true;
                const auto& conflict = impl.admission->conflict;
                out.pending_alerts.emplace(conflict.id, PendingConflictAlert{
                    conflict.invader, conflict.defender, conflict.layer == BattleLayer::land});
                out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::conflict_pending, {},
                    impl.admission->conflict.id, 0, impl.admission->conflict.planet, galactic::GalacticRejectReason::none});
            }
            if (impl.admission && impl.conflict_pause) {
                const auto route = impl.admission->arbitration.route;
                const auto automatic = route == ConflictRoute::instant_auto || route == ConflictRoute::dialog_auto;
                if (automatic || route == ConflictRoute::tactical) {
                    auto selected = select_pending_battle(!automatic, executor);
                    if (!selected) { impl.failure = selected.error(); return core::Result<void>::failure(*impl.failure); }
                }
            }
            impl.world().publish_conflict_events(out.conflict_events);
            published_conflict_events = out.conflict_events.size();
            return core::Result<void>::success();
        }, outer_timer);
        auto stepped = [&]() -> core::Result<galactic::GalacticTick> {
            if (!impl.scripts) return impl.world().step(executor, impl.staged.has_value() || impl.admission.has_value());
            auto hosted = impl.scripts->step(executor, galaxy_inputs, impl.staged.has_value() || impl.admission.has_value());
            if (!hosted) return core::Result<galactic::GalacticTick>::failure(hosted.error());
            out.diagnostics.insert(out.diagnostics.end(), hosted.value().refused_input.begin(), hosted.value().refused_input.end());
            for (const auto& routed : hosted.value().script_input) {
                if (routed.dropped) out.diagnostics.push_back(*routed.dropped);
                if (!routed.key) continue;
                // The bridge's accepted orders are part of the reconstruction envelope.
                auto recorded = *routed.accepted; recorded.key.tick = impl.tick;
                impl.executed.push_back(recorded);
                auto& last = impl.last[recorded.key.seat];
                last = std::max(last, std::pair(recorded.key.tick, recorded.key.sequence));
            }
            hosted.value().world.state_hash = hosted.value().state_hash;
            hosted.value().world.state_sha256 = impl.hasher ? std::string{} : hosted.value().state_hash.get();
            return core::Result<galactic::GalacticTick>::success(std::move(hosted).value().world);
        }();
        if (!stepped) { impl.failure = stepped.error(); return core::Result<GameTick>::failure(*impl.failure); }
        out.galaxy_work = stepped.value().work;
        out.state_sha256 = std::move(stepped.value().state_sha256);
        out.state_hash = std::move(stepped.value().state_hash);
        for (const auto& event : stepped.value().snapshot->events())
            if (event.kind == galactic::GalacticEventKind::save_requested || event.kind == galactic::GalacticEventKind::save_rejected)
                out.save_events.push_back(event);
        if (impl.staged) {
            impl.conflicts.push_back({impl.tick, impl.staged->conflict});
            impl.child = std::move(impl.staged); impl.staged.reset();
            impl.timelines->child = ModeTimelineSnapshot{{SessionMode::battle, impl.child->conflict.id}, impl.tick + 1, 0};
            impl.timelines->child_slot = std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl.timelines->child, false});
            impl.child->owns_pause = impl.conflict_pause;
            if (auto noticed = impl.battle_notice(GalacticAiBattleKind::entered, impl.child->conflict); !noticed) {
                impl.failure = noticed.error(); return core::Result<GameTick>::failure(*impl.failure);
            }
            if (impl.conflict_pause) out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::child_battle_selected,
                {}, impl.child->conflict.id, 0, impl.child->conflict.planet, galactic::GalacticRejectReason::none});
            auto role = impl.world().begin_battle_roles(impl.child->conflict.id, impl.child->conflict.invader, impl.child->conflict.defender);
            if (!role) { impl.failure = role.error(); return core::Result<GameTick>::failure(*impl.failure); }
            if (auto refreshed = impl.refresh_battle_seats(); !refreshed) { impl.failure = refreshed.error(); return core::Result<GameTick>::failure(*impl.failure); }
            impl.child->galaxy_hash = impl.galaxy_hash().value();
            out.battle = impl.child->session.world().snapshot();
            out.battle_state_hash = impl.child->world_hash;
        }
    }
    // GFD-25 / GFS-U07: explicitly selected campaign-return boundary. This does
    // not decide automatic conflict admission or add a galaxy frame service.
    while (!impl.world().pending_outcomes().empty()
        && impl.world().pending_outcomes().front().completed_tick <= impl.tick + 1) {
        if (impl.child || impl.staged) { impl.failure = fail<void>("auto-resolve return overlaps a child battle").error(); return core::Result<GameTick>::failure(*impl.failure); }
        const auto entry = impl.world().pending_outcomes().front();
        auto application = decode_auto_resolve_outcome(entry.payload);
        if (!application || application.value().result.conflict != entry.conflict) {
            impl.failure = fail<void>("invalid recorded auto-resolve result").error(); return core::Result<GameTick>::failure(*impl.failure);
        }
        auto committed = impl.world().commit_outcome(entry, [&](const auto&) {
            auto applied = apply_auto_resolve_outcome(impl.world(), application.value(), impl.outcome_services);
            if (!applied) return applied;
            return impl.world().finish_native_invasion(entry.conflict, application.value().result.winner);
        }, executor);
        if (!committed) { impl.failure = committed.error(); return core::Result<GameTick>::failure(*impl.failure); }
        if (auto noticed = impl.battle_story(application.value().planet,
            application.value().land ? BattleLayer::land : BattleLayer::space, application.value().result.winner,
            application.value().result.winner == application.value().invader ? application.value().defender : application.value().invader); !noticed) {
            impl.failure = noticed.error(); return core::Result<GameTick>::failure(*impl.failure);
        }
        if (impl.native_ai.battle) {
            const auto context = std::find_if(impl.admissions.begin(), impl.admissions.end(), [&](const auto& admission) {
                return admission.conflict.id == entry.conflict;
            });
            if (context == impl.admissions.end()) {
                impl.failure = fail<void>("native AI auto-resolve return needs its admitted conflict context").error();
                return core::Result<GameTick>::failure(*impl.failure);
            }
            auto noticed = impl.battle_notice(GalacticAiBattleKind::returned, context->conflict, &application.value().result, true);
            if (!noticed) { impl.failure = noticed.error(); return core::Result<GameTick>::failure(*impl.failure); }
        }
        out.result = application.value().result;
        impl.latest_auto_outcome = std::make_shared<const AutoResolveOutcomeApplication>(application.value());
        impl.outcome_history[impl.outcome_record_index] = std::move(application).value();
        impl.outcome_record_index = (impl.outcome_record_index + 1) % impl.outcome_history.size();
    }
    impl.world().publish_conflict_events(std::span(out.conflict_events).subspan(published_conflict_events));
    // GFD-26/GFSC-18: query final active mode/pause after players and host scripts.
    // A selected child retains the parent; its transition-time timer reader is the
    // separately qualified scene/mode probe, not an assumed parent increment.
    // GFS-15/GFD-04: hosted AI/story finished; victory sees t before timer advance.
    if (parent_mode_serviced && !impl.child && impl.world().victory()) impl.world().finish_victory_frame();
    if (parent_mode_serviced && (impl.conflict_services || impl.world().victory()) && !impl.child) impl.world().finish_conflict_frame_timer();
    ++impl.tick; out.completed_tick = impl.tick; out.galaxy = impl.world().snapshot();
    gc_profile::Scope profile_scope(gc_profile::Phase::frame_end_hash_wall);
    if (impl.child) out.battle_seats = impl.child->conflict.seats;
    if (impl.child) {
        const auto& galaxy = impl.child->galaxy_hash;
        world_hash = impl.child->world_hash;
        auto native = impl.native_hash();
        if (!native) { impl.failure = native.error(); return core::Result<GameTick>::failure(*impl.failure); }
        if (impl.hasher) {
            out.state_sha256.clear();
            out.state_hash = impl.hasher->derive(world_hash, [galaxy, script = impl.child->script_hash, context = impl.child_context_hash(), ai = native.value()](const std::string& world) {
                return Impl::with_native(combined_hash(galaxy, world, script, context), ai);
            });
        } else {
            out.state_sha256 = Impl::with_native(combined_hash(galaxy, world_hash.get(), impl.child->script_hash, impl.child_context_hash()), native.value());
            out.state_hash = sim::StateHash(out.state_sha256);
        }
    } else if (out.mode == SessionMode::battle || impl.conflict_services || impl.world().victory() || impl.admission || !impl.waiting_admissions.empty() || impl.conflict_pause || out.result || impl.return_state != TacticalReturnState{}) {
        auto digest = impl.base_hash();
        if (digest) {
            auto native = impl.native_hash();
            if (!native) digest = native;
            else digest = core::Result<std::string>::success(Impl::with_native(digest.value(), native.value()));
        }
        if (!digest) { impl.failure = digest.error(); return core::Result<GameTick>::failure(*impl.failure); }
        out.state_sha256 = digest.value(); out.state_hash = sim::StateHash(out.state_sha256);
    } else if (impl.native_ai.state) {
        // Preserve the world's section cache and the bridge's already composed
        // Lua hash. Native ownership alone must not force a whole-world rehash.
        auto native = impl.native_hash();
        if (!native) { impl.failure = native.error(); return core::Result<GameTick>::failure(*impl.failure); }
        if (impl.hasher) {
            out.state_sha256.clear();
            out.state_hash = impl.hasher->derive(out.state_hash, [ai = std::move(native).value()](const std::string& base) {
                return Impl::with_native(base, ai);
            });
        } else {
            out.state_sha256 = Impl::with_native(out.state_hash.get(), native.value());
            out.state_hash = sim::StateHash(out.state_sha256);
        }
    }
    // Capture the scalar inputs by value for asynchronous hashing. No reader
    // can accidentally hash the next completed publication.
    const auto publication = *impl.timelines;
    const bool ai_sync = bool(impl.native_ai.state);
    if (impl.hasher) {
        out.state_sha256.clear();
        out.state_hash = impl.hasher->derive(out.state_hash, [publication, ai_sync](const std::string& base) { return Impl::with_timeline(base, publication, ai_sync); });
    } else {
        out.state_sha256 = Impl::with_timeline(out.state_hash.get(), publication, ai_sync);
        out.state_hash = sim::StateHash(out.state_sha256);
    }
    out.story_notices = std::move(impl.story_notices);
    return core::Result<GameTick>::success(std::move(out));
    } catch (const std::exception& error) {
        auto failed = fail<GameTick>("game session service threw: " + std::string(error.what()), galactic::diagnostic_codes::worker_failure);
        impl_->failure = failed.error();
        return failed;
    } catch (...) {
        auto failed = fail<GameTick>("game session service threw", galactic::diagnostic_codes::worker_failure);
        impl_->failure = failed.error();
        return failed;
    }
}
core::Result<void> GameSession::set_galactic_ai_services(GalacticAiServices services) {
    if (impl_->failure || impl_->native_ai.state || !services.state)
        return fail<void>("native AI services need one owned state contributor at an inactive boundary");
    auto saved = services.state->capture();
    if (!saved) return core::Result<void>::failure(saved.error());
    auto encoded = script::foc::galactic_ai::encode_native_state(saved.value());
    if (!encoded) return core::Result<void>::failure(encoded.error());
    impl_->native_ai = std::move(services);
    return core::Result<void>::success();
}
core::Result<void> GameSession::configure_galactic_ai_services(GalacticAiFactory factory) {
    if (impl_->failure || impl_->native_ai.state || !factory)
        return fail<void>("native AI configuration requires a fresh owned boundary and factory");
    auto services = factory(impl_->world());
    if (!services) return core::Result<void>::failure(services.error());
    return set_galactic_ai_services(std::move(services).value());
}
core::Result<void> GameSession::restore_planet_access(std::span<const std::uint8_t> bytes) {
    if (impl_->failure || impl_->native_ai.state) return fail<void>("planet access must precede native AI reconstruction");
    return impl_->world().restore_planet_access(bytes);
}
core::Result<void> GameSession::configure_native_replay_owners(NativeReplayConfigurator configure) {
    if (impl_->failure || !configure) return fail<void>("native reconstruction requires a typed owner configurator");
    return configure(impl_->world());
}
void GameSession::begin_native_reconstruction() noexcept { impl_->reconstructing_native_notices = true; }
core::Result<void> GameSession::finish_native_reconstruction(const sim::PartitionExecutor& executor) {
    if (auto applied = impl_->world().replay_native_barrier(executor); !applied) return applied;
    if (auto finished = impl_->world().finish_native_replay(); !finished) return finished;
    impl_->reconstructing_native_notices = false;
    return core::Result<void>::success();
}
bool GameSession::native_ai_installed() const noexcept { return static_cast<bool>(impl_->native_ai.state); }
core::Result<GalacticAiSave> GameSession::save_galactic_ai() const {
    if (impl_->failure) return core::Result<GalacticAiSave>::failure(*impl_->failure);
    GalacticAiSave save{impl_->tick, impl_->world().clock().galactic_frame, {}};
    if (impl_->native_ai.state) {
        auto captured = impl_->native_ai.state->capture();
        if (!captured) return core::Result<GalacticAiSave>::failure(captured.error());
        save.state = std::move(captured).value();
    }
    return core::Result<GalacticAiSave>::success(std::move(save));
}
core::Result<void> GameSession::restore_galactic_ai(const GalacticAiSave& save) {
    if (impl_->failure || save.completed_tick != impl_->tick || save.galactic_frame != impl_->world().clock().galactic_frame
        || save.state.has_value() != static_cast<bool>(impl_->native_ai.state))
        return fail<void>("native AI restore boundary or adapter presence differs (tick "
            + std::to_string(save.completed_tick) + "/" + std::to_string(impl_->tick) + ", frame "
            + std::to_string(save.galactic_frame) + "/" + std::to_string(impl_->world().clock().galactic_frame)
            + ", adapter " + std::to_string(save.state.has_value()) + "/" + std::to_string(static_cast<bool>(impl_->native_ai.state))
            + ", failed " + std::to_string(static_cast<bool>(impl_->failure)) + ")");
    if (!save.state) return core::Result<void>::success();
    auto encoded = script::foc::galactic_ai::encode_native_state(*save.state);
    if (!encoded) return core::Result<void>::failure(encoded.error());
    return impl_->native_ai.state->restore(*save.state);
}
core::Result<void> GameSession::install_galactic_scripts(authoritative::ScriptScheduler ai,
    authoritative::ScriptScheduler story, const std::optional<authoritative::GalacticScriptSave>& saved) {
    if (impl_->scripts || impl_->failure || (!saved && impl_->tick != 0))
        return fail<void>("install galactic scripts once at creation or at a restored barrier");
    auto fresh = galactic::GalacticSession::create(impl_->galaxy.setup());
    if (!fresh) return core::Result<void>::failure(fresh.error());
    auto host = saved ? authoritative::ScriptedGalacticSession::restore_preserving(impl_->galaxy, std::move(ai), std::move(story), *saved)
                      : authoritative::ScriptedGalacticSession::create(std::move(fresh).value(), std::move(ai), std::move(story));
    if (!host) return core::Result<void>::failure(host.error());
    if (!saved) host.value().parent_world() = std::move(impl_->galaxy);
    host.value().set_state_hasher(impl_->hasher);
    impl_->scripts.emplace(std::move(host).value());
    impl_->world().enable_conflict_requests(impl_->conflict_services);
    impl_->world().defer_victory_tail(impl_->world().setup().victory.has_value());
    if (impl_->child) impl_->child->galaxy_hash = impl_->galaxy_hash().value();
    return core::Result<void>::success();
}
authoritative::ScriptedGalacticSession* GameSession::galactic_scripts() noexcept {
    return impl_->scripts ? &*impl_->scripts : nullptr;
}
core::Result<std::optional<authoritative::GalacticScriptSave>> GameSession::save_galactic_scripts() const {
    using Save = std::optional<authoritative::GalacticScriptSave>;
    if (impl_->failure) return core::Result<Save>::failure(*impl_->failure);
    if (!impl_->scripts) return core::Result<Save>::success({});
    auto saved = impl_->scripts->save_scripts();
    if (!saved) return core::Result<Save>::failure(saved.error());
    return core::Result<Save>::success(std::move(saved).value());
}
void GameSession::set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept {
    if (impl_->scripts) impl_->scripts->set_state_hasher(hasher);
    else impl_->world().set_state_hasher(hasher);
    if (impl_->child) impl_->child->session.set_state_hasher(hasher);
    if (impl_->staged) impl_->staged->session.set_state_hasher(hasher);
    impl_->hasher = std::move(hasher);
}
void GameSession::set_control_effects_handler(galactic::GalacticSession::ControlEffectsHandler handler) { impl_->control_observer = std::move(handler); }
void GameSession::set_story_notice_handler(StoryNoticeHandler handler) { impl_->story_handler = std::move(handler); }
void GameSession::set_story_identities(std::map<galactic::PlanetId, std::string> planets,
    std::map<galactic::PlayerId, std::string> factions) {
    impl_->story_planets = std::move(planets); impl_->story_factions = std::move(factions);
}
std::vector<GameStoryNotice> GameTick::observer_story_notices(galactic::PlayerId player) const {
    std::vector<GameStoryNotice> notices;
    if (!player) return notices;
    for (const auto& notice : story_notices) {
        if (notice.observer_planet && (!galaxy || std::none_of(galaxy->observations().begin(), galaxy->observations().end(), [&](const auto& row) {
            return row.player == player && row.planet == notice.observer_planet && row.discovered;
        }))) continue;
        if (notice.player ? *notice.player == player : notice.old_owner == player || notice.new_owner == player)
            notices.push_back(notice);
    }
    return notices;
}
void GameSession::set_movement_access_provider(galactic::GalacticSession::MovementAccessProvider provider) { impl_->world().set_movement_access_provider(std::move(provider)); }
void GameSession::set_entry_resolver(EntryResolver resolver) { impl_->entry_resolver = std::move(resolver); }
bool GameSession::has_entry_resolver() const noexcept { return static_cast<bool>(impl_->entry_resolver); }
void GameSession::set_result_applier(ResultApplier applier) { impl_->applier = std::move(applier); }
void GameSession::set_result_decorator(ResultDecorator decorate) { impl_->decorator = std::move(decorate); }
void GameSession::set_hero_defeat_resolver(HeroDefeatResolver resolver) { impl_->hero_resolver = std::move(resolver); }
core::Result<galactic::HeroSchedule> GameSession::schedule_hero_defeat(const galactic::HeroDefeat& defeat,
    const sim::PartitionExecutor& executor) {
    if (impl_->failure) return core::Result<galactic::HeroSchedule>::failure(*impl_->failure);
    auto scheduled = galactic::schedule_hero_return(impl_->return_state.heroes, defeat,
        impl_->world().clock().galactic_frame, impl_->world().snapshot()->planets());
    if (!scheduled) return scheduled;
    if (auto admitted = impl_->world().apply_hero_defeat_victory(defeat, impl_->return_state.heroes, executor); !admitted)
        return core::Result<galactic::HeroSchedule>::failure(admitted.error());
    return scheduled;
}
void GameSession::set_hero_placement(galactic::HeroPlacement placement) { impl_->hero_placement = std::move(placement); }
void GameSession::set_hero_structure_resolver(galactic::HeroStructureResolver resolver) { impl_->hero_structures = std::move(resolver); }
void GameSession::set_live_hero_returns(LiveHeroReturns services) { impl_->live_hero_returns = std::move(services); }
const TacticalReturnState& GameSession::return_state() const noexcept { return impl_->return_state; }
std::optional<std::uint32_t> GameSession::remembered_structure_marker(const sim::EntityId id) const {
    const auto& rows = impl_->return_state.structure_markers;
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, const auto value) { return row.first < value; });
    return at != rows.end() && at->first == id ? std::optional(at->second) : std::nullopt;
}
core::Result<std::vector<std::uint8_t>> GameSession::save_return_state() const {
    if (impl_->failure) return core::Result<std::vector<std::uint8_t>>::failure(*impl_->failure);
    return core::Result<std::vector<std::uint8_t>>::success(encode_tactical_return_state(impl_->return_state));
}
core::Result<void> GameSession::restore_return_state(const std::span<const std::uint8_t> bytes) {
    auto saved = decode_tactical_return_state(bytes);
    if (!saved) return core::Result<void>::failure(saved.error());
    impl_->return_state = std::move(saved).value();
    impl_->world().sync_victory_hero_returns(impl_->return_state.heroes);
    return core::Result<void>::success();
}
const BattleResult* GameSession::latest_tactical_result() const noexcept { return impl_->latest_result ? &*impl_->latest_result : nullptr; }
const ConflictRecord* GameSession::latest_tactical_context() const noexcept { return impl_->latest_context ? &*impl_->latest_context : nullptr; }
const ConflictRecord* GameSession::latest_tactical_conflict() const noexcept { return latest_tactical_context(); }
core::Result<void> GameSession::restore_latest_tactical_result(ConflictRecord context, BattleResult result) {
    if (!context.id || context.id != result.conflict || context.forces.size() != result.forces.size())
        return fail<void>("saved tactical summary does not match its context");
    for (std::size_t i = 0; i < context.forces.size(); ++i)
        if (context.forces[i].id != result.forces[i].id) return fail<void>("saved tactical summary force identity differs");
    impl_->latest_context = std::move(context); impl_->latest_result = std::move(result);
    return core::Result<void>::success();
}
void GameSession::clear_latest_tactical_result() noexcept { impl_->latest_result.reset(); impl_->latest_context.reset(); }
core::Result<void> GameSession::stage_auto_resolve_outcome(const AutoResolveOutcomeApplication& application) {
    if (impl_->failure || impl_->child || impl_->staged) return fail<void>("auto-resolve return needs an idle campaign boundary");
    auto bytes = encode_auto_resolve_outcome(application);
    if (!bytes) return core::Result<void>::failure(bytes.error());
    return impl_->world().stage_outcome({impl_->tick + 1, application.result.conflict, std::move(bytes).value()});
}
void GameSession::set_auto_resolve_outcome_services(AutoResolveOutcomeServices services) { impl_->outcome_services = std::move(services); }
std::vector<AutoResolveOutcomeApplication> GameSession::auto_resolve_history() const {
    std::vector<AutoResolveOutcomeApplication> records;
    for (std::size_t i = 0; i < impl_->outcome_history.size(); ++i) {
        const auto& entry = impl_->outcome_history[(impl_->outcome_record_index + i) % impl_->outcome_history.size()];
        if (entry) records.push_back(*entry);
    }
    return records;
}
void GameSession::clear_auto_resolve_history() noexcept { impl_->outcome_history = {}; impl_->outcome_record_index = 0; impl_->latest_auto_outcome.reset(); }
std::shared_ptr<const AutoResolveOutcomeApplication> GameSession::latest_auto_resolve_outcome() const noexcept { return impl_->latest_auto_outcome; }
void GameSession::set_auto_resolve_withdrawal(AutoResolveWithdrawal withdraw) { impl_->auto_withdraw = std::move(withdraw); }
void GameSession::set_auto_resolve_progress_reader(AutoResolveProgressReader reader) { impl_->auto_progress = std::move(reader); }
std::optional<AutoResolveProgress> GameSession::auto_resolve_progress() const {
    auto progress = auto_resolve_needs_service() && impl_->auto_progress ? impl_->auto_progress() : std::nullopt;
    // Solver attrition can clear retreat when no forces remain (WAR-46).
    // The session's first admitted command still owns this conflict's admission.
    if (progress && impl_->withdrawing_player) progress->withdraw_admitted = false;
    return progress;
}
bool GameSession::auto_resolve_needs_service() const noexcept {
    return impl_->auto_resolver && impl_->admission && (impl_->admission->arbitration.route == ConflictRoute::instant_auto
        || impl_->admission->arbitration.route == ConflictRoute::dialog_auto);
}
void GameSession::set_child_configurator(ChildConfigurator configure) { impl_->configure = std::move(configure); }
core::Result<void> GameSession::set_child_script_services(ScriptFactory scripts, ChildConfigurator configure) {
    if (impl_->child) return fail<void>("cannot replace child script services while a tactical child is active");
    if (!scripts || !configure) return fail<void>("child script services require a paired factory and configurator");
    impl_->script_factory = std::move(scripts); impl_->configure = std::move(configure);
    return core::Result<void>::success();
}
void GameSession::set_native_conflict_service(NativeConflictService service) {
    impl_->native_conflict_service = std::move(service);
    impl_->conflict_services = impl_->conflict_services || static_cast<bool>(impl_->native_conflict_service);
    impl_->world().enable_native_invasions(static_cast<bool>(impl_->native_conflict_service));
    impl_->world().enable_conflict_requests(impl_->conflict_services);
}
bool GameSession::has_outstanding_conflicts() const noexcept {
    return impl_->child || impl_->staged || impl_->admission || !impl_->queued_admissions.empty()
        || !impl_->waiting_admissions.empty() || impl_->world().pending_native_invasion();
}
bool GameSession::has_pending_conflict_for_admission() const noexcept { return impl_->staged || impl_->admission; }
GameSession::NativeConflictReaders GameSession::native_conflict_readers() const {
    return {[owner = impl_.get()] { return owner->staged || owner->admission; },
        [owner = impl_.get()] { return owner->child || owner->staged || owner->admission || !owner->queued_admissions.empty()
            || !owner->waiting_admissions.empty() || owner->world().pending_native_invasion(); }};
}
void GameSession::set_conflict_service(ConflictService service, ContentResolver content, ScriptFactory scripts) {
    impl_->conflict_service = std::move(service); impl_->content_resolver = std::move(content); impl_->script_factory = std::move(scripts);
    impl_->conflict_services = impl_->conflict_services || static_cast<bool>(impl_->conflict_service);
    impl_->world().enable_conflict_requests(impl_->conflict_services);
    impl_->world().defer_victory_tail(impl_->world().setup().victory.has_value());
}
void GameSession::set_auto_resolver(AutoResolver resolve) { impl_->auto_resolver = std::move(resolve); }
void GameSession::set_auto_resolve_checkpoint_services(AutoResolveStateWriter writer, AutoResolveStateReader reader) {
    impl_->auto_state_writer = std::move(writer); impl_->auto_state_reader = std::move(reader);
}
void GameSession::set_vote_progress(VoteProgress progress) { impl_->vote_progress = std::move(progress); }
void GameSession::set_conflict_gate(ConflictGate gate) { impl_->conflict_gate = std::move(gate); }
const ConflictRecord* GameSession::pending_conflict() const noexcept { return impl_->admission ? &impl_->admission->conflict : nullptr; }
void GameSession::set_pending_battle_alert_reader(PendingBattleAlertReader reader) { impl_->pending_alert = std::move(reader); }
std::optional<PendingConflictView> GameSession::pending_conflict_view() const {
    if (!impl_->admission) return std::nullopt;
    const auto route = impl_->admission->arbitration.route;
    const bool choice_ready = impl_->conflict_pause
        && (route == ConflictRoute::pending_tactical || route == ConflictRoute::vote);
    PendingConflictView view{impl_->admission->conflict, impl_->admission->arbitration, choice_ready, {}};
    if (impl_->pending_alert) if (const auto inputs = impl_->pending_alert(view.conflict)) {
        const auto vote = impl_->votes.find(inputs->local);
        view.alert = pending_battle_alert_view(view.conflict, *inputs, choice_ready,
            vote == impl_->votes.end() ? std::optional<bool>{} : vote->second);
    }
    return view;
}
std::uint64_t GameSession::completed_tick() const noexcept { return impl_->tick; }
SessionMode GameSession::mode() const noexcept { return impl_->child ? SessionMode::battle : SessionMode::galaxy; }
const galactic::GalacticSession& GameSession::galaxy() const noexcept { return impl_->world(); }
galactic::story::RewardResult GameSession::apply_story_reward(const galactic::story::Reward& reward,
    const sim::PartitionExecutor& executor) {
    namespace story = galactic::story;
    if (impl_->failure) return {story::RewardStatus::invalid, "game session has failed", {}};
    if (impl_->child) return {story::RewardStatus::unverified,
        "active tactical story monitor provider is unbound; parent victory is not addressed", {}};
    const bool pending = impl_->world().victory() && impl_->world().victory()->pending.has_value();
    auto result = story::apply(impl_->world(), reward, executor);
    const auto& monitor = impl_->world().victory();
    // WGS-179: registration notification is kind-gated, before maturation.
    if (!pending && monitor && monitor->pending && monitor->pending->kind == galactic::VictoryKind::story_forced) {
        const auto winner = monitor->pending->player;
        const auto found = std::find_if(monitor->context.players.begin(), monitor->context.players.end(),
            [&](const auto& p) { return p.player == winner; });
        story::BattleNotice notice; notice.kind = story::Battle::victory; notice.actor = winner;
        if (found != monitor->context.players.end()) notice.faction = found->faction;
        if (auto delivered = impl_->story_notice({std::nullopt, std::move(notice), 0, 0}); !delivered) {
            impl_->failure = delivered.error();
            return {story::RewardStatus::invalid, delivered.error().message, {}};
        }
    }
    return result;
}
core::Result<void> GameSession::restore_victory_state(const galactic::VictoryState& state) {
    auto restored = impl_->world().restore_victory_state(state);
    if (restored && impl_->child) {
        auto hash = impl_->galaxy_hash();
        if (!hash) return core::Result<void>::failure(hash.error());
        impl_->child->galaxy_hash = std::move(hash).value();
    }
    return restored;
}
core::Result<void> GameSession::restore_actor_representations(std::span<const std::uint8_t> census_section) {
    auto restored = impl_->world().restore_actor_representations(census_section);
    if (restored && impl_->child) impl_->child->galaxy_hash = impl_->galaxy_hash().value();
    return restored;
}
std::string GameSession::state_sha256() const { auto hash = impl_->hash(); return hash ? std::move(hash).value() : std::string{}; }
core::Result<int> GameSession::timeline(ModeId id) const {
    if (id == ModeId{SessionMode::galaxy, 0}) return core::Result<int>::success(impl_->timelines->galaxy.value);
    const auto& child = impl_->timelines->child;
    if (child && child->mode == id) return core::Result<int>::success(child->value);
    return fail<int>("WGA-380: missing or mismatched timeline mode");
}
core::Result<int> GameSession::active_timeline() const {
    return timeline(impl_->timelines->child ? impl_->timelines->child->mode : ModeId{});
}
std::function<core::Result<int>()> GameSession::active_timeline_reader() const {
    return [weak = std::weak_ptr<Impl::TimelinePublication>(impl_->timelines)] {
        const auto owner = weak.lock();
        if (!owner) return fail<int>("WGA-380: timeline owner expired");
        return core::Result<int>::success(owner->child ? owner->child->value : owner->galaxy.value);
    };
}
std::shared_ptr<const ModeTimelineSnapshot> GameSession::historical_child_timeline() const {
    const auto& row = impl_->timelines->child ? impl_->timelines->child : impl_->timelines->historical;
    return row ? std::make_shared<const ModeTimelineSnapshot>(*row) : nullptr;
}
GameSession::ChildTimelineReaderFactory GameSession::child_timeline_reader_factory() const {
    return [weak = std::weak_ptr<Impl::TimelinePublication>(impl_->timelines)](ModeId id) -> ChildTimelineValueReader {
        const auto owner = weak.lock();
        auto slot = owner ? owner->child_slot : nullptr;
        if ((!slot || slot->snapshot.mode != id) && owner) slot = owner->historical_slot;
        if (slot && slot->snapshot.mode != id) slot.reset();
        return [weak, id, slot = std::move(slot)]() mutable -> core::Result<ModeTimelineSnapshot> {
            const auto current = weak.lock();
            if (!slot && current) {
                slot = current->child_slot;
                if (!slot || slot->snapshot.mode != id) slot = current->historical_slot;
                if (slot && slot->snapshot.mode != id) slot.reset();
            }
            if (!slot || (!slot->retired && !current)) return fail<ModeTimelineSnapshot>("WGA-380: child timeline missing, mismatched or expired");
            return core::Result<ModeTimelineSnapshot>::success(slot->snapshot);
        };
    };
}
GameSession::ChildTimelineReader GameSession::child_timeline_reader(ModeId id) const {
    return [reader = child_timeline_reader_factory()(id)]() mutable {
        using Snapshot = std::shared_ptr<const ModeTimelineSnapshot>;
        auto value = reader();
        if (!value) return core::Result<Snapshot>::failure(value.error());
        return core::Result<Snapshot>::success(std::make_shared<const ModeTimelineSnapshot>(std::move(value).value()));
    };
}
ModeTimelineState GameSession::save_timeline() const { return {impl_->timelines->galaxy.value, impl_->timelines->historical}; }
core::Result<void> GameSession::restore_timeline(const ModeTimelineState& saved) {
    if (saved.galaxy < 0 || saved.galaxy > 5 || (saved.historical_child &&
        (saved.historical_child->mode.mode != SessionMode::battle || !saved.historical_child->mode.conflict ||
         saved.historical_child->completed_tick > impl_->tick || saved.historical_child->value < 0 || saved.historical_child->value > 5)))
        return fail<void>("WGA-381: invalid saved mode timeline");
    impl_->timelines->galaxy = {{SessionMode::galaxy, 0}, impl_->tick, saved.galaxy};
    impl_->timelines->historical = saved.historical_child;
    impl_->timelines->historical_slot = saved.historical_child
        ? std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*saved.historical_child, true}) : nullptr;
    return core::Result<void>::success();
}
void GameSession::set_child_timeline_tech(ChildTimelineTech provider) { impl_->child_tech = std::move(provider); }
core::Result<void> GameSession::change_actor_current_type(sim::EntityId actor, sim::galactic::TypeId type) {
    return impl_->world().change_actor_current_type(actor, type);
}
core::Result<void> GameSession::set_ability_lifecycle(sim::EntityId actor, bool enabled, bool cancelled, galactic::AbilityKind kind) {
    return impl_->world().set_ability_lifecycle(actor, enabled, cancelled, kind);
}
void GameSession::set_corruption_mission_service(CorruptionMissionService service) { impl_->corruption_mission_service = std::move(service); }
core::Result<bool> GameSession::request_planet_ruins(galactic::PlanetId planet, bool destroy) {
    return impl_->world().request_planet_ruins(planet, destroy);
}
core::Result<void> GameSession::finish_corruption_mission(std::uint64_t request, galactic::CorruptionMissionState state) {
    return impl_->world().finish_corruption_mission(request, state);
}
void GameSession::set_technology_notice_handler(sim::galactic::GalacticSession::TechnologyNoticeHandler handler) {
    impl_->world().set_technology_notice_handler(std::move(handler));
}
void GameSession::set_feedback_notice_handler(sim::galactic::GalacticSession::FeedbackNoticeHandler handler) {
    impl_->world().set_feedback_notice_handler(std::move(handler));
}
core::Result<sim::math::Fixed> GameSession::set_technology_price_modifier(const sim::galactic::GalacticSession::TechnologyPriceWrite& write) {
    return impl_->world().set_technology_price_modifier(write);
}
core::Result<sim::galactic::GalacticSession::IncomeBlockerReceipt> GameSession::halt_planet_credits(
    sim::EntityId actor, sim::galactic::PlanetId planet) {
    return impl_->world().halt_planet_credits(actor, planet);
}
core::Result<bool> GameSession::remove_income_blocker(sim::galactic::PlanetId planet, sim::EntityId source) {
    return impl_->world().remove_income_blocker(planet, source);
}
core::Result<std::int64_t> GameSession::set_technology(const sim::galactic::GalacticSession::TechnologyWrite& write) {
    return impl_->world().set_technology(write);
}
core::Result<std::int64_t> GameSession::increment_technology(sim::galactic::PlayerId player) {
    return impl_->world().increment_technology(player);
}
galactic::GalacticReplay GameSession::record() const {
    auto commands = impl_->executed;
    std::sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    const auto effects = impl_->world().native_effects();
    return {impl_->world().setup(), impl_->tick, std::move(commands), {effects.begin(), effects.end()}};
}
GameRecording GameSession::recording() const { return {record(), impl_->conflicts, impl_->load_fixups, impl_->admissions, impl_->conflict_services}; }
core::Result<GameRecording> GameSession::checkpoint_recording() const {
    if (impl_->failure) return core::Result<GameRecording>::failure(*impl_->failure);
    auto result = recording();
    for (const auto& [key, command] : impl_->pending) {
        result.commands.commands.push_back(command);
        result.commands.final_tick_count = std::max(result.commands.final_tick_count, key.tick + 1);
    }
    for (const auto& [tick, conflict] : impl_->replay_conflicts) result.conflicts.push_back({tick, conflict});
    for (const auto& [id, selected] : impl_->replay_selected) { (void)id; result.conflicts.push_back(selected); }
    std::sort(result.conflicts.begin(), result.conflicts.end(), [](const auto& a, const auto& b) { return a.entry_tick < b.entry_tick; });
    if (impl_->staged) result.conflicts.push_back({impl_->tick, impl_->staged->conflict});
    for (const auto& [tick, admission] : impl_->replay_admissions) { (void)tick; result.admissions.push_back(admission); }
    for (const auto& entry : impl_->queued_admissions) result.admissions.push_back(entry);
    for (const auto& entry : result.admissions) result.commands.final_tick_count = std::max(result.commands.final_tick_count, entry.entry_tick + 1);
    for (const auto& entry : result.conflicts) result.commands.final_tick_count = std::max(result.commands.final_tick_count, entry.entry_tick + 1);
    for (const auto& entry : result.commands.setup.outcomes) result.commands.final_tick_count = std::max(result.commands.final_tick_count, entry.completed_tick);
    for (const auto& [tick, command] : impl_->replay_fixups) result.load_fixups.push_back(command);
    return core::Result<GameRecording>::success(std::move(result));
}
core::Result<void> GameSession::apply_load_fixup(const galactic::GameCommand& command) {
    if (command.key.tick != impl_->tick || !std::holds_alternative<galactic::PostLoadFixup>(command.payload))
        return fail<void>("load prologue needs the current session frontier");
    const std::array controls{command};
    // WGUI-102 / TR-01: active-mode frames and synchronized logical FPS, never
    // the retained parent's timer or either mode's wall-clock speed setting.
    const auto observer_clock = impl_->child
        ? std::optional(std::pair{impl_->child->session.world().completed_tick(), tactical::logical_frames_per_second})
        : std::nullopt;
    auto applied = impl_->world().apply_session_controls(controls, false, false, observer_clock);
    if (!applied) return applied;
    if (impl_->child) impl_->child->galaxy_hash = impl_->galaxy_hash().value();
    impl_->load_fixups.push_back(command);
    return core::Result<void>::success();
}
core::Result<void> GameSession::apply_restore_prologue() {
    while (!impl_->replay_fixups.empty() && impl_->replay_fixups.begin()->first == impl_->tick) {
        const auto command = impl_->replay_fixups.begin()->second;
        auto applied = apply_load_fixup(command);
        if (!applied) return applied;
        impl_->replay_fixups.erase(impl_->replay_fixups.begin());
    }
    return core::Result<void>::success();
}
const ConflictRecord* GameSession::conflict() const noexcept { return impl_->child ? &impl_->child->conflict : nullptr; }
const tactical::TacticalSession* GameSession::battle() const noexcept { return impl_->child ? &impl_->child->session.world() : nullptr; }
std::weak_ptr<const void> GameSession::battle_lifetime() const noexcept {
    return impl_->child ? std::weak_ptr<const void>(impl_->child->lifetime) : std::weak_ptr<const void>{};
}
std::optional<tactical::TacticalReplay> GameSession::battle_record() const {
    return impl_->child ? std::optional(impl_->child->session.record()) : impl_->last_battle;
}
core::Result<SeatBattleView> seat_battle_view(const GameTick& tick, const sim::SeatId id) {
    if (!tick.galaxy || !tick.battle) return fail<SeatBattleView>("seat has no battle view");
    const auto seats = tick.galaxy->seats();
    const auto seat = std::find_if(seats.begin(), seats.end(), [&](const auto& row) { return row.seat_id == id; });
    const auto binding = std::find_if(tick.battle_seats.begin(), tick.battle_seats.end(), [&](const auto& row) { return row.seat == id; });
    const auto& state = tick.galaxy->seat_control();
    if (seat == seats.end() || !seat->active || binding == tick.battle_seats.end()
        || (state && state->battle && state->battle->role == galactic::BattleTeamRole::pending))
        return fail<SeatBattleView>("seat has no ready battle view");
    SeatBattleView view; view.completed_tick = tick.battle->completed_tick();
    view.perspective = binding->player; view.commandable = view.perspective != 0;
    if (!view.commandable) {
        if (!state || !state->battle || seat->human_side_id != state->battle->choosing_side)
            return fail<SeatBattleView>("observer has no setup-approved perspective");
        view.perspective = state->battle->observer_player;
    }
    const auto players = tick.battle->players();
    const auto perspective = std::find_if(players.begin(), players.end(), [&](const auto& player) { return player.player_id == view.perspective; });
    if (perspective == players.end()) return fail<SeatBattleView>("battle perspective is absent");
    const auto mask = std::uint64_t{1} << static_cast<unsigned>(perspective - players.begin());
    for (const auto& instance : tick.battle->instances()) if ((instance.visible_to & mask) != 0) view.instances.push_back(instance);
    return core::Result<SeatBattleView>::success(std::move(view));
}
core::Result<SeatBattleView> GameSession::battle_view(const sim::SeatId id) const {
    if (!impl_->child) return fail<SeatBattleView>("seat has no battle view");
    GameTick tick; tick.galaxy = impl_->world().snapshot(); tick.battle = impl_->child->session.world().snapshot();
    tick.battle_seats = impl_->child->conflict.seats;
    return seat_battle_view(tick, id);
}
galactic::FactionControl GameSession::battle_controller(const galactic::PlayerId player) const noexcept {
    if (!impl_->child) return galactic::FactionControl::passive;
    for (const auto& binding : impl_->child->conflict.seats) {
        const auto* seat = impl_->seat(binding.seat);
        if (binding.player == player && seat && seat->active && seat->kind == galactic::SeatKind::human)
            return galactic::FactionControl::human;
    }
    const auto& state = impl_->world().seat_control();
    if (!state) return galactic::FactionControl::ai;
    const auto& controllers = state->control.controllers;
    const auto controller = std::find_if(controllers.begin(), controllers.end(), [&](const auto& row) { return row.player == player; });
    return controller == controllers.end() ? galactic::FactionControl::passive : controller->kind;
}
core::Result<std::optional<BattleCheckpoint>> GameSession::save_battle_checkpoint() const {
    using Save = std::optional<BattleCheckpoint>;
    if (impl_->failure) return core::Result<Save>::failure(*impl_->failure);
    if (!impl_->child) return core::Result<Save>::success({});
    auto state = impl_->child->session.world().save_checkpoint_state();
    if (!state) return core::Result<Save>::failure(state.error());
    auto scripts = impl_->child->session.save_scripts();
    if (!scripts) return core::Result<Save>::failure(scripts.error());
    return core::Result<Save>::success(BattleCheckpoint{std::move(state).value(), std::move(scripts).value(),
        impl_->child->sequences, impl_->child->killers, impl_->timelines->child->value,
        impl_->timelines->child->completed_tick});
}
core::Result<void> GameSession::restore_battle_checkpoint(const BattleCheckpoint& saved,
    const tactical::TacticalReplay& log, const std::uint64_t completed,
    const sim::PartitionExecutor& executor) {
    if (saved.timeline < 0 || saved.timeline > 5) return fail<void>("WGA-378: invalid saved child timeline");
    if (!impl_->child || impl_->child->session.completed_tick() != 0 || completed < impl_->tick
        || completed > galactic::galactic_max_ticks
        || completed - impl_->tick < saved.scripts.completed_tick || log.setup != impl_->child->conflict.tactical)
        return fail<void>("child checkpoint does not match its entry and shared barrier");
    // The timeline's serviced tick lags the parent across save-only and paused barriers: restore it, never derive it.
    if (!impl_->timelines->child || saved.timeline_tick < impl_->timelines->child->completed_tick || saved.timeline_tick > completed)
        return fail<void>("WGA-378: invalid saved child timeline tick");
    // Restore before child scheduler/native configuration can evaluate a token.
    impl_->timelines->child = ModeTimelineSnapshot{{SessionMode::battle, impl_->child->conflict.id}, saved.timeline_tick, saved.timeline};
    impl_->timelines->child_slot = std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl_->timelines->child, false});
    auto content = impl_->content_resolver(impl_->child->conflict);
    if (!content) return core::Result<void>::failure(content.error());
    auto world = tactical::TacticalSession::from_replay(log, content.value());
    if (!world) return core::Result<void>::failure(world.error());
    if (auto loaded = world.value().load_checkpoint_state(saved.state); !loaded) return loaded;
    if (world.value().completed_tick() != saved.scripts.completed_tick)
        return fail<void>("child state and host clocks differ");
    auto scripts = [&]() -> core::Result<authoritative::ScriptScheduler> {
        if (impl_->script_factory) return impl_->script_factory(impl_->child->conflict);
        authoritative::SessionConfig config; config.seed = child_seed(impl_->world().setup().seed, impl_->child->conflict.id);
        return authoritative::ScriptScheduler::create(config, {});
    }();
    if (!scripts) return core::Result<void>::failure(scripts.error());
    auto hosted = authoritative::ScriptedTacticalSession::restore(std::move(world).value(), std::move(scripts).value(), saved.scripts);
    if (!hosted) return core::Result<void>::failure(hosted.error());
    if (impl_->configure) {
        if (auto configured = impl_->configure(hosted.value()); !configured) return configured;
    }
    hosted.value().set_state_hasher(impl_->hasher);
    hosted.value().set_authoritative_hash(true);
    auto script_hash = hosted.value().script_state_hash();
    if (!script_hash) return core::Result<void>::failure(script_hash.error());
    // WGS-44/62/65/66: keep the parent and its controls, restore the child's barrier,
    // and retain attribution/cursors for the eventual ordinary return route.
    impl_->restoring_child = true;
    while (impl_->tick < completed) {
        auto stepped = step(executor);
        if (!stepped) { impl_->restoring_child = false; return core::Result<void>::failure(stepped.error()); }
    }
    impl_->restoring_child = false;
    if (impl_->child->sequences != saved.sequences) return fail<void>("child command cursors differ from recording");
    for (const auto& [id, killer] : saved.killers) {
        if (id == 0 || (killer != 0 && std::none_of(log.setup.players.begin(), log.setup.players.end(),
            [&](const auto& row) { return row.player_id == killer; }))) return fail<void>("invalid child death attribution");
    }
    hosted.value().set_shared_mode_service([owner = impl_.get()] { return owner->update_child_timeline(); });
    impl_->child->session = std::move(hosted).value();
    impl_->timelines->child = ModeTimelineSnapshot{{SessionMode::battle, impl_->child->conflict.id}, saved.timeline_tick, saved.timeline};
    impl_->timelines->child_slot->snapshot = *impl_->timelines->child;
    impl_->child->killers = saved.killers;
    impl_->child->script_hash = std::move(script_hash).value();
    impl_->child->world_hash = sim::StateHash(impl_->child->session.world().state_sha256());
    return core::Result<void>::success();
}
void GameSession::scramble_storage_for_testing() {
    impl_->world().scramble_storage_for_testing();
    if (impl_->child) impl_->child->session.scramble_storage_for_testing();
}
} // namespace eawr::campaign
