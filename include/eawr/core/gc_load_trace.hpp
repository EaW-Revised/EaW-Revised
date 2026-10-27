#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::core::gc_load {

// Presentation diagnostics: one monotonic-clock record per Galactic Conquest load, from the press of Start to the first
// drawn frame. Never saved, hashed or read by the simulation. Spans nest by dotted name (a child lies inside its parent).
using Clock = std::chrono::steady_clock;

class Trace final {
public:
    // The first caller owns the origin; a later call while a load is running is ignored unless it is the press of Start
    // (restart), which drops a load that never reached its first frame.
    void begin(std::string_view kind, const bool restart = false, const Clock::time_point at = Clock::now()) {
        const std::lock_guard lock(mutex_);
        if (active_ && !restart) return;
        active_ = true;
        kind_ = kind;
        origin_ = at;
        entries_.clear();
    }
    [[nodiscard]] bool active() const {
        const std::lock_guard lock(mutex_);
        return active_;
    }
    void span(std::string_view name, const Clock::time_point start, const Clock::time_point end) {
        const std::lock_guard lock(mutex_);
        if (active_) entries_.push_back({std::string(name), milliseconds(origin_, start), milliseconds(start, end)});
    }
    void mark(std::string_view name) {
        const auto now = Clock::now();
        const std::lock_guard lock(mutex_);
        if (active_) entries_.push_back({std::string(name), milliseconds(origin_, now), 0.0});
    }
    // Ends the load and returns its single log line, or nothing when no load was running.
    [[nodiscard]] std::optional<std::string> finish(std::string_view build) {
        const auto now = Clock::now();
        const std::lock_guard lock(mutex_);
        if (!active_) return std::nullopt;
        active_ = false;
        std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.start_ms < b.start_ms; });
        char number[64];
        std::snprintf(number, sizeof(number), "%.1f", milliseconds(origin_, now));
        std::string line = "EAWR GC load [" + kind_ + "] build=" + std::string(build) + " total_ms=" + number + " phases(name@start+ms):";
        for (const auto& entry : entries_) {
            std::snprintf(number, sizeof(number), "@%.1f+%.1f", entry.start_ms, entry.ms);
            line += ' ' + entry.name + number;
        }
        return line;
    }
private:
    struct Entry final { std::string name; double start_ms{}, ms{}; };
    static double milliseconds(const Clock::time_point from, const Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }
    mutable std::mutex mutex_;
    bool active_{};
    std::string kind_;
    Clock::time_point origin_{};
    std::vector<Entry> entries_;
};
inline Trace trace;

class Scope final {
public:
    explicit Scope(std::string_view name) : name_(name), start_(Clock::now()) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { trace.span(name_, start_, Clock::now()); }
private:
    std::string_view name_;
    Clock::time_point start_;
};

} // namespace eawr::core::gc_load
