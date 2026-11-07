#pragma once

#include "eawr/net/setup.hpp"

namespace eawr::net {
inline constexpr std::uint8_t lan_credentials_kind = 80, lan_credentials_ack_kind = 81;

// Trusted LAN only: admitted TCP streams are plaintext. An encrypted transport
// can supply the same provider without exposing credentials to the network.
// Delivery: kind, version:u8, destination:u32, offer SHA-256:32, count:u32,
// count-1 distinct pairwise keys:32 in remote-seat order, omitting destination.
// The count field is the number of keys. ACK: kind, acknowledging seat:u32, offer SHA-256:32.
class LanReconnectCredentials final {
public:
    [[nodiscard]] core::Result<ReconnectTokens> provide(const SetupOffer&, MessageTransport&);
    // Encrypted online room mesh: authenticate replacement pair streams without
    // enabling the offer's separate returning-seat/rebuild recovery policy.
    // Binds the actual frozen offer; delivery and ACK are unchanged.
    [[nodiscard]] core::Result<ReconnectTokens> provide_mesh_repair(const SetupOffer&, MessageTransport&);
    // GC recovery binds the frozen campaign offer for two or more saved seats;
    // it does not enable tactical grace or alter the ordinary mesh provider.
    [[nodiscard]] core::Result<ReconnectTokens> provide_galactic_recovery(const SetupOffer&, MessageTransport&, std::span<const PeerIndex> native_peers = {});
    // Consumes and erases credential frames before setup/monitor/replay see them.
    [[nodiscard]] core::Result<bool> receive(Received&, MessageTransport&);
    void clear() noexcept;
    [[nodiscard]] bool delivered() const noexcept;
private:
    [[nodiscard]] core::Result<ReconnectTokens> provide_for_offer(const SetupOffer&, MessageTransport&, std::span<const PeerIndex> native_peers = {});
    ReconnectTokens tokens_;
    std::array<std::uint8_t, 32> identity_{};
    PeerIndex destination_{};
    bool received_{}, verified_{};
    std::vector<bool> acknowledged_;
};
} // namespace eawr::net
