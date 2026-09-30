#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string_view>

namespace eawr::bench {

// A development-only sampling profiler for path_bench --melee --profile on (#601,
// docs/worker-offload.md#benchmarks-and-profiles): a thread that interrupts every other thread of
// the process at a fixed interval, reads its stack and counts the functions on it, with the tick
// phase the stepping thread was in. Windows x64 only (a build with debug information names the
// functions: EAWR_DEBUG_SYMBOLS); elsewhere it reports that it cannot sample. Never part of the
// simulation or the viewer; sampling perturbs the timing, so profile runs are kept apart from
// timed runs.
//
// With a process ID it samples that process instead (path_bench --profile-attach, for the viewer:
// the sampler never runs inside the viewer), walking the stacks with DbgHelp; there is no phase.
class Sampler {
public:
    explicit Sampler(unsigned interval_us, std::uint32_t process_id = 0);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    [[nodiscard]] static bool supported() noexcept;
    // False when the process to attach to could not be opened.
    [[nodiscard]] bool attached() const noexcept;
    void start();
    void stop();
    // The top `rows` functions by self and by inclusive samples, and the busy samples per phase.
    void report(std::ostream& output, std::size_t rows) const;

    // The phase the stepping thread is in; `phase` must outlive the sampler (a string literal).
    // An empty name is the serial remainder.
    static void enter_phase(std::string_view phase) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::bench
