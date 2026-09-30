// Per-tick cost of the authoritative script service (#247, budget from #246):
// the representative scenario (scenario.hpp) with N instances, stepped at
// 30 Hz, timed per tick. Prints median, p99 and maximum service time per
// worker count and exits 1 when two worker counts disagree on any tick's
// state digest or command stream.
//
//   lua_script_bench [--instances 64] [--ticks 3000] [--warmup 30] [--workers 1,4] [--seed n] [--quick]

#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "eawr/sim/world.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace auth = eawr::script::authoritative;
namespace b64 = eawr::script::numeric::binary64;
using eawr::script::numeric::LuaNumber;

struct Options {
    int instances = 64;
    int ticks = 3000;
    int warmup = 30;
    std::uint64_t seed = 0x5EED;
    std::vector<std::size_t> workers{1, 4};
};

std::int64_t whole(const auth::Value& value) {
    const auto* data = std::get_if<LuaNumber>(&value.data);
    return data == nullptr ? 0 : b64::to_int64_truncate(data->repr);
}

LuaNumber integer(std::int64_t value) { return LuaNumber::from_repr(b64::from_int64(value)); }

void register_bindings(auth::ScriptScheduler& scheduler) {
    auto ok = [](eawr::core::Result<void> result) {
        if (!result) throw std::runtime_error(result.error().message);
    };
    ok(scheduler.register_binding("Test_Report", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        context.issue_command("report", arguments);
        return eawr::core::Result<auth::ValueList>::success({});
    }));
    ok(scheduler.register_binding("Test_Random", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        const std::int64_t bound = arguments.empty() ? 0 : whole(arguments.front());
        const std::uint64_t word = context.random().next_below(static_cast<std::uint64_t>(bound));
        return eawr::core::Result<auth::ValueList>::success(
            {auth::Value::number(integer(static_cast<std::int64_t>(bound == 0 ? (word >> 12) : word)))});
    }));
    ok(scheduler.register_binding("Test_Unit", [](auth::BindingContext& context, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value::number(context.random().next_unit())});
    }));
    ok(scheduler.register_binding("Test_Handle", [](auth::BindingContext&, const auth::ValueList& arguments) {
        auth::Handle handle{static_cast<std::uint32_t>(whole(arguments.at(0))), static_cast<std::uint64_t>(whole(arguments.at(1)))};
        return eawr::core::Result<auth::ValueList>::success({auth::Value{handle}});
    }));
}

struct Run {
    std::vector<double> milliseconds;
    std::vector<std::string> digests;
    std::uint64_t commands{};
};

Run run(const Options& options, const eawr::sim::PartitionExecutor& executor) {
    auth::ModuleManifest manifest;
    static_cast<void>(manifest.add("Library/Library.lua", auth::test::scenario_library));
    static_cast<void>(manifest.add("Ai.lua", auth::test::scenario_ai));
    auth::SessionConfig config;
    config.seed = options.seed;
    config.tick_duration = auth::TickDuration{1, 30};
    config.script_directories = {"Library/"};
    auto created = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!created) throw std::runtime_error(created.error().message);
    auth::ScriptScheduler scheduler = std::move(created).value();
    register_bindings(scheduler);
    for (int index = 1; index <= options.instances; ++index) {
        auto instance = scheduler.create_instance(static_cast<std::uint64_t>(index), "Ai.lua");
        if (!instance) throw std::runtime_error(instance.error().message);
    }
    Run result;
    std::uint64_t sequence = 0;
    for (int tick = 1; tick <= options.warmup + options.ticks; ++tick) {
        for (int index = 1; index <= options.instances; ++index) {
            if (tick % 7 != index % 7) continue;
            auth::ScriptEvent event;
            event.key = auth::EventKey{static_cast<std::uint64_t>(tick), 30, static_cast<std::uint64_t>(index), sequence++};
            event.target = static_cast<std::uint64_t>(index);
            event.name = "attacked";
            event.arguments = {auth::Value{auth::Handle{7, static_cast<std::uint64_t>(index % 24)}}, auth::Value::number(integer(tick))};
            static_cast<void>(scheduler.submit_event(std::move(event)));
        }
        const auto start = std::chrono::steady_clock::now();
        auto report = scheduler.service(executor);
        const auto stop = std::chrono::steady_clock::now();
        if (!report) throw std::runtime_error(report.error().message);
        if (!report.value().diagnostics.empty()) throw std::runtime_error(report.value().diagnostics.front().message);
        result.commands += report.value().commands.size();
        if (tick > options.warmup) {
            result.milliseconds.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
            if (tick % 30 == 0) result.digests.push_back(scheduler.state_digest());
        }
    }
    return result;
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

std::vector<std::size_t> parse_workers(std::string_view text) {
    std::vector<std::size_t> workers;
    std::stringstream stream{std::string(text)};
    std::string item;
    while (std::getline(stream, item, ',')) {
        workers.push_back(item == "hardware" ? eawr::platform::ThreadWorkerAdapter::hardware_worker_count() : std::stoul(item));
    }
    return workers;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        const auto value = [&]() -> std::string { return index + 1 < argc ? argv[++index] : ""; };
        if (flag == "--instances") {
            options.instances = std::stoi(value());
        } else if (flag == "--ticks") {
            options.ticks = std::stoi(value());
        } else if (flag == "--warmup") {
            options.warmup = std::stoi(value());
        } else if (flag == "--seed") {
            options.seed = std::stoull(value());
        } else if (flag == "--workers") {
            options.workers = parse_workers(value());
        } else if (flag == "--quick") {
            options.instances = 8;
            options.ticks = 60;
            options.warmup = 5;
        } else {
            std::cerr << "usage: lua_script_bench [--instances n] [--ticks n] [--warmup n] [--workers 1,4,hardware] [--seed n] [--quick]\n";
            return 2;
        }
    }
    try {
        std::printf("instances %d, ticks %d (+%d warm-up), 30 Hz\n", options.instances, options.ticks, options.warmup);
        std::printf("%-8s %12s %12s %12s %10s\n", "workers", "median ms", "p99 ms", "max ms", "commands");
        std::vector<std::string> reference;
        bool agree = true;
        for (const std::size_t workers : options.workers) {
            const eawr::platform::ThreadWorkerAdapter pool(workers);
            const Run result = run(options, pool);
            std::printf("%-8zu %12.3f %12.3f %12.3f %10llu\n", workers, percentile(result.milliseconds, 0.5),
                        percentile(result.milliseconds, 0.99), percentile(result.milliseconds, 1.0),
                        static_cast<unsigned long long>(result.commands));
            if (reference.empty()) {
                reference = result.digests;
            } else if (result.digests != reference) {
                agree = false;
                std::printf("worker count %zu disagrees with the first run\n", workers);
            }
        }
        return agree ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "lua_script_bench: " << error.what() << '\n';
        return 1;
    }
}
