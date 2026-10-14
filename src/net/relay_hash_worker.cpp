#include "relay_hash_worker.hpp"
namespace eawr::net::relay {
PasswordHashWorker::PasswordHashWorker(Hash hash) : hash_(std::move(hash)), worker_([this](std::stop_token stop) { run(stop); }) {}
PasswordHashWorker::~PasswordHashWorker() {
    { std::lock_guard lock(mutex_); worker_.request_stop(); }
    condition_.notify_all(); worker_.join();
}
bool PasswordHashWorker::submit(Job job) {
    std::lock_guard lock(mutex_);
    if (outstanding_ >= 16 || job.input.password.empty() || job.input.password.size() > 128 || job.input.salt.size() != 32) return false;
    ++outstanding_; jobs_.push_back(std::move(job)); condition_.notify_all(); return true;
}
std::optional<PasswordHashWorker::Completed> PasswordHashWorker::take() {
    std::lock_guard lock(mutex_);
    if (completed_.empty()) return {};
    auto result = std::move(completed_.front()); completed_.pop_front(); --outstanding_; return result;
}
void PasswordHashWorker::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stop.stop_requested() || !jobs_.empty(); });
            if (stop.stop_requested()) return;
            job = std::move(jobs_.front()); jobs_.pop_front();
        }
        auto result = hash_(job->input.password, job->input.salt);
        const auto salt = job->input.salt;
        job->input.password.clear();
        std::lock_guard lock(mutex_);
        completed_.push_back({std::move(*job), {salt, std::move(result)}});
    }
}
} // namespace eawr::net::relay
