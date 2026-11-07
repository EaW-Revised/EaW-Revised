#pragma once

#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/galactic_checkpoint_transfer.hpp"

#include <atomic>
#include <deque>
#include <future>

namespace eawr::net {

// In-match recovery uses its own envelopes; the checkpoint bytes use 90..94.
inline constexpr std::uint8_t gc_recovery_request_kind = 95, gc_recovery_log_kind = 96,
    gc_recovery_proof_kind = 97, gc_recovery_refusal_kind = 98;

// Named reasons are values, so an unsuccessful candidate never fails the live pump.
enum class GalacticRecoveryRefusal : std::uint8_t {
    none, unavailable, divergent_epoch, anchor_hash, log_too_old, seat_binding,
    malformed, limit, restore, replay, state_hash, stale_barrier, busy, cancelled
};
[[nodiscard]] std::string_view galactic_recovery_reason(GalacticRecoveryRefusal) noexcept;

using RecoveryDigest = std::array<std::uint8_t, 32>;
struct GalacticRecoveryAnchor {
    std::uint64_t epoch{}, completed_tick{};
    RecoveryDigest lineage{}, state{};
    friend bool operator==(const GalacticRecoveryAnchor&, const GalacticRecoveryAnchor&) = default;
};
struct GalacticRecoveryRequest {
    GalacticRecoveryAnchor anchor;
    sim::SeatId seat{};
    std::uint64_t transfer_epoch{}; // nonzero on the wire; independent of the simulation lineage
};
struct GalacticRecoveryTick {
    std::uint64_t tick{};
    std::vector<SeatInput> inputs; // sealed human/session input; private services regenerate engine input
    RecoveryDigest state{};       // completed tick tick+1
};
struct GalacticRecoveryPackage {
    GalacticRecoveryAnchor checkpoint, frontier;
    RecoveryDigest checkpoint_digest{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::vector<PeerSeat> seats;
    std::vector<GalacticRecoveryTick> ticks;
    std::optional<RecoverySessionFrontier> session_frontier;
};
struct GalacticRecoveryProof {
    PeerIndex peer{}; // supplied by the authenticated stream, never the payload
    GalacticRecoveryAnchor frontier;
    RecoveryDigest sealed_log{};
};

enum class GalacticRecoveryStage : std::uint8_t { staged, fence, vote, commit, published, release };
struct GalacticRecoveryControl {
    std::uint64_t transfer_epoch{}, barrier{};
    PeerIndex source{}, target{};
    GalacticRecoveryStage stage{};
    GalacticRecoveryAnchor frontier;
    RecoveryDigest sealed_log{};
    std::uint8_t removed{}; // release only: unpublished peers quarantined before ordinary departure
};
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_galactic_recovery_control(const GalacticRecoveryControl&);
[[nodiscard]] core::Result<GalacticRecoveryControl> decode_galactic_recovery_control(std::span<const std::uint8_t>);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_galactic_recovery_request(const GalacticRecoveryRequest&);
[[nodiscard]] core::Result<GalacticRecoveryRequest> decode_galactic_recovery_request(std::span<const std::uint8_t>);
// Only the sealed suffix travels here. The immutable checkpoint buffer is
// distributed by GalacticCheckpointTransfer and attached after digest validation.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_galactic_recovery_log(const GalacticRecoveryPackage&);
[[nodiscard]] core::Result<GalacticRecoveryPackage> decode_galactic_recovery_log(
    std::span<const std::uint8_t>, std::shared_ptr<const std::vector<std::uint8_t>> checkpoint);

// One ordered catch-up stream, with independent epoch, byte and chunk bounds.
// A duplicate never extends progress or grows storage. An out-of-order or changed
// duplicate refuses this candidate; the caller keeps pumping ordinary lockstep.
class GalacticRecoveryStream final {
public:
    explicit GalacticRecoveryStream(std::uint64_t epoch, std::size_t limit = 8U * 1024U * 1024U)
        : epoch_(epoch), limit_(limit) {}
    [[nodiscard]] static core::Result<std::vector<std::uint8_t>> chunk(
        std::uint64_t epoch, std::span<const std::uint8_t> log, std::size_t offset);
    [[nodiscard]] GalacticRecoveryRefusal receive(std::span<const std::uint8_t>);
    [[nodiscard]] bool complete() const noexcept { return total_ && bytes_.size() == *total_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t progress() const noexcept { return bytes_.size(); }
private:
    std::uint64_t epoch_{};
    std::size_t limit_{};
    std::optional<std::size_t> total_;
    std::vector<std::uint8_t> bytes_;
};

// Simulation-thread owned. Checkpoints are installed only after canonical capture
// agreement. Exhaustion evicts old rows and explicitly refuses incomplete recovery;
// it never changes admission, the departure policy, or the running owner.
class GalacticRecoveryLog final {
public:
    explicit GalacticRecoveryLog(std::size_t limit = 8U * 1024U * 1024U) : limit_(limit) {}
    [[nodiscard]] GalacticRecoveryRefusal checkpoint(GalacticRecoveryAnchor,
        std::shared_ptr<const std::vector<std::uint8_t>>, std::vector<PeerSeat>);
    [[nodiscard]] GalacticRecoveryRefusal append(std::uint64_t tick,
        std::span<const SeatInput>, RecoveryDigest completed_state);
    [[nodiscard]] GalacticRecoveryRefusal package(PeerIndex authenticated_peer,
        const GalacticRecoveryRequest&, GalacticRecoveryPackage&) const;
    // A split invalidates the checkpoint in this lineage. A new agreed capture is
    // required before recovery; divergent descendants cannot borrow the old base.
    void invalidate() noexcept;
    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_; }
    [[nodiscard]] const GalacticRecoveryAnchor& frontier() const noexcept { return frontier_; }
    [[nodiscard]] const GalacticRecoveryAnchor& checkpoint_anchor() const noexcept { return checkpoint_; }
private:
    std::size_t limit_{}, retained_{};
    GalacticRecoveryAnchor checkpoint_{}, frontier_{};
    RecoveryDigest digest_{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
    std::vector<PeerSeat> seats_;
    std::deque<GalacticRecoveryTick> ticks_;
    bool expired_{};
};

// The factory owns fresh mounted services, game, adapter and executor. In
// particular it must not capture a live owner or a transport. The adapter's
// destructor releases those resources through the factory's concrete owner.
class GalacticRecoveryWorker final {
public:
    using Restore = std::function<core::Result<std::unique_ptr<SessionAdapter>>(
        const GalacticRecoveryPackage&)>;
    struct Result {
        GalacticRecoveryRefusal refusal{GalacticRecoveryRefusal::none};
        GalacticRecoveryAnchor frontier;
        std::unique_ptr<SessionAdapter> owner;
    };
    GalacticRecoveryWorker() = default;
    ~GalacticRecoveryWorker();
    GalacticRecoveryWorker(const GalacticRecoveryWorker&) = delete;
    GalacticRecoveryWorker& operator=(const GalacticRecoveryWorker&) = delete;
    [[nodiscard]] GalacticRecoveryRefusal start(GalacticRecoveryPackage, Restore);
    // A ready candidate is released only at the exact agreed lockstep barrier.
    // Refused/stale candidates are discarded without publishing any private state.
    [[nodiscard]] std::optional<Result> take(const GalacticRecoveryAnchor& barrier);
    void cancel() noexcept;
    [[nodiscard]] bool busy() const noexcept { return result_.valid(); }
private:
    std::future<Result> result_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};

} // namespace eawr::net
