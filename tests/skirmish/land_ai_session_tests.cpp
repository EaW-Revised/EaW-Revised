// #3207 G1-b: the FoC AI host mounted on a LAND tactical session (domain land), fixture content only.
// Pins the determinism the design asks for (out/ls/land-ai-g1-design.md section 5, items 5 and 6):
// the same AI state digest and the same freestore progress on 1/2/4/8 workers and in reverse partition
// order, and a checkpoint taken mid-run that restores into a fresh host and continues hash-identically.
// The freestore is a counting fixture script; the goal system's XML is not mounted (the mounted-data
// selection by domain is covered by foc_ai_contract_bindings_tests).
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "land_test_support.hpp"
#include "../support/check.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace foc = script::foc;
namespace auth = script::authoritative;
using namespace sim::tactical;

testing::Checker checks;
void expect(bool ok, const std::string& message) { checks.check(ok, message.c_str(), std::cerr); }

// Two players, no units or companies, so the land movement phase has nothing to expand and the
// session exercises the AI host's own phase beside the land world.
TacticalContent content() {
    TacticalContent result;
    result.domain = BattleDomain::land;
    // The land host requires a terrain field (L1), so the session is built on an open synthetic one.
    result.land_terrain = land_test::make_field(std::vector<std::string>(8, std::string(8, '.'))).terrain;
    result.land.clear_bit = land_test::clear;
    result.land.contacts = {4096, 1, sim::math::Fixed::from_ratio(1, 3).value(), land_test::whole(1)};
    return result;
}
TacticalSetup setup() {
    TacticalSetup result;
    result.domain = BattleDomain::land;
    result.seed = 3207;
    result.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    return result;
}
foc::AiSetup ai_setup() {
    foc::AiSetup result;
    result.domain = BattleDomain::land;
    result.players = {{1, "SYNTHETIC", false, true, ""}, {2, "SYNTHETIC", false, true, ""}};
    result.freestore_module = "LandProbe.lua";
    return result;
}
const std::map<std::string, std::string>& modules() {
    static const std::map<std::string, std::string> value{
        {"LandProbe.lua", "function Base_Definitions() ServiceRate=0; UnitServiceRate=0; pumps=0 end\n"
            "function main() while true do pumps=pumps+1; coroutine.yield(true) end end"},
        {"data/scripts/library/PGAICommands.lua",
            "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
    return value;
}

struct Run { std::vector<std::string> hashes; std::vector<std::string> world_hashes; };

core::Result<auth::ScriptedTacticalSession> fresh() {
    auto world = TacticalSession::create(setup(), content());
    if (!world) {
        std::cerr << "land world: " << world.error().code << ": " << world.error().message << '\n';
        return core::Result<auth::ScriptedTacticalSession>::failure(world.error());
    }
    auto wrapped = foc::create_session(std::move(world).value(), ai_setup(), modules());
    if (!wrapped) std::cerr << "land AI host: " << wrapped.error().code << ": " << wrapped.error().message << '\n';
    return wrapped;
}

// Steps `frames` ticks; with `restore_at` the session is saved there, rebuilt into a fresh host and continued.
bool run(const sim::PartitionExecutor& executor, const unsigned frames, const unsigned restore_at, Run& out) {
    auto session = fresh();
    expect(session.has_value(), "G1-b: land session wraps with the AI host");
    if (!session) return false;
    for (unsigned frame = 1; frame <= frames; ++frame) {
        auto tick = session.value().step(executor);
        expect(tick.has_value(), "G1-b: land AI session steps, frame=" + std::to_string(frame));
        if (!tick) return false;
        out.hashes.push_back(tick.value().state_sha256);
        out.world_hashes.push_back(tick.value().world.state_sha256);
        if (frame != restore_at) continue;
        auto saved = session.value().save_scripts();
        auto checkpoint = session.value().world().save_checkpoint_state();
        auto land_section = session.value().world().save_land_checkpoint_state();
        auto rebuilt_world = TacticalSession::create(setup(), content());
        auto rebuilt_ai = foc::prepare_session(ai_setup(), modules());
        // The land world checkpoints in two sections: the shared child state, then the land section.
        expect(saved && checkpoint && land_section && rebuilt_world && rebuilt_ai
                && rebuilt_world.value().load_checkpoint_state(checkpoint.value())
                && rebuilt_world.value().load_land_checkpoint_state(land_section.value()),
            "G1-b: the land world and AI host rebuild from a checkpoint");
        if (!saved || !checkpoint || !land_section || !rebuilt_world || !rebuilt_ai) return false;
        auto rebuilt = auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(),
            std::move(rebuilt_ai.value().scripts), saved.value());
        expect(rebuilt && rebuilt_ai.value().bind(rebuilt.value()), "G1-b: the land AI host binds to the restored world");
        if (!rebuilt) return false;
        expect(rebuilt.value().save_scripts().value().native == saved.value().native,
            "G1-b: the restored land AI state round trips byte for byte");
        session = std::move(rebuilt);
    }
    return true;
}

void worker_counts_and_reverse_order() {
    constexpr unsigned frames = 12;
    Run golden;
    const platform::ThreadWorkerAdapter serial(1);
    if (!run(serial, frames, 0, golden)) return;
    for (const unsigned workers : {2U, 4U, 8U}) {
        Run other;
        const platform::ThreadWorkerAdapter executor(workers);
        if (!run(executor, frames, 0, other)) return;
        expect(other.hashes == golden.hashes && other.world_hashes == golden.world_hashes,
            "G1-b: land AI state digests agree on " + std::to_string(workers) + " workers");
    }
    Run reversed;
    const test_support::ReverseOrderExecutor reverse(4);
    if (!run(reverse, frames, 0, reversed)) return;
    expect(reversed.hashes == golden.hashes && reversed.world_hashes == golden.world_hashes,
        "G1-b: land AI state digests agree in reverse partition order");
}

void save_and_restore() {
    constexpr unsigned frames = 12;
    const platform::ThreadWorkerAdapter executor(2);
    Run straight;
    if (!run(executor, frames, 0, straight)) return;
    for (const unsigned at : {1U, 3U, 7U}) {
        Run resumed;
        if (!run(executor, frames, at, resumed)) return;
        expect(resumed.hashes == straight.hashes && resumed.world_hashes == straight.world_hashes,
            "G1-b: a land AI checkpoint at frame " + std::to_string(at) + " continues identically");
    }
}

void freestore_progress() {
    const platform::ThreadWorkerAdapter executor(1);
    auto session = fresh();
    expect(session.has_value(), "G1-b: land session wraps with the AI host (progress)");
    if (!session) return;
    for (unsigned frame = 1; frame <= 6; ++frame) {
        auto tick = session.value().step(executor);
        if (!tick) { expect(false, "G1-b: land AI session steps (progress)"); return; }
    }
    auto saved = session.value().save_scripts();
    auto inspector = foc::prepare_session(ai_setup(), modules());
    expect(saved && inspector && inspector.value().scripts.load(saved.value().scheduler), "G1-b: land freestore state inspects");
    if (!saved || !inspector) return;
    for (unsigned id = 1; id <= 2; ++id) {
        auto count = inspector.value().scripts.read_global(foc::freestore_instance(id), "pumps");
        expect(count && count.value(), "G1-b: each land AI player's freestore instance has run");
    }
}

} // namespace

int main() {
    try {
        worker_counts_and_reverse_order();
        save_and_restore();
        freestore_progress();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return checks.finish();
}
