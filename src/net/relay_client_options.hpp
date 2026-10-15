#pragma once

#include "eawr/net/network_introduction.hpp"
#include "relay_protocol.hpp"
#include "relay_tls.hpp"

namespace eawr::net::relay {
// Shared by every TLS client of the introduction service (pairing endpoints, the directory client, room members).
inline bool client_options_valid(const NetworkIntroductionOptions& o, bool token_optional) {
    return !o.address.empty() && o.port != 0
        && (!o.certificate_name.empty() || o.certificate_sha256.size() == 64)
        // System trust consistently takes a DNS certificate name on both TLS backends.
        && (!o.ca_file.empty() || !o.certificate_sha256.empty()
            || (!socket::numeric_ip(o.certificate_name, false) && !socket::numeric_ip(o.certificate_name, true)
                && o.certificate_name.find(':') == std::string::npos))
        && (token_optional || o.access_token.empty() || (o.access_token.size() >= 32 && o.access_token.size() <= 256))
        && o.renewal_retry.count() > 0 && o.renewal_retry <= std::chrono::minutes(1)
        && o.timeout.count() > 0 && o.timeout <= std::chrono::seconds(60)
        && o.queue_bytes >= max_body + 4 && o.queue_bytes <= 16 * 1024 * 1024
        && o.queue_frames > 0 && o.queue_frames <= 4096;
}
} // namespace eawr::net::relay
