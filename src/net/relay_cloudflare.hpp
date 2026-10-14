#pragma once

#include "relay_https.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace eawr::net::relay {
// Cloudflare Realtime TURN credential provider. The key id and API token are
// secrets: they only travel inside the TLS request, are never copied into a
// diagnostic and are never returned by any accessor.
struct CloudflareTurnConfig {
    std::string key_id;
    std::string api_token;
    std::chrono::milliseconds timeout{5000};
    // Fixed production endpoint; the overrides exist for contracts with a local
    // fake HTTPS endpoint and are not configuration keys.
    std::string host{"rtc.live.cloudflare.com"};
    std::string connect_address;
    std::uint16_t port{443};
    std::string ca_file;
};
bool cloudflare_key_id_valid(std::string_view key_id);
bool cloudflare_api_token_valid(std::string_view token);

// Pure response handling, shared with the contracts. Typed failures: 1557 for a
// non-2xx status, 1558 for malformed JSON or a response without a UDP TURN URL.
// Picks the first server that has a username, a credential and at least one UDP
// `turn:` URL (`turns:` and TCP transports are skipped) and keeps up to two URLs,
// without their query, comma-joined in the same wire field coturn uses.
core::Result<GnsTurnCredentials> cloudflare_credentials(const HttpsResponse& response, std::chrono::seconds ttl);

// Blocking calls for the worker thread.
core::Result<GnsTurnCredentials> cloudflare_generate(const CloudflareTurnConfig& config, std::chrono::seconds ttl, std::stop_token stop);
core::Result<void> cloudflare_revoke(const CloudflareTurnConfig& config, const std::string& username, std::stop_token stop);

// One bounded worker thread, like PasswordHashWorker: the relay owner thread hands
// over copied jobs and later takes finished ones; no TLS, session or budget state
// is shared. At most 16 jobs are outstanding; submit refuses beyond that.
class CloudflareTurnWorker {
public:
    enum class Kind : std::uint8_t { generate, revoke };
    struct Job { Kind kind{}; std::string session; std::chrono::seconds ttl{}; std::string username; };
    // Exactly one of `credentials` (generate) or no `failure` (revoke success) holds on success.
    struct Completed { Job job; std::optional<GnsTurnCredentials> credentials; std::optional<core::Diagnostic> failure; };
    explicit CloudflareTurnWorker(CloudflareTurnConfig config);
    ~CloudflareTurnWorker();
    bool submit(Job job);
    std::optional<Completed> take();
private:
    CloudflareTurnConfig config_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::deque<Completed> completed_;
    std::size_t outstanding_{};
    std::jthread worker_;
    void run(std::stop_token);
};
} // namespace eawr::net::relay
