#pragma once
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/gns_transport.hpp"

namespace eawr::presentation::godot_backend {
// Shared setup/staging owner; all native operations stay on its I/O thread.
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
    bool has_pending_output() const {
        if (tcp_) return tcp_->has_pending_output();
        for (net::PeerIndex peer = 0; peer < gns_->peer_count(); ++peer)
            if (peer != gns_->self() && gns_->queue_progress(peer).outbound_frames != 0) return true;
        return false;
    }
    bool listening() const { return tcp_ && tcp_->listening(); }
    core::Result<void> close_peer(net::PeerIndex peer) {
        if (tcp_) return tcp_->close_peer(peer);
        return gns_->close_peer(peer);
    }
    // Service mesh operations stay separate from opt-in TCP staging.
    bool mesh() const { return gns_ && gns_->mesh(); }
    bool mesh_complete() const { return !mesh() || gns_->mesh_complete(); }
    bool staging_capacity() const { return tcp_ && tcp_->staging_capacity(); }
    bool staging_mesh_ready() const { return !tcp_ || tcp_->staging_mesh_ready(); }
    bool mesh_complete(std::uint32_t active) const {
        return staging_capacity() ? tcp_->staging_roster_ready(active) : !mesh() || gns_->mesh_complete(active);
    }
    bool room_locked() const { return gns_ && gns_->room_locked(); }
    core::Result<void> lock_room() { return mesh() ? gns_->lock_room() : core::Result<void>::success(); }
    core::Result<bool> lock_roster(std::uint32_t active) {
        return staging_capacity() ? tcp_->lock_staging_roster(active) : mesh() ? gns_->lock_roster(active) : core::Result<bool>::success(true);
    }
    core::Result<void> commit_room() { return mesh() ? gns_->commit_room() : core::Result<void>::success(); }
    std::optional<net::GnsMeshEvent> take_mesh_event() { return mesh() ? gns_->take_event() : std::nullopt; }
    net::GnsPairDiagnostic pair(net::PeerIndex peer) const { return gns_ ? gns_->pair(peer) : net::GnsPairDiagnostic{}; }
    net::GnsQueueProgress queue_progress(net::PeerIndex peer) const { return gns_ ? gns_->queue_progress(peer) : net::GnsQueueProgress{}; }
    net::GnsSignalCounts signal_counts(net::PeerIndex peer) const { return gns_ ? gns_->signal_counts(peer) : net::GnsSignalCounts{}; }
    std::uint64_t edge_identity(net::PeerIndex peer) const override { return stream().edge_identity(peer); }
    void fail_repair(net::PeerIndex peer, unsigned code) override { stream().fail_repair(peer, code); }
    core::Result<void> reopen_staging_listener() { return tcp_ ? tcp_->reopen_staging_listener() : core::Result<void>::success(); }
    core::Result<void> repair_closed_links(std::optional<net::TcpRejoinAuth> auth = {}) {
        if (tcp_) return tcp_->repair_closed_links(std::move(auth));
        return auth ? gns_->repair_closed_links(std::move(*auth)) : unavailable();
    }
    core::Result<void> restart_links(std::optional<net::TcpRejoinAuth> auth = {}) {
        if (tcp_) return tcp_->restart_links(std::move(auth));
        return auth ? gns_->restart_links(std::move(*auth)) : unavailable();
    }
    void erase_reconnect_credentials() noexcept { if (tcp_) tcp_->erase_reconnect_credentials(); else gns_->erase_reconnect_credentials(); }
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
