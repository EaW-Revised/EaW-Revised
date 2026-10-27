#pragma once

#include "../support/result.hpp"
#include "../support/check.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "ai_campaign_gate_access.hpp"
#include "ai_unattended_input.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace galactic_campaign_gate {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace ga = eawr::script::foc::galactic_ai;
namespace ck = eawr::checkpoint;
template<class T> inline T required(eawr::core::Result<T> result) {
    return eawr::testing::result_code_space::take(std::move(result));
}
inline void check(bool value, std::string_view message) {
    if (!value) eawr::testing::require(value, message);
}
using galactic_campaign_input::unattended_human_input;
inline void write_human_input(std::ostream& output, const gc::GameCommand& command) {
    const auto* choice = std::get_if<gc::BattleChoice>(&command.payload);
    const auto conflict = choice ? choice->conflict : std::get<gc::BattleSummaryAcknowledge>(command.payload).conflict;
    output << "{\"tick\":" << command.key.tick << ",\"seat\":" << command.key.seat.value
        << ",\"sequence\":" << command.key.sequence << ",\"conflict\":" << conflict
        << ",\"opcode\":" << gc::opcode_of(command.payload)
        << ",\"command\":\"" << (choice ? (choice->tactical ? "BeginTactical" : "AutoResolve") : "BattleSummaryAcknowledge") << "\"}" << std::endl;
}
struct PlayerProgress {
    std::uint64_t builds_started{}, builds_completed{}, expansions{}, attacks{}, defences{}, heroes{};
    std::uint64_t movements{};
    std::int64_t maximum_tech{};
    std::uint64_t first_tech3_frame{};
};
// Read the retained action rows only when reporting progress; this is not a
// gameplay observation or part of the continuation/digest comparison.
inline void sample_movements(std::map<gc::PlayerId, PlayerProgress>& players,
    const ga::NativeActions& actions) {
    for (auto& [id, player] : players) {
        static_cast<void>(id); player.movements = 0;
    }
    for (const auto& [request, movement] : actions.movements()) {
        static_cast<void>(request);
        const auto player = players.find(movement.player);
        if (player != players.end()) ++player->second.movements;
    }
}
inline void json_string(std::ostream& output, std::string_view value);
inline void sample_targeted_plans(const cp::GalacticAiRuntimeView& view,
    const cp::GalacticAiRuntimeSetup& setup, std::uint64_t frame, std::ostream& output,
    std::set<gc::PlayerId>& witnessed) {
    const auto engine = view.engine.lock(); const auto world = view.world.lock();
    if (!engine || !world) return;
    for (const auto& selected : setup.lifecycle.players) {
        if (witnessed.contains(selected.player)) continue;
        for (const auto& goal : engine->galactic_goals(selected.player)) {
            if (!goal.target || !goal.running_plan) continue;
            const auto definition = std::find_if(engine->plans().begin(), engine->plans().end(),
                [&](const auto& plan) { return plan.name == goal.definition; });
            if (definition == engine->plans().end() || definition->magic || definition->ignore_target
                || !definition->galactic_contrast_qualified || definition->galactic_contrast_order.empty()) continue;
            const auto* target = engine->target(goal.target); if (!target) continue;
            // Read already-published classified perception rows. Calling the
            // ordinary force getter here could refresh gameplay-owned caches.
            const auto perception = world->state();
            std::vector<std::pair<const eawr::script::foc::ai::GalacticCachedValue*, std::uint64_t>> facts;
            for (const auto& cached : perception.cache) {
                if (cached.key.player != selected.player || cached.key.planet != target->object
                    || cached.key.tokens.size() != 3 || cached.key.tokens[0] != "VARIABLE_TARGET"
                    || cached.key.tokens[1] != "ENEMYFORCE"
                    || (cached.key.tokens[2] != "GROUNDTOTALUNNORMALIZED" && cached.key.tokens[2] != "SPACETOTALUNNORMALIZED")) continue;
                for (const auto& binding : cached.key.bindings) if (const auto* value = std::get_if<eawr::script::foc::ai::Real>(&binding.value);
                    value && binding.parameter == "PARAMETER_CATEGORY"
                    && std::any_of(definition->galactic_contrast_order.begin(), definition->galactic_contrast_order.end(),
                        [&](auto category) { return *value == eawr::script::foc::ai::Real(category); })) {
                    facts.emplace_back(&cached, value->repr); break;
                }
            }
            if (facts.empty()) continue;
            output << "{\"galactic_frame\":" << frame << ",\"player\":" << selected.player
                << ",\"goal\":" << goal.goal << ",\"target\":" << target->object << ",\"plan\":";
            json_string(output, definition->name);
            output << ",\"running_plan\":" << goal.running_plan << ",\"cached_classified_force\":[";
            bool first = true;
            for (const auto& [fact, category] : facts) {
                if (!first) { output << ','; }
                first = false;
                output << "{\"space\":" << (fact->key.tokens[2] == "SPACETOTALUNNORMALIZED" ? "true" : "false")
                    << ",\"category_bits\":" << category << ",\"force_bits\":" << fact->value.repr << ",\"cache_frame\":" << fact->frame << '}';
            }
            output << "]}" << std::endl; witnessed.insert(selected.player); break;
        }
    }
}
// A retained pending invasion that waits behind another conflict: either a live child battle or a
// pending/staged admission. Shared by the installed gate and its deterministic overlap contract.
inline bool invasion_blocked_by_conflict(const cp::GameSession& game) {
    const auto* invasion = game.galaxy().pending_native_invasion();
    return invasion && invasion->state == gc::GalacticSession::NativeInvasion::State::pending
        && (game.mode() == cp::SessionMode::battle || game.has_pending_conflict_for_admission());
}
struct Observations {
    std::vector<cp::GalacticAiRuntimeView> runtimes;
    std::map<gc::PlayerId, PlayerProgress> players;
    std::set<std::tuple<std::uint64_t, std::uint64_t, cp::GalacticAiBattleKind>> delivered;
    std::set<std::uint64_t> conflicts, returned;
    std::set<eawr::sim::EntityId> heroes;
    std::optional<cp::ConflictRecord> last_tactical_return;
    std::set<std::uint32_t> historical_query_workers;
    std::set<std::pair<gc::PlayerId, std::uint64_t>> paid_reserved_goals;
    bool normal_paid_task{};
    void battle(const cp::GalacticAiBattleNotice& notice) {
        if (!delivered.emplace(notice.synchronization_tick, notice.conflict.id, notice.kind).second) return;
        if (conflicts.insert(notice.conflict.id).second) {
            if (players.contains(notice.conflict.invader)) ++players.at(notice.conflict.invader).attacks;
            if (players.contains(notice.conflict.defender)) ++players.at(notice.conflict.defender).defences;
        }
        if (notice.kind == cp::GalacticAiBattleKind::returned) returned.insert(notice.conflict.id);
        if (notice.kind == cp::GalacticAiBattleKind::returned && !notice.auto_resolve)
            last_tactical_return = notice.conflict;
        for (const auto& force : notice.conflict.forces) if (force.hero && heroes.insert(force.id).second
            && players.contains(force.owner)) ++players.at(force.owner).heroes;
    }
};
struct LearningProbe {
    std::function<void(std::string_view, std::uint32_t, const cp::GalacticAiRuntimeView&, const cp::GalacticAiRuntimeView&)> capture;
    std::function<void(std::string_view, std::uint32_t, const cp::GameTick&, const cp::GameTick&,
        const cp::GalacticAiRuntimeView&, const cp::GalacticAiRuntimeView&)> compare;
    std::function<bool()> passed;
    // True once this branch's own evidence is complete; absent means no evidence is awaited.
    std::function<bool(std::string_view, std::uint32_t)> satisfied;
};
// Every restored branch must have stepped at least this many frames equal to the live run.
inline constexpr std::uint64_t minimum_branch_frames = 90;
template<class Workers> inline bool expected_workers_present(const Workers& observed,
    std::span<const std::size_t> expected) {
    return observed.size() == expected.size() && std::all_of(expected.begin(), expected.end(),
        [&](auto workers) { return observed.contains(static_cast<std::uint32_t>(workers)); });
}
inline bool continuation_workers_valid(const std::map<std::uint32_t, std::uint64_t>& observed,
    std::span<const std::size_t> expected) {
    return expected_workers_present(observed, expected)
        && std::all_of(observed.begin(), observed.end(), [](const auto& row) { return row.second >= minimum_branch_frames; });
}
struct DiskBranch {
    std::string boundary;
    std::uint32_t workers{};
    std::uint64_t saved_tick{}, compared_frames{};
    std::unique_ptr<eawr::platform::ThreadWorkerAdapter> executor;
    ck::GameRestore restore;
    cp::GalacticAiRuntimeView runtime;
};
inline void disk_capture(std::string name, const std::filesystem::path& directory, cp::GameSession& game,
    const ck::Identities& identities, const ck::GameLoadOptions& options, std::vector<DiskBranch>& branches,
    Observations& observed, const cp::GalacticAiRuntimeView& control, const LearningProbe& probe) {
    const auto bytes = required(ck::save_game_checkpoint(game, identities));
    std::filesystem::create_directories(directory);
    const auto path = directory / (name + ".eawr");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        check(static_cast<bool>(output), "disk checkpoint output unavailable");
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close(); check(static_cast<bool>(output), "disk checkpoint write failed");
    }
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> read{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    check(!input.bad() && read == bytes, "disk checkpoint bytes differ from live capture");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        auto executor = std::make_unique<eawr::platform::ThreadWorkerAdapter>(workers);
        const auto runtime_count = observed.runtimes.size();
        auto restored = required(ck::load_game_checkpoint(read, identities, options, *executor));
        check(observed.runtimes.size() == runtime_count + 1, "disk restoration did not publish one fresh retained runtime");
        check(required(ck::save_game_checkpoint(restored.session, identities)) == bytes,
            "disk native/world/child/Lua identities or canonical state differ at restore");
        if (name == "post-return") {
            check(observed.last_tactical_return.has_value() && game.mode() == cp::SessionMode::galaxy,
                "historical query witness requires a real completed tactical child");
            const auto& conflict = *observed.last_tactical_return;
            const cp::ModeId mode{cp::SessionMode::battle, conflict.id};
            const auto history = game.historical_child_timeline();
            const auto restored_history = restored.session.historical_child_timeline();
            check(history && restored_history && history->mode == mode && restored_history->mode == mode
                && history->completed_tick == restored_history->completed_tick && history->value == restored_history->value,
                "historical child identity/tick/scalar differs across disk restore");
            const auto& resumed_view = observed.runtimes.back();
            check(control.child_parent_queries && resumed_view.child_parent_queries,
                "stock historical parent-query factory is absent");
            const auto query = control.child_parent_queries(conflict);
            const auto resumed_query = resumed_view.child_parent_queries(conflict);
            const auto parent_before = required(game.save_galactic_ai()).state->components;
            const auto resumed_before = required(restored.session.save_galactic_ai()).state->components;
            // WGA-382: actual stock equation, with the exact completed child
            // context used by entry and reconstruction, never active galaxy mode.
            constexpr std::string_view equation = "Empire_Should_Upgrade_Tech";
            const auto value = query(equation, conflict.invader);
            const auto resumed_value = resumed_query(equation, conflict.invader);
            check(value && resumed_value && value == resumed_value,
                "actual stock historical child equation refuses or differs after disk restore");
            check(required(game.save_galactic_ai()).state->components == parent_before
                && required(restored.session.save_galactic_ai()).state->components == resumed_before,
                "historical child equation published parent native state");
            std::ofstream witness(directory / "historical-child-queries.ndjson", std::ios::app);
            witness << "{\"workers\":" << workers << ",\"conflict\":" << conflict.id
                << ",\"planet\":" << conflict.planet << ",\"child_tick\":" << history->completed_tick
                << ",\"child_timeline\":" << history->value << ",\"equation\":\"" << equation
                << "\",\"result_bits\":" << value->repr << ",\"native_bytes_equal\":true}\n";
            witness.close(); check(static_cast<bool>(witness), "historical child query receipt write failed");
            observed.historical_query_workers.insert(static_cast<std::uint32_t>(workers));
        }
        if (probe.capture) probe.capture(name, static_cast<std::uint32_t>(workers), control, observed.runtimes.back());
        branches.push_back({name, static_cast<std::uint32_t>(workers), game.completed_tick(), 0, std::move(executor), std::move(restored), observed.runtimes.back()});
    }
}
inline void compare(const cp::GameTick& control, const cp::GameTick& resumed) {
    check(control.completed_tick == resumed.completed_tick && control.mode == resumed.mode
        && control.state_hash.get() == resumed.state_hash.get(), "live disk continuation hash/clock/mode differs");
    check(control.galaxy->events().size() == resumed.galaxy->events().size()
        && std::equal(control.galaxy->events().begin(), control.galaxy->events().end(), resumed.galaxy->events().begin())
        && control.conflict_events == resumed.conflict_events && control.save_events == resumed.save_events,
        "live disk continuation events differ");
    check(control.galaxy->player_economy() == resumed.galaxy->player_economy()
        && control.galaxy->planets().size() == resumed.galaxy->planets().size()
        && control.galaxy->fleets().size() == resumed.galaxy->fleets().size()
        && std::equal(control.galaxy->planets().begin(), control.galaxy->planets().end(), resumed.galaxy->planets().begin())
        && std::equal(control.galaxy->fleets().begin(), control.galaxy->fleets().end(), resumed.galaxy->fleets().begin()),
        "live disk continuation accounts or persistent identities differ");
    check(static_cast<bool>(control.battle) == static_cast<bool>(resumed.battle)
        && (!control.battle || control.battle_state_hash.get() == resumed.battle_state_hash.get()),
        "live disk continuation child state differs");
    // The composed digest contains the six native components, including all
    // synchronized RNG state. The child digest contains its native and Lua RNG.
}
inline void json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const auto c : value) {
        if (c == '"' || c == '\\') output << '\\' << c;
        else if (c == '\n') output << "\\n";
        else if (c == '\r') output << "\\r";
        else if (c == '\t') output << "\\t";
        else if (static_cast<unsigned char>(c) < 32) output << '?';
        else output << c;
    }
    output << '"';
}
struct BlockedInvasionWitness {
    std::uint64_t tick{}, request{}, fleet{}, planet{}, other_conflict{};
    bool child{};
};
struct FreestoreHopWitness {
    std::uint64_t tick{}, request{}, selected_actor{}, fleet{}, source{}, hop{}, destination{};
    gc::PlayerId player{};
};
struct Report {
    std::uint64_t synchronized_frames{}, galactic_frames{}, battles{}, returns{}, mature_samples{}, changed_samples{}, unchanged_samples{};
    std::uint64_t changed_ns{}, unchanged_ns{}, maximum_digest_ns{}, maximum_originals{}, maximum_goals{};
    std::uint64_t encoder_free_samples{}, encoder_free_ns{}, same_payload_encoded_samples{};
    std::uint64_t mature_native_bytes{}, mature_goals{}, mature_originals{}, mature_purchases{}, mature_movements{};
    std::uint64_t freestore_move_refusals{};
    std::map<gc::PlayerId, PlayerProgress> players;
    std::map<std::string, std::map<std::uint32_t, std::uint64_t>> boundaries;
    std::optional<gc::CampaignResult> terminal;
    std::optional<BlockedInvasionWitness> blocked_invasion;
    std::optional<FreestoreHopWitness> freestore_hop;
    std::string failure, stop_reason;
    bool progression{}, continuation{}, normal_paid_selection{}, historical_queries{}, learning{}, digest_budget{}, correctness{}, passed{};
    std::string final_hash;
    bool includes_internal_oracle{};
    std::array<std::uint64_t, 6> owner_encodes{};
    std::map<std::string, std::uint64_t, std::less<>> external_encodes;
    void write(std::ostream& output) const {
        output << "{\"run_started\":true,\"release_gate_passed\":" << (passed ? "true" : "false")
            << ",\"unattended_human_policy\":\"ordinary_auto_resolve_and_interactive_summary_ack\",\"correctness_passed\":" << (correctness ? "true" : "false") << ",\"final_hash\":";
        json_string(output, final_hash);
        output
            << ",\"synchronized_frames\":" << synchronized_frames << ",\"galactic_frames\":" << galactic_frames
            << ",\"game_seconds_limit\":900,\"stop_reason\":";
        json_string(output, stop_reason); output << ",\"failure\":"; json_string(output, failure);
        // WGA-309: a destination-found result can consume Lua's quota despite
        // a silent unbound native mover refusal. Keep that limitation visible.
        output << ",\"freestore_move_refusals\":" << freestore_move_refusals
            << ",\"battles\":" << battles << ",\"returns\":" << returns << ",\"terminal_result\":";
        if (terminal) output << "{\"winner\":" << terminal->condition.player << ",\"condition\":" << static_cast<unsigned>(terminal->condition.kind) << '}';
        else output << "null";
        output << ",\"blocked_invasion\":";
        if (blocked_invasion) {
            const auto& row = *blocked_invasion;
            output << "{\"tick\":" << row.tick << ",\"request\":" << row.request << ",\"fleet\":" << row.fleet
                << ",\"planet\":" << row.planet << ",\"other_conflict\":" << row.other_conflict
                << ",\"child\":" << (row.child ? "true" : "false") << '}';
        } else output << "null";
        output << ",\"stock_freestore_hop\":";
        if (freestore_hop) {
            const auto& row = *freestore_hop;
            output << "{\"tick\":" << row.tick << ",\"request\":" << row.request << ",\"selected_actor\":" << row.selected_actor
                << ",\"fleet\":" << row.fleet << ",\"player\":" << row.player << ",\"source\":" << row.source
                << ",\"hop\":" << row.hop << ",\"destination\":" << row.destination << '}';
        } else output << "null";
        output << ",\"players\":[";
        bool first = true;
        for (const auto& [id, player] : players) {
            if (!first) { output << ','; }
            first = false;
            output << "{\"id\":" << id << ",\"builds_started\":" << player.builds_started << ",\"builds_completed\":" << player.builds_completed
                << ",\"expansions\":" << player.expansions << ",\"attacks\":" << player.attacks << ",\"defences\":" << player.defences
                << ",\"movements\":" << player.movements
                << ",\"heroes_observed\":" << player.heroes << ",\"maximum_tech\":" << player.maximum_tech
                << ",\"first_tech3_frame\":" << player.first_tech3_frame << '}';
        }
        output << "],\"disk_continuations\":["; first = true;
        for (const auto& [name, workers] : boundaries) for (const auto& [count, frames] : workers) {
            if (!first) { output << ','; }
            first = false;
            output << "{\"boundary\":"; json_string(output, name);
            output << ",\"workers\":" << count << ",\"compared_frames\":" << frames << '}';
        }
        output << "],\"normal_paid_selection_verified\":" << (normal_paid_selection ? "true" : "false")
            << ",\"historical_child_queries_verified\":" << (historical_queries ? "true" : "false")
            << ",\"learning_verified\":" << (learning ? "true" : "false")
            << ",\"native_digest\":{\"mature_samples\":" << mature_samples << ",\"changed_samples\":" << changed_samples
            << ",\"unchanged_samples\":" << unchanged_samples << ",\"changed_nanoseconds\":" << changed_ns
            << ",\"unchanged_nanoseconds\":" << unchanged_ns << ",\"maximum_nanoseconds\":" << maximum_digest_ns
            << ",\"encoder_free_samples\":" << encoder_free_samples << ",\"encoder_free_nanoseconds\":" << encoder_free_ns
            << ",\"same_payload_encoded_samples\":" << same_payload_encoded_samples
            << ",\"mature_native_bytes\":" << mature_native_bytes << ",\"mature_goals\":" << mature_goals
            << ",\"mature_originals\":" << mature_originals << ",\"mature_purchases\":" << mature_purchases
            << ",\"mature_movements\":" << mature_movements
            << ",\"maximum_originals\":" << maximum_originals << ",\"maximum_goals\":" << maximum_goals
            << ",\"includes_internal_oracle\":" << (includes_internal_oracle ? "true" : "false")
            << ",\"independent_full_oracle_after_timing\":true,\"core_owner_encodes\":[";
        first = true;
        for (const auto count : owner_encodes) {
            if (!first) { output << ','; }
            first = false; output << count;
        }
        output << "],\"external_owner_encodes\":{"; first = true;
        for (const auto& [name, count] : external_encodes) {
            if (!first) { output << ','; }
            first = false; json_string(output, name); output << ':' << count;
        }
        output << "},\"unchanged_budget_ns\":100000,\"budget_verified\":" << (digest_budget ? "true" : "false") << "}}\n";
    }
};
inline Report run(cp::GameSession& game, const cp::GalacticAiRuntimeSetup& setup,
    const eawr::platform::CampaignServiceConfiguration& configuration, const std::shared_ptr<Observations>& observed,
    const std::filesystem::path& directory, const LearningProbe& probe = {}, bool human_encounter_only = false,
    bool hash_baseline = false) {
    const auto interactive = game.record().setup.interactive_seat;
    Report report;
    std::optional<std::uint64_t> answered_frame;
    std::filesystem::create_directories(directory);
    std::ofstream inputs(directory / "unattended-human-inputs.ndjson", std::ios::trunc);
    check(static_cast<bool>(inputs), "unattended human input receipt unavailable");
    std::ofstream timeline(directory / "ownership.ndjson", std::ios::trunc);
    check(static_cast<bool>(timeline), "campaign timeline output unavailable");
    // Wall-clock cost per ten game seconds, kept apart from the compared timeline so an
    // interrupted two-hour run still shows how far it got and how fast.
    std::ofstream pace(directory / "progress.ndjson", std::ios::trunc);
    check(static_cast<bool>(pace), "campaign progress output unavailable");
    std::ofstream targeted(directory / "targeted-plan-witnesses.ndjson", std::ios::trunc);
    check(static_cast<bool>(targeted), "campaign targeted-plan output unavailable");
    std::set<gc::PlayerId> targeted_players;
    const auto started = std::chrono::steady_clock::now();
    check(!observed->runtimes.empty(), "campaign factory did not publish real native runtime owners");
    const auto view = observed->runtimes.front();
    if (hash_baseline) {
        check(static_cast<bool>(view.state.lock()), "hash baseline requires composed state");
        view.state.lock()->measure_sync_owners(true);
        std::cout << "HASH-BASELINE diagnostic_only=true disk_continuations=false sample_limit=512\n";
    }
    std::vector<DiskBranch> branches;
    // A branch that met its frame minimum and its evidence is retired: stepping it to the end of
    // the run only repeated comparisons the live run's canonical hash already covers.
    std::map<std::string, std::map<std::uint32_t, std::uint64_t>> retired;
    std::set<std::string> captured;
    const auto identities = ck::game_identities(game.galaxy().setup());
    std::map<gc::PlanetId, gc::PlayerId> ownership;
    std::map<eawr::sim::EntityId, gc::PlayerId> production_owners;
    std::optional<std::uint64_t> last_timeline_frame, last_progress_frame;
    for (const auto& planet : game.galaxy().snapshot()->planets()) ownership[planet.id] = planet.owner;
    try {
        while (game.galaxy().clock().galactic_frame < 900U * gc::galactic_frames_per_second) {
            // A stalled child/arbitration remains a failed gate, never a victory.
            if (game.completed_tick() >= 1800U * gc::galactic_frames_per_second) { report.stop_reason = "synchronization_guard"; break; }
            const auto input = unattended_human_input(game);
            if (input) {
                write_human_input(inputs, *input);
                if (std::holds_alternative<gc::BattleChoice>(input->payload)) answered_frame = game.galaxy().clock().galactic_frame;
            }
            const auto tick = required(game.step(*setup.executor));
            check(interactive.value || !game.battle_summary_hold(),
                "noninteractive campaign harness acquired a summary hold");
            for (auto& branch : branches) {
                const auto resumed_input = unattended_human_input(branch.restore.session);
                check(input == resumed_input, "restored unattended human command differs");
                const auto resumed = required(branch.restore.session.step(*branch.executor));
                if (tick.state_hash.get() != resumed.state_hash.get()) {
                    std::cerr << "continuation mismatch boundary=" << branch.boundary << " workers=" << branch.workers
                        << " tick=" << tick.completed_tick << " control=" << tick.state_hash.get()
                        << " resumed=" << resumed.state_hash.get() << '\n';
                    const auto left = required(ck::parse_checkpoint(required(ck::save_game_checkpoint(game, identities))));
                    const auto right = required(ck::parse_checkpoint(required(ck::save_game_checkpoint(branch.restore.session, identities))));
                    for (const auto& section : left.sections) {
                        const auto other = std::find_if(right.sections.begin(), right.sections.end(),
                            [&](const auto& row) { return row.id == section.id; });
                        const auto equal = other != right.sections.end() && section.bytes == other->bytes;
                        std::cerr << "continuation section=" << ck::section_name(section.id) << " equal=" << equal;
                        if (!equal && other != right.sections.end()) {
                            const auto mismatch = std::mismatch(section.bytes.begin(), section.bytes.end(), other->bytes.begin(), other->bytes.end());
                            std::cerr << " control_size=" << section.bytes.size() << " resumed_size=" << other->bytes.size()
                                << " first_offset=" << std::distance(section.bytes.begin(), mismatch.first);
                        }
                        std::cerr << '\n';
                    }
                    for (const auto& [label, owner] : {std::pair{"control", &game}, std::pair{"resumed", &branch.restore.session}}) {
                        const auto native = required(owner->save_galactic_ai());
                        if (native.state) for (const auto& [name, bytes] : native.state->components)
                            std::cerr << "continuation native=" << label << '/' << name << " bytes=" << bytes.size()
                                << " hash=" << eawr::core::sha256_hex(bytes) << '\n';
                    }
                }
                compare(tick, resumed); ++branch.compared_frames;
                if (probe.compare) probe.compare(branch.boundary, branch.workers, tick, resumed, view, branch.runtime);
            }
            std::erase_if(branches, [&](const DiskBranch& branch) {
                if (branch.compared_frames < minimum_branch_frames
                    || (probe.satisfied && !probe.satisfied(branch.boundary, branch.workers))) return false;
                retired[branch.boundary][branch.workers] = branch.compared_frames;
                return true;
            });
            const auto snapshot = tick.galaxy;
            report.synchronized_frames = tick.completed_tick; report.galactic_frames = snapshot->clock().galactic_frame;
            if (human_encounter_only && answered_frame && report.galactic_frames > *answered_frame
                && !game.pending_conflict() && !game.galaxy().clock().pause_depth) {
                report.stop_reason = "unattended_human_encounter_advanced"; break;
            }
            if (human_encounter_only && report.galactic_frames > 12400) {
                report.stop_reason = "human_encounter_not_observed"; break;
            }
            for (const auto& planet : snapshot->planets()) if (ownership.at(planet.id) != planet.owner) {
                if (observed->players.contains(planet.owner)) ++observed->players.at(planet.owner).expansions;
                ownership.at(planet.id) = planet.owner;
            }
            const auto actions = view.actions.lock(); check(static_cast<bool>(actions), "campaign native action owner lifetime ended during gate");
            report.freestore_move_refusals = actions->freestore_move_refusals();
            if (!report.freestore_hop) for (const auto& [request, movement] : actions->movements()) {
                if (!movement.freestore_actor || !movement.actor || !movement.registered || movement.finished) continue;
                const auto fleet = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(),
                    [&movement](const auto& row) { return row.id == movement.actor; });
                if (fleet == snapshot->fleets().end() || !fleet->motion.transit) continue;
                const auto& transit = *fleet->motion.transit;
                check(fleet->owner == movement.player && transit.hop + 1 < transit.path.size(),
                    "actual stock free movement fleet owner or next hop is invalid");
                report.freestore_hop = FreestoreHopWitness{game.completed_tick(), request, movement.freestore_actor,
                    movement.actor, transit.path[transit.hop], transit.path[transit.hop + 1], movement.destination, movement.player};
                break;
            }
            for (const auto& [build, purchase] : actions->purchases()) production_owners[build] = purchase.player;
            for (const auto& event : snapshot->events()) if ((event.kind == gc::GalacticEventKind::production_started
                || event.kind == gc::GalacticEventKind::production_completed) && production_owners.contains(event.sequence)
                && observed->players.contains(production_owners.at(event.sequence))) {
                auto& player = observed->players.at(production_owners.at(event.sequence));
                if (event.kind == gc::GalacticEventKind::production_started) ++player.builds_started;
                if (event.kind == gc::GalacticEventKind::production_completed) ++player.builds_completed;
            }
            if (snapshot->player_economy()) for (const auto& account : snapshot->player_economy()->players) if (observed->players.contains(account.player)) {
                auto& progress = observed->players.at(account.player);
                progress.maximum_tech = std::max(progress.maximum_tech, account.current_tech);
                if (account.current_tech >= 3 && !progress.first_tech3_frame) progress.first_tech3_frame = report.galactic_frames;
            }
            if (report.galactic_frames % (10U * gc::galactic_frames_per_second) == 0 && last_progress_frame != report.galactic_frames) {
                last_progress_frame = report.galactic_frames;
                sample_movements(observed->players, *actions);
                sample_targeted_plans(view, setup, report.galactic_frames, targeted, targeted_players);
                pace << "{\"galactic_frame\":" << report.galactic_frames << ",\"branches\":" << branches.size() << ",\"wall_seconds\":"
                    << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()
                    << ",\"battles\":" << observed->conflicts.size() << ",\"returns\":" << observed->returned.size()
                    << ",\"movements\":" << actions->movements().size() << ",\"players\":[";
                bool first = true;
                for (const auto& [id, player] : observed->players) {
                    if (!first) pace << ',';
                    first = false;
                    pace << "{\"id\":" << id << ",\"builds_started\":" << player.builds_started
                        << ",\"builds_completed\":" << player.builds_completed << ",\"expansions\":" << player.expansions
                        << ",\"attacks\":" << player.attacks << ",\"defences\":" << player.defences
                        << ",\"movements\":" << player.movements
                        << ",\"maximum_tech\":" << player.maximum_tech << ",\"first_tech3_frame\":" << player.first_tech3_frame << '}';
                }
                pace << "]}" << std::endl;
            }
            if (report.galactic_frames % (30U * gc::galactic_frames_per_second) == 0 && last_timeline_frame != report.galactic_frames) {
                last_timeline_frame = report.galactic_frames;
                timeline << "{\"tick\":" << report.synchronized_frames << ",\"galactic_frame\":" << report.galactic_frames << ",\"owned\":[";
                bool first = true;
                for (const auto& [player, unused] : observed->players) {
                    static_cast<void>(unused); if (!first) timeline << ','; first = false;
                    const auto count = std::count_if(snapshot->planets().begin(), snapshot->planets().end(), [player](const auto& planet) { return planet.owner == player; });
                    timeline << "{\"player\":" << player << ",\"planets\":" << count << '}';
                }
                timeline << "]}" << std::endl;
            }
            const auto engine = view.engine.lock(); check(static_cast<bool>(engine), "campaign native engine lifetime ended during gate");
            std::uint64_t goals{}; bool paid_reserved{};
            for (const auto& selected : setup.lifecycle.players) {
                goals += engine->galactic_goals(selected.player).size();
                if (eawr::script::foc::ai::upper_case(selected.player_type) != "BASICEMPIRE") continue;
                const auto current = eawr::script::foc::ai::EngineContractAccess::paid_reservations(*engine, selected.player);
                paid_reserved = paid_reserved || !current.empty();
                for (const auto goal : current)
                    observed->paid_reserved_goals.emplace(selected.player, goal);
            }
            if (!observed->normal_paid_task) {
                const auto funding = view.funding.lock(); check(static_cast<bool>(funding), "actual paid build Funding owner absent");
                const auto builds = required(engine->galactic_build_budgets(*actions, *funding));
                for (const auto& row : *builds) {
                    const auto purchase = actions->purchases().find(row.build);
                    if (purchase == actions->purchases().end()) continue;
                    const auto& paid = purchase->second;
                    if (!observed->paid_reserved_goals.contains({paid.player, paid.goal})
                        || !(eawr::sim::math::Fixed{} < paid.price)
                        || !eawr::script::foc::ai::EngineContractAccess::unstaged_paid_task(*engine, paid.player, paid.goal, row.build)) continue;
                    check(row.builder == paid.player && row.planet == paid.producer && row.type == paid.requested_type
                        && row.channel == paid.channel, "actual Normal paid Lua task build correlation differs");
                    std::ofstream witness(directory / "normal-paid-selection.json", std::ios::trunc);
                    witness << "{\"player\":" << paid.player << ",\"goal\":" << paid.goal << ",\"build\":" << row.build
                        << ",\"producer\":" << paid.producer << ",\"type\":" << paid.requested_type
                        << ",\"positive_authored_reserved_proposal\":true,\"actual_unstaged_lua_task\":true,\"build_correlation_equal\":true}\n";
                    witness.close(); check(static_cast<bool>(witness), "Normal paid selection receipt write failed");
                    observed->normal_paid_task = true; break;
                }
            }
            report.maximum_goals = std::max(report.maximum_goals, goals);
            report.maximum_originals = std::max(report.maximum_originals, static_cast<std::uint64_t>(snapshot->commissioned_originals().size()));
            if (report.galactic_frames >= 600U * gc::galactic_frames_per_second && view.digest_work) {
                if (hash_baseline) {
                    std::cout << "HASH-SAMPLE {\"frame\":" << report.galactic_frames
                        << ",\"nanoseconds\":" << view.digest_work->last_nanoseconds
                        << ",\"changed\":" << (view.digest_work->last_changed ? "true" : "false")
                        << ",\"encoded\":" << (view.digest_work->last_encoded ? "true" : "false")
                        << ",\"owners\":{";
                    bool first_owner = true;
                    for (const auto& [name, ns] : view.state.lock()->last_sync_owner_nanoseconds()) {
                        if (!first_owner) std::cout << ',';
                        first_owner = false; json_string(std::cout, name); std::cout << ':' << ns;
                    }
                    std::cout << "}}\n";
                }
                ++report.mature_samples; report.maximum_digest_ns = std::max(report.maximum_digest_ns, view.digest_work->last_nanoseconds);
                report.mature_native_bytes = std::max(report.mature_native_bytes, view.digest_work->native_bytes);
                report.mature_goals = std::max(report.mature_goals, goals);
                report.mature_originals = std::max(report.mature_originals, static_cast<std::uint64_t>(snapshot->commissioned_originals().size()));
                report.mature_purchases = std::max(report.mature_purchases, static_cast<std::uint64_t>(actions->purchases().size()));
                report.mature_movements = std::max(report.mature_movements, static_cast<std::uint64_t>(actions->movements().size()));
                if (view.digest_work->last_changed) { ++report.changed_samples; report.changed_ns += view.digest_work->last_nanoseconds; }
                else { ++report.unchanged_samples; report.unchanged_ns += view.digest_work->last_nanoseconds; }
                if (!view.digest_work->last_encoded) { ++report.encoder_free_samples; report.encoder_free_ns += view.digest_work->last_nanoseconds; }
                else if (!view.digest_work->last_changed) ++report.same_payload_encoded_samples;
                if (hash_baseline && report.mature_samples >= 512) { report.stop_reason = "diagnostic_hash_sample_limit"; break; }
            }
            const auto capture = [&](std::string name, bool ready) {
                if (!hash_baseline && !human_encounter_only && ready && captured.insert(name).second) disk_capture(name, directory, game, identities, configuration.restore, branches, *observed, view, probe);
            };
            // The checkpoint contains a naturally selected positive paid
            // proposal, rather than an arbitrary empty/free reservation.
            capture("reservation", paid_reserved);
            capture("transit", std::any_of(snapshot->fleets().begin(), snapshot->fleets().end(), [](const auto& fleet) { return fleet.motion.transit.has_value(); }));
            const auto* invasion = game.galaxy().pending_native_invasion();
            const bool child = game.mode() == cp::SessionMode::battle;
            const bool blocked = invasion_blocked_by_conflict(game);
            if (blocked && !captured.contains("blocked-invasion")) {
                const auto* other = child ? game.conflict() : game.pending_conflict();
                report.blocked_invasion = BlockedInvasionWitness{game.completed_tick(), invasion->request, invasion->fleet,
                    invasion->planet, other ? other->id : 0, child};
            }
            capture("blocked-invasion", blocked);
            capture("post-return", observed->last_tactical_return.has_value() && game.mode() == cp::SessionMode::galaxy);
            if (snapshot->victory()) report.terminal = gc::campaign_result(*snapshot->victory(), snapshot->observer() ? snapshot->observer()->solo_reference_human : 0, false, false);
            if (report.terminal) { report.stop_reason = "campaign_result"; break; }
        }
        if (report.stop_reason.empty()) report.stop_reason = "galactic_game_time_budget";
    } catch (const std::exception& error) { report.failure = error.what(); report.stop_reason = "runtime_refusal_or_continuation_failure"; }
    report.players = observed->players; report.battles = observed->conflicts.size(); report.returns = observed->returned.size();
    if (const auto actions = view.actions.lock()) sample_movements(report.players, *actions);
    report.boundaries = retired;
    for (const auto& branch : branches) report.boundaries[branch.boundary][branch.workers] = branch.compared_frames;
    report.progression = !report.players.empty() && std::all_of(report.players.begin(), report.players.end(), [](const auto& row) {
        const auto& p = row.second; return p.builds_completed && p.expansions && p.attacks && p.defences && p.maximum_tech >= 3
            && p.first_tech3_frame && p.first_tech3_frame <= 900U * gc::galactic_frames_per_second;
    });
    report.continuation = report.boundaries.size() == 4 && report.blocked_invasion.has_value();
    const auto expected_workers = eawr::platform::determinism_worker_counts();
    for (const auto& [name, workers] : report.boundaries) { static_cast<void>(name);
        report.continuation = report.continuation && continuation_workers_valid(workers, expected_workers);
    }
    report.digest_budget = report.encoder_free_samples && report.encoder_free_ns / report.encoder_free_samples < 100000;
    if (view.digest_work) {
        report.includes_internal_oracle = view.digest_work->includes_internal_oracle;
        report.owner_encodes = view.digest_work->owner_encodes; report.external_encodes = view.digest_work->external_encodes;
    }
    report.digest_budget = report.digest_budget && !report.includes_internal_oracle;
    report.learning = probe.capture && probe.compare && probe.passed && probe.passed();
    report.historical_queries = expected_workers_present(observed->historical_query_workers, expected_workers);
    report.normal_paid_selection = observed->normal_paid_task && report.boundaries.contains("reservation")
        && expected_workers_present(report.boundaries.at("reservation"), expected_workers);
    report.correctness = report.failure.empty() && !report.freestore_move_refusals && report.freestore_hop.has_value()
        && report.progression && report.continuation && report.normal_paid_selection && report.historical_queries && report.learning;
    report.passed = report.correctness && report.digest_budget; report.final_hash = game.state_sha256();
    std::ofstream summary(directory / "result.json", std::ios::trunc); report.write(summary);
    check(static_cast<bool>(summary), "campaign gate report write failed");
    return report;
}
} // namespace galactic_campaign_gate
