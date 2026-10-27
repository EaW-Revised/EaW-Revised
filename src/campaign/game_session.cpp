#include "game_session_impl.hpp"
#include "game_session_diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "battle_return_internal.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <span>
#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace eawr::campaign {
using game_session_detail::fail;
namespace galactic = sim::galactic;
namespace tactical = sim::tactical;
namespace authoritative = script::authoritative;
namespace {
std::string corruption_name(galactic::corruption_fixture::Type type) {
    const std::array<std::string_view, 9> names{"CORRUPTION_NONE", "CORRUPTION_INTIMIDATION", "CORRUPTION_PIRACY", "CORRUPTION_KIDNAPPING", "CORRUPTION_RACKETEERING", "CORRUPTION_CORRUPT_MILITIA", "CORRUPTION_BRIBERY", "CORRUPTION_BLACK_MARKET", "CORRUPTION_BONDED_CITIZENS"};
    return std::string(names[static_cast<std::size_t>(type)]);
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
namespace game_session_detail {
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
std::string combined_hash(const std::string& galaxy, const std::string& world,
    const std::string& scripts, const std::string& context) {
    // ADR-014: coordinator-reserved domain followed by four raw digests.
    constexpr std::string_view domain = "eawr-game-state-v1";
    std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
    for (const auto* digest : {&galaxy, &world, &scripts, &context}) put_digest(bytes, *digest);
    return core::sha256_hex(bytes);
}
} // namespace game_session_detail

GameSession::Impl::Impl(galactic::GalacticSession session) : galaxy(std::move(session)) {
    layer(BattleLayer::land).services.tactical_refusal = "land tactical provider is unavailable";
    // WGM-187: bind a stateless return writer for automatic and future land children.
    layer(BattleLayer::land).services.apply = [](auto& world, const auto& conflict, const auto& result) {
        return world.fix_tactical_built_structures(conflict.planet, result.winner);
    };
    galaxy.defer_victory_tail(galaxy.setup().victory.has_value());
    // A multiplayer observer is local presentation, never synchronized policy.
    const auto& victory = galaxy.victory();
    interactive_seat = victory && victory->context.multiplayer ? galactic::SeatId{} : galaxy.setup().interactive_seat;
}

core::Result<void> GameSession::Impl::update_galaxy_timeline(const std::optional<galactic::PlayerEconomySetup>& inputs) {
    // Missing tech owner is a named seam, never an invented zero input list.
    if (!inputs) return core::Result<void>::success();
    std::int64_t value{};
    for (const auto& row : inputs->players) value = std::max(value, row.current_tech);
    if (value > 5) return fail<void>("WGA-378: timeline maximum outside 0..5");
    timelines->galaxy.value = static_cast<std::int32_t>(value);
    timelines->galaxy.completed_tick = tick + 1;
    return core::Result<void>::success();
}

core::Result<void> GameSession::Impl::update_child_timeline() {
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

std::string GameSession::Impl::with_timeline(const std::string& base, const TimelinePublication& publication, bool ai_sync) {
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

core::Result<void> GameSession::Impl::story_notice(GameStoryNotice notice) {
    story_notices.push_back(std::move(notice));
    // The plot engine rebuilds its state from the same notices when a session is reconstructed, so it
    // sees them even while the UI handler is held back.
    if (story_engine) {
        const auto& stored = story_notices.back();
        if (auto told = story_engine->notify(stored.player, stored.payload); !told) return told;
        if (reconstructing_native_notices) static_cast<void>(story_engine->take_script_calls()); // the scripts are restored, not called again
    }
    return story_handler && !reconstructing_native_notices ? story_handler(story_notices.back()) : core::Result<void>::success();
}

// The engine's queue is applied only here, at the end of the galaxy step's story service, never from inside a notice
// (the world is mid-service then). A reward can notify the engine again (a tech change), so the queue is drained until
// it is empty. A record already refused, or run by the engine itself, is only published.
core::Result<void> GameSession::Impl::apply_story_rewards() {
    constexpr unsigned max_rounds = 64;
    for (unsigned round = 0; round < max_rounds; ++round) {
        if (auto forwarded = forward_story_calls(); !forwarded) return forwarded;
        auto batch = story_engine->take_triggered();
        if (batch.empty()) return core::Result<void>::success();
        const bool replaying = world().replaying_native_effects();
        for (auto& record : batch) {
            bool declined = false;
            if (record.engine_applied) record.outcome = "applied by the engine";
            else if (!record.refusal.empty()) record.outcome = "refused: " + record.refusal; // already diagnosed by the engine
            else if (record.reward_type.empty()) record.outcome = "no reward";
            else if (replaying) record.outcome = "replayed from the native journal";
            else if (!story_rewards || !executor) { record.outcome = "refused: no reward provider is bound"; declined = true; }
            else {
                auto applied = story_rewards(world(), record, *executor);
                if (!applied) return core::Result<void>::failure(applied.error());
                record.outcome = applied.value().applied ? "applied" : "refused: " + applied.value().note;
                declined = !applied.value().applied;
            }
            if (declined) { // once per reward type and reason, not once per event
                auto text = record.reward_type + " not applied: " + record.outcome.substr(std::string_view("refused: ").size());
                if (story_reported.insert(text).second) story_notes.push_back(std::move(text));
            }
            if (!record.dialog.empty()) apply_story_dialog(record); // WST-20 (6): the dialog follows the reward
            story_triggers.push_back(std::move(record));
        }
    }
    story_notes.push_back("reward chain exceeds " + std::to_string(max_rounds) + " rounds; the rest waits for the next step");
    return core::Result<void>::success();
}

namespace {
class StoryPresentationQueue final : public galactic::story::PresentationSink {
public:
    explicit StoryPresentationQueue(std::vector<galactic::story::PresentationEvent>& events) : events_(events) {}
    void receive(const galactic::story::PresentationEvent& event) override { events_.push_back(event); }
private:
    std::vector<galactic::story::PresentationEvent>& events_;
};
} // namespace

// WST-58: a triggered event with a Story_Dialog adds a holocron goal for the plot's owner. Whether the summary is
// already open is the local renderer's fact, so quiet requests defer refresh versus flash to that local state.
// WST-70 quiet advisor/history and mission-sound providers/sinks are not wired: see goals_policy_seam.
void GameSession::Impl::apply_story_dialog(const galactic::story::Triggered& record) {
    namespace st = galactic::story;
    auto at = story_goals.try_emplace(record.owner, record.owner).first;
    StoryPresentationQueue queue(story_presentation);
    st::LocalPresentation local; local.defer_summary_state = true;
    static_cast<void>(at->second.dialog(st::Dialog{record.dialog, record.variable, record.tag, record.chapter, record.popup, record.sound,
        record.remove_after_viewed}, local, queue));
}

core::Result<void> GameSession::Impl::forward_story_calls() {
    auto calls = story_engine->take_script_calls();
    if (calls.empty() || reconstructing_native_notices || !story_scripts || !scripts) return core::Result<void>::success();
    return forward_galactic_story_calls(scripts->story_scheduler(), *story_scripts, story_engine->definitions(), calls, story_call_sequence);
}

// WGEP-130/147: the assigned level reaches the story before the unlocks it causes. Not a UI notice.
core::Result<void> GameSession::Impl::tech_assigned(galactic::PlayerId player, std::int64_t level) {
    if (!story_engine) return core::Result<void>::success();
    galactic::story::EconomyNotice notice;
    notice.kind = galactic::story::Economy::tech_level; notice.value = level; notice.object_owner = player;
    return story_engine->notify(player, notice);
}

// GFS-15: the plot story is serviced once the hosted AI/story work of a galaxy step is done, with the
// mode time before the frame timer advances. The tech levels set up before the first service reach
// the plots first (WGS-140: plots load before the authored forces, credits and tech).
core::Result<void> GameSession::Impl::service_story() {
    if (!story_engine) return core::Result<void>::success();
    if (!story_engine->started()) {
        if (const auto& economy = world().snapshot()->player_economy())
            for (const auto& row : economy->players)
                if (auto told = tech_assigned(row.player, row.current_tech); !told) return told;
        story_engine->mark_started();
    }
    const auto& clock = world().clock();
    auto time = sim::math::Fixed::from_ratio(static_cast<std::int64_t>(clock.galactic_frame), galactic::galactic_frames_per_second);
    if (!time) return core::Result<void>::failure(time.error());
    if (auto served = story_engine->service(time.value(), clock.pause_depth != 0); !served) return served;
    return apply_story_rewards();
}

std::string GameSession::Impl::with_story(const std::string& base, const std::string& story) {
    if (story.empty()) return base;
    constexpr std::string_view domain = "eawr-game-story-v1";
    std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
    put_digest(bytes, base); put_digest(bytes, story); return core::sha256_hex(bytes);
}

core::Result<void> GameSession::Impl::control_notices(std::span<const galactic::PoliticalControlEffect> effects,
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

core::Result<void> GameSession::Impl::battle_story(galactic::PlanetId planet, BattleLayer layer,
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

core::Result<void> GameSession::Impl::battle_notice(GalacticAiBattleKind kind, const ConflictRecord& conflict,
        const BattleResult* result, bool automatic) {
    if (!native_ai.battle) return core::Result<void>::success();
    const auto snapshot = world().snapshot();
    return native_ai.battle({kind, tick, conflict, *snapshot, result, automatic});
}

core::Result<std::string> GameSession::Impl::native_hash() const {
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

std::string GameSession::Impl::with_native(const std::string& base, const std::string& native) {
    if (native.empty()) return base;
    constexpr std::string_view domain = "eawr-game-native-ai-v1";
    std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
    put_digest(bytes, base); put_digest(bytes, native); return core::sha256_hex(bytes);
}

const galactic::GalacticSession& GameSession::Impl::world() const { return scripts ? scripts->world() : galaxy; }

galactic::GalacticSession& GameSession::Impl::world() { return scripts ? scripts->parent_world() : galaxy; }

core::Result<std::string> GameSession::Impl::galaxy_hash() {
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

std::string GameSession::Impl::Child::context_hash() const {
    std::vector<std::uint8_t> bytes;
    put_digest(bytes, conflict_hash);
    put(bytes, sequences.size(), 8);
    for (const auto& [player, sequence] : sequences) { put(bytes, player, 8); put(bytes, sequence, 8); }
    put(bytes, killers.size(), 8);
    for (const auto& [id, killer] : killers) { put(bytes, id, 8); put(bytes, killer, 8); }
    if (owns_pause) put(bytes, 1, 1);
    if (conflict.tactical.controllers) {
        put(bytes, 0x4c525443U, 4); put(bytes, 4, 4); put(bytes, controller_inputs.size(), 8);
        for (const auto& input : controller_inputs) {
            put(bytes, input.outer.tick, 8); put(bytes, input.outer.seat.value, 4); put(bytes, input.outer.sequence, 8);
            put(bytes, input.producer_sequence, 8); put(bytes, input.body.size(), 8);
            bytes.insert(bytes.end(), input.body.begin(), input.body.end());
        }
    }
    return core::sha256_hex(bytes);
}

GameSession::Impl::Layer& GameSession::Impl::layer(BattleLayer value) { return layers[value == BattleLayer::land ? 1 : 0]; }

const GameSession::Impl::Layer& GameSession::Impl::layer(BattleLayer value) const { return layers[value == BattleLayer::land ? 1 : 0]; }

bool GameSession::Impl::assembling_layers() {
    if (std::none_of(layers.begin(), layers.end(), [](const auto& value) { return value.installed; })) return true;
    failure = fail<void>("conflict layer assembly cannot replace installed services").error();
    return false;
}

GameSession::ConflictLayerServices& GameSession::Impl::services(const ConflictRecord& value) { return layer(value.layer).services; }

core::Result<bool> GameSession::Impl::tactical_selection(const ConflictRecord& record, bool requested, bool forced) const {
    const auto& refusal = layer(record.layer).services.tactical_refusal;
    if (!requested || refusal.empty()) return core::Result<bool>::success(requested);
    if (forced) return fail<bool>(refusal, unavailable_conflict_tactical);
    return core::Result<bool>::success(false);
}

core::Result<authoritative::ScriptScheduler> GameSession::Impl::scripts_for(const ConflictRecord& record,
        ScriptRequirement requirement) {
    if (auto allowed = tactical_selection(record, true, true); !allowed)
        return core::Result<authoritative::ScriptScheduler>::failure(allowed.error());
    const auto& service = services(record);
    if (service.scripts) return service.scripts(record);
    if ((layer(record.layer).installed || requirement == ScriptRequirement::required)
        && service.script_requirement == ScriptRequirement::required)
        return fail<authoritative::ScriptScheduler>("conflict layer requires a script factory", missing_conflict_scripts);
    authoritative::SessionConfig config; config.seed = child_seed(world().setup().seed, record.id);
    return authoritative::ScriptScheduler::create(config, {});
}

void GameSession::Impl::refresh_system_hold() {
    world().set_system_hold(conflict_pause || summary_conflict != 0 || child.has_value());
}

core::Result<void> GameSession::Impl::release_summary() {
    if (!summary_conflict) return core::Result<void>::success();
    auto released = world().adjust_conflict_pause(false);
    if (released) { summary_conflict = 0; refresh_system_hold(); }
    return released;
}

core::Result<void> GameSession::Impl::hold_summary(sim::EntityId conflict, galactic::PlayerId invader, galactic::PlayerId defender) {
    // WGP-81/89: the modal belongs to the involved solo observer, in either battle mode.
    const auto* observer = seat(interactive_seat);
    if (!observer || !observer->active || observer->kind != galactic::SeatKind::human
        || (observer->player != invader && observer->player != defender)
        || (world().victory() && world().victory()->context.multiplayer)
        || admission || staged || child || !waiting_admissions.empty()) return core::Result<void>::success();
    if (summary_conflict == conflict) return core::Result<void>::success();
    if (auto released = release_summary(); !released) return released;
    auto held = world().adjust_conflict_pause(true);
    if (held) { summary_conflict = conflict; refresh_system_hold(); }
    return held;
}

std::string GameSession::Impl::with_summary(const std::string& base, galactic::SeatId observer, sim::EntityId conflict) {
    if (!observer.value) return base;
    std::vector<std::uint8_t> bytes;
    constexpr std::string_view domain = "eawr-summary-hold-v1";
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    put_digest(bytes, base); put(bytes, observer.value, 4); put(bytes, conflict, 8);
    return core::sha256_hex(bytes);
}

const galactic::Seat* GameSession::Impl::seat(const galactic::SeatId id) const {
    const auto seats = world().seats();
    const auto found = std::lower_bound(seats.begin(), seats.end(), id,
        [](const auto& value, const auto key) { return value.seat_id < key; });
    return found == seats.end() || found->seat_id != id ? nullptr : &*found;
}
bool GameSession::Impl::human_side(const galactic::PlayerId owner) const {
    const auto seats = world().seats();
    return std::any_of(seats.begin(), seats.end(), [&](const auto& seat) {
        return seat.active && seat.kind == galactic::SeatKind::human && seat.player == owner;
    });
}

tactical::PlayerId GameSession::Impl::player(const galactic::SeatId id) const {
    if (!child) return 0;
    const auto& seats = child->conflict.seats;
    const auto found = std::lower_bound(seats.begin(), seats.end(), id,
        [](const auto& value, const auto key) { return value.seat < key; });
    return found == seats.end() || found->seat != id ? 0 : found->player;
}

tactical::CampaignReserveAdmission GameSession::Impl::reserve_admission(const tactical::PlayerId owner) const {
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

core::Result<void> GameSession::Impl::refresh_battle_seats(const bool initial_policy) {
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
    if (initial_policy && record.tactical.controllers && !restoring_child && !role_pending()
        && child->session.completed_tick() == 0 && child->controller_inputs.empty()) {
        auto policy = *record.tactical.controllers;
        const auto previous = policy.seats;
        policy.seats.clear();
        // ADR-020: only the agreed effective battle mapping chooses controllers.
        // Retain inactive bindings so a spectator cannot erase the opt-in roster.
        for (const auto& source : world().seats()) {
            const auto effective = std::find_if(record.seats.begin(), record.seats.end(),
                [&](const auto& row) { return row.seat == source.seat_id; });
            const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& row) { return row.seat == source.seat_id; });
            const bool active = source.active && effective != record.seats.end() && effective->player != 0;
            const auto owner = active ? effective->player : old != previous.end() ? old->player : source.player;
            if ((source.kind == sim::SeatKind::human || source.kind == sim::SeatKind::ai)
                && std::any_of(policy.factions.begin(), policy.factions.end(), [&](const auto& row) { return row.player == owner; }))
                policy.seats.push_back({source.seat_id, owner, source.kind, active});
        }
        for (auto& faction : policy.factions) {
            const bool human = std::any_of(policy.seats.begin(), policy.seats.end(), [&](const auto& row) {
                return row.player == faction.player && row.active && row.kind == sim::SeatKind::human;
            });
            if (human) faction.mode = tactical::FactionController::humans;
            else {
                const auto fallback = std::find_if(state.control.controllers.begin(), state.control.controllers.end(),
                    [&](const auto& row) { return row.player == faction.player; });
                faction.mode = fallback != state.control.controllers.end() && fallback->dropout == galactic::FactionControl::passive
                    ? tactical::FactionController::passive : tactical::FactionController::ai;
            }
            for (auto& binding : policy.seats) if (binding.player == faction.player && binding.kind == sim::SeatKind::ai) {
                const auto* source = seat(binding.seat);
                binding.active = !human && source && source->active;
            }
        }
        if (policy != *record.tactical.controllers) {
            if (auto installed = child->session.set_initial_controller_policy(std::move(policy)); !installed) return installed;
            record.tactical = child->session.world().record().setup;
            child->world_hash = sim::StateHash(child->session.world().state_sha256());
        }
    }
    auto digest = conflict_hash(record);
    if (!digest) return core::Result<void>::failure(digest.error());
    child->conflict_hash = std::move(digest).value();
    return core::Result<void>::success();
}

bool GameSession::Impl::role_pending() const {
    const auto& state = world().seat_control();
    return state && state->battle && state->battle->role == galactic::BattleTeamRole::pending;
}

std::string GameSession::Impl::queued_context(std::string digest) const {
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

std::string GameSession::Impl::child_context_hash() const {
    if (!world().seat_control()) return queued_context(child->context_hash());
    std::vector<std::uint8_t> bytes;
    put_digest(bytes, child->context_hash()); put(bytes, tick, 8);
    return queued_context(core::sha256_hex(bytes));
}

core::Result<std::string> GameSession::Impl::base_hash() {
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
        if (multiplayer_vote_timeout) {
            put(bytes, *multiplayer_vote_timeout, 8);
            put(bytes, vote_started.has_value() ? 1 : 0, 1);
            if (vote_started) put(bytes, *vote_started, 8);
            put(bytes, tick, 8);
        }
        return core::Result<std::string>::success(queued_context(core::sha256_hex(bytes)));
    }
    return core::Result<std::string>::success(combined_hash(child->galaxy_hash, child->world_hash.get(), child->script_hash, child_context_hash()));
}

core::Result<std::string> GameSession::Impl::hash() {
    auto base = base_hash(); if (!base) return base;
    auto native = native_hash(); if (!native) return native;
    const auto native_hashed = with_native(base.value(), native.value());
    const auto story = story_engine ? with_story(native_hashed, story_engine->digest()) : native_hashed;
    return core::Result<std::string>::success(with_timeline(with_summary(story, interactive_seat, summary_conflict), *timelines, bool(native_ai.state)));
}

BattleResult GameSession::Impl::result() const {
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
    // WGCH-108: successful ground spy insertion notifies story before consumption.
    impl_->world().set_ground_ability_story_handler([owner = impl_.get()](galactic::PlayerId player, galactic::PlanetId planet,
        const std::string& hero, bool hero_source) -> core::Result<void> {
        const auto name = owner->story_planets.find(planet);
        galactic::story::TravelNotice notice; notice.kind = galactic::story::Travel::land;
        notice.planet = name == owner->story_planets.end() ? std::string{} : name->second;
        notice.fleet.owner = player; notice.fleet.types = {hero};
        if (hero_source) { notice.fleet.heroes = {hero}; notice.hero = hero; }
        // WST-31: land-on first, then deploy for an authored named/generic hero.
        if (const auto told = owner->story_notice({player, notice, 0, 0, planet}); !told) return told;
        if (!hero_source) return core::Result<void>::success();
        notice.kind = galactic::story::Travel::deploy_hero;
        return owner->story_notice({player, std::move(notice), 0, 0, planet});
    });
    // WGCH-140: announce destruction before cleanup or flag writes.
    impl_->world().set_planet_ruins_story_handler([owner = impl_.get()](galactic::PlanetId planet) -> core::Result<void> {
        const auto name = owner->story_planets.find(planet);
        galactic::story::TravelNotice notice;
        notice.kind = galactic::story::Travel::planet_destroyed;
        notice.planet = name == owner->story_planets.end() ? std::string{} : name->second;
        return owner->story_notice({std::nullopt, std::move(notice), 0, 0, planet});
    });
    impl_->world().set_technology_notice_handler([owner = impl_.get()](const auto& notice) -> core::Result<void> {
        if (notice.kind == galactic::GalacticSession::TechnologyNotice::Kind::assigned)
            if (auto told = owner->tech_assigned(notice.player, notice.tech); !told) return told;
        return owner->technology_handler ? owner->technology_handler(notice) : core::Result<void>::success();
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
    auto galaxy = [&] {
        if (setup.interactive_seat.value && setup.victory && setup.victory->context.multiplayer) {
            auto canonical = setup; canonical.interactive_seat = {};
            return galactic::GalacticSession::create(canonical);
        }
        return galactic::GalacticSession::create(setup);
    }();
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
    auto host = saved ? authoritative::ScriptedGalacticSession::restore_preserving(
                            impl_->galaxy, std::move(ai), std::move(story), *saved,
                            impl_->conflict_pause || impl_->summary_conflict != 0 || impl_->child.has_value() || impl_->staged.has_value())
                      : authoritative::ScriptedGalacticSession::create(std::move(fresh).value(), std::move(ai), std::move(story));
    if (!host) return core::Result<void>::failure(host.error());
    if (!saved) host.value().parent_world() = std::move(impl_->galaxy);
    // WGP-146: bind service ownership to the adopted world as well as the shell.
    // The hosted barrier records History after AI; the world step must defer it.
    host.value().parent_world().defer_history_service(true);
    host.value().set_state_hasher(impl_->hasher);
    impl_->scripts.emplace(std::move(host).value());
    impl_->refresh_system_hold();
    impl_->world().enable_conflict_requests(impl_->conflict_services);
    impl_->world().defer_victory_tail(impl_->world().setup().victory.has_value());
    if (impl_->child) impl_->child->galaxy_hash = impl_->galaxy_hash().value();
    return core::Result<void>::success();
}
void GameSession::bind_story_scripts(std::shared_ptr<const GalacticStoryScripts> scripts) noexcept {
    impl_->story_scripts = std::move(scripts);
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
void GameSession::set_entry_resolver(EntryResolver resolver) { if (impl_->assembling_layers()) impl_->layer(BattleLayer::space).services.entry = std::move(resolver); }
void GameSession::set_result_applier(ResultApplier applier) {
    if (!impl_->assembling_layers()) return;
    auto& target = impl_->layer(BattleLayer::space).services.apply;
    target = {};
    if (applier) target = [applier = std::move(applier)](auto& world, const auto&, const auto& result) {
        return applier(world, result);
    };
}
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
void GameSession::set_live_hero_returns(LiveHeroReturns services) { impl_->live_hero_returns = std::move(services); }
void GameSession::set_story_rewards(StoryRewardApplier applier) { impl_->story_rewards = std::move(applier); }
const TacticalReturnState& GameSession::return_state() const noexcept { return impl_->return_state; }
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
core::Result<void> GameSession::set_interactive_seat(galactic::SeatId observer) {
    if (!observer.value && !impl_->interactive_seat.value) return core::Result<void>::success();
    const auto* seat = impl_->seat(observer);
    if (!seat || !seat->active || seat->kind != galactic::SeatKind::human)
        return fail<void>("interactive observer requires a fresh active human seat");
    const auto& victory = impl_->world().victory();
    const auto policy = victory && victory->context.multiplayer ? galactic::SeatId{} : observer;
    if (policy == impl_->interactive_seat) return core::Result<void>::success();
    if (impl_->tick || impl_->summary_conflict)
        return fail<void>("interactive observer requires a fresh active human seat");
    impl_->interactive_seat = policy;
    return core::Result<void>::success();
}
sim::EntityId GameSession::battle_summary_hold() const noexcept { return impl_->summary_conflict; }
const BattleResult* GameSession::latest_tactical_result() const noexcept { return impl_->latest_result ? &*impl_->latest_result : nullptr; }
const ConflictRecord* GameSession::latest_tactical_context() const noexcept { return impl_->latest_context ? &*impl_->latest_context : nullptr; }
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
void GameSession::set_child_configurator(ChildConfigurator configure) { if (impl_->assembling_layers()) impl_->layer(BattleLayer::space).services.configure = std::move(configure); }
core::Result<void> GameSession::set_child_script_services(ScriptFactory scripts, ChildConfigurator configure) {
    return set_child_script_services(BattleLayer::space, std::move(scripts), std::move(configure));
}
core::Result<void> GameSession::set_child_script_services(BattleLayer layer, ScriptFactory scripts, ChildConfigurator configure) {
    if (impl_->child) return fail<void>("cannot replace child script services while a tactical child is active");
    if (!scripts || !configure) return fail<void>("child script services require a paired factory and configurator");
    auto& target = impl_->layer(layer);
    if (target.installed || impl_->staged || impl_->failure) return fail<void>("child script assembly cannot replace installed layer services");
    target.services.scripts = std::move(scripts); target.services.configure = std::move(configure);
    return core::Result<void>::success();
}
void GameSession::set_native_conflict_service(NativeConflictService service) {
    if (!impl_->assembling_layers()) return;
    impl_->layer(BattleLayer::land).services.native_admission = std::move(service);
    impl_->conflict_services = impl_->conflict_services || static_cast<bool>(impl_->layer(BattleLayer::land).services.native_admission);
    impl_->world().enable_native_invasions(static_cast<bool>(impl_->layer(BattleLayer::land).services.native_admission));
    impl_->world().enable_conflict_requests(impl_->conflict_services);
}
bool GameSession::has_pending_conflict_for_admission() const noexcept { return impl_->staged || impl_->admission; }
GameSession::NativeConflictReaders GameSession::native_conflict_readers() const {
    return {[owner = impl_.get()] { return owner->staged || owner->admission; },
        [owner = impl_.get()] { return owner->child || owner->staged || owner->admission || !owner->queued_admissions.empty()
            || !owner->waiting_admissions.empty() || owner->world().pending_native_invasion(); }};
}
void GameSession::set_conflict_service(ConflictService service, ContentResolver content, ScriptFactory scripts, ScriptRequirement requirement) {
    if (!impl_->assembling_layers()) return;
    for (auto& layer : impl_->layers) {
        layer.services.admission = service;
    }
    auto& space = impl_->layer(BattleLayer::space).services;
    space.content = std::move(content); space.scripts = std::move(scripts); space.script_requirement = requirement;
    impl_->conflict_services = impl_->conflict_services || static_cast<bool>(service);
    impl_->world().enable_conflict_requests(impl_->conflict_services);
    impl_->world().defer_victory_tail(impl_->world().setup().victory.has_value());
}
core::Result<void> GameSession::install_conflict_layer(BattleLayer layer, ConflictLayerServices services) {
    auto& target = impl_->layer(layer);
    if (target.installed || impl_->child || impl_->staged || impl_->failure)
        return fail<void>("conflict layer services need one install before child construction");
    // Checkpoint assembly can supply an optional return consumer before the stock installer.
    if (!services.apply) services.apply = std::move(target.services.apply);
    target.services = std::move(services);
    target.installed = true;
    impl_->conflict_services = impl_->conflict_services || static_cast<bool>(target.services.admission)
        || static_cast<bool>(target.services.native_admission);
    impl_->world().enable_conflict_requests(impl_->conflict_services);
    impl_->world().enable_native_invasions(static_cast<bool>(impl_->layer(BattleLayer::land).services.native_admission));
    impl_->world().defer_victory_tail(impl_->world().setup().victory.has_value());
    return core::Result<void>::success();
}
core::Result<void> GameSession::add_conflict_parent_queries(BattleLayer layer, GalacticParentQueryFactory queries) {
    auto& target = impl_->layer(layer);
    if (!queries || target.queries_installed || impl_->child || impl_->staged)
        return fail<void>("conflict parent queries need one binding before child construction");
    *target.parent_queries = std::move(queries);
    target.queries_installed = true;
    return core::Result<void>::success();
}
bool GameSession::has_conflict_parent_queries(BattleLayer layer) const noexcept {
    return impl_->layer(layer).queries_installed;
}
GalacticParentQueryFactory GameSession::conflict_parent_queries(BattleLayer layer) const {
    return [queries = impl_->layer(layer).parent_queries](const ConflictRecord& record) {
        return *queries ? (*queries)(record) : GalacticParentQuery{};
    };
}
void GameSession::set_auto_resolver(AutoResolver resolve) { impl_->auto_resolver = std::move(resolve); }
void GameSession::set_auto_resolve_checkpoint_services(AutoResolveStateWriter writer, AutoResolveStateReader reader) {
    impl_->auto_state_writer = std::move(writer); impl_->auto_state_reader = std::move(reader);
}
void GameSession::set_vote_progress(VoteProgress progress) { impl_->vote_progress = std::move(progress); }
core::Result<void> GameSession::configure_multiplayer_votes(std::uint64_t timeout_ticks) {
    if (!impl_->world().multiplayer() || impl_->tick || timeout_ticks >= galactic::galactic_max_ticks)
        return fail<void>("multiplayer voting needs an authored timeout at campaign creation");
    impl_->multiplayer_vote_timeout = timeout_ticks;
    set_vote_progress([impl = impl_.get()](const ConflictRecord& conflict, const auto& votes) {
        // GCMP-06 policy: either refusal selects auto-resolve; every voting side saying yes selects Begin.
        // Only sides with an active human seat vote (a human fighting the AI is a one-side vote).
        std::optional<bool> selected{true};
        for (const auto owner : {conflict.invader, conflict.defender}) {
            if (!impl->human_side(owner)) continue;
            const auto vote = votes.find(owner);
            if (vote != votes.end() && !vote->second) return core::Result<std::optional<bool>>::success(std::optional<bool>{false});
            if (vote == votes.end()) selected.reset();
        }
        return core::Result<std::optional<bool>>::success(selected);
    });
    return core::Result<void>::success();
}
void GameSession::set_conflict_gate(ConflictGate gate) { impl_->conflict_gate = std::move(gate); }
const ConflictRecord* GameSession::pending_conflict() const noexcept { return impl_->admission ? &impl_->admission->conflict : nullptr; }
std::optional<PendingConflictView> GameSession::pending_conflict_view(galactic::SeatId observer) const {
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
    if (impl_->multiplayer_vote_timeout) {
        view.alert.reset();
        const auto* seat = impl_->seat(observer);
        if (seat && seat->active && seat->kind == galactic::SeatKind::human
            && (seat->player == view.conflict.invader || seat->player == view.conflict.defender)) {
            PendingBattleAlertInputs inputs;
            inputs.local = seat->player; inputs.active_mode = inputs.queue_tactical = inputs.multiplayer = inputs.authorized_issuer = true;
            inputs.always_bypass = view.arbitration.forced_tactical;
            inputs.timeout_ms = *impl_->multiplayer_vote_timeout * 1000 / galactic::galactic_frames_per_second;
            inputs.elapsed_wall_ms = impl_->vote_started ? (impl_->tick - *impl_->vote_started) * 1000 / galactic::galactic_frames_per_second : 0;
            const auto vote = impl_->votes.find(seat->player);
            view.alert = pending_battle_alert_view(view.conflict, inputs, choice_ready,
                vote == impl_->votes.end() ? std::optional<bool>{} : vote->second);
            // The canonical session service owns MP defaults. Presentation never injects a second local timeout.
            view.alert->timeout_due = false;
        }
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
    impl_->technology_handler = std::move(handler); // the session's own world handler forwards to it
}
core::Result<void> GameSession::install_story(galactic::story::Config config, std::vector<galactic::story::PlotRecord> plots,
    std::vector<galactic::story::SubPlotDef> subplots, std::vector<std::string> notes) {
    if (impl_->failure || impl_->story_engine || impl_->tick != 0)
        return fail<void>("install the story plots once, before the first step");
    if (subplots.empty()) return core::Result<void>::success(); // no plot, no engine: hashes stay as without a story
    auto engine = galactic::story::Engine::create(config, std::move(plots), std::move(subplots));
    if (!engine) return core::Result<void>::failure(engine.error());
    impl_->story_engine.emplace(std::move(engine).value());
    // One line per loader note and engine refusal; refused or evaluator-less events are counted per kind and reason.
    impl_->story_notes = std::move(notes);
    for (const auto& row : impl_->story_engine->diagnostics())
        impl_->story_notes.push_back(row.subplot + " / " + row.event + ": " + row.what + ": " + row.reason);
    std::map<std::string, std::size_t> refused;
    for (const auto& row : impl_->story_engine->unavailable()) ++refused[row.what + ": " + row.reason];
    for (const auto& [text, count] : refused) impl_->story_notes.push_back(std::to_string(count) + " not run, " + text);
    return core::Result<void>::success();
}
const galactic::story::GoalsState* GameSession::story_goals(const galactic::story::Player owner) const noexcept {
    const auto at = impl_->story_goals.find(owner);
    return at == impl_->story_goals.end() ? nullptr : &at->second.state();
}
const galactic::story::Engine* GameSession::story_engine() const noexcept {
    return impl_->story_engine ? &*impl_->story_engine : nullptr;
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
core::Result<sim::galactic::GalacticSession::ProductionLockReceipt> GameSession::apply_production_lock(
    const sim::galactic::GalacticSession::ProductionLockWrite& write) {
    return impl_->world().apply_production_lock(write);
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
    auto setup = impl_->world().setup(); setup.interactive_seat = impl_->interactive_seat;
    // The replay format numbers its journal from 1; a bounded session's window follows the dropped entries (#4058).
    std::vector<galactic::NativeEffect> window(effects.begin(), effects.end());
    for (auto& effect : window) effect.sequence -= impl_->world().native_dropped();
    return {std::move(setup), impl_->tick, std::move(commands), std::move(window)};
}
GameRecording GameSession::recording() const {
    return {record(), impl_->conflicts, impl_->load_fixups, impl_->admissions, impl_->conflict_services, impl_->history,
        impl_->multiplayer_vote_timeout};
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
    if (const auto& policy = impl_->child->session.world().controller_policy(); policy) {
        for (const auto& faction : policy->factions) if (faction.player == player)
            return faction.mode == tactical::FactionController::humans ? galactic::FactionControl::human
                : faction.mode == tactical::FactionController::ai ? galactic::FactionControl::ai : galactic::FactionControl::passive;
    }
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
void GameSession::scramble_storage_for_testing() {
    impl_->world().scramble_storage_for_testing();
    if (impl_->child) impl_->child->session.scramble_storage_for_testing();
}
} // namespace eawr::campaign
