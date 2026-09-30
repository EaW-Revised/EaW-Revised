// Load-budget benchmark for the authoritative Lua numeric profile (#246).
//
//   lua_numeric_bench [--ticks N] [--quick]
//
// Prints (1) nanoseconds per soft-float operation next to the same hardware
// operation, and (2) per-tick Lua time of the synthetic FoC-shaped workload
// (bench_workload.hpp) on the soft-float VM and on a benchmark-only
// hardware-double twin of the same VM: median, p99 and maximum per 30 Hz tick.
// This program is a measuring tool; it is not on the authoritative path.

#include "bench_vm.hpp"

#include "eawr/script/numeric/binary64.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace b64 = eawr::script::numeric::binary64;
namespace decimal = eawr::script::numeric::decimal;

std::uint64_t splitmix(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

// Script-like magnitudes: exponents within 2^-20 .. 2^20, both signs.
std::vector<std::uint64_t> typical_values(std::size_t count, std::uint64_t seed) {
    std::vector<std::uint64_t> values(count);
    for (auto& value : values) {
        const std::uint64_t bits = splitmix(seed);
        value = (bits & 0x800FFFFFFFFFFFFFULL) | ((1003U + (bits >> 52U) % 41U) << 52U);
    }
    return values;
}

template <typename Function>
double nanoseconds_per_call(std::size_t calls, Function&& function) {
    const auto start = std::chrono::steady_clock::now();
    function();
    const auto stop = std::chrono::steady_clock::now();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
           static_cast<double>(calls);
}

volatile std::uint64_t sink = 0;

void micro(std::size_t count) {
    const std::vector<std::uint64_t> a = typical_values(count, 1);
    const std::vector<std::uint64_t> b = typical_values(count, 2);
    std::printf("operation      soft ns   hardware ns   ratio\n");
    const auto row = [](const char* name, double soft, double hard) {
        std::printf("%-12s %9.2f %13.2f %7.1f\n", name, soft, hard, hard > 0 ? soft / hard : 0.0);
    };
    const auto binary = [&](const char* name, auto soft_op, auto hard_op) {
        std::uint64_t accumulator = 0;
        const double soft = nanoseconds_per_call(count, [&] {
            for (std::size_t i = 0; i < count; ++i) accumulator ^= soft_op(a[i], b[i]);
        });
        double hard_accumulator = 0;
        const double hard = nanoseconds_per_call(count, [&] {
            for (std::size_t i = 0; i < count; ++i)
                hard_accumulator += hard_op(std::bit_cast<double>(a[i]), std::bit_cast<double>(b[i]));
        });
        sink = accumulator ^ std::bit_cast<std::uint64_t>(hard_accumulator);
        row(name, soft, hard);
    };
    binary("add", [](auto x, auto y) { return b64::add(x, y); }, [](double x, double y) { return x + y; });
    binary("subtract", [](auto x, auto y) { return b64::subtract(x, y); }, [](double x, double y) { return x - y; });
    binary("multiply", [](auto x, auto y) { return b64::multiply(x, y); }, [](double x, double y) { return x * y; });
    binary("divide", [](auto x, auto y) { return b64::divide(x, y); }, [](double x, double y) { return x / y; });
    binary("less", [](auto x, auto y) { return static_cast<std::uint64_t>(b64::less(x, y)); },
           [](double x, double y) { return x < y ? 1.0 : 0.0; });
    binary("equal", [](auto x, auto y) { return static_cast<std::uint64_t>(b64::equal(x, y)); },
           [](double x, double y) { return x == y ? 1.0 : 0.0; });

    const std::size_t text_count = count / 20;
    std::uint64_t accumulator = 0;
    const double format_soft = nanoseconds_per_call(text_count, [&] {
        char buffer[32];
        for (std::size_t i = 0; i < text_count; ++i) accumulator += decimal::format_lua_number(buffer, 32, a[i]);
    });
    const double format_hard = nanoseconds_per_call(text_count, [&] {
        char buffer[32];
        for (std::size_t i = 0; i < text_count; ++i)
            accumulator += static_cast<std::uint64_t>(std::snprintf(buffer, 32, "%.14g", std::bit_cast<double>(a[i])));
    });
    row("tostring", format_soft, format_hard);
    std::vector<std::string> texts;
    for (std::size_t i = 0; i < text_count; ++i) {
        texts.push_back(decimal::format_lua_number(a[i]));
    }
    const double parse_soft = nanoseconds_per_call(text_count, [&] {
        for (const std::string& text : texts) accumulator ^= decimal::parse_prefix(text).value;
    });
    const double parse_hard = nanoseconds_per_call(text_count, [&] {
        for (const std::string& text : texts) accumulator ^= std::bit_cast<std::uint64_t>(std::strtod(text.c_str(), nullptr));
    });
    row("tonumber", parse_soft, parse_hard);
    sink = accumulator;
}

struct Stats {
    double median_us;
    double p99_us;
    double max_us;
};

Stats summarize(std::vector<std::int64_t> samples) {
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double fraction) {
        const std::size_t index = std::min(samples.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(samples.size())));
        return static_cast<double>(samples[index]) / 1000.0;
    };
    return {at(0.5), at(0.99), static_cast<double>(samples.back()) / 1000.0};
}

} // namespace

int main(int argc, char** argv) {
    int ticks = 3000; // 100 s of game time at 30 Hz
    bool quick = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--ticks" && index + 1 < argc) {
            ticks = std::stoi(argv[++index]);
        } else if (argument == "--quick") {
            quick = true;
        }
    }
    if (quick) {
        ticks = 30;
    }
    micro(quick ? 20000 : 2000000);

    std::printf("\nper-tick Lua time, %d ticks (microseconds)\n", ticks);
    std::printf("steps/tick   soft median    p99     max   | hardware median    p99     max   | soft-hw p99\n");
    for (const int steps : {8, 64, 256}) {
        std::string soft_message;
        std::string hard_message;
        // Warm up allocators and caches once per VM.
        eawr::script::sflua::run_tick_workload(5, steps, soft_message);
        eawr::script::hwlua_bench::run_tick_workload(5, steps, hard_message);
        const Stats soft = summarize(eawr::script::sflua::run_tick_workload(ticks, steps, soft_message));
        const Stats hard = summarize(eawr::script::hwlua_bench::run_tick_workload(ticks, steps, hard_message));
        std::printf("%10d %11.1f %7.1f %7.1f | %15.1f %7.1f %7.1f | %9.1f\n", steps, soft.median_us, soft.p99_us,
                    soft.max_us, hard.median_us, hard.p99_us, hard.max_us, soft.p99_us - hard.p99_us);
        if (soft_message != hard_message) {
            std::printf("  results differ: soft \"%s\" hardware \"%s\"\n", soft_message.c_str(), hard_message.c_str());
            return 1;
        }
    }
    return 0;
}
