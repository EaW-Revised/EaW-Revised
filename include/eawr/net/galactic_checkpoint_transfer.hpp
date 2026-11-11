#pragma once

#include "eawr/core/checkpoint_limits.hpp"
#include "eawr/net/galactic_setup.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace eawr::net {

// Coordinator-reserved pre-setup messages. These never enter the setup or input tail queues.
inline constexpr std::uint8_t save_manifest_kind = 90, save_chunk_kind = 91,
    save_progress_kind = 92, save_restore_kind = 93, save_cancel_kind = 94;
inline constexpr std::size_t save_chunk_bytes = 64U * 1024U;
// Unacknowledged chunks per peer. The host also tops the window up as soon as an
// acknowledgement arrives, so a small save needs a couple of frames, not one per window.
inline constexpr std::size_t save_window_bytes = 2U * 1024U * 1024U;
inline constexpr std::size_t save_window_chunks = save_window_bytes / save_chunk_bytes;
inline constexpr std::size_t save_description_limit = 160, save_date_limit = 80;
// Manifest extension 2 carries display-only text; an absent extension is the legacy lobby form.
inline constexpr std::uint8_t save_display_metadata_version = 2;
inline constexpr std::size_t save_transfer_limit = core::checkpoint_byte_limit;

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
    // #6130 timing, in the caller's millisecond clock. Host: first chunk sent .. last chunk acknowledged
    // (every guest), then .. every guest restored. Guest: first chunk received .. last chunk received,
    // then .. restore finished. `pumps` counts pump calls from selection (host) or manifest (guest) to done.
    std::uint64_t chunks{}, first_chunk_ms{}, last_chunk_ms{}, restored_ms{}, pumps{};
    // Guest only: how long the restore really took, on a steady clock (the caller's `now` is one value per lobby frame, so a
    // restore that blocks the frame does not show in restored_ms - last_chunk_ms). Zero until it has finished.
    std::uint64_t restore_wall_ms{};
    bool timed_chunks{};
    std::optional<core::Diagnostic> refusal;
};

// Caller-driven on the staging transport owner. One bounded allocation per receiver,
// one immutable host buffer shared by all sends, at most save_window_chunks unacknowledged chunks
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
    // The same private-worker restore for a lobby receiver (#6130): the final chunk is acknowledged at once, the callback is
    // polled from pump until it answers true, and the restore ACK waits for it. The verified file stays retained
    // (`restored_bytes`) afterwards. The callback runs on the caller's thread and must not block.
    [[nodiscard]] core::Result<void> receive_async(MessageTransport&, const Received&, AsyncRestore, std::uint64_t now);
    [[nodiscard]] core::Result<void> pump(MessageTransport&, std::uint64_t now);
    // #6178 follow-up: a progress observer for the lobby log. Called with a short event name (selected, manifest-sent, manifest,
    // first-chunk, chunk, chunk-ack, restore-start, restore-end, peer-restored, agreed, refused) and a "key=value" detail
    // string that carries the caller's millisecond clock as now=. It never carries payload bytes, addresses or hashes.
    using Trace = std::function<void(std::string_view event, const std::string& detail)>;
    void set_trace(Trace);
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
    std::shared_ptr<Trace> trace_; // shared with every Impl this object creates, so a reset keeps the observer
};
} // namespace eawr::net
