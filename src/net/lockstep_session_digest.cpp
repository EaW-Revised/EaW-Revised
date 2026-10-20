#include "lockstep_session_internal.hpp"
#include "eawr/core/sha256.hpp"

namespace eawr::net {
using namespace lockstep_detail;

std::array<std::uint8_t, 32> rejoin_authenticator(const std::array<std::uint8_t, 32>& token,
    const std::array<std::uint8_t, 32>& identity, std::uint32_t seat, std::uint64_t epoch, const RejoinNonce& nonce, bool transfer) {
    // RFC 2104: SHA256((K xor opad) || SHA256((K xor ipad) || text)).
    Bytes inner(64, 0x36), outer(64, 0x5c);
    inner.reserve(192); outer.reserve(96); // Allocate before adding key material.
    for (std::size_t index = 0; index < token.size(); ++index) {
        inner[index] ^= token[index]; outer[index] ^= token[index];
    }
    const std::string_view domain = transfer ? "EAWR-rejoin-v1" : "EAWR-rejoin-link-v1";
    inner.insert(inner.end(), domain.begin(), domain.end());
    inner.insert(inner.end(), identity.begin(), identity.end());
    append(inner, seat, 4); append(inner, epoch, 8);
    inner.insert(inner.end(), nonce.begin(), nonce.end());
    auto digest = core::sha256(inner);
    outer.insert(outer.end(), digest.begin(), digest.end());
    const auto result = core::sha256(outer);
    erase_reconnect_secret(inner); erase_reconnect_secret(outer); erase_reconnect_secret(digest);
    return result;
}

std::string LockstepSession::resync_log_hash() const {
    Bytes bytes;
    const std::string_view domain = "eawr-resync-input-v1";
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    const auto& options = *options_.reconnect;
    bytes.insert(bytes.end(), options.match_identity.begin(), options.match_identity.end());
    append(bytes, options.setup.size(), 4);
    bytes.insert(bytes.end(), options.setup.begin(), options.setup.end());
    append(bytes, reconnect_log_.size(), 8);
    for (const auto& row : reconnect_log_) {
        append(bytes, row.size(), 4); bytes.insert(bytes.end(), row.begin(), row.end());
    }
    return core::sha256_hex(bytes);
}

} // namespace eawr::net
