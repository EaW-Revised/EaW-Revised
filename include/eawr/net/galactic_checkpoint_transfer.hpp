#pragma once

#include "eawr/net/galactic_setup.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace eawr::net {

// Coordinator-reserved pre-setup messages. These never enter the setup or input tail queues.
inline constexpr std::uint8_t save_manifest_kind = 90, save_chunk_kind = 91,
    save_progress_kind = 92, save_restore_kind = 93, save_cancel_kind = 94;
inline constexpr std::size_t save_chunk_bytes = 64U * 1024U;
inline constexpr std::size_t save_description_limit = 160, save_date_limit = 80;
// Manifest extension 2 carries display-only text; an absent extension is the legacy lobby form.
inline constexpr std::uint8_t save_display_metadata_version = 2;
inline constexpr std::size_t save_transfer_limit = 64U * 1024U * 1024U;

// Present only on an explicitly recovery-only transfer. The agreed offer remains
// byte-exact; a loading-barrier tick-zero base is never a selectable lobby save.
struct RecoveryTransferIdentity {
    std::uint64_t initial_tick{};
    sim::SeatId source{}, target{};
    std::array<std::array<std::uint8_t, 32>, 4> proof{}; // file, state, frontier, offer
};
struct SaveManifest {
    std::uint64_t epoch{}, byte_length{};
    std::vector<PeerIndex> peers; // staging stream indices, in dense offer-seat order; host first
    // Includes build/content identities, N, frontier, mapping/terms and saved logical seats.
    SetupOffer offer;
    std::optional<RecoveryTransferIdentity> recovery{};
    std::string description, date; // Untrusted display text, excluded from the offer and restore proof.
};
struct SaveTransferView {
    std::uint64_t epoch{}, received{}, total{};
    bool restored{}, agreed{};
    std::optional<core::Diagnostic> refusal;
};

// Caller-driven on the staging transport owner. One 64 MiB allocation per receiver,
// one immutable host buffer shared by all sends, at most four unacknowledged chunks
// per peer. Call receive only for kinds 90..94 and pump even while UI is unready.
class GalacticCheckpointTransfer final {
public:
    using Restore = std::function<core::Result<void>(const SaveManifest&, std::span<const std::uint8_t>)>;
    // False keeps the lobby manifest contract strict, including N > 0.
    explicit GalacticCheckpointTransfer(bool recovery_only = false);
    using AsyncRestore = std::function<core::Result<bool>(const SaveManifest&,
        std::shared_ptr<const std::vector<std::uint8_t>>)>;
    ~GalacticCheckpointTransfer();
    GalacticCheckpointTransfer(GalacticCheckpointTransfer&&) noexcept;
    GalacticCheckpointTransfer& operator=(GalacticCheckpointTransfer&&) noexcept;
    [[nodiscard]] core::Result<void> select(MessageTransport&, SaveManifest,
        std::shared_ptr<const std::vector<std::uint8_t>>, Restore, std::uint64_t now);
    [[nodiscard]] core::Result<void> receive(MessageTransport&, const Received&, Restore, std::uint64_t now);
    // Polls the callback from pump after the final chunk; false means the private
    // worker is still restoring. No ACK is sent before it returns true.
    [[nodiscard]] core::Result<void> receive_recovery(MessageTransport&, const Received&, AsyncRestore, std::uint64_t now);
    [[nodiscard]] core::Result<void> pump(MessageTransport&, std::uint64_t now);
    void cancel(MessageTransport&, std::string reason);
    void reset(); // explicit New Campaign only; stale epoch high-water mark survives
    [[nodiscard]] SaveTransferView view() const;
    [[nodiscard]] const SaveManifest* manifest() const;
    // The verified lobby receiver buffer, retained without a second file copy.
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> restored_bytes() const noexcept;
    [[nodiscard]] static bool handles(std::span<const std::uint8_t> frame) noexcept {
        return !frame.empty() && frame.front() >= save_manifest_kind && frame.front() <= save_cancel_kind;
    }
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
