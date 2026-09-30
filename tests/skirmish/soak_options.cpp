#include "soak_options.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace eawr::soak {

namespace {

constexpr std::uint64_t max_ticks = 1'000'000; // the replay tick limit (tactical::max_ticks)
constexpr std::size_t max_count = 256;         // workers; battles are capped lower

std::optional<std::uint64_t> whole_number(const std::string& text) {
    if (text.empty() || text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    return std::strtoull(text.c_str(), nullptr, 10);
}

std::optional<double> real_number(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::string real_text(const double value) {
    std::ostringstream text;
    text << value;
    return text.str();
}

} // namespace

std::optional<std::vector<std::uint64_t>> parse_seed_list(const std::string& text) {
    std::vector<std::uint64_t> seeds;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find(',', start), text.size());
        const std::string item = text.substr(start, end - start);
        const auto dash = item.find('-');
        const auto low = whole_number(item.substr(0, dash));
        const auto high = dash == std::string::npos ? low : whole_number(item.substr(dash + 1));
        if (!low || !high || *high < *low || *high - *low > 100000) return std::nullopt;
        for (auto seed = *low; seed <= *high; ++seed) seeds.push_back(seed);
        start = end + 1;
    }
    return seeds;
}

std::variant<Options, std::string> parse_options(const std::vector<std::string>& arguments) {
    Options options;
    for (std::size_t at = 0; at < arguments.size(); ++at) {
        const std::string& flag = arguments[at];
        if (flag == "--strict") {
            options.strict = true;
            continue;
        }
        if (flag == "--require-decision") {
            options.require_decision = true;
            continue;
        }
        if (flag == "--no-invariants") {
            options.invariants = false;
            continue;
        }
        if (!flag.starts_with("--")) return "unexpected argument '" + flag + "'";
        if (at + 1 >= arguments.size()) return "the flag " + flag + " needs a value";
        const std::string& value = arguments[++at];
        const auto count = [&](const std::uint64_t low, const std::uint64_t high) -> std::optional<std::uint64_t> {
            const auto number = whole_number(value);
            if (!number || *number < low || *number > high) return std::nullopt;
            return number;
        };
        const auto fail = [&](const std::string& expected) -> std::variant<Options, std::string> {
            return flag + " needs " + expected + ", not '" + value + "'";
        };
        if (flag == "--seeds") {
            auto seeds = parse_seed_list(value);
            if (!seeds) return fail("a seed list such as 12,18,40-47");
            options.seeds = std::move(*seeds);
        } else if (flag == "--ticks") {
            const auto number = count(1, max_ticks);
            if (!number) return fail("a tick count from 1 to 1000000");
            options.ticks = *number;
        } else if (flag == "--battles") {
            const auto number = count(1, 64);
            if (!number) return fail("a number of battles from 1 to 64");
            options.battles = static_cast<std::size_t>(*number);
        } else if (flag == "--workers") {
            const auto number = count(1, max_count);
            if (!number) return fail("a worker count from 1 to 256");
            options.workers = static_cast<std::size_t>(*number);
        } else if (flag == "--hash-workers") {
            const auto number = count(0, max_count);
            if (!number) return fail("a worker count from 0 (off) to 256");
            options.hash_workers = static_cast<std::size_t>(*number);
        } else if (flag == "--hash-every") {
            const auto number = count(1, 1'000'000);
            if (!number) return fail("a number of seeds, at least 1");
            options.hash_every = static_cast<std::size_t>(*number);
        } else if (flag == "--check-every") {
            const auto number = count(1, 10'000);
            if (!number) return fail("a tick interval from 1 to 10000");
            options.check_every = static_cast<std::size_t>(*number);
        } else if (flag == "--out") {
            if (value.empty()) return fail("a directory");
            options.out = std::filesystem::path(value);
        } else if (flag == "--time-budget") {
            const auto number = real_number(value);
            if (!number || *number <= 0.0) return fail("a positive number of seconds");
            options.time_budget = *number;
        } else if (flag == "--spawn-window") {
            const auto number = count(0, max_ticks);
            if (!number) return fail("a tick count from 0 to 1000000");
            options.limits.spawn_window = *number;
        } else if (flag == "--hull-penetration") {
            const auto number = real_number(value);
            if (!number || *number <= 0.0 || *number > 10.0) return fail("a share in (0, 10]");
            options.limits.hull_penetration = *number;
        } else if (flag == "--hull-ticks") {
            const auto number = count(1, max_ticks);
            if (!number) return fail("a tick count from 1 to 1000000");
            options.limits.hull_ticks = *number;
        } else if (flag == "--slot-radius") {
            const auto number = real_number(value);
            if (!number || *number <= 0.0) return fail("a positive distance");
            options.limits.slot_radius = *number;
        } else if (flag == "--slot-ticks") {
            const auto number = count(1, max_ticks);
            if (!number) return fail("a tick count from 1 to 1000000");
            options.limits.slot_ticks = *number;
        } else if (flag == "--map-margin") {
            const auto number = real_number(value);
            if (!number || *number < 0.0) return fail("a distance, at least 0");
            options.limits.map_margin = *number;
        } else if (flag == "--speed-factor") {
            const auto number = real_number(value);
            if (!number || *number <= 0.0) return fail("a positive factor");
            options.limits.speed_factor = *number;
        } else if (flag == "--craft-speed-factor") {
            const auto number = real_number(value);
            if (!number || *number <= 0.0) return fail("a positive factor");
            options.limits.craft_speed_factor = *number;
        } else {
            return "unknown flag '" + flag + "'";
        }
    }
    if (options.hash_workers != 0 && options.hash_workers == options.workers) {
        return std::string("--hash-workers must differ from --workers: the comparison is between two worker counts");
    }
    return options;
}

std::string reproduction_flags(const Options& options, const std::uint64_t seed) {
    const Limits defaults;
    std::ostringstream flags;
    flags << "--seeds " << seed << " --ticks " << options.ticks << " --workers " << options.workers;
    if (options.hash_workers != 0) flags << " --hash-workers " << options.hash_workers;
    if (options.check_every != 1) flags << " --check-every " << options.check_every;
    if (options.require_decision) flags << " --require-decision";
    if (!options.invariants) flags << " --no-invariants";
    const auto& limits = options.limits;
    if (limits.spawn_window != defaults.spawn_window) flags << " --spawn-window " << limits.spawn_window;
    if (limits.hull_penetration != defaults.hull_penetration) {
        flags << " --hull-penetration " << real_text(limits.hull_penetration);
    }
    if (limits.hull_ticks != defaults.hull_ticks) flags << " --hull-ticks " << limits.hull_ticks;
    if (limits.slot_radius != defaults.slot_radius) flags << " --slot-radius " << real_text(limits.slot_radius);
    if (limits.slot_ticks != defaults.slot_ticks) flags << " --slot-ticks " << limits.slot_ticks;
    if (limits.map_margin != defaults.map_margin) flags << " --map-margin " << real_text(limits.map_margin);
    if (limits.speed_factor != defaults.speed_factor) flags << " --speed-factor " << real_text(limits.speed_factor);
    if (limits.craft_speed_factor != defaults.craft_speed_factor) {
        flags << " --craft-speed-factor " << real_text(limits.craft_speed_factor);
    }
    return flags.str();
}

} // namespace eawr::soak
