#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "../support/check.hpp"
#include "../support/result.hpp"
// WGA-23/24/99 and WGEP-28/43/46: native world reconstruction consumes journaled
// admissions and billing outcomes, independently of native AI initialization.
#include "galactic_ai_actions.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <exception>
using eawr::testing::must;

namespace gc = eawr::sim::galactic;
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
using eawr::sim::math::Fixed;
namespace {

void expect(bool value, std::string_view message) {
    if (!value) eawr::testing::require(value, message);
}
Fixed f(std::int64_t value) { return must(Fixed::from_integer(value)); }
struct DiskJournal {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("eawr-economic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".replay");
    ~DiskJournal() { std::error_code error; std::filesystem::remove(path, error); }
};
gc::GalacticSetup setup() {
    gc::GalacticSetup out;
    out.players = {{1, 1, 0}}; out.planets = {{10, {}, 1}};
    out.player_economy = gc::PlayerEconomySetup{{{1, f(200), f(250), 2, 5}}};
    out.fiscal = gc::FiscalSetup{f(2500), {{1, true, f(1)}}, {{10, {}}}};
    out.rules.fiscal_cycle_frames = 10000;
    out.rules.maximum_fleet_movement_distance = Fixed{};
    out.fleet_control = gc::FleetControlSetup{{}, {{100, false, true, false}}, {}};
    out.production = gc::ProductionSetup{5, {{1, false, true, true, f(50), {}, {}, {}, {}}}, {}};
    gc::PlanetProduction planet; planet.planet = 10;
    planet.target_owner = planet.starbase_owner = planet.ground_occupier = 1;
    planet.ground_slots = planet.space_slots = 3; planet.maximum_ground_forces = 10;
    planet.enabled = planet.surface_accessible = true;
    gc::ProductionOffer offer; offer.type = 100; offer.price = f(100); offer.seconds = f(1);
    offer.tech = 2; offer.affiliated = offer.direct_build_allowed = true;
    planet.offers = {offer}; out.production->planets = {planet};
    return out;
}
std::vector<std::string> world_service_ticks(const eawr::sim::PartitionExecutor& executor) {
    auto live = must(gc::GalacticSession::create(setup()));
    auto funding = std::make_shared<ai::GalacticFunding>();
    const std::array policy{ai::EvaluatedFundingTemplate{1, {{1, f(1)}}, {}}};
    must(funding->initialize(1, policy, f(200))); must(funding->reserve(1, {7, 1, f(100), {}, {}, {}}));
    ga::NativeActions actions(live, funding, {}, {{1, f(1)}}, {}, &executor,
        [](auto) { return eawr::core::Result<Fixed>::success(f(100)); });
    actions.bind_handlers();
    ga::ActionRequest request; request.kind = ga::ActionKind::produce; request.player = 1; request.target = 10; request.type = 100;
    expect(must(actions.admit_paid(request, 7)).admitted, "tick comparison starts with real paid native admission");
    std::vector<std::string> ticks;
    for (unsigned frame = 0; frame < 35; ++frame) {
        const auto tick = must(live.step(executor));
        expect(tick.state_hash.get() == live.state_sha256() && tick.state_sha256 == live.state_sha256(),
            "live returned tick hash describes its completed publication");
        ticks.push_back(tick.state_hash.get());
    }
    const auto replay = live.record();
    expect(std::any_of(replay.native_effects.begin(), replay.native_effects.end(), [](const auto& effect) {
        return effect.kind == gc::NativeEffectKind::production_payment && effect.phase == gc::NativeEffectPhase::world_service;
    }), "tick comparison includes journaled world-service payment effects");
    auto restored = must(gc::GalacticSession::from_replay(replay));
    ga::NativeActions::bind_economic_replay_owners(restored);
    for (std::size_t frame = 0; frame < ticks.size(); ++frame) {
        const auto tick = must(restored.step(executor));
        expect(tick.completed_tick == frame + 1 && tick.state_hash.get() == ticks[frame]
            && tick.state_sha256 == ticks[frame] && restored.state_sha256() == ticks[frame],
            "every replay tick, including world-service native effects, equals the corresponding live publication");
    }
    must(restored.finish_native_replay());
    return ticks;
}
gc::GalacticSession reconstruct(const gc::GalacticReplay& replay, const eawr::sim::PartitionExecutor& executor) {
    auto world = must(gc::GalacticSession::from_replay(replay));
    ga::NativeActions::bind_economic_replay_owners(world);
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) (void)must(world.step(executor));
    must(world.replay_native_barrier(executor)); must(world.finish_native_replay());
    return world;
}
}
int case_galactic_ai_economic_replay_tests() {
    try {
        std::vector<std::string> tick_baseline;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(count); const auto ticks = world_service_ticks(executor);
            if (tick_baseline.empty()) tick_baseline = ticks;
            else expect(ticks == tick_baseline, "world-service tick sequence is identical on 1/2/4/8");
        }
        std::array<std::string, 3> baseline;
        for (const auto count : {1U, 2U, 4U, 8U}) for (const unsigned mode : {0U, 1U, 2U}) {
            const bool cancel = mode == 1;
            eawr::platform::ThreadWorkerAdapter executor(count);
            auto world = must(gc::GalacticSession::create(setup()));
            auto funding = std::make_shared<ai::GalacticFunding>();
            const std::array policy{ai::EvaluatedFundingTemplate{1, {{1, f(1)}}, {}}};
            must(funding->initialize(1, policy, f(200))); must(funding->reserve(1, {7, 1, f(100), {}, {}, {}}));
            ga::NativeActions actions(world, funding, {}, {{1, f(1)}}, {}, &executor,
                [](auto) { return eawr::core::Result<Fixed>::success(f(100)); }); actions.bind_handlers();
            ga::ActionRequest request; request.kind = ga::ActionKind::produce; request.player = 1; request.target = 10; request.type = 100;
            const auto accepted = must(actions.admit_paid(request, 7)); expect(accepted.admitted, "live paid admission accepted");
            if (mode == 2) {
                auto state = funding->state(); state.at(1).goals.at(7).capital = {};
                must(funding->restore(std::move(state)));
            }
            for (unsigned frame = 0; frame < (cancel ? 15U : 35U); ++frame) (void)must(world.step(executor));
            if (cancel) must(actions.finish_task(accepted.request, false));
            ga::ActionRequest cash; cash.kind = ga::ActionKind::give_money; cash.player = 1; cash.amount = ai::real(500);
            expect(must(actions.admit(cash)).admitted, "live cash clamps through account owner");
            gc::GalacticReplay replay{world.setup(), world.snapshot()->completed_tick(), {},
                std::vector<gc::NativeEffect>(world.native_effects().begin(), world.native_effects().end())};
            expect(std::any_of(replay.native_effects.begin(), replay.native_effects.end(), [](const auto& effect) {
                return effect.kind == gc::NativeEffectKind::production_payment && effect.phase == gc::NativeEffectPhase::world_service;
            }), "journal contains actual world-service payment callbacks");
            if (mode == 2) expect(actions.purchases().at(accepted.request).billed == Fixed{}, "refused installments retain the real production timer");
            const auto encoded = must(gc::write_replay(replay));
            const DiskJournal disk;
            { std::ofstream output(disk.path, std::ios::binary); output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
                output.close(); expect(static_cast<bool>(output), "economic journal writes an actual disk file"); }
            std::ifstream input(disk.path, std::ios::binary);
            const std::vector<std::uint8_t> from_disk{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            expect(from_disk == encoded, "economic journal reads all bytes from disk");
            const auto decoded = must(gc::parse_replay(from_disk));
            expect(decoded == replay && must(gc::write_replay(decoded)) == encoded, "economic replay envelope preserves canonical journal bytes");
            auto restored = reconstruct(decoded, executor);
            expect(restored.state_sha256() == world.state_sha256(), "economic replay reconstructs exact world with no AI/funding/RNG/Lua owner");
            const auto index = static_cast<std::size_t>(mode); const auto trace = world.state_sha256();
            if (baseline[index].empty()) baseline[index] = trace; else expect(baseline[index] == trace, "economic journal continuation equals on 1/2/4/8");
            auto malformed = decoded; malformed.native_effects.front().payload.push_back(0);
            bool refused{};
            try { (void)reconstruct(malformed, executor); } catch (const std::exception&) { refused = true; }
            expect(refused, "economic owner refuses trailing payload without native initialization");
            malformed = decoded;
            const auto payment = std::find_if(malformed.native_effects.begin(), malformed.native_effects.end(), [](const auto& effect) { return effect.kind == gc::NativeEffectKind::production_payment; });
            payment->payload.resize(8); refused = false;
            try { (void)reconstruct(malformed, executor); } catch (const std::exception&) { refused = true; }
            expect(refused, "economic owner refuses truncated producer payment");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
