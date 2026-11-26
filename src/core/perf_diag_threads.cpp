#include "eawr/core/perf_diag/thread_cpu.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "eawr/core/perf_diag/feed_writer.hpp"
#include "eawr/core/perf_diag/pc_context.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif
#elif defined(__linux__)
#include <dirent.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

namespace eawr::core::perf_diag {

namespace {

struct Registry final {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::string>> items;
};

// Never destroyed: a detached thread may end while the process shuts down.
Registry& registry() {
    static Registry* const value = new Registry();
    return *value;
}

} // namespace

std::uint64_t current_thread_os_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

ThreadLabel::ThreadLabel(const std::string_view name, const std::size_t index) noexcept {
    if (!FeedWriter::active()) return;
    try {
        std::string label(name);
        if (index != no_index) label += "-" + std::to_string(index);
        id_ = current_thread_os_id();
        if (id_ == 0) return;
        Registry& reg = registry();
        const std::lock_guard lock(reg.mutex);
        reg.items.emplace_back(id_, std::move(label));
    } catch (...) {
        id_ = 0;
    }
}

ThreadLabel::~ThreadLabel() {
    if (id_ == 0) return;
    try {
        Registry& reg = registry();
        const std::lock_guard lock(reg.mutex);
        const auto found = std::find_if(reg.items.begin(), reg.items.end(), [&](const auto& item) { return item.first == id_; });
        if (found != reg.items.end()) reg.items.erase(found);
    } catch (...) {
    }
}

std::vector<std::pair<std::uint64_t, std::string>> labeled_threads() {
    Registry& reg = registry();
    const std::lock_guard lock(reg.mutex);
    return reg.items;
}

std::vector<ThreadShare> ThreadCpuSampler::shares_between(const std::vector<ThreadCounters>& counters, const std::uint64_t now_us,
    const std::vector<std::pair<std::uint64_t, std::string>>& labels, const double units_per_second) {
    std::vector<ThreadShare> shares;
    const bool primed = previous_us_ != 0 && now_us > previous_us_ && units_per_second > 0.0;
    const double seconds = primed ? static_cast<double>(now_us - previous_us_) / 1e6 : 0.0;
    std::unordered_map<std::uint64_t, std::uint64_t> next;
    next.reserve(counters.size());
    std::size_t unlabeled = 0;
    std::vector<ThreadShare> unlabeled_shares;
    for (const ThreadCounters& counter : counters) {
        next[counter.id] = counter.cpu;
        if (!primed) continue;
        const auto before = previous_.find(counter.id);
        if (before == previous_.end() || counter.cpu < before->second) continue; // a thread first seen now has no interval
        ThreadShare share;
        share.share = static_cast<float>(static_cast<double>(counter.cpu - before->second) / units_per_second / seconds);
        const auto label = std::find_if(labels.begin(), labels.end(), [&](const auto& item) { return item.first == counter.id; });
        if (label != labels.end()) {
            share.name = label->second;
            share.labeled = true;
            shares.push_back(std::move(share));
            continue;
        }
        if (share.share < 0.02F) continue; // idle engine threads are noise
        share.name = counter.engine_name.empty() ? "t" + std::to_string(counter.id) : counter.engine_name;
        unlabeled_shares.push_back(std::move(share));
        ++unlabeled;
    }
    static_cast<void>(unlabeled);
    std::sort(unlabeled_shares.begin(), unlabeled_shares.end(), [](const ThreadShare& a, const ThreadShare& b) { return a.share > b.share; });
    constexpr std::size_t unlabeled_limit = 12;
    if (unlabeled_shares.size() > unlabeled_limit) unlabeled_shares.resize(unlabeled_limit);
    for (ThreadShare& share : unlabeled_shares) shares.push_back(std::move(share));
    std::sort(shares.begin(), shares.end(), [](const ThreadShare& a, const ThreadShare& b) { return a.share > b.share; });
    previous_ = std::move(next);
    previous_us_ = now_us;
    return shares;
}

std::vector<ThreadShare> ThreadCpuSampler::sample(const std::uint64_t now_us) {
    return shares_between(read_thread_counters(), now_us, labeled_threads(), thread_counter_units_per_second());
}

double thread_counter_units_per_second() noexcept {
#if defined(_WIN32)
    return 1e7; // FILETIME: 100 ns
#elif defined(__linux__)
    const long ticks = ::sysconf(_SC_CLK_TCK);
    return ticks > 0 ? static_cast<double>(ticks) : 100.0;
#else
    return 0.0;
#endif
}

std::vector<ThreadCounters> read_thread_counters() {
    std::vector<ThreadCounters> out;
#if defined(_WIN32)
    using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const DescriptionFunction describe = reinterpret_cast<DescriptionFunction>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;
    const DWORD process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process) continue;
            const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                const auto value = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
                ThreadCounters counter;
                counter.id = entry.th32ThreadID;
                counter.cpu = value(kernel) + value(user);
                if (describe != nullptr) {
                    PWSTR text = nullptr;
                    if (SUCCEEDED(describe(thread, &text)) && text != nullptr) {
                        char buffer[96]{};
                        const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, static_cast<int>(sizeof buffer) - 1, nullptr, nullptr);
                        if (length > 1) counter.engine_name = buffer;
                        LocalFree(text);
                    }
                }
                out.push_back(std::move(counter));
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
#elif defined(__linux__)
    const auto slurp = [](const std::string& path) {
        std::string text;
        std::ifstream in(path, std::ios::binary);
        char buffer[1024];
        while (in.read(buffer, sizeof buffer) || in.gcount() > 0) text.append(buffer, static_cast<std::size_t>(in.gcount()));
        return text;
    };
    if (DIR* const directory = opendir("/proc/self/task")) {
        while (const dirent* const item = readdir(directory)) {
            const std::string name = item->d_name;
            if (name.empty() || name[0] < '0' || name[0] > '9') continue;
            const auto cpu = parse_proc_self_stat_cpu(slurp("/proc/self/task/" + name + "/stat"));
            if (!cpu) continue;
            ThreadCounters counter;
            counter.id = std::stoull(name);
            counter.cpu = *cpu;
            counter.engine_name = slurp("/proc/self/task/" + name + "/comm");
            while (!counter.engine_name.empty() && (counter.engine_name.back() == '\n' || counter.engine_name.back() == ' '))
                counter.engine_name.pop_back();
            out.push_back(std::move(counter));
        }
        closedir(directory);
    }
#endif
    return out;
}

std::string clean_cpu_brand(const std::string_view raw) {
    std::string out;
    bool blank = true; // swallows leading blanks
    for (const char c : raw) {
        if (c == '\0') break;
        const bool space = c == ' ' || c == '\t' || c == '\n';
        if (space) {
            if (!blank) out += ' ';
            blank = true;
        } else {
            out += c;
            blank = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::optional<std::string> parse_cpuinfo_model(const std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (line.substr(0, 10) == "model name") {
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string model = clean_cpu_brand(line.substr(colon + 1));
                if (!model.empty()) return model;
            }
        }
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    return std::nullopt;
}

namespace {

CpuInfo read_cpu_info() {
    CpuInfo info;
    info.logical = info.cores = info.performance_cores = info.performance_logical =
        std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
#if (defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86)))
    {
        int regs[4]{};
        __cpuid(regs, static_cast<int>(0x80000000U));
        if (static_cast<unsigned>(regs[0]) >= 0x80000004U) {
            char brand[49]{};
            for (unsigned leaf = 0; leaf < 3; ++leaf) {
                __cpuid(regs, static_cast<int>(0x80000002U + leaf));
                std::memcpy(brand + leaf * 16, regs, 16);
            }
            info.model = clean_cpu_brand(brand);
        }
    }
#elif defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
    {
        unsigned regs[4]{};
        if (__get_cpuid_max(0x80000000U, nullptr) >= 0x80000004U) {
            char brand[49]{};
            for (unsigned leaf = 0; leaf < 3; ++leaf) {
                __get_cpuid(0x80000002U + leaf, &regs[0], &regs[1], &regs[2], &regs[3]);
                std::memcpy(brand + leaf * 16, regs, 16);
            }
            info.model = clean_cpu_brand(brand);
        }
    }
#endif
#if defined(_WIN32)
    try {
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length == 0) return info;
        std::vector<unsigned char> buffer(length);
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) return info;
        struct Core { unsigned char efficiency; std::size_t threads; };
        std::vector<Core> cores;
        for (DWORD offset = 0; offset < length;) {
            const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            if (entry->Size == 0) break;
            if (entry->Relationship == RelationProcessorCore) {
                std::size_t threads = 0;
                for (WORD group = 0; group < entry->Processor.GroupCount; ++group)
                    threads += static_cast<std::size_t>(std::popcount(static_cast<unsigned long long>(entry->Processor.GroupMask[group].Mask)));
                cores.push_back({entry->Processor.EfficiencyClass, std::max<std::size_t>(threads, 1)});
            }
            offset += entry->Size;
        }
        if (cores.empty()) return info;
        unsigned char best = 0;
        for (const Core& core : cores) best = std::max(best, core.efficiency);
        info.cores = cores.size();
        info.logical = info.performance_cores = info.efficiency_cores = info.performance_logical = 0;
        for (const Core& core : cores) {
            info.logical += core.threads;
            if (core.efficiency == best) {
                ++info.performance_cores;
                info.performance_logical += core.threads;
            } else {
                ++info.efficiency_cores;
            }
        }
        info.topology_known = true;
    } catch (...) {
        // Out of memory while reading: keep the logical count.
    }
#elif defined(__linux__)
    try {
        std::ifstream in("/proc/cpuinfo", std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const std::string content = text.str();
        if (info.model.empty()) info.model = parse_cpuinfo_model(content).value_or(std::string{});
        std::set<std::pair<std::string, std::string>> cores;
        std::string physical, core;
        std::size_t at = 0;
        while (at < content.size()) {
            const std::size_t end = content.find('\n', at);
            const std::string_view line(content.data() + at, (end == std::string::npos ? content.size() : end) - at);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string key = clean_cpu_brand(line.substr(0, colon));
                const std::string value = clean_cpu_brand(line.substr(colon + 1));
                if (key == "physical id") physical = value;
                else if (key == "core id") { core = value; cores.emplace(physical, core); }
            }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        if (!cores.empty()) {
            info.cores = info.performance_cores = cores.size();
            info.performance_logical = info.logical;
            info.topology_known = true;
        }
    } catch (...) {
    }
#endif
    return info;
}

} // namespace

const CpuInfo& cpu_info() noexcept {
    static const CpuInfo info = read_cpu_info();
    return info;
}

} // namespace eawr::core::perf_diag
