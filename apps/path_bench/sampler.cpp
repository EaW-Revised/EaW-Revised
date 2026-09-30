#include "sampler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(_M_X64)
#define EAWR_SAMPLER 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// DbgHelp and the tool help snapshot need windows.h first.
#include <dbghelp.h>
#include <tlhelp32.h>
#else
#define EAWR_SAMPLER 0
#endif

namespace eawr::bench {
namespace {

// The phase names seen so far, written by the stepping thread only; index 0 is the serial remainder.
constexpr std::size_t max_phases = 64;
std::array<std::string_view, max_phases> phase_names{std::string_view{}};
std::atomic<std::size_t> phase_count{1};
std::atomic<std::size_t> current_phase{0};

} // namespace

void Sampler::enter_phase(const std::string_view phase) noexcept {
    const auto count = phase_count.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count; ++index) {
        if (phase_names[index] == phase) {
            current_phase.store(index, std::memory_order_relaxed);
            return;
        }
    }
    if (count == max_phases) return;
    phase_names[count] = phase;
    phase_count.store(count + 1, std::memory_order_release);
    current_phase.store(count, std::memory_order_relaxed);
}

struct Sampler::State {
    unsigned interval_us{};
    std::uint32_t process_id{}; // 0: this process
#if EAWR_SAMPLER
    HANDLE process{GetCurrentProcess()};
#endif
    std::atomic<bool> running{false};
    std::thread thread;
    std::uint64_t samples{};
    std::uint64_t busy{};
    std::unordered_map<std::uint64_t, std::uint64_t> self;      // leaf function start -> samples
    std::unordered_map<std::uint64_t, std::uint64_t> inclusive; // function start -> samples it is on the stack
    std::map<std::pair<std::size_t, std::uint64_t>, std::uint64_t> phase_self; // (phase, leaf) -> samples
    std::vector<std::uint64_t> phase_busy = std::vector<std::uint64_t>(max_phases, 0);
    std::unordered_map<std::uint64_t, bool> idle; // leaf function start -> a wait or a sleep
#if EAWR_SAMPLER
    void run();
    [[nodiscard]] std::string name(std::uint64_t address) const;
#endif
};

#if EAWR_SAMPLER

namespace {

constexpr std::size_t max_depth = 96;

struct Frames {
    std::array<std::uint64_t, max_depth> starts{}; // each frame's function start (its address when it has no unwind data)
    std::size_t depth{};
};

// Walks a suspended thread's stack with the unwind data. Allocates nothing: the thread may hold
// the heap lock.
void walk(CONTEXT context, Frames& frames) {
    frames.depth = 0;
    while (frames.depth < max_depth && context.Rip != 0) {
        DWORD64 image = 0;
        auto* function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (function == nullptr) {
            // A leaf function: its return address is on top of the stack.
            frames.starts[frames.depth++] = context.Rip;
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        frames.starts[frames.depth++] = image + function->BeginAddress;
        void* handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler, &establisher, nullptr);
    }
}

// Walks a suspended thread of another process with DbgHelp, which reads that process's memory and
// unwind data. The sampler's own process holds no lock the target could.
void walk_remote(HANDLE process, HANDLE thread, CONTEXT context, Frames& frames) {
    frames.depth = 0;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    while (frames.depth < max_depth
        && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
               SymGetModuleBase64, nullptr) != FALSE
        && frame.AddrPC.Offset != 0) {
        const auto pc = frame.AddrPC.Offset;
        const auto* function = static_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(SymFunctionTableAccess64(process, pc));
        const auto base = SymGetModuleBase64(process, pc);
        frames.starts[frames.depth++] = function != nullptr && base != 0 ? base + function->BeginAddress : pc;
    }
}

} // namespace

std::string Sampler::State::name(const std::uint64_t address) const {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string text;
    if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
        text.assign(symbol->Name, symbol->NameLen);
    } else {
        std::ostringstream fallback;
        fallback << "0x" << std::hex << address;
        text = fallback.str();
    }
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(process, address, &module) != FALSE) {
        text = std::string(module.ModuleName) + "!" + text;
    }
    if (text.size() > 150) text = text.substr(0, 147) + "...";
    return text;
}

void Sampler::State::run() {
    const DWORD self_id = GetCurrentThreadId();
    const bool remote = process_id != 0;
    const DWORD target = remote ? process_id : GetCurrentProcessId();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::vector<std::pair<DWORD, HANDLE>> threads;
    auto next_refresh = std::chrono::steady_clock::now();
    Frames frames;
    std::vector<std::uint64_t> seen;
    while (running.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= next_refresh) {
            // The pool starts its threads per run: list the process's threads again every 100 ms.
            for (auto& entry : threads) CloseHandle(entry.second);
            threads.clear();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Thread32First(snapshot, &entry); more != FALSE; more = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != target || entry.th32ThreadID == self_id) continue;
                HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                    entry.th32ThreadID);
                if (handle != nullptr) threads.emplace_back(entry.th32ThreadID, handle);
            }
            CloseHandle(snapshot);
            next_refresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        }
        const auto phase = current_phase.load(std::memory_order_relaxed);
        for (const auto& [id, handle] : threads) {
            static_cast<void>(id);
            if (SuspendThread(handle) == static_cast<DWORD>(-1)) continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            const bool read = GetThreadContext(handle, &context) != FALSE;
            if (read && remote) {
                walk_remote(process, handle, context, frames);
            } else if (read) {
                walk(context, frames);
            }
            ResumeThread(handle);
            if (!read || frames.depth == 0) continue;
            ++samples;
            const auto leaf = frames.starts[0];
            auto found = idle.find(leaf);
            if (found == idle.end()) {
                const auto text = name(leaf);
                const bool waits = text.find("Wait") != std::string::npos || text.find("Sleep") != std::string::npos
                    || text.find("DelayExecution") != std::string::npos || text.find("SwitchToThread") != std::string::npos
                    || text.find("RemoveIoCompletion") != std::string::npos || text.find("GetMessage") != std::string::npos;
                found = idle.emplace(leaf, waits).first;
            }
            if (found->second) continue;
            ++busy;
            ++self[leaf];
            ++phase_self[{phase, leaf}];
            ++phase_busy[phase];
            seen.assign(frames.starts.begin(), frames.starts.begin() + static_cast<std::ptrdiff_t>(frames.depth));
            std::sort(seen.begin(), seen.end());
            seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
            for (const auto start : seen) ++inclusive[start];
        }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(interval_us) * 10; // relative, in 100 ns
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
    }
    for (auto& entry : threads) CloseHandle(entry.second);
    if (timer != nullptr) CloseHandle(timer);
}

#endif

Sampler::Sampler(const unsigned interval_us, const std::uint32_t process_id) : state_(std::make_unique<State>()) {
    state_->interval_us = std::max(100U, interval_us);
    state_->process_id = process_id;
#if EAWR_SAMPLER
    if (process_id != 0) {
        state_->process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process_id);
        if (state_->process == nullptr) return;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(state_->process, nullptr, TRUE);
#endif
}

Sampler::~Sampler() {
    stop();
#if EAWR_SAMPLER
    if (state_->process == nullptr) return;
    SymCleanup(state_->process);
    if (state_->process_id != 0) CloseHandle(state_->process);
#endif
}

bool Sampler::attached() const noexcept {
#if EAWR_SAMPLER
    return state_->process != nullptr;
#else
    return false;
#endif
}

bool Sampler::supported() noexcept { return EAWR_SAMPLER != 0; }

void Sampler::start() {
#if EAWR_SAMPLER
    if (state_->process == nullptr || state_->running.exchange(true)) return;
    state_->thread = std::thread([this] { state_->run(); });
#endif
}

void Sampler::stop() {
    state_->running.store(false);
    if (state_->thread.joinable()) state_->thread.join();
}

void Sampler::report(std::ostream& output, const std::size_t rows) const {
#if EAWR_SAMPLER
    const auto& state = *state_;
    const auto percent = [&](const std::uint64_t count) {
        return state.busy == 0 ? 0.0 : 100.0 * static_cast<double>(count) / static_cast<double>(state.busy);
    };
    // Modules the target loaded after the attach are named too.
    if (state.process_id != 0) SymRefreshModuleList(state.process);
    const auto phase_label = [&](const std::size_t phase) {
        if (state.process_id != 0) return std::string("process");
        return phase == 0 ? std::string("serial") : std::string(phase_names[phase]);
    };
    output << std::fixed << std::setprecision(1);
    output << "  profile: " << state.samples << " samples every " << state.interval_us << " us, " << state.busy
           << " busy (waits and sleeps left out)\n";
    output << "    busy samples by phase:";
    std::vector<std::pair<std::uint64_t, std::size_t>> phases;
    for (std::size_t phase = 0; phase < max_phases; ++phase) {
        if (state.phase_busy[phase] != 0) phases.emplace_back(state.phase_busy[phase], phase);
    }
    std::sort(phases.rbegin(), phases.rend());
    for (const auto& [count, phase] : phases) output << ' ' << phase_label(phase) << ' ' << percent(count) << '%';
    output << '\n';
    const auto top = [&](const std::unordered_map<std::uint64_t, std::uint64_t>& counts) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted;
        for (const auto& [start, count] : counts) sorted.emplace_back(count, start);
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
            return left.first != right.first ? left.first > right.first : left.second < right.second;
        });
        if (sorted.size() > rows) sorted.resize(rows);
        return sorted;
    };
    output << "    | self % | function | phases (share of its self samples) |\n    |---:|---|---|\n";
    for (const auto& [count, start] : top(state.self)) {
        std::vector<std::pair<std::uint64_t, std::size_t>> where;
        for (const auto& [key, samples] : state.phase_self) {
            if (key.second == start) where.emplace_back(samples, key.first);
        }
        std::sort(where.rbegin(), where.rend());
        output << "    | " << percent(count) << " | " << state.name(start) << " |";
        for (std::size_t index = 0; index < std::min<std::size_t>(where.size(), 3); ++index) {
            output << ' ' << phase_label(where[index].second) << ' '
                   << 100.0 * static_cast<double>(where[index].first) / static_cast<double>(count) << '%';
        }
        output << " |\n";
    }
    output << "    | inclusive % | function |\n    |---:|---|\n";
    for (const auto& [count, start] : top(state.inclusive)) {
        output << "    | " << percent(count) << " | " << state.name(start) << " |\n";
    }
#else
    static_cast<void>(rows);
    output << "  profile: the built-in sampler runs on Windows x64 only\n";
#endif
}

} // namespace eawr::bench
