#pragma once

#include <algorithm>
#include <chrono>
#include <string_view>
#include <vector>

namespace eawr::core::tick_profile {

// Caller-thread diagnostics only: no clocks or allocation without a recording.
// Worker barriers are subtracted from all enclosing scopes, and nested serial
// scopes contribute only to their own row. Costs are write-only diagnostics for
// profiling consumers; they never feed simulation state, hashes or replay bytes.
using Clock = std::chrono::steady_clock;
struct Cost final { std::string_view name; double ms{}; };
struct Recording final { std::vector<Cost> costs; double barriers_ms{}; };
class Scope;
inline thread_local Recording* active{};
inline thread_local Scope* parent{};

class Capture final {
public:
    explicit Capture(Recording& recording) : previous_(active) { active = &recording; }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() { active = previous_; }
private:
    Recording* previous_{};
};

// Inline callbacks belong to their enclosing worker barrier, even when that
// barrier runs on the caller thread. Restore its serial recording afterwards.
class Pause final {
public:
    Pause() : previous_(active) { active = nullptr; }
    Pause(const Pause&) = delete;
    Pause& operator=(const Pause&) = delete;
    ~Pause() { active = previous_; }
private:
    Recording* previous_{};
};

class Scope final {
public:
    explicit Scope(const std::string_view name) : recording_(active), name_(name) {
        if (recording_) {
            parent_ = parent;
            parent = this;
            barriers_ = recording_->barriers_ms;
            started_ = Clock::now();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!recording_) return;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started_).count();
        const double serial = elapsed - (recording_->barriers_ms - barriers_);
        recording_->costs.push_back({name_, std::max(0.0, serial - children_ms_)});
        if (parent_) parent_->children_ms_ += serial;
        parent = parent_;
    }
private:
    Recording* recording_{};
    Scope* parent_{};
    std::string_view name_;
    Clock::time_point started_{};
    double barriers_{};
    double children_ms_{};
};

} // namespace eawr::core::tick_profile
