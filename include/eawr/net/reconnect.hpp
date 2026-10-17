#pragma once

#include "eawr/net/transport.hpp"
#include "eawr/core/sha256.hpp"
#include <array>
#include <algorithm>
#include <functional>
#include <string_view>

namespace eawr::net {

inline constexpr std::uint64_t max_recovery_timeout_ms = 3'600'000;

// MP-5 reserved kinds. Payloads are little-endian and incident-epoch bound.
inline constexpr std::uint8_t rejoin_hello_kind = 64, rejoin_offer_kind = 65,
    rejoin_chunk_kind = 66, rejoin_barrier_kind = 67, rejoin_proof_kind = 68,
    rejoin_ready_kind = 69, rejoin_resume_kind = 70, rejoin_expire_kind = 71;
namespace reconnect_errors {
inline constexpr std::string_view options = "EAWR-NET-1000", malformed = "EAWR-NET-1001",
    identity = "EAWR-NET-1002", epoch = "EAWR-NET-1003", log = "EAWR-NET-1004",
    limit = "EAWR-NET-1005", hash = "EAWR-NET-1006", timeout = "EAWR-NET-1007",
    unavailable = "EAWR-NET-1008", stale = "EAWR-NET-1009",
    auth = "EAWR-NET-1011", nonce = "EAWR-NET-1012";
}

// HELLO 64 now carries epoch:u64, phase:u8, seat:u32, identity:32;
// request(0) has no suffix, challenge(1) adds nonce:32, response(2) adds
// nonce:32 and HMAC:32. Integers are little-endian. No secret is transmitted.
inline constexpr std::uint8_t rejoin_request = 0, rejoin_challenge = 1, rejoin_response = 2;
// After catch-up, authenticate the returning stream independently at every
// other active survivor. These phases use the separate EAWR-rejoin-link-v1 domain.
inline constexpr std::uint8_t rejoin_link_request = 3, rejoin_link_challenge = 4, rejoin_link_response = 5;
using RejoinNonce = std::array<std::uint8_t, 32>;
// Volatile stores keep credential erasure observable to the optimizer.
inline void erase_reconnect_secret(std::span<std::uint8_t> bytes) noexcept {
    volatile std::uint8_t* target = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index) target[index] = 0;
}
// Session envelopes copy tokens, but every owner erases its storage on release.
class ReconnectTokens : public std::vector<RejoinNonce> {
    using Base = std::vector<RejoinNonce>;
public:
    using Base::Base;
    ReconnectTokens() = default;
    ReconnectTokens(const Base& tokens) : Base(tokens) {}
    ReconnectTokens(Base&& tokens) noexcept : Base(std::move(tokens)) {}
    ReconnectTokens(const ReconnectTokens&) = default;
    ReconnectTokens(ReconnectTokens&&) noexcept = default;
    ReconnectTokens& operator=(const ReconnectTokens& tokens) {
        if (this != &tokens) { erase(); Base::operator=(tokens); }
        return *this;
    }
    ReconnectTokens& operator=(ReconnectTokens&& tokens) noexcept {
        if (this != &tokens) { erase(); Base::operator=(std::move(tokens)); }
        return *this;
    }
    ~ReconnectTokens() { erase(); }
    void erase() noexcept { for (auto& token : *this) erase_reconnect_secret(token); }
    void clear() noexcept { erase(); Base::clear(); }
};
// Each row holds K(self, remote); the unused self entry is not a credential.
[[nodiscard]] inline bool valid_reconnect_tokens(const ReconnectTokens& tokens, PeerIndex self, std::uint32_t count) noexcept {
    if (tokens.size() != count || self >= count) return false;
    for (PeerIndex remote = 0; remote < count; ++remote) {
        if (remote == self) continue;
        bool nonzero{};
        for (const auto byte : tokens[remote]) nonzero = nonzero || byte != 0;
        if (!nonzero) return false;
        for (PeerIndex previous = 0; previous < remote; ++previous)
            if (previous != self && tokens[previous] == tokens[remote]) return false;
    }
    return true;
}
// OS cryptographic random source shared by rejoin admission owners.
[[nodiscard]] core::Result<RejoinNonce> make_rejoin_nonce();
// RFC 2104 HMAC-SHA256, fixed 32-byte key. Authenticated text is the domain
// "EAWR-rejoin-v1", match identity:32, seat:u32, epoch:u64, nonce:32.
[[nodiscard]] std::array<std::uint8_t, 32> rejoin_authenticator(
    const std::array<std::uint8_t, 32>& token, const std::array<std::uint8_t, 32>& identity,
    std::uint32_t seat, std::uint64_t epoch, const RejoinNonce& nonce, bool transfer = true);

// Mutual, nonce-bound replacement-stream proof, using the held-seat pair key.
// A room capability and a GNS identity never substitute for this proof. The
// caller supplies a transport domain and only publishes the edge when proven.
class RejoinLinkProof final {
public:
    using Bytes = std::vector<std::uint8_t>;
    RejoinLinkProof(const RejoinNonce& match, const RejoinNonce& key, PeerIndex self,
        PeerIndex remote, std::uint64_t epoch, std::string_view domain)
        : key_(key), self_(self), remote_(remote), epoch_(epoch) {
        Bytes text(domain.begin(), domain.end()); text.insert(text.end(), match.begin(), match.end());
        put(text, std::min(self, remote), 4); put(text, std::max(self, remote), 4);
        identity_ = core::sha256(text);
    }
    ~RejoinLinkProof() { erase_reconnect_secret(key_); }
    core::Result<Bytes> start() {
        if (nonce_) return core::Result<Bytes>::success(frame(rejoin_link_challenge, remote_, *nonce_));
        auto nonce = make_rejoin_nonce();
        if (!nonce) return core::Result<Bytes>::failure(nonce.error());
        nonce_ = nonce.value();
        return core::Result<Bytes>::success(frame(rejoin_link_challenge, remote_, *nonce_));
    }
    core::Result<std::optional<Bytes>> receive(std::span<const std::uint8_t> bytes) {
        using Result = core::Result<std::optional<Bytes>>;
        const auto bad = [] {
            core::Diagnostic error; error.code = "EAWR-NET-1792"; error.message = "Replacement pair proof differs";
            return Result::failure(std::move(error));
        };
        if (bytes.size() < 78 || bytes[0] != rejoin_hello_kind || get(bytes.subspan(1), 8) != epoch_
            || !std::equal(identity_.begin(), identity_.end(), bytes.begin() + 14)) return bad();
        const auto phase = bytes[9]; const auto seat = get(bytes.subspan(10), 4);
        RejoinNonce nonce{}; std::copy_n(bytes.begin() + 46, 32, nonce.begin());
        if (phase == rejoin_link_challenge && seat == remote_seat(self_) && bytes.size() == 78) {
            if (peer_nonce_) {
                if (*peer_nonce_ != nonce) return bad();
                return Result::success({}); // The reliable stream already carries the response.
            }
            peer_nonce_ = nonce;
            auto reply = frame(rejoin_link_response, self_, nonce);
            const auto tag = rejoin_authenticator(key_, identity_, remote_seat(self_), epoch_, nonce, false);
            reply.insert(reply.end(), tag.begin(), tag.end());
            return Result::success(std::move(reply));
        }
        if (phase == rejoin_link_response && seat == remote_seat(remote_) && bytes.size() == 110 && nonce_ && nonce == *nonce_) {
            const auto tag = rejoin_authenticator(key_, identity_, remote_seat(remote_), epoch_, nonce, false);
            std::uint8_t difference{};
            for (std::size_t i = 0; i < tag.size(); ++i) difference |= tag[i] ^ bytes[78 + i];
            if (difference != 0) return bad();
            verified_ = true; return Result::success({});
        }
        return bad();
    }
    bool proven() const { return verified_ && peer_nonce_.has_value(); }
    bool recognizes(std::span<const std::uint8_t> bytes) const {
        return bytes.size() >= 46 && bytes[0] == rejoin_hello_kind
            && std::equal(identity_.begin(), identity_.end(), bytes.begin() + 14);
    }
private:
    static std::uint32_t remote_seat(PeerIndex peer) { return peer + 1; }
    static void put(Bytes& bytes, std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    static std::uint64_t get(std::span<const std::uint8_t> bytes, unsigned count) {
        std::uint64_t value{};
        for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
        return value;
    }
    Bytes frame(std::uint8_t phase, PeerIndex seat, const RejoinNonce& nonce) const {
        Bytes bytes{rejoin_hello_kind}; put(bytes, epoch_, 8); bytes.push_back(phase); put(bytes, remote_seat(seat), 4);
        bytes.insert(bytes.end(), identity_.begin(), identity_.end()); bytes.insert(bytes.end(), nonce.begin(), nonce.end()); return bytes;
    }
    RejoinNonce key_, identity_{};
    PeerIndex self_{}, remote_{};
    std::uint64_t epoch_{};
    std::optional<RejoinNonce> nonce_, peer_nonce_;
    bool verified_{};
};

struct ReconnectOptions {
    // False retains history for connected-peer recovery only; no new stream is admitted.
    bool allow_rejoin{true};
    // Agreed before tick zero, including grace and log budget. Credentials belong
    // to the session envelope, never the sim/replay. The caller supplies random,
    // nonzero pairwise seat keys through its setup delivery policy (trusted plaintext
    // LAN explicitly accepts eavesdropping; confidential channels can replace it).
    std::array<std::uint8_t, 32> match_identity{};
    ReconnectTokens seat_tokens; // Pairwise K(local seat, remote seat), indexed by remote; self is unused.
    std::vector<std::uint8_t> setup; // Locally verified, canonical, command-free setup.
    std::uint64_t grace_ms{30'000};
    // Authenticated return starts a progress deadline. The original incident's
    // hard cap governs admission before READY. All intervals are agreed before play.
    std::uint64_t progress_timeout_ms{30'000}, hard_cap_ms{300'000};
    std::uint64_t hello_retry_ms{1000};
    std::size_t max_hello_attempts{8};
    std::size_t max_log_bytes{64U * 1024U * 1024U};
    // A fresh tick-zero adapter, rebuilt from this same locally verified setup,
    // can enter as a returning seat. Source is an existing surviving peer.
    std::optional<PeerIndex> rejoin_source;
    std::uint64_t rejoin_epoch{};
    // Timing/session inputs, applied before their original tick's commands.
    // The callback updates session policy only, never injects engine-generated AI.
    std::function<core::Result<void>(std::span<const std::uint8_t>)> replay_control;
};

enum class ReconnectPhase { playing, agreeing, held, catching_up, proving };
struct ReconnectStatus {
    ReconnectPhase phase{ReconnectPhase::playing};
    PeerIndex peer{};
    std::uint64_t epoch{}, barrier_tick{};
};

} // namespace eawr::net
