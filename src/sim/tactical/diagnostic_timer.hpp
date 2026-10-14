#pragma once

#include <chrono>
#include <cstdint>

namespace eawr::sim::tactical {

// LI-1: measurement only. Consumers write elapsed time solely to unhashed work diagnostics.
class DiagnosticTimer {
public:
    DiagnosticTimer() : begin_(std::chrono::steady_clock::now()) {}
    [[nodiscard]] std::uint64_t elapsed_nanoseconds() const {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin_).count());
    }
private:
    std::chrono::steady_clock::time_point begin_;
};

} // namespace eawr::sim::tactical
