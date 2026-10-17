#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <system_error>
#include <vector>

namespace eawr::skirmish::detail {
struct SetupMapLauncher final {
    std::future<void> operator()(std::function<void()> work) const {
        return std::async(std::launch::async, std::move(work));
    }
};
// Each record owns its output/error. Finish every claimed record, including
// after a worker throws, and let the caller inspect errors in record order.
// A launch failure drains the same atomic queue on the calling thread.
template<class Work, class Launch = SetupMapLauncher>
std::vector<std::exception_ptr> run_setup_map_workers(const std::size_t count,
    const unsigned requested, Work work, Launch launch = {}) {
    std::vector<std::exception_ptr> errors(count);
    const auto workers = std::min(std::max(1U, requested), static_cast<unsigned>(std::min<std::size_t>(count, 8)));
    std::atomic<std::size_t> next{};
    const auto worker = [&] {
        for (;;) {
            const auto index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) break;
            try { work(index); }
            catch (...) { errors[index] = std::current_exception(); }
        }
    };
    std::vector<std::future<void>> jobs;
    jobs.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        try { jobs.push_back(launch(worker)); }
        catch (const std::system_error&) { worker(); break; }
    }
    for (auto& job : jobs) job.get();
    return errors;
}
} // namespace eawr::skirmish::detail
