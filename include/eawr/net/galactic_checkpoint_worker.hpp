#pragma once

#include "eawr/net/galactic_checkpoint_transfer.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace eawr::net {

// Native staging operations, including GNS's process-wide callback registry, share this gate.
// Never hold it while hashing, invoking application callbacks, or joining a worker.
std::recursive_mutex& checkpoint_io_mutex();

// Protocol and native receive owner during staging. The foreground drains unrelated frames
// and services immutable restore requests; it never advances the wire protocol while attached.
class GalacticCheckpointWorker final {
public:
    using Bytes = std::vector<std::uint8_t>;
    GalacticCheckpointWorker();
    ~GalacticCheckpointWorker();
    GalacticCheckpointWorker(const GalacticCheckpointWorker&) = delete;
    GalacticCheckpointWorker& operator=(const GalacticCheckpointWorker&) = delete;
    void start(MessageTransport& native);
    void stop(); // joins outside all locks; native must outlive this call
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::optional<Received> poll_foreground();
    [[nodiscard]] std::optional<core::Diagnostic> failure() const;
    [[nodiscard]] bool pending_from(PeerIndex) const;
    [[nodiscard]] std::optional<std::uint64_t> take_cleared_epoch();
    [[nodiscard]] core::Result<void> select(MessageTransport&, SaveManifest,
        std::shared_ptr<const Bytes>, GalacticCheckpointTransfer::Restore, std::uint64_t now);
    [[nodiscard]] core::Result<void> receive_async(MessageTransport&, const Received&,
        GalacticCheckpointTransfer::AsyncRestore, std::uint64_t now);
    [[nodiscard]] core::Result<void> pump(MessageTransport&, std::uint64_t now);
    void service_restore(const GalacticCheckpointTransfer::AsyncRestore&);
    void cancel(MessageTransport&, std::string reason);
    void reset(); // authenticated clear notices survive until take_cleared_epoch(), attached or stopped
    void set_trace(GalacticCheckpointTransfer::Trace);
    [[nodiscard]] SaveTransferView view() const;
    [[nodiscard]] std::uint64_t requested_epoch() const;
    [[nodiscard]] std::shared_ptr<const SaveManifest> manifest() const;
    [[nodiscard]] std::shared_ptr<const Bytes> restored_bytes() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
