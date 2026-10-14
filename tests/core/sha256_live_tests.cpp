#include "eawr/core/sha256.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
namespace core = eawr::core;
namespace platform = eawr::platform;
namespace tactical = eawr::sim::tactical;
namespace auth = eawr::script::authoritative;

template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
double elapsed_us(const Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
std::string digest_rows(const std::vector<std::string>& rows) {
    std::string bytes;
    for (const auto& row : rows) bytes += row;
    return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
std::shared_ptr<platform::LiveScripts> scripts() {
    auto result = std::make_shared<platform::LiveScripts>();
    result->wrap = [](tactical::TacticalSession world) {
        auth::ModuleManifest modules;
        take(modules.add("HashFixture.lua", "Saved = {}; for i = 1, 512 do Saved[i] = i end"));
        auth::SessionConfig config;
        config.seed = 66;
        config.tick_duration = {1, 30};
        auto scheduler = take(auth::ScriptScheduler::create(config, std::move(modules)));
        for (std::uint64_t id = 1; id <= 8; ++id) take(scheduler.create_instance(id, "HashFixture.lua"));
        return auth::ScriptedTacticalSession::create(std::move(world), std::move(scheduler));
    };
    return result;
}
}

int main(const int argc, const char* const argv[]) {
    try {
        const bool profile = argc == 2 && std::string_view(argv[1]) == "--profile";
        const std::uint64_t ticks = profile ? 512U : 32U;
        tactical::TacticalSetup setup;
        setup.seed = 66;
        setup.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable}};
        for (std::uint64_t id = 1; id <= 256; ++id) {
            tactical::UnitState unit;
            unit.entity_id = id; unit.type_id = 1; unit.owner = id <= 128 ? 1U : 2U;
            unit.position.x = eawr::sim::math::Fixed::from_raw(static_cast<std::int64_t>(id) * 1000);
            unit.rotation = eawr::sim::math::identity_quat();
            setup.units.push_back(unit);
        }
        auto reference = take(tactical::TacticalSession::create(setup));
        const auto canonical = reference.canonical_state_bytes();
        const auto expected = core::sha256_hex(canonical, core::Sha256Backend::scalar);
        double hash_us{};
        for (std::uint64_t i = 0; i < ticks; ++i) {
            const auto start = Clock::now();
            const auto actual = core::sha256_hex(canonical);
            hash_us += elapsed_us(start);
            check(actual == expected, "canonical world compression changed digest");
        }
        platform::ThreadStateHasher hasher(4);
        std::vector<eawr::sim::StateHash> pending;
        pending.reserve(static_cast<std::size_t>(ticks));
        double submit_us{}, max_submit_us{};
        const auto queue_start = Clock::now();
        for (std::uint64_t i = 0; i < ticks; ++i) {
            const auto start = Clock::now();
            pending.push_back(hasher.hash(canonical));
            const auto cost = elapsed_us(start);
            submit_us += cost; max_submit_us = std::max(max_submit_us, cost);
        }
        const auto drain_start = Clock::now();
        for (const auto& hash : pending) check(hash.get() == expected, "live queue digest changed");
        const auto drain_us = elapsed_us(drain_start);
        const auto queue_us = elapsed_us(queue_start);
        const auto features = core::sha256_cpu_features();
        std::cout << "{\"backend\":\"" << core::sha256_backend_name(core::sha256_backend())
            << "\",\"sse2\":" << features.sse2 << ",\"ssse3\":" << features.ssse3 << ",\"sha\":" << features.sha
            << ",\"canonical_bytes\":" << canonical.size() << ",\"canonical_digest\":\"" << expected
            << "\",\"ticks\":" << ticks << ",\"queue_backlog\":4,\"world_hash_us\":" << hash_us / static_cast<double>(ticks)
            << ",\"queue_submit_us\":" << submit_us / static_cast<double>(ticks) << ",\"queue_max_submit_us\":" << max_submit_us
            << ",\"queue_drain_us\":" << drain_us << ",\"queue_us_per_hash\":" << queue_us / static_cast<double>(ticks)
            << ",\"runs\":[";
        std::vector<std::string> baseline_world, baseline_lan;
        bool first = true;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            for (const bool lan : {false, true}) {
                platform::LiveSession::Options options;
                options.workers = workers;
                options.pacing = platform::LiveSession::Pacing::driven;
                options.scripts = scripts();
                std::vector<std::string> lan_rows;
                double callback_us{};
                if (lan) {
                    options.step_driver = [&](platform::LiveStepContext& context,
                        std::span<const tactical::PlayerCommand>, std::uint64_t) {
                        auto stepped = context.step();
                        if (!stepped) return core::Result<bool>::failure(stepped.error());
                        const auto start = Clock::now();
                        lan_rows.push_back(context.state_hash());
                        callback_us += elapsed_us(start);
                        return core::Result<bool>::success(true);
                    };
                }
                auto live = take(platform::LiveSession::start(setup, {}, {}, {}, {}, options));
                const auto start = Clock::now();
                live->advance_to(ticks);
                check(live->wait_for(ticks, std::chrono::minutes(2)), "live fixture timed out");
                live->stop();
                const auto total_us = elapsed_us(start);
                check(!live->failure(), "live fixture failed");
                const auto hashes = live->tick_hashes();
                check(hashes.size() == ticks, "live fixture lost a tick hash");
                if (baseline_world.empty()) baseline_world = hashes;
                check(hashes == baseline_world, "world rows differ across live/LAN or 1/2/4/8");
                if (lan) {
                    check(lan_rows.size() == ticks, "LAN fixture lost a per-tick digest");
                    if (baseline_lan.empty()) baseline_lan = lan_rows;
                    check(lan_rows == baseline_lan, "LAN world+script rows differ across 1/2/4/8");
                }
                double step_ms{};
                const auto costs = live->tick_costs_after(0);
                for (const auto& cost : costs) step_ms += cost.total_ms;
                const auto script_report = live->script_report();
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"workers\":" << workers << ",\"lan\":" << lan
                    << ",\"step_ms\":" << step_ms / static_cast<double>(costs.size())
                    << ",\"elapsed_us_per_tick\":" << total_us / static_cast<double>(ticks)
                    << ",\"callback_us\":" << callback_us / static_cast<double>(ticks)
                    << ",\"units\":256,\"instances\":8,\"projectiles\":0,\"instructions\":" << script_report.total_instructions
                    << ",\"world_rows_digest\":\"" << digest_rows(hashes)
                    << "\",\"lan_rows_digest\":\"" << digest_rows(lan_rows) << "\"}";
            }
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
