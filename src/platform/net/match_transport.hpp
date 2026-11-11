#pragma once
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/gns_transport.hpp"
#include "eawr/net/galactic_checkpoint_worker.hpp"

namespace eawr::presentation::godot_backend {
// Shared setup/staging owner. Native operations are serialized with the checkpoint I/O worker.
class MatchTransport final : public net::MessageTransport {
public:
    explicit MatchTransport(std::unique_ptr<net::TcpTransport> value) : tcp_(std::move(value)) {}
    explicit MatchTransport(std::unique_ptr<net::GnsTransport> value) : gns_(std::move(value)) {}
    ~MatchTransport() override { stop_checkpoint(); }
    void start_checkpoint(std::shared_ptr<net::GalacticCheckpointWorker> worker) {
        if (checkpoint_ == worker && (worker->running() || worker->failure())) return;
        stop_checkpoint(); checkpoint_ = std::move(worker); checkpoint_->start(native_);
    }
    void stop_checkpoint() { if (checkpoint_) checkpoint_->stop(); }
    core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> bytes) override { const std::lock_guard lock(net::checkpoint_io_mutex()); return stream().send(peer, bytes); }
    std::optional<net::Received> poll() override {
        if (checkpoint_) {
            if (auto frame = checkpoint_->poll_foreground()) return frame;
            if (checkpoint_->running()) return {};
        }
        const std::lock_guard lock(net::checkpoint_io_mutex());
        // A failed checkpoint attachment remains fatal. Never hand its protocol frames
        // to the ordinary lobby owner; retain bounded polling and ordinary frame order.
        for (unsigned i = 0; i < 256; ++i) {
            auto frame = stream().poll();
            if (!frame || !checkpoint_ || !checkpoint_->failure()
                || !net::GalacticCheckpointTransfer::handles(frame->frame)) return frame;
        }
        return {};
    }
    net::LinkState link(net::PeerIndex peer) const override {
        const std::lock_guard lock(net::checkpoint_io_mutex());
        const auto state = stream().link(peer);
        return state == net::LinkState::closed && checkpoint_ && checkpoint_->pending_from(peer) ? net::LinkState::open : state;
    }
    net::PeerIndex self() const override { const std::lock_guard lock(net::checkpoint_io_mutex()); return stream().self(); }
    std::uint32_t peer_count() const override { const std::lock_guard lock(net::checkpoint_io_mutex()); return stream().peer_count(); }
    std::optional<net::LinkStats> link_stats(net::PeerIndex peer) const override { const std::lock_guard lock(net::checkpoint_io_mutex()); return stream().link_stats(peer); }
    std::uint16_t port() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return tcp_ ? tcp_->port() : gns_->port(); }
    std::optional<core::Diagnostic> diagnostic() const {
        if (checkpoint_) if (auto error = checkpoint_->failure()) return error;
        const std::lock_guard lock(net::checkpoint_io_mutex()); return tcp_ ? tcp_->diagnostic() : gns_->diagnostic();
    }
    void close() { stop_checkpoint(); const std::lock_guard lock(net::checkpoint_io_mutex()); if (tcp_) tcp_->close(); else gns_->close(); }
    bool online() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return static_cast<bool>(gns_); }
    bool closed() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return tcp_ ? tcp_->closed() : gns_->closed(); }
    bool has_pending_output() const { const std::lock_guard lock(net::checkpoint_io_mutex());
        if (tcp_) return tcp_->has_pending_output();
        for (net::PeerIndex peer = 0; peer < gns_->peer_count(); ++peer)
            if (peer != gns_->self() && gns_->queue_progress(peer).outbound_frames != 0) return true;
        return false;
    }
    bool listening() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return tcp_ && tcp_->listening(); }
    core::Result<void> close_peer(net::PeerIndex peer) { const std::lock_guard lock(net::checkpoint_io_mutex());
        if (tcp_) return tcp_->close_peer(peer);
        return gns_->close_peer(peer);
    }
    // Service mesh operations stay separate from opt-in TCP staging.
    bool mesh() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return gns_ && gns_->mesh(); }
    bool mesh_complete() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return !mesh() || gns_->mesh_complete(); }
    bool staging_capacity() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return tcp_ && tcp_->staging_capacity(); }
    bool staging_mesh_ready() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return !tcp_ || tcp_->staging_mesh_ready(); }
    bool mesh_complete(std::uint32_t active) const { const std::lock_guard lock(net::checkpoint_io_mutex());
        return staging_capacity() ? tcp_->staging_roster_ready(active) : !mesh() || gns_->mesh_complete(active);
    }
    bool room_locked() const { const std::lock_guard lock(net::checkpoint_io_mutex()); return gns_ && gns_->room_locked(); }
    core::Result<void> lock_room() { const std::lock_guard lock(net::checkpoint_io_mutex()); return mesh() ? gns_->lock_room() : core::Result<void>::success(); }
    core::Result<bool> lock_roster(std::uint32_t active) { const std::lock_guard lock(net::checkpoint_io_mutex());
        return staging_capacity() ? tcp_->lock_staging_roster(active) : mesh() ? gns_->lock_roster(active) : core::Result<bool>::success(true);
    }
    core::Result<void> commit_room() { const std::lock_guard lock(net::checkpoint_io_mutex()); return mesh() ? gns_->commit_room() : core::Result<void>::success(); }
    std::optional<net::GnsMeshEvent> take_mesh_event() { const std::lock_guard lock(net::checkpoint_io_mutex()); return mesh() ? gns_->take_event() : std::nullopt; }
    net::GnsPairDiagnostic pair(net::PeerIndex peer) const { const std::lock_guard lock(net::checkpoint_io_mutex()); return gns_ ? gns_->pair(peer) : net::GnsPairDiagnostic{}; }
    net::GnsQueueProgress queue_progress(net::PeerIndex peer) const { const std::lock_guard lock(net::checkpoint_io_mutex()); return gns_ ? gns_->queue_progress(peer) : net::GnsQueueProgress{}; }
    net::GnsSignalCounts signal_counts(net::PeerIndex peer) const { const std::lock_guard lock(net::checkpoint_io_mutex()); return gns_ ? gns_->signal_counts(peer) : net::GnsSignalCounts{}; }
    std::uint64_t edge_identity(net::PeerIndex peer) const override { const std::lock_guard lock(net::checkpoint_io_mutex()); return stream().edge_identity(peer); }
    void fail_repair(net::PeerIndex peer, unsigned code) override { const std::lock_guard lock(net::checkpoint_io_mutex()); stream().fail_repair(peer, code); }
    core::Result<void> reopen_staging_listener() { const std::lock_guard lock(net::checkpoint_io_mutex()); return tcp_ ? tcp_->reopen_staging_listener() : core::Result<void>::success(); }
    core::Result<void> repair_closed_links(std::optional<net::TcpRejoinAuth> auth = {}) { const std::lock_guard lock(net::checkpoint_io_mutex());
        if (tcp_) return tcp_->repair_closed_links(std::move(auth));
        return auth ? gns_->repair_closed_links(std::move(*auth)) : unavailable();
    }
    core::Result<void> restart_links(std::optional<net::TcpRejoinAuth> auth = {}) { const std::lock_guard lock(net::checkpoint_io_mutex());
        if (tcp_) return tcp_->restart_links(std::move(auth));
        return auth ? gns_->restart_links(std::move(*auth)) : unavailable();
    }
    void erase_reconnect_credentials() noexcept { const std::lock_guard lock(net::checkpoint_io_mutex()); if (tcp_) tcp_->erase_reconnect_credentials(); else gns_->erase_reconnect_credentials(); }
private:
    // The worker bypasses foreground routing, but uses the same native access gate.
    class Native final : public net::MessageTransport {
    public:
        explicit Native(MatchTransport& owner) : owner_(owner) {}
        core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> bytes) override { return owner_.send(peer, bytes); }
        std::optional<net::Received> poll() override { return owner_.stream().poll(); }
        net::LinkState link(net::PeerIndex peer) const override { return owner_.stream().link(peer); }
        net::PeerIndex self() const override { return owner_.self(); }
        std::uint32_t peer_count() const override { return owner_.peer_count(); }
    private:
        MatchTransport& owner_;
    } native_{*this};
    std::shared_ptr<net::GalacticCheckpointWorker> checkpoint_;
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
