// Skirmish battle saves in a live session (LiveSession::Options::save_at and the start-from-save
// overload, docs/checkpoint-format.md "Skirmish battle saves"), on the content-free tactical-v2
// corpus battle with a script host. Run A saves at tick T and continues to T+K; run B loads that
// save and continues to T+K. On 1 and 8 workers B's hashes of ticks T+1..T+K and its record equal
// A's, B starts at T without re-running a tick, and its issuers continue past every saved key.
#include "../support/check.hpp"

#include "eawr/checkpoint/skirmish.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace auth = eawr::script::authoritative;
namespace checkpoint = eawr::checkpoint;
namespace tactical = eawr::sim::tactical;
using eawr::platform::LiveSession;
using Bytes = std::vector<std::uint8_t>;

eawr::testing::Checker checker;
void expect(const bool condition, const std::string& message) { checker.check(condition, message, std::cerr); }

eawr::core::Result<auth::ScriptScheduler> scheduler(const std::uint64_t seed) {
    auth::SessionConfig config;
    config.seed = seed;
    config.tick_duration = auth::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    return auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
}

// A script host with no scripts: the live session's save and load path without game data.
std::shared_ptr<const eawr::platform::LiveScripts> host_scripts() {
    using Scripted = auth::ScriptedTacticalSession;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [](tactical::TacticalSession world) -> eawr::core::Result<Scripted> {
        auto fresh = scheduler(world.record().setup.seed);
        if (!fresh) return eawr::core::Result<Scripted>::failure(fresh.error());
        return Scripted::create(std::move(world), std::move(fresh).value());
    };
    scripts->save = [](Scripted& session) {
        const auto setup = session.world().record().setup;
        return checkpoint::save_skirmish_checkpoint(session, checkpoint::default_seats(setup),
            checkpoint::skirmish_identities(setup.content_identity, {}));
    };
    scripts->restore = [](std::span<const std::uint8_t> bytes, const tactical::TacticalSetup& battle,
        const eawr::platform::LiveWorldFactory& world, const eawr::sim::PartitionExecutor& executor)
        -> eawr::core::Result<Scripted> {
        const checkpoint::SkirmishHostFactory host = [&](const tactical::TacticalReplay&)
            -> eawr::core::Result<checkpoint::SkirmishHost> {
            auto fresh = scheduler(battle.seed);
            if (!fresh) return eawr::core::Result<checkpoint::SkirmishHost>::failure(fresh.error());
            return eawr::core::Result<checkpoint::SkirmishHost>::success(checkpoint::SkirmishHost{std::move(fresh).value(), {}});
        };
        auto loaded = checkpoint::load_skirmish_checkpoint(bytes, checkpoint::skirmish_identities(battle.content_identity, {}),
            world, host, executor);
        if (!loaded) return eawr::core::Result<Scripted>::failure(loaded.error());
        if (loaded.value().session.record().setup != battle) {
            eawr::core::Diagnostic error; error.code = "TEST"; error.message = "another battle";
            return eawr::core::Result<Scripted>::failure(std::move(error));
        }
        return eawr::core::Result<Scripted>::success(std::move(loaded.value().session));
    };
    return scripts;
}

LiveSession::Options options_for(const std::size_t workers, std::shared_ptr<const eawr::platform::LiveScripts> scripts) {
    LiveSession::Options options;
    options.workers = workers;
    options.pacing = LiveSession::Pacing::driven;
    options.scripts = std::move(scripts);
    return options;
}

bool run_to(LiveSession& session, const std::uint64_t tick) {
    session.advance_to(tick);
    return session.wait_for(tick, std::chrono::seconds(60));
}

void run(const tactical::TacticalReplay& replay) {
    const tactical::TacticalContent content;
    const auto scripts = host_scripts();
    const std::uint64_t saved_tick = replay.final_tick_count + 2U;
    const std::uint64_t end = saved_tick + 12U;
    std::optional<std::vector<std::string>> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{8}}) {
        const auto label = " on " + std::to_string(workers) + " workers";
        // Run A: the corpus battle's commands as held orders, saved after `saved_tick`.
        auto options = options_for(workers, scripts);
        auto save = std::make_shared<std::optional<eawr::core::Result<Bytes>>>();
        options.save_at = saved_tick;
        options.save_sink = [save](std::uint64_t, eawr::core::Result<Bytes> bytes) { save->emplace(std::move(bytes)); };
        auto first = LiveSession::start(replay.setup, content, options);
        expect(static_cast<bool>(first), "run A starts" + label);
        if (!first) return;
        for (const auto& command : replay.commands)
            static_cast<void>(first.value()->submit({command.key.player_id, command.units, command.payload, command.key.tick}));
        expect(run_to(*first.value(), end), "run A reaches its end" + label);
        first.value()->stop();
        expect(save->has_value() && save->value() && !save->value().value().empty(), "run A hands its save to the sink once" + label);
        if (!save->has_value() || !save->value()) return;
        const auto hashes = first.value()->tick_hashes();
        const auto record = first.value()->record();
        expect(hashes.size() == end && !hashes.back().empty(), "run A hashes every tick" + label);
        if (!reference) reference = hashes;
        expect(hashes == *reference, "saving leaves run A's hashes equal at every worker count" + label);

        // Run B: the same battle loaded from that save.
        auto second = LiveSession::start(save->value().value(), replay.setup, content, options_for(workers, scripts));
        expect(static_cast<bool>(second), "run B loads" + label + (second ? std::string() : ": " + second.error().message));
        if (!second) return;
        expect(second.value()->completed_tick() == saved_tick, "run B starts at the saved tick" + label);
        // Run A keyed the held orders with its own sequences; the keys before the saved tick are the saved ones
        // (the step after the save takes the saved tick's own commands).
        std::size_t saved_keys = 0;
        for (const auto& command : record.commands) {
            if (command.key.tick >= saved_tick) continue;
            ++saved_keys;
            expect(second.value()->next_sequence(command.key.player_id) > command.key.sequence,
                "run B's issuers continue past every saved key" + label);
        }
        expect(saved_keys > 0, "run A recorded commands before its save" + label);
        expect(run_to(*second.value(), end), "run B reaches the same end" + label);
        second.value()->stop();
        const auto loaded = second.value()->tick_hashes();
        expect(loaded.size() == hashes.size(), "run B indexes its hashes by tick" + label);
        bool before_empty = loaded.size() == hashes.size();
        bool after_equal = loaded.size() == hashes.size();
        for (std::size_t index = 0; index < loaded.size() && index < hashes.size(); ++index) {
            if (index < saved_tick) before_empty = before_empty && loaded[index].empty();
            else after_equal = after_equal && loaded[index] == hashes[index];
        }
        expect(before_empty, "run B knows no hash before the saved tick" + label);
        expect(after_equal, "run B's hashes after the saved tick equal run A's" + label);
        expect(second.value()->record() == record, "run B records the whole battle as run A did" + label);
        std::cout << "live save" << label << ": saved tick " << saved_tick << ", " << save->value().value().size()
                  << " bytes, ticks " << saved_tick + 1 << ".." << end << " equal\n";
    }

    // A player's save (request_save, #3989): taken on the simulation thread between two ticks while the
    // battle is paused, at the paused tick; loading it continues with run A's hashes.
    for (const std::size_t workers : {std::size_t{1}, std::size_t{8}}) {
        const auto label = " on " + std::to_string(workers) + " workers";
        struct Taken {
            std::mutex mutex;
            std::optional<std::pair<std::uint64_t, eawr::core::Result<Bytes>>> result;
            std::thread::id thread;
        };
        auto taken = std::make_shared<Taken>();
        const auto wait_taken = [&] {
            for (int attempt = 0; attempt < 6000; ++attempt) {
                {
                    const std::lock_guard lock(taken->mutex);
                    if (taken->result) return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };
        auto session = LiveSession::start(replay.setup, content, options_for(workers, scripts));
        if (session) {
            for (const auto& command : replay.commands)
                static_cast<void>(session.value()->submit({command.key.player_id, command.units, command.payload, command.key.tick}));
        }
        if (!session || !run_to(*session.value(), saved_tick)) { expect(false, "the requested-save battle runs" + label); return; }
        session.value()->set_paused(true);
        session.value()->request_save([taken](const std::uint64_t tick, eawr::core::Result<Bytes> bytes) {
            const std::lock_guard lock(taken->mutex);
            taken->result.emplace(tick, std::move(bytes));
            taken->thread = std::this_thread::get_id();
        });
        expect(wait_taken(), "a paused battle takes the requested save" + label);
        expect(session.value()->completed_tick() == saved_tick, "the requested save runs no tick" + label);
        session.value()->stop();
        const std::lock_guard lock(taken->mutex);
        if (!taken->result || !taken->result->second) { expect(false, "the requested save succeeds" + label); return; }
        expect(taken->result->first == saved_tick, "the requested save names the paused tick" + label);
        expect(taken->thread != std::this_thread::get_id(), "the requested save runs on the simulation thread" + label);
        auto loaded = LiveSession::start(taken->result->second.value(), replay.setup, content, options_for(workers, scripts));
        if (!loaded) { expect(false, "the requested save loads" + label + ": " + loaded.error().message); return; }
        expect(run_to(*loaded.value(), end), "the requested save continues" + label);
        loaded.value()->stop();
        const auto continued = loaded.value()->tick_hashes();
        bool equal = reference && continued.size() == reference->size();
        for (std::size_t index = saved_tick; equal && index < continued.size(); ++index) equal = continued[index] == (*reference)[index];
        expect(equal, "the requested save continues with run A's hashes" + label);
    }
    {
        // A battle without the AI host's save reports why on the simulation thread instead of saving.
        auto session = LiveSession::start(replay.setup, content, options_for(1, nullptr));
        auto refused = std::make_shared<std::optional<eawr::core::Result<Bytes>>>();
        auto guard = std::make_shared<std::mutex>();
        if (session && run_to(*session.value(), 2)) {
            session.value()->request_save([refused, guard](std::uint64_t, eawr::core::Result<Bytes> bytes) {
                const std::lock_guard lock(*guard);
                refused->emplace(std::move(bytes));
            });
            bool reported = false;
            for (int attempt = 0; attempt < 6000 && !reported; ++attempt) {
                { const std::lock_guard lock(*guard); reported = refused->has_value(); }
                if (!reported) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            session.value()->stop();
            expect(reported && !refused->value(), "a requested save of a battle without an AI host reports a failure");
        } else {
            expect(false, "the unscripted requested-save battle runs");
        }
    }

    // Refusals: a save needs a solo scripted session, and a load needs the restore and the same battle.
    {
        auto options = options_for(1, scripts);
        options.save_at = 0;
        expect(!LiveSession::start(replay.setup, content, options), "a save tick of 0 is refused");
        options.save_at = 3;
        options.step_driver = [](eawr::platform::LiveStepContext&, std::span<const tactical::PlayerCommand>, std::uint64_t) {
            return eawr::core::Result<bool>::success(false);
        };
        expect(!LiveSession::start(replay.setup, content, options), "a save under a step driver is refused");
    }
    {
        auto options = options_for(1, scripts);
        auto save = std::make_shared<std::optional<eawr::core::Result<Bytes>>>();
        options.save_at = 2;
        options.save_sink = [save](std::uint64_t, eawr::core::Result<Bytes> bytes) { save->emplace(std::move(bytes)); };
        auto session = LiveSession::start(replay.setup, content, options);
        if (!session || !run_to(*session.value(), 3)) { expect(false, "the refusal battle runs"); return; }
        session.value()->stop();
        if (!save->has_value() || !save->value()) { expect(false, "the refusal battle saves"); return; }
        const auto& bytes = save->value().value();
        auto without = scripts;
        auto bare = std::make_shared<eawr::platform::LiveScripts>(*scripts);
        bare->restore = {};
        expect(!LiveSession::start(bytes, replay.setup, content, options_for(1, bare)), "a load without a restore is refused");
        auto other = replay.setup;
        ++other.seed;
        expect(!LiveSession::start(bytes, other, content, options_for(1, without)), "a save of another battle is refused");
        Bytes damaged = bytes;
        damaged.resize(damaged.size() / 2);
        expect(!LiveSession::start(damaged, replay.setup, content, options_for(1, without)), "a damaged save is refused");

        // A battle without scripts cannot save; the sink is told why.
        auto plain = options_for(1, nullptr);
        auto refused = std::make_shared<std::optional<eawr::core::Result<Bytes>>>();
        plain.save_at = 1;
        plain.save_sink = [refused](std::uint64_t, eawr::core::Result<Bytes> result) { refused->emplace(std::move(result)); };
        auto unscripted = LiveSession::start(replay.setup, content, plain);
        if (unscripted && run_to(*unscripted.value(), 2)) {
            unscripted.value()->stop();
            expect(refused->has_value() && !refused->value(), "a battle without an AI host reports that it cannot save");
        } else {
            expect(false, "the unscripted battle runs");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: live_save_tests <replay fixture directory>\n";
        return 2;
    }
    const auto path = std::filesystem::path(argv[1]) / "tactical-v2.eawr-replay";
    std::ifstream file(path, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto replay = tactical::parse_replay(bytes, path.string());
    expect(static_cast<bool>(replay), "the tactical-v2 fixture parses");
    if (replay) run(replay.value());
    if (checker.failures == 0) std::cout << "live save tests passed\n";
    return checker.finish();
}
