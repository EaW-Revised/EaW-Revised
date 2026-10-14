#pragma once

#include "eawr/core/result.hpp"
#include "eawr/net/gns_signalling.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace eawr::net {
// Internal TURN credential refresh step (ticket E). The relay service asks it for fresh credentials when a
// paired session renews its lease. The coturn HMAC credentials are issued synchronously; a provider that needs an
// outbound call (the Cloudflare provider of #3647) queues the job and answers on a later poll, so the owner thread
// never blocks. Not a public interface: RelayServer::listen takes it only as a test seam, like PasswordHash.
//
// Both calls run on the relay's single poll owner thread and must not block: a provider that needs the network
// queues the job in submit() and hands the outcome back from take() on a later poll. The service never stores a
// credential past the wire reply and never logs one.
struct TurnRefreshJob {
    std::string session;               // Opaque per-peer attempt key; echo unchanged, including for late completions.
    std::chrono::seconds lifetime{};   // Remaining lease time plus the usual grace; the credential should cover it.
};
struct TurnRefreshResult {
    std::string session;
    // Set on success. A default-constructed value (empty uri) means the service has no relay configured: the
    // lease still renews and peers keep their direct candidates.
    std::optional<GnsTurnCredentials> credentials;
    std::optional<core::Diagnostic> failure;   // Set when credentials are not set; text must carry no secrets.
};
class TurnRefresh {
public:
    virtual ~TurnRefresh() = default;
    // false: the provider's own budget or queue refused the job (the renewal is refused, the lease is unchanged).
    virtual bool submit(TurnRefreshJob job) = 0;
    virtual std::optional<TurnRefreshResult> take() = 0;
};
} // namespace eawr::net
