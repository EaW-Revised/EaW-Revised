#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace eawr::core::perf_diag {

// PC-wide context of design section 4, sampled once per second on the feed writer thread (never on the simulation or main
// thread). Aggregates only: other processes' names, hosts, users and paths never enter the feed.
//
// The raw counters of one instant. Times are in one common unit per platform (Windows: 100 ns, Linux: clock ticks);
// only ratios between two samples are used.
struct PcCounters final {
    bool valid{};
    std::uint64_t system_busy{};  // all processors, busy time
    std::uint64_t system_total{}; // all processors, busy + idle time
    std::uint64_t own{};          // this process, user + kernel time
    std::uint64_t commit_used{};  // bytes (or any unit shared with commit_limit)
    std::uint64_t commit_limit{};
    std::uint64_t memory_available_kib{};
};

// One second of context: shares are 0..1, negative = not measured.
struct PcSample final {
    float system_busy{-1.0F};
    float own_busy{-1.0F};
    float others_busy{-1.0F}; // system - own, floored at 0: the share of the machine other processes used
    float commit_ratio{-1.0F};
    std::uint64_t memory_available_mib{};
};

// The delta of two counter sets over the interval between them. Counters that went backwards or an empty interval give
// "not measured" shares, never a made-up number.
[[nodiscard]] inline PcSample pc_sample_between(const PcCounters& before, const PcCounters& after) noexcept {
    PcSample out;
    if (after.commit_limit != 0) out.commit_ratio = static_cast<float>(static_cast<double>(after.commit_used) / static_cast<double>(after.commit_limit));
    out.memory_available_mib = after.memory_available_kib / 1024;
    if (!before.valid || !after.valid || after.system_total <= before.system_total || after.system_busy < before.system_busy
        || after.own < before.own)
        return out;
    const double total = static_cast<double>(after.system_total - before.system_total);
    const double busy = static_cast<double>(after.system_busy - before.system_busy);
    const double own = static_cast<double>(after.own - before.own);
    out.system_busy = static_cast<float>(busy / total > 1.0 ? 1.0 : busy / total);
    out.own_busy = static_cast<float>(own / total > 1.0 ? 1.0 : own / total);
    out.others_busy = out.system_busy > out.own_busy ? out.system_busy - out.own_busy : 0.0F;
    return out;
}

// Linux text forms (also the unit-test surface on every platform). /proc/stat first line:
// "cpu  user nice system idle iowait irq softirq steal ...": busy = everything but idle and iowait.
[[nodiscard]] inline std::optional<std::pair<std::uint64_t, std::uint64_t>> parse_proc_stat_cpu(const std::string_view text) noexcept {
    if (text.substr(0, 4) != "cpu ") return std::nullopt;
    std::uint64_t field[8]{};
    std::size_t count = 0;
    std::size_t at = 4;
    while (count < 8 && at < text.size() && text[at] != '\n') {
        while (at < text.size() && text[at] == ' ') ++at;
        if (at >= text.size() || text[at] < '0' || text[at] > '9') break;
        std::uint64_t value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') value = value * 10 + static_cast<std::uint64_t>(text[at++] - '0');
        field[count++] = value;
    }
    if (count < 4) return std::nullopt;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < count; ++i) total += field[i];
    const std::uint64_t idle = field[3] + (count > 4 ? field[4] : 0);
    return std::make_pair(total - idle, total); // (busy, total)
}

// /proc/self/stat: utime and stime are fields 14 and 15, counted after the "(comm)" field whose text may hold spaces.
[[nodiscard]] inline std::optional<std::uint64_t> parse_proc_self_stat_cpu(const std::string_view text) noexcept {
    const std::size_t close = text.rfind(')');
    if (close == std::string_view::npos) return std::nullopt;
    std::size_t at = close + 1;
    std::size_t index = 3; // the field after the comm is field 3 (state)
    std::uint64_t utime = 0, stime = 0;
    while (at < text.size() && index <= 15) {
        while (at < text.size() && text[at] == ' ') ++at;
        const std::size_t begin = at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\n') ++at;
        if (index == 14 || index == 15) {
            std::uint64_t value = 0;
            for (std::size_t i = begin; i < at; ++i) {
                if (text[i] < '0' || text[i] > '9') return std::nullopt;
                value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
            }
            (index == 14 ? utime : stime) = value;
        }
        ++index;
    }
    if (index <= 15) return std::nullopt;
    return utime + stime;
}

// One "Key:   123 kB" value of /proc/meminfo, in kB.
[[nodiscard]] inline std::optional<std::uint64_t> parse_meminfo_kib(const std::string_view text, const std::string_view key) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == ':') {
            std::size_t i = key.size() + 1;
            while (i < line.size() && line[i] == ' ') ++i;
            if (i >= line.size() || line[i] < '0' || line[i] > '9') return std::nullopt;
            std::uint64_t value = 0;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') value = value * 10 + static_cast<std::uint64_t>(line[i++] - '0');
            return value;
        }
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    return std::nullopt;
}

// Reads the counters of the running machine (one syscall group, a few microseconds). Returns valid = false where the
// platform offers nothing. Defined in src/core/perf_diag_feed_writer.cpp.
[[nodiscard]] PcCounters read_pc_counters() noexcept;

} // namespace eawr::core::perf_diag
