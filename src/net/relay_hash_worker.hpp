#pragma once
#include "relay_directory.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <stop_token>
#include <thread>

namespace eawr::net::relay {
// Only immutable copied hash inputs cross the worker boundary. No worker reads
// room, principal, prefix-budget, TLS or seat state; the owner commits results.
class PasswordHashWorker {
public:
    using Hash = std::function<core::Result<std::string>(std::string_view, std::string_view)>;
    struct Job { std::uint64_t client{}; PasswordHashInput input; };
    struct Completed { Job job; PasswordHashOutput output; };
    explicit PasswordHashWorker(Hash hash = password_hash);
    ~PasswordHashWorker();
    bool submit(Job);
    std::optional<Completed> take();
private:
    Hash hash_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::deque<Completed> completed_;
    std::size_t outstanding_{};
    std::jthread worker_;
    void run(std::stop_token);
};
} // namespace eawr::net::relay
