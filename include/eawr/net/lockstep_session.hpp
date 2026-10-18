#pragma once

#include "eawr/net/session_monitor.hpp"
#include "eawr/net/mesh_incident.hpp"
#include "eawr/net/reconnect.hpp"
#include "eawr/sim/seat.hpp"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace eawr::net {

// Coordinator-reserved LAN-1 message kind, in the 16..31 range.
inline constexpr std::uint8_t sealed_batch_kind = 16;
inline constexpr std::size_t max_batch_bytes = 1024U * 1024U;
inline constexpr std::size_t max_batch_commands = 1024;
inline constexpr std::uint64_t max_session_ticks = 1'000'000;
inline constexpr std::uint64_t max_input_delay = 4096;
// Bounded retained departure controls cover the sealed horizon from every remote.
inline constexpr std::uint64_t departure_frame_limit(std::uint64_t horizon, std::size_t remote_peers) noexcept {
    const auto retained = (2 * horizon + 4) * remote_peers;
    return retained < 1024 ? 1024 : retained;
}

inline constexpr std::uint8_t timing_proposal_kind = 56;
inline constexpr std::uint8_t timing_ready_kind = 57;
namespace timing_errors {
inline constexpr std::string_view options = "EAWR-NET-0900";
inline constexpr std::string_view malformed = "EAWR-NET-0901";
inline constexpr std::string_view agreement = "EAWR-NET-0902";
inline constexpr std::string_view timeout = "EAWR-NET-0903";
}
// MP-05: logical 30 Hz, factor 2, latency floor 4/6 and lead cap 64.
[[nodiscard]] std::uint64_t adaptive_input_delay(std::uint64_t worst_rtt_ms, bool broadband = true) noexcept;
struct AdaptiveTiming {
    PeerIndex host{};
    bool broadband{true};
    // Override for qualification. Empty uses MessageTransport::link_stats smoothed RTT.
    // Missing samples defer the policy. Only the host reads this non-authoritative input.
    std::function<std::optional<std::uint64_t>(PeerIndex)> rtt_ms;
};

namespace session_errors {
inline constexpr std::string_view options = "EAWR-NET-0100";
inline constexpr std::string_view malformed = "EAWR-NET-0101";
inline constexpr std::string_view admission = "EAWR-NET-0102";
inline constexpr std::string_view stream_gap = "EAWR-NET-0103";
inline constexpr std::string_view conflicting_duplicate = "EAWR-NET-0104";
inline constexpr std::string_view link_closed = "EAWR-NET-0105";
inline constexpr std::string_view stalled = "EAWR-NET-0106";
inline constexpr std::string_view clock = "EAWR-NET-0107";
inline constexpr std::string_view resource_limit = "EAWR-NET-0108";
inline constexpr std::string_view simulation = "EAWR-NET-0109";
}

struct SeatInput {
    sim::SeatCommandKey key{};
    std::vector<std::uint8_t> bytes; // Already encoded by the mode's adapter, immutable after entry.
    friend bool operator==(const SeatInput&, const SeatInput&) = default;
};

struct PeerSeat {
    sim::SeatId seat{};
    bool may_submit{true}; // Observers still seal empty batches and participate in completeness.
};

struct SealedBatch {
    PeerIndex sender{};
    std::uint64_t tick{};
    std::uint64_t sequence{}; // Consecutive per sender, starting at zero, including empty batches.
    std::vector<SeatInput> commands;
};

// kind:u8, sender:u32, tick:u64, batch sequence:u64, count:u32; then for each command
// seat:u32, seat sequence:u64, byte length:u32, opaque bytes. All integers are little-endian.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_batch(const SealedBatch& batch);
[[nodiscard]] core::Result<SealedBatch> decode_batch(std::span<const std::uint8_t> frame);

// Coordinator-reserved LAN-6 kinds, 48..55. Reliable full-mesh streams authenticate voters.
inline constexpr std::uint8_t departure_proposal_kind = 48;
inline constexpr std::uint8_t departure_recovery_kind = 49;
inline constexpr std::uint8_t departure_ready_kind = 50;
inline constexpr std::uint8_t departure_ack_kind = 51;
namespace departure_errors {
inline constexpr std::string_view malformed = "EAWR-NET-0500";
inline constexpr std::string_view agreement = "EAWR-NET-0501";
inline constexpr std::string_view timeout = "EAWR-NET-0502";
inline constexpr std::string_view reconnect_hold = "EAWR-NET-0503";
}
// pair_loss: an N-peer room mesh removed the seat because a pair could not be restored (docs/net-online-npeer.md, section 3).
// Only a confirmed MeshIncident decision may propose it; every survivor must vote it.
enum class DepartureReason : std::uint8_t { quit, disconnect, timeout, pair_loss };
struct DepartureEvent {
    sim::SeatCommandKey key{};
    DepartureReason reason{DepartureReason::quit};
    friend bool operator==(const DepartureEvent&, const DepartureEvent&) = default;
};

struct CompletedTick {
    std::uint64_t completed_tick{};
    std::string state_sha256;
    // The adapter skipped its expensive (script) part. The session asks state_hash() for the full
    // value on exchange ticks and the end tick; DesyncMonitor::finish() replaces the last row with
    // the full value. Other rows stay world-only and still enter their window digest.
    bool world_only{};
};

// Mode providers implement admission, canonical routing, stepping and hashing.
// No tactical command type or world enters the core's sealed-input protocol.
class SessionAdapter {
public:
    virtual ~SessionAdapter() = default;
    virtual std::uint64_t completed_tick() const = 0;
    virtual std::string state_hash() const = 0;
    virtual std::vector<std::uint8_t> canonical_state() const = 0;
    virtual core::Result<void> validate(const SeatInput& input) const = 0;
    virtual core::Result<void> install(std::span<const SeatInput> canonical) = 0;
    // Replay encoding of the most recently completed tick, including engine-side inputs.
    virtual core::Result<std::vector<std::uint8_t>> canonical_input() const = 0;
    virtual core::Result<CompletedTick> step() = 0;
    virtual bool is_departure(const SeatInput& input) const = 0;
};

// MP-6 is an intentional remake extension (walk MP-24), agreed before play.
// Factories rebuild from verified local content without presentation side effects.
struct ResyncOptions {
    std::function<core::Result<std::unique_ptr<SessionAdapter>>(std::span<const std::uint8_t>)> rebuild;
    // Custom controls must target staging, never the live adapter/session.
    std::function<core::Result<void>(SessionAdapter&, std::span<const std::uint8_t>)> replay_control;
    std::uint64_t timeout_ms{30'000};
};
inline constexpr std::uint8_t resync_vote_kind = 58, resync_offer_kind = 59,
    resync_chunk_kind = 60, resync_proof_kind = 61, resync_ready_kind = 62, resync_abort_kind = 63;
namespace resync_errors {
inline constexpr std::string_view options = "EAWR-NET-1200", malformed = "EAWR-NET-1201",
    agreement = "EAWR-NET-1202", replay = "EAWR-NET-1203", hash = "EAWR-NET-1204",
    timeout = "EAWR-NET-1205", overlap = "EAWR-NET-1206", unavailable = "EAWR-NET-1207";
}

struct SessionOptions {
    // Fixed LAN MVP policy: MP-03 retail lead/rounding gives initial targets 4/6/15,
    // not simply D. D=3 is the qualified loopback default, not a retail latency claim.
    std::uint64_t input_delay{3};
    std::uint64_t final_tick_count{max_session_ticks};
    // Caller-time expiry fails a silent stream instead of inventing an empty batch.
    // This configurable guard is not a claim about MP-34's retail timeout policy.
    std::uint64_t completeness_timeout_ms{30'000};
    std::size_t max_frames_per_pump{256};
    // Optional transport-specific classification (e.g. keepalive expiry). Empty
    // means disconnect. Voters agree the reason along with the receive frontier.
    std::function<DepartureReason(PeerIndex)> closed_link_reason;
    // Live LAN intents can arrive again while the same frame is held. Keep C+D
    // open for that admission boundary; only ticks below it may be sealed.
    bool keep_admission_tick_open{};
    // A LAN producer may still be decoding or handing off an earlier input.
    // Observation is thread-safe and must not call back into this session.
    std::function<std::optional<std::uint64_t>()> pending_input_epoch;
    std::optional<AdaptiveTiming> adaptive_timing;
    // MP-05/MP-U1: host selects Internet/platform play explicitly; LAN stays fixed.
    bool non_lan{};
    // Reconnect owner retains the immutable agreed control, before its tick executes.
    std::function<core::Result<void>(std::uint64_t, std::span<const std::uint8_t>)> retain_timing_event;
    std::optional<ReconnectOptions> reconnect;
    std::optional<ResyncOptions> resync;
    // N-peer room mesh (slice N4). Requires enable_departures. The host is seat 0. Pair losses between three or more
    // seats go through the single-decider agreement instead of the closed-link scan, and the host's adaptive timing reads
    // the per-pair minimum of the seats' reports.
    std::optional<MeshIncidentOptions> mesh;
};

struct WaitingFor {
    std::uint64_t tick{};
    std::vector<PeerIndex> peers;
    std::uint64_t since_ms{}; // Start of this tick's completeness wait (MP-33).
};

struct PumpResult {
    std::optional<CompletedTick> completed;
    std::vector<DepartureEvent> departure_events;
    std::vector<sim::SeatCommandKey> departures; // MP-36 frame-tail notifications; policy is external.
};

// Caller-driven and single-threaded; adapter, transport, monitor outlive this session.
// An agreed setup/tick-zero barrier precedes play. The adapter is exclusively driven here.
class LockstepSession final {
public:
    LockstepSession(LockstepSession&&) noexcept = default;
    LockstepSession& operator=(LockstepSession&&) noexcept = default;
    LockstepSession(const LockstepSession&) = delete;
    LockstepSession& operator=(const LockstepSession&) = delete;
    [[nodiscard]] static core::Result<LockstepSession> create(SessionAdapter& adapter,
        MessageTransport& transport, std::vector<PeerSeat> seats,
        SessionOptions options = {}, SessionMonitor* monitor = nullptr);
    // The core stamps once at entry, then asks the mode to encode that stamped intent.
    using EncodeIntent = std::function<core::Result<std::vector<std::uint8_t>>(sim::SeatCommandKey)>;
    // LAN may retain the producer's completed-frame epoch when draining later.
    // Requires keep_admission_tick_open; an immutable sealed slot is never reopened.
    [[nodiscard]] core::Result<sim::SeatCommandKey> submit(const EncodeIntent& encode,
        std::optional<std::uint64_t> input_epoch = std::nullopt);
    // Optional observation only; never changes admission or the sealed frontier.
    [[nodiscard]] std::uint64_t next_seal() const noexcept { return next_seal_; }
    // Diagnostic frontiers: first unaccepted remote tick and latest locally
    // sealed batch accepted by send(). A successful send is not a peer ACK.
    [[nodiscard]] std::uint64_t received_through(PeerIndex peer) const { return received_through_.at(peer); }
    [[nodiscard]] std::optional<std::uint64_t> last_sent_batch(PeerIndex peer) const { return last_sent_batch_.at(peer); }
    [[nodiscard]] std::uint64_t input_delay() const noexcept { return options_.input_delay; }
    [[nodiscard]] std::uint64_t input_horizon() const noexcept;
    // Trusted verified catch-up log only: apply before the adapter steps the encoded tick.
    // No wire votes or gameplay commands are injected during replay.
    [[nodiscard]] core::Result<void> replay_timing_control(std::span<const std::uint8_t> bytes);
    using StageObserver = std::function<void(std::string_view, const SeatInput&, std::uint64_t)>;
    void set_stage_observer(StageObserver observer) { stage_observer_ = std::move(observer); }
    // Services transport and steps at most one tick; callers may loop while a tick completes.
    [[nodiscard]] core::Result<PumpResult> pump(std::uint64_t now_ms);
    [[nodiscard]] const std::optional<WaitingFor>& waiting_for() const noexcept { return waiting_; }
    [[nodiscard]] const std::optional<core::Diagnostic>& failure() const noexcept { return failure_; }
    [[nodiscard]] const SessionAdapter& active_adapter() const noexcept { return *adapter_; }
    [[nodiscard]] bool resyncing() const noexcept { return resync_.has_value(); }
    [[nodiscard]] ReconnectStatus reconnect_status() const noexcept;
    // Presentation reads existing incident clocks; no additional recovery timer.
    [[nodiscard]] std::optional<std::uint64_t> reconnect_grace_remaining_ms() const noexcept;
    [[nodiscard]] bool reconnect_available() const noexcept { return options_.reconnect && options_.reconnect->allow_rejoin && reconnect_available_; }
    // Terminal session owner only; no new stream may use this match's secrets.
    void erase_reconnect_credentials() noexcept {
        if (options_.reconnect) options_.reconnect->seat_tokens.clear();
        reconnect_available_ = false;
    }
    [[nodiscard]] std::optional<PeerIndex> connection_lost_peer() const noexcept { return connection_lost_peer_; }
    [[nodiscard]] const std::optional<core::Diagnostic>& reconnect_notice() const noexcept { return reconnect_notice_; }
    // Pair-loss incidents of the room mesh, oldest first and bounded: restored, departure and leave results. A departure
    // names the removed seats and, for each, the seats it could not reach; a leave carries the typed EAWR-NET-1778..1782.
    [[nodiscard]] const std::vector<MeshIncidentResult>& mesh_results() const noexcept { return mesh_results_; }
    [[nodiscard]] bool mesh_running() const noexcept { return mesh_ && mesh_->running(); }
    [[nodiscard]] std::uint64_t mesh_incident_id() const noexcept { return mesh_ ? mesh_->incident() : 0; }
    // Adaptive timing proposals pause while the host is gone or a pair-loss incident is running. Changes already agreed for
    // future ticks still apply; a returning host resumes the policy (EAWR-NET-1786 is the notice code).
    [[nodiscard]] bool timing_frozen() const noexcept;
    // Record agreed session-control bytes BEFORE stepping tick. Admission-policy
    // owners retain the original immutable event, including its activation tick.
    [[nodiscard]] core::Result<void> retain_reconnect_control(std::uint64_t tick,
        std::span<const std::uint8_t> bytes);
    void set_message_handler(std::function<core::Result<void>(Received)> handler) { handler_ = std::move(handler); }
    // Opt-in for modes supporting replayable synthetic departure inputs. Without it,
    // closed/silent streams retain the fail-closed LAN-1 behavior.
    using EncodeDeparture = std::function<core::Result<std::vector<std::uint8_t>>(sim::SeatCommandKey)>;
    void enable_departures(EncodeDeparture encode) { encode_departure_ = std::move(encode); }
    // Protocol (little-endian): proposal 48,target:u32,reason:u8,frontier:u64;
    // recovery 49,target:u32,sealed batch; ready 50,target:u32; ack 51,target:u32,cutoff:u64.
    // Suspicion freezes stepping and snapshots this peer's consecutive receive frontier.
    // Each survivor sends its retained batches before ready. After every ready, all
    // choose the maximum proposed frontier: the first tick missing from the union.
    // If recovery contains an ordinary quit, its original stamp is the agreed cutoff
    // instead; a survivor that already executed it still votes and relays that batch.
    // Otherwise this preserves inputs already executed by a faster survivor (a minimum without
    // rollback would not). All acknowledge that cutoff; only then may stepping resume.
    // Recovery validates the original seat/sequence and conflicts; it never restamps.
    // The synthetic quit executes at cutoff, with no missing batch treated as empty
    // before agreement. Further batches for that seat are excluded. Retained recovery
    // history and cutoff lead are bounded by sealed_input_horizon(D) = 2*D+2. An overlapping departure or
    // failed survivor stream fails closed rather than guessing a new voting roster.
    // A real desync freeze never starts application-liveness expiry. A drained
    // closed voter at the completed final-tick hash barrier gives link_closed in
    // both modes; a silent voter expires with stalled instead. Replay cannot
    // admit a new quit beyond its agreed final tick.
    // Called after frame service and monitor recording. LAN-6/GC own departure policy;
    // a policy requiring exchange agreement uses the monitor gate before applying it.
    void set_departure_handler(std::function<core::Result<void>(std::span<const sim::SeatCommandKey>)> handler) {
        departure_handler_ = std::move(handler);
    }

private:
    LockstepSession(SessionAdapter& adapter, MessageTransport& transport, std::vector<PeerSeat> seats,
        SessionOptions options, SessionMonitor* monitor);
    [[nodiscard]] core::Result<void> receive(Received received);
    [[nodiscard]] core::Result<void> seal();
    [[nodiscard]] core::Result<void> receive_timing(const Received& received);
    [[nodiscard]] core::Result<void> advance_timing();
    [[nodiscard]] core::Result<void> broadcast_timing(const std::vector<std::uint8_t>& frame);
    [[nodiscard]] core::Result<void> notify_timing();
    [[nodiscard]] core::Result<void> acknowledge_timing();
    [[nodiscard]] bool timing_voter(PeerIndex peer) const noexcept;
    [[nodiscard]] bool can_replay_control(std::span<const std::uint8_t> bytes) const noexcept;
    [[nodiscard]] core::Result<void> apply_reconnect_control(std::span<const std::uint8_t> bytes);
    [[nodiscard]] core::Result<void> restore_future_timing(std::span<const std::uint8_t> bytes);
    struct TimingEvent {
        std::uint64_t tick{}, delay{};
        friend bool operator==(const TimingEvent&, const TimingEvent&) = default;
    };
    struct TimingAgreement {
        TimingEvent event;
        std::vector<bool> ready;
        bool proposed{};
        std::uint64_t since_ms{};
        bool retained{};
    };
    [[nodiscard]] core::Result<void> begin_departure(PeerIndex target, DepartureReason reason, bool expired = false);
    [[nodiscard]] core::Result<void> receive_departure(const Received& received, bool deferred = false);
    [[nodiscard]] core::Result<void> advance_departure();
    [[nodiscard]] core::Result<void> service_departures(bool receive_drained);
    [[nodiscard]] core::Result<void> broadcast_departure(const std::vector<std::uint8_t>& frame);
    [[nodiscard]] core::Result<void> receive_reconnect(const Received& received);
    [[nodiscard]] core::Result<void> offer_reconnect(PeerIndex peer, std::uint64_t epoch);
    [[nodiscard]] core::Result<bool> service_reconnect();
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> reconnect_package() const;
    [[nodiscard]] core::Result<void> install_reconnect_package();
    [[nodiscard]] core::Result<void> check_held_survivors() const;
    [[nodiscard]] core::Result<void> reconnect_broadcast(std::span<const std::uint8_t> frame);
    [[nodiscard]] core::Result<void> finish_reconnect();
    void disable_reconnect(std::string message);
    void reconnect_notice(std::string_view code, std::string message);
    [[nodiscard]] core::Result<void> report_replay_progress(std::uint64_t tick);
    [[nodiscard]] core::Result<void> service_mesh(std::uint64_t now_ms, bool receive_drained);
    [[nodiscard]] core::Result<void> send_mesh_control(PeerIndex peer, const std::vector<std::uint8_t>& frame);
    [[nodiscard]] core::Result<void> restore_mesh_controls(PeerIndex peer);
    [[nodiscard]] core::Result<void> queue_mesh_departure(const Received& received, bool& queued);
    [[nodiscard]] MeshSeats mesh_voters() const noexcept;
    // The agreed active set of the pair-loss incident: the voters minus every seat that is leaving by an ordinary departure
    // (its quit batch arrived, or a timeout/disconnect agreement names it). Every seat has received a quitter's batch before
    // it can see the quitter's link close, so all seats leave it out at the same point of their own streams.
    [[nodiscard]] MeshSeats mesh_active() const noexcept;
    [[nodiscard]] bool mesh_excluded(PeerIndex peer) const noexcept { return (mesh_removed_ & mesh_seat(peer)) != 0; }
    [[nodiscard]] core::Result<void> receive_resync(const Received& received);
    [[nodiscard]] core::Result<bool> service_resync();
    [[nodiscard]] core::Result<void> start_resync(std::uint64_t epoch, std::uint64_t tick);
    [[nodiscard]] core::Result<void> resync_broadcast(std::span<const std::uint8_t> frame);
    [[nodiscard]] std::string resync_log_hash() const;
    [[nodiscard]] core::Result<void> stage_resync();
    struct ResyncVote { std::string state, log; friend bool operator==(const ResyncVote&, const ResyncVote&) = default; };
    struct ResyncIncident {
        std::uint64_t epoch{}, tick{}, since_ms{};
        std::map<PeerIndex, ResyncVote> votes;
        std::vector<bool> proofs, ready;
        std::optional<PeerIndex> source;
        std::string hash;
        bool sent_vote{}, offered{}, sent_proof{}, sent_ready{}, receiving{};
        std::size_t size{}, sent_bytes{};
        std::array<std::uint8_t, 32> digest{};
        std::vector<std::uint8_t> package;
        std::unique_ptr<SessionAdapter> adapter;
        std::vector<Received> monitor_frames;
        std::vector<Received> deferred;
        std::size_t deferred_bytes{};
    };
    struct HeldSeat {
        PeerIndex target{};
        std::uint64_t cutoff{}, since_ms{}, epoch{};
        std::vector<bool> proofs, ready, expired;
        bool offered{}, sent_proof{}, sent_ready{}, expiry_sent{}, resuming{};
        std::vector<std::uint8_t> outgoing;
        std::size_t sent_bytes{};
        DepartureReason reason{DepartureReason::disconnect};
        std::uint64_t progress_since_ms{}, progress_bytes{}, progress_tick{};
        std::optional<PeerIndex> source{};
        bool progress_started{}, replay_started{}, unavailable{};
    };
    struct Agreement {
        PeerIndex target{};
        std::vector<std::optional<std::uint64_t>> frontiers;
        std::vector<DepartureReason> reasons;
        std::vector<bool> ready;
        std::vector<std::optional<std::uint64_t>> acknowledgements;
        std::optional<std::uint64_t> cutoff;
        std::uint64_t since_ms{};
        bool reconnect_expired{};
        bool pair_loss{};
        std::uint64_t reconnect_epoch{};
    };
    [[nodiscard]] core::Diagnostic connection_loss(PeerIndex peer, std::string_view cause);
    [[nodiscard]] core::Result<PumpResult> fail(core::Diagnostic diagnostic);
    SessionAdapter* adapter_{};
    std::unique_ptr<SessionAdapter> owned_adapter_;
    MessageTransport* transport_{};
    std::vector<PeerSeat> seats_;
    SessionOptions options_;
    PeerIndex self_{};
    std::vector<std::optional<std::uint64_t>> last_sent_batch_;
    std::uint64_t next_seal_{}, next_sequence_{};
    StageObserver stage_observer_;
    std::vector<std::uint64_t> received_through_;
    std::size_t mesh_tail_bytes_{};
    bool mesh_tail_unavailable_{};
    std::uint64_t mesh_monitor_retry_ms_{};
    std::map<PeerIndex, std::uint64_t> mesh_monitor_generation_;
    struct MeshReplay {
        MeshReplay(std::uint64_t edge, std::uint64_t tick) : initial_generation(edge), generation(edge), cursor(tick) {}
        std::uint64_t initial_generation{}, generation{}, cursor{};
        std::deque<std::vector<std::uint8_t>> controls;
        std::size_t bytes{};
    };
    std::map<PeerIndex, MeshReplay> mesh_replay_;
    std::optional<std::pair<PeerIndex, std::uint64_t>> mesh_departure_ack_;
    struct SettledMeshVotes {
        std::vector<std::optional<std::uint64_t>> frontiers;
        std::vector<DepartureReason> reasons;
    };
    std::map<PeerIndex, SettledMeshVotes> mesh_settled_votes_; // At most the roster's departed seats.
    std::vector<std::optional<std::uint64_t>> command_cursors_;
    std::map<std::uint64_t, std::vector<SeatInput>> pending_;
    std::map<std::uint64_t, std::map<PeerIndex, SealedBatch>> ready_;
    std::vector<std::vector<std::array<std::uint8_t, 32>>> accepted_;
    std::optional<WaitingFor> waiting_;
    std::optional<WaitingFor> monitor_wait_;
    std::optional<std::uint64_t> last_now_;
    std::optional<core::Diagnostic> failure_;
    std::optional<PeerIndex> connection_lost_peer_;
    std::function<core::Result<void>(Received)> handler_;
    std::function<core::Result<void>(std::span<const sim::SeatCommandKey>)> departure_handler_;
    SessionMonitor* monitor_{};
    bool recorded_zero_{};
    bool receiving_recovery_{};
    EncodeDeparture encode_departure_;
    std::optional<Agreement> agreement_;
    // Ordered post-ack controls wait for the current roster decision to settle.
    std::map<PeerIndex, std::deque<Received>> deferred_departures_;
    std::size_t deferred_departure_bytes_{}, deferred_departure_frames_{};
    std::vector<bool> departed_;
    std::vector<bool> quit_in_batch_;
    std::vector<bool> quit_seen_; // a received batch of this seat carries a departure command
    std::vector<std::map<std::uint64_t, SealedBatch>> recovery_;
    std::map<PeerIndex, DepartureEvent> agreed_departures_;
    std::optional<TimingAgreement> timing_;
    std::optional<TimingEvent> last_timing_;
    std::uint64_t next_policy_tick_{}, retained_delay_{}, retain_until_{};
    std::optional<HeldSeat> held_;
    std::uint64_t reconnect_epoch_{};
    std::uint64_t completed_reconnect_epoch_{}, last_hello_ms_{};
    std::size_t hello_attempts_{};
    bool reconnect_available_{true}, replay_in_progress_{};
    std::optional<core::Diagnostic> reconnect_notice_;
    bool rejoin_hello_sent_{}, rejoin_offer_received_{}, rejoin_restored_{};
    std::string last_reconnect_hash_;
    std::optional<Received> pending_rejoin_;
    struct RejoinChallenge {
        std::uint64_t epoch{};
        RejoinNonce nonce{};
        bool consumed{}, transfer{};
    };
    std::map<std::pair<PeerIndex, bool>, RejoinChallenge> issued_challenges_;
    std::map<PeerIndex, RejoinNonce> returning_nonces_;
    std::uint64_t link_hello_ms_{};
    std::size_t link_hello_attempts_{};
    std::map<PeerIndex, std::string> pending_rejoin_proofs_;
    std::map<PeerIndex, std::string> pending_expiry_proofs_;
    std::optional<std::uint64_t> expired_reconnect_epoch_;
    std::vector<std::uint64_t> recovered_control_ticks_;
    std::size_t reconnect_bytes_{}, incoming_size_{};
    std::array<std::uint8_t, 32> incoming_digest_{};
    std::vector<std::uint8_t> incoming_package_;
    std::vector<std::vector<std::uint8_t>> reconnect_log_;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> reconnect_controls_;
    std::optional<ResyncIncident> resync_;
    std::unique_ptr<MeshIncident> mesh_;
    MeshSeats mesh_removed_{};
    std::deque<PeerIndex> mesh_pending_;
    std::deque<Received> mesh_early_;
    std::set<PeerIndex> mesh_early_senders_;
    std::size_t mesh_early_bytes_{};
    std::uint64_t mesh_early_since_ms_{};
    std::vector<MeshIncidentResult> mesh_results_;
    std::uint64_t resync_epoch_{}, initial_input_delay_{};
    std::optional<std::uint64_t> refused_resync_epoch_;
    bool resync_staging_{};
};

} // namespace eawr::net
