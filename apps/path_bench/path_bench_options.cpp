#include "path_bench_internal.hpp"

namespace path_bench {

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

template <typename T>
[[nodiscard]] std::optional<T> parse_number(const std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<Options> parse_options(const int argc, const char* const argv[]) {
    Options options;
    options.game_root = environment("EAWR_EAW_GAME_ROOT");
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) return std::nullopt;
        const std::string_view value(argv[++index]);
        if (argument == "--game-root") {
            options.game_root = std::string(value);
        } else if (argument == "--order-tick") {
            const auto tick = parse_number<std::uint64_t>(value);
            if (!tick || *tick < 1 || *tick > 50'000) return std::nullopt;
            options.order_tick = *tick;
        } else if (argument == "--ticks") {
            const auto ticks = parse_number<std::uint64_t>(value);
            if (!ticks || *ticks < 1 || *ticks > 10'000) return std::nullopt;
            options.ticks = *ticks;
        } else if (argument == "--workers") {
            options.workers.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto count = parse_number<std::size_t>(rest.substr(0, comma));
                if (!count || *count == 0 || *count > eawr::platform::ThreadWorkerAdapter::max_worker_count) {
                    return std::nullopt;
                }
                options.workers.push_back(*count);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (options.workers.empty()) return std::nullopt;
        } else if (argument == "--selection") {
            options.selections.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto item = rest.substr(0, comma);
                if (item != "owner" && item != "owner-group" && item != "all" && item != "large") return std::nullopt;
                options.selections.emplace_back(item);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (options.selections.empty()) return std::nullopt;
        } else if (argument == "--timing") {
            if (value != "on" && value != "off") return std::nullopt;
            options.timing = value == "on";
        } else if (argument == "--execution") {
            if (value != "live" && value != "legacy") return std::nullopt;
            options.live_execution = value == "live";
        } else if (argument == "--profile") {
            if (value != "on" && value != "off") return std::nullopt;
            options.profile = value == "on";
        } else if (argument == "--profile-interval") {
            const auto interval = parse_number<unsigned>(value);
            if (!interval || *interval < 100 || *interval > 100'000) return std::nullopt;
            options.profile_interval_us = *interval;
        } else if (argument == "--csv") {
            options.csv = std::string(value);
        } else if (argument == "--destination") {
            const auto comma = value.find(',');
            if (comma == std::string_view::npos) return std::nullopt;
            const auto x = parse_number<std::int64_t>(value.substr(0, comma));
            const auto y = parse_number<std::int64_t>(value.substr(comma + 1));
            if (!x || !y || *x < -100'000 || *x > 100'000 || *y < -100'000 || *y > 100'000) return std::nullopt;
            options.destination = std::pair{*x, *y};
        } else if (argument == "--list") {
            options.list = value == "1";
        } else if (argument == "--pin") {
            options.pin = std::string(value);
        } else if (argument == "--update-pin") {
            options.update_pin = value == "1";
        } else if (argument == "--replay-out") {
            options.replay_out = std::string(value);
        } else if (argument == "--ships") {
            options.ships = std::string(value);
        } else if (argument == "--search-budget") {
            const auto budget = parse_number<std::uint64_t>(value);
            if (!budget || *budget < 1) return std::nullopt;
            options.search_budget = *budget;
        } else {
            return std::nullopt;
        }
    }
    return options;
}


} // namespace path_bench
