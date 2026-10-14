#pragma once
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/gns_transport.hpp"

namespace eawr::presentation::godot_backend {
// Shared setup/staging owner. Online recovery is supplied by the later repair slice.
class MatchTransport final : public net::MessageTransport {
public:
    explicit MatchTransport(std::unique_ptr<net::TcpTransport> value) : tcp_(std::move(value)) {}
    explicit MatchTransport(std::unique_ptr<net::GnsTransport> value) : gns_(std::move(value)) {}
    core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> bytes) override { return stream().send(peer, bytes); }
    std::optional<net::Received> poll() override { return stream().poll(); }
    net::LinkState link(net::PeerIndex peer) const override { return stream().link(peer); }
    net::PeerIndex self() const override { return stream().self(); }
    std::uint32_t peer_count() const override { return stream().peer_count(); }
    std::optional<net::LinkStats> link_stats(net::PeerIndex peer) const override { return stream().link_stats(peer); }
    std::uint16_t port() const { return tcp_ ? tcp_->port() : gns_->port(); }
    const std::optional<core::Diagnostic>& diagnostic() const { return tcp_ ? tcp_->diagnostic() : gns_->diagnostic(); }
    void close() { if (tcp_) tcp_->close(); else gns_->close(); }
    bool online() const { return static_cast<bool>(gns_); }
    bool closed() const { return tcp_ ? tcp_->closed() : gns_->closed(); }
    bool listening() const { return tcp_ && tcp_->listening(); }
    core::Result<void> close_peer(net::PeerIndex peer) {
        if (tcp_) return tcp_->close_peer(peer);
        if (peer != self() && peer < 2) gns_->close();
        return core::Result<void>::success();
    }
    std::uint64_t edge_identity(net::PeerIndex peer) const { return tcp_ ? tcp_->edge_identity(peer) : (peer != self() && link(peer) == net::LinkState::open ? 1 : 0); }
    core::Result<void> reopen_staging_listener() { return tcp_ ? tcp_->reopen_staging_listener() : core::Result<void>::success(); }
    core::Result<void> repair_closed_links(std::optional<net::TcpRejoinAuth> auth = {}) { return tcp_ ? tcp_->repair_closed_links(std::move(auth)) : unavailable(); }
    core::Result<void> restart_links(std::optional<net::TcpRejoinAuth> auth = {}) { return tcp_ ? tcp_->restart_links(std::move(auth)) : unavailable(); }
    void erase_reconnect_credentials() noexcept { if (tcp_) tcp_->erase_reconnect_credentials(); }
private:
    core::Result<void> unavailable() const {
        core::Diagnostic error; error.code = "EAWR-NET-1530"; error.message = "Online seat repair is unavailable.";
        return core::Result<void>::failure(std::move(error));
    }
    net::MessageTransport& stream() { return tcp_ ? static_cast<net::MessageTransport&>(*tcp_) : *gns_; }
    const net::MessageTransport& stream() const { return tcp_ ? static_cast<const net::MessageTransport&>(*tcp_) : *gns_; }
    std::unique_ptr<net::TcpTransport> tcp_;
    std::unique_ptr<net::GnsTransport> gns_;
};
}
