#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace eawr::core::tick_profile {

// Opt-in diagnostics only: no clocks or allocation without a recording.
// Worker barriers are subtracted from all enclosing scopes, and nested serial
// scopes contribute only to their own row. Costs are write-only diagnostics for
// profiling consumers; they never feed simulation state, hashes or replay bytes.
using Clock = std::chrono::steady_clock;
struct JobWork final {
    std::uint64_t before{}, after{}, deadline{}, limit{}, children{}, queries{}, leaves{}, narrow{};
};
struct Cost final { std::string_view name; double ms{}; std::uint64_t identity{}; JobWork work{}; };
struct Recording final {
    std::vector<Cost> costs;
    double barriers_ms{};
    std::size_t pending_jobs{};
    std::vector<Cost> auxiliary_costs;
};
class Scope;
inline thread_local Recording* active{};
inline thread_local Scope* parent{};
inline thread_local Recording* active_jobs{};

// The phase's caller supplies cardinality before dispatch, so the measured
// executor reserves each callback's job slots on the caller thread.
inline void prepare_jobs(const std::size_t count) {
    if (active) active->pending_jobs = count;
}

class JobCapture final {
public:
    explicit JobCapture(Recording& recording) : previous_(active_jobs) { active_jobs = &recording; }
    JobCapture(const JobCapture&) = delete;
    JobCapture& operator=(const JobCapture&) = delete;
    ~JobCapture() { active_jobs = previous_; }
private:
    Recording* previous_{};
};

// The caller installs a fixed set of auxiliary slots before dispatch. Worker
// scopes only add to their own slot; absent slots read no clock or allocate.
class AuxiliaryScope final {
public:
    explicit AuxiliaryScope(const std::string_view name) {
        if (!active_jobs) return;
        const auto found = std::find_if(active_jobs->auxiliary_costs.begin(), active_jobs->auxiliary_costs.end(),
            [&](const Cost& cost) { return cost.name == name; });
        if (found == active_jobs->auxiliary_costs.end()) return;
        cost_ = &*found;
        started_ = Clock::now();
    }
    AuxiliaryScope(const AuxiliaryScope&) = delete;
    AuxiliaryScope& operator=(const AuxiliaryScope&) = delete;
    ~AuxiliaryScope() {
        if (cost_) cost_->ms += std::chrono::duration<double, std::milli>(Clock::now() - started_).count();
    }
private:
    Cost* cost_{};
    Clock::time_point started_{};
};

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

// Independent per-job wall samples, including any nested work. These are
// diagnostics within a worker barrier, never additive serial/worker cost.
class JobScope final {
public:
    JobScope(const std::string_view name, const std::uint64_t identity, const JobWork* work = nullptr)
        : recording_(active_jobs), name_(name), identity_(identity), work_(work) {
        if (recording_) started_ = Clock::now();
    }
    JobScope(const JobScope&) = delete;
    JobScope& operator=(const JobScope&) = delete;
    ~JobScope() {
        if (!recording_) return;
        recording_->costs.push_back({name_,
            std::chrono::duration<double, std::milli>(Clock::now() - started_).count(), identity_, work_ ? *work_ : JobWork{}});
    }
private:
    Recording* recording_{};
    std::string_view name_;
    std::uint64_t identity_{};
    const JobWork* work_{};
    Clock::time_point started_{};
};

} // namespace eawr::core::tick_profile
