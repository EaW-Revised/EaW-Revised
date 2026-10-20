#include "foc_ai_fixtures.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/script/foc/ai_engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace foc_ai_test_support {

namespace auth = eawr::script::authoritative;

namespace {

// GFS-A24 acceptance on the established stock Coruscant 3v3 profile. Measurements
// observe ordinary routed commands/admitted purchases and lifetime loss attribution.
// S-T3b: what the AI player turns staged, and what a later turn read of an earlier turn's uncommitted effects,
// summed over the barriers since the last report (the overlay depth the unstacked S-T4 has to remove).
foc::ai::EffectCounts effect_sum;

} // namespace
std::uint64_t effect_barriers = 0;

void count_effects(std::uint64_t, const foc::ai::EffectCounts& counts) noexcept {
    ++effect_barriers;
    effect_sum.turns += counts.turns; effect_sum.events += counts.events; effect_sum.orders += counts.orders;
    effect_sum.removals += counts.removals; effect_sum.credit_deltas += counts.credit_deltas;
    effect_sum.pad_effects += counts.pad_effects; effect_sum.attach_intents += counts.attach_intents;
    effect_sum.index_effects += counts.index_effects; effect_sum.operations += counts.operations;
    effect_sum.diagnostics += counts.diagnostics; effect_sum.invalidations += counts.invalidations;
    effect_sum.reads.credits += counts.reads.credits; effect_sum.reads.pads += counts.reads.pads;
    effect_sum.reads.pad_rows += counts.reads.pad_rows;
    effect_sum.reads.handles += counts.reads.handles; effect_sum.reads.row_scans += counts.reads.row_scans;
}

void report_effects(const std::string& label) {
    std::cout << "AI-EFFECT-TOTALS," << label << ',' << effect_barriers << ',' << effect_sum.turns << ',' << effect_sum.events
        << ',' << effect_sum.orders << ',' << effect_sum.removals << ',' << effect_sum.credit_deltas
        << ',' << effect_sum.pad_effects << ',' << effect_sum.attach_intents << ',' << effect_sum.index_effects
        << ',' << effect_sum.operations << ',' << effect_sum.diagnostics << ',' << effect_sum.invalidations
        << ',' << effect_sum.reads.credits << ',' << effect_sum.reads.pads << ',' << effect_sum.reads.pad_rows
        << ',' << effect_sum.reads.handles << ',' << effect_sum.reads.row_scans << std::endl;
    effect_sum = {};
    effect_barriers = 0;
}

int player_stream_profile(const std::filesystem::path& root, std::uint64_t ticks,
    std::size_t only_workers, bool checkpoint_restore) {
    expect(ticks >= 2, "stream acceptance needs a checkpoint and continuation");
    if (ticks < 2) return 1;
    for (const std::uint64_t seed : {66U, 67U, 68U}) {
        auto content = load(root, false, false, {}, false, seed, {}, {}, false, true);
        if (!content) return 1;
        std::vector<std::string> reference;
        std::map<tactical::PlayerId, std::array<std::uint64_t, 3>> reference_metrics;
        const auto counts = only_workers ? std::vector<std::size_t>{only_workers} : std::vector<std::size_t>{1, 2, 4, 8};
        for (const auto workers : counts) {
            auto make_world = [&] { return tactical::TacticalSession::create(content->start.setup, content->content.sensors,
                content->content.durability, content->content.motion, content->content.fog, content->content.combat,
                content->victory, content->content.abilities, content->economy, content->content.map_box); };
            auto world = make_world();
            if (!world) { expect(false, "stream acceptance world creates"); return 1; }
            auto session = foc::create_session(std::move(world).value(), content->ai, content->modules);
            if (!session) { expect(false, "stream acceptance AI creates"); return 1; }
            const eawr::platform::ThreadWorkerAdapter executor(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
            const eawr::platform::ThreadWorkerAdapter checkpoint_executor(2, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
            std::vector<std::string> hashes;
            std::optional<auth::ScriptedTacticalSession> continuation_control;
            std::size_t seen_losses{};
            std::map<tactical::PlayerId, std::array<std::uint64_t, 3>> metrics;
            for (const auto& p : content->ai.players) if (p.ai) metrics[p.player] = {};
            for (std::uint64_t frame = 0; frame < ticks; ++frame) {
                const auto result = session.value().step(checkpoint_restore && frame < ticks / 2 ? checkpoint_executor : executor);
                if (!result) { expect(false, "stream acceptance tick: " + result.error().message); return 1; }
                if (continuation_control) {
                    const auto control = continuation_control->step(checkpoint_executor);
                    if (!control) { expect(false, "stream acceptance control tick: " + control.error().message); return 1; }
                    if (control.value().state_sha256 != result.value().state_sha256
                        || continuation_control->record().commands != session.value().record().commands) {
                        const auto left = continuation_control->save_scripts();
                        const auto right = session.value().save_scripts();
                        std::cerr << "STREAM-DIVERGENCE,seed=" << seed << ",tick=" << frame + 1
                            << ",world_equal=" << (control.value().world.state_sha256 == result.value().world.state_sha256)
                            << ",script_equal=" << (control.value().script_sha256 == result.value().script_sha256)
                            << ",commands_equal=" << (continuation_control->record().commands == session.value().record().commands)
                            << ",native_equal=" << (left && right && left.value().native == right.value().native)
                            << ",scheduler_equal=" << (left && right && left.value().scheduler == right.value().scheduler) << '\n';
                        expect(false, "GFS-A24: stock 2-to-8 continuation matches its uninterrupted two-worker control");
                        return 1;
                    }
                }
                hashes.push_back(result.value().state_sha256);
                for (const auto& routed : result.value().script_input)
                    if (routed.submitted && metrics.contains(routed.key.player_id)) ++metrics[routed.key.player_id][0];
                for (const auto& event : result.value().world.snapshot->events())
                    if (event.kind == tactical::EventKind::order_accepted && metrics.contains(event.player)
                        && (event.order == tactical::OrderKind::buy || event.order == tactical::OrderKind::pad_build)) ++metrics[event.player][1];
                const auto losses = result.value().world.snapshot->losses();
                for (std::size_t index = seen_losses; index < losses.size(); ++index)
                    if (metrics.contains(losses[index].killer)) metrics[losses[index].killer][2] += losses[index].count;
                seen_losses = losses.size();
                if (frame + 1 == ticks / 2 || frame + 1 == ticks) {
                    const auto saved = session.value().save_scripts();
                    const auto replay = tactical::write_replay(session.value().record());
                    expect(saved && saved.value().native && replay, "ownership identity captures native and command bytes");
                    if (!saved || !saved.value().native || !replay) return 1;
                    std::cout << "STREAM-BYTES," << seed << ',' << workers << ',' << frame + 1 << ','
                        << eawr::core::sha256_hex(*saved.value().native) << ','
                        << eawr::core::sha256_hex(replay.value()) << std::endl;
                }
                if (checkpoint_restore && frame + 1 == ticks / 2) {
                    const auto saved = session.value().save_scripts();
                    const auto checkpoint = session.value().world().save_checkpoint_state();
                    // The checkpoint's command cursors belong to its sealed replay log (WGS-66).
                    auto rebuilt_world = tactical::TacticalSession::from_replay(session.value().record(), content->content.sensors,
                        content->content.durability, content->content.motion, content->content.fog, content->content.combat,
                        content->victory, content->content.abilities, content->economy, content->content.map_box);
                    auto prepared = foc::prepare_session(content->ai, content->modules);
                    if (!saved || !checkpoint || !rebuilt_world || !prepared) { expect(false, "stream acceptance checkpoint captures"); return 1; }
                    expect(saved.value().native && !saved.value().native->empty(), "stream acceptance checkpoint retains native AI streams");
                    const auto loaded = rebuilt_world.value().load_checkpoint_state(checkpoint.value());
                    if (!loaded) { expect(false, "stream acceptance world restores: " + loaded.error().message); return 1; }
                    auto restored = auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(), std::move(prepared.value().scripts), saved.value());
                    if (!restored) { expect(false, "stream acceptance scripts restore: " + restored.error().message); return 1; }
                    const auto bound = prepared.value().bind(restored.value());
                    if (!bound) { expect(false, "stream acceptance native streams restore: " + bound.error().message); return 1; }
                    const auto roundtrip = restored.value().save_scripts();
                    expect(roundtrip && roundtrip.value().native == saved.value().native && roundtrip.value().scheduler == saved.value().scheduler,
                        "GFS-A24: mid-battle native/script bytes round-trip across 2 to 8 workers");
                    continuation_control.emplace(std::move(session).value());
                    session = std::move(restored);
                    seen_losses = session.value().world().snapshot()->losses().size();
                }
            }
            if (reference.empty()) { reference = hashes; reference_metrics = metrics; }
            else {
                expect(hashes == reference, "GFS-A24: all authoritative 3v3 hashes equal on 1/2/4/8");
                expect(metrics == reference_metrics, "GFS-A24: commands, purchases and kills equal across worker counts");
            }
            std::uint64_t commands{};
            for (const auto& [player, values] : metrics) {
                commands += values[0];
                const auto p = std::find_if(content->ai.players.begin(), content->ai.players.end(), [&](const auto& row) { return row.player == player; });
                std::cout << "STREAM-METRICS," << seed << ',' << workers << ',' << player << ',' << p->faction << ','
                    << values[0] << ',' << values[1] << ',' << values[2] << '\n';
            }
            expect(commands > 0, "stream acceptance observes active AI commands");
            std::vector<std::uint8_t> hash_bytes;
            const auto nibble = [](char value) -> std::uint8_t {
                return static_cast<std::uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10);
            };
            for (const auto& hash : hashes) for (std::size_t index = 0; index < hash.size(); index += 2)
                hash_bytes.push_back(static_cast<std::uint8_t>((nibble(hash[index]) << 4) | nibble(hash[index + 1])));
            std::cout << "STREAM-HASHES," << seed << ',' << workers << ',' << ticks << ','
                << eawr::core::sha256_hex(hash_bytes) << std::endl;
            std::cout << "STREAM-IDENTITY," << seed << ',' << workers << ',' << ticks << ',' << hashes.back() << std::endl;
            report_effects("stream," + std::to_string(seed) + ',' + std::to_string(workers));
        }
    }
    return failures == 0 ? 0 : 1;
}

int serial_profile(const std::filesystem::path& root, const std::uint64_t ticks,
    const std::vector<std::size_t>& worker_counts) {
    struct QueryReporting {
        QueryReporting() {
            foc::ai::ThreatGrid::observe_queries([](const std::uint64_t tick, const std::size_t keys,
                const foc::ai::ThreatGrid::QueryCounts& counts) noexcept {
                std::cout << "THREAT-COUNTS," << tick << ',' << counts.prepared_keys << ',' << keys << ','
                    << counts.queries << ',' << counts.warm << ',' << counts.cold << ',' << counts.repeats << ','
                    << counts.cross_player << ',' << counts.scalar_hits << '\n';
            });
            foc::ai::Engine::observe_inputs([](const std::uint64_t tick, const foc::ai::Engine::InputCounts& counts) noexcept {
                std::cout << "AI-INPUT-COUNTS," << tick << ',' << counts.view_rows << ',' << counts.producer_entities
                    << ',' << counts.producer_prepasses << ',' << counts.pad_rows << ',' << counts.pad_prepasses
                    << ',' << counts.pad_queries << ',' << counts.late_prepasses << '\n';
            });
        }
        ~QueryReporting() {
            foc::ai::ThreatGrid::observe_queries(nullptr);
            foc::ai::Engine::observe_inputs(nullptr);
        }
    } query_reporting;
    auto content = load(root, false, false, {}, false, {}, {}, {}, false, true);
    if (!content) return 1;
    std::vector<std::string> reference;
    const bool trace_jobs = environment("EAWR_SERIAL_JOB_TRACE").value_or("") == "1";
    for (const bool measure : {false, true}) {
        for (const std::size_t workers : worker_counts) {
            auto object_work = std::make_shared<foc::AiJournal>();
            content->ai.journal = object_work;
            eawr::platform::LiveSession::Options options;
            options.workers = workers;
            options.pacing = eawr::platform::LiveSession::Pacing::driven;
            options.measure_phases = measure;
            auto scripts = std::make_shared<eawr::platform::LiveScripts>();
            scripts->wrap = [&content](tactical::TacticalSession world) {
                return foc::create_session(std::move(world), content->ai, content->modules);
            };
            options.scripts = scripts;
            auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors,
                content->content.durability, content->content.motion, content->content.combat, options,
                content->victory, content->content.fog, content->content.abilities, content->economy,
                content->content.map_box);
            expect(static_cast<bool>(live), "stock serial profile starts");
            if (!live) return 1;
            std::map<std::string, double> totals;
            std::uint64_t samples = 0;
            bool job_rows = false, script_rows = false, sliced_rows = false;
            for (std::uint64_t begin = 0; begin < ticks; begin += 256) {
                const auto end = std::min(ticks, begin + 256);
                live.value()->advance_to(end);
                expect(live.value()->wait_for(end, std::chrono::minutes(10)), "stock serial profile reaches its batch");
                if (live.value()->failure()) { live.value()->stop(); return 1; }
                const auto costs = live.value()->tick_costs_after(begin);
                expect(costs.size() == end - begin, "serial profile retains every measured tick");
                for (const auto& cost : costs) {
                    if (cost.tick <= 150) continue;
                    ++samples;
                    totals["total"] += cost.total_ms;
                    for (const auto& phase : cost.phases) {
                        if (phase.name.starts_with("partition-job.")) {
                            job_rows = true;
                            script_rows = script_rows || phase.name.starts_with("partition-job.script-instances.");
                            sliced_rows = sliced_rows || phase.name.find(".sliced-") != std::string::npos;
                            std::istringstream label(phase.name);
                            std::vector<std::string> fields;
                            for (std::string field; std::getline(label, field, '.');) fields.push_back(std::move(field));
                            expect(fields.size() == 15, "job rows have a fixed identity/has-work/eight-counter schema");
                            if (fields.size() == 15) {
                                expect(!fields[5].empty() && fields[5] != "0", "live job rows retain a nonzero instance/entity identity");
                                expect((fields[6] == "0" && fields[9] == "0") || (fields[6] == "1" && fields[9] != "0"),
                                    "job work flag distinguishes search counters from non-search zeros");
                            }
                        }
                        // Detailed partition rows are available through LiveTickCost;
                        // opt in to harness CSV without investigation tick/worker pins.
                        if (phase.name.starts_with("partition-")) {
                            if (trace_jobs) std::cout << "SERIAL-JOB," << workers << ',' << cost.tick << ','
                                << phase.name << ',' << phase.ms << '\n';
                            continue;
                        }
                        totals[phase.name] += phase.ms;
                    }
                }
            }
            expect(job_rows == measure && script_rows == measure && sliced_rows == measure,
                "stock scenario exercises script and sliced-search job identities only with measurement enabled");
            live.value()->stop();
            for (const auto& work : object_work->costs)
                std::cout << "OBJECT-TURN-COUNTS," << work.tick << ',' << work.object_turn_due_ticks
                    << ',' << work.object_pumps << ',' << work.object_turn_rows << '\n';
            const auto hashes = live.value()->tick_hashes();
            expect(hashes.size() == ticks, "stock serial profile retains every world hash");
            const auto recording = live.value()->record();
            expect(!recording.commands.empty(), "stock serial profile observes active AI commands");
            if (reference.empty()) {
                reference = hashes;
                for (std::size_t index = 0; index < hashes.size(); ++index)
                    std::cout << "SERIAL-HASH," << index + 1 << ',' << hashes[index] << '\n';
            } else expect(hashes == reference, "stock 3v3 hashes are byte-equal across measurement modes and 1/2/4/6/8 workers");
            std::cout << "SERIAL-IDENTITY," << workers << ',' << ticks << ',' << samples << ','
                << recording.commands.size() << ',' << content->start.units.size() << ','
                << (hashes.empty() ? std::string{} : hashes.back()) << ',' << (measure ? 1 : 0) << '\n';
            std::cout.precision(12);
            if (measure) for (const auto& [name, total] : totals)
                std::cout << "SERIAL-PHASE," << workers << ',' << name << ','
                    << (samples ? total / static_cast<double>(samples) : 0.0) << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}

} // namespace foc_ai_test_support
