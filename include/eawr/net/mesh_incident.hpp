#pragma once

#include "eawr/net/mesh_policy.hpp"

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::net {

// Lockstep application kinds of the N-peer pair-loss agreement (docs/net-online-npeer.md, section 3). All four ride the
// authenticated pair streams of the mesh and carry no sealed input: a link report (unsealed, never forwarded), the
// decider's proposal, a seat's pledge for exactly that proposal and the decider's frozen confirmation.
inline constexpr std::uint8_t mesh_report_kind = 82, mesh_proposal_kind = 83, mesh_pledge_kind = 84, mesh_confirm_kind = 85;
namespace mesh_errors {
inline constexpr std::string_view left = "EAWR-NET-1778";        // this seat was removed (or cut off) and leaves
inline constexpr std::string_view departure = "EAWR-NET-1779";   // a survivor's notice: a seat left over a lost pair
inline constexpr std::string_view no_majority = "EAWR-NET-1780"; // nobody could decide; the match ends
inline constexpr std::string_view no_decision = "EAWR-NET-1781"; // a decision or its confirmation did not arrive in time
inline constexpr std::string_view overlap = "EAWR-NET-1782";     // a further loss while the departures settle
inline constexpr std::string_view malformed = "EAWR-NET-1783";   // forged, malformed or unexpected mesh control
inline constexpr std::string_view options = "EAWR-NET-1784";     // invalid roster or incident options
inline constexpr std::string_view flapping = "EAWR-NET-1785";    // a repeatedly failing pair counts as lost
inline constexpr std::string_view frozen = "EAWR-NET-1786";      // adaptive timing is frozen at its last agreed delay
}

// A report row with this value means the pair has no smoothed round trip yet; a pair whose two ends both say so is unknown,
// and the host defers its timing policy instead of reading it as zero.
inline constexpr std::uint32_t mesh_rtt_unmeasured = 600'000;

struct MeshIncidentOptions {
    PeerIndex host{};
    // The agreed repair window. The pledge deadline is this plus the policy's fixed ten-second decision window, and every
    // seat uses the same value, so seats that notice a loss at different moments still end the window together.
    std::chrono::milliseconds repair_grace{30'000};
    // Cadence of link reports while an incident runs (the design's once a second) and of the steady-state RTT reports
    // every seat sends the host. Both resend an unchanged proposal or frozen confirmation at this cadence too.
    std::chrono::milliseconds report_interval{1'000};
    // A steady-state report older than this no longer counts for adaptive timing (the host then defers its policy).
    std::chrono::milliseconds report_freshness{10'000};
    // A pair that was lost this many times within the window is reported as lost for the rest of the incident, even while
    // it is open again, so the agreement cannot oscillate with it. Zero turns the hold-down off.
    unsigned flap_limit{3};
    std::chrono::milliseconds flap_window{std::chrono::minutes{2}};
    // A window (the repair grace plus the decision window) that ends without any decision, on a seat that pledged to nothing
    // and was confirmed nothing, is followed by another one regardless of repair state (every seat counts the same
    // incident time, so all of them start it together); a later window only has the decision window's twenty seconds. The
    // match ends with EAWR-NET-1780 only after this many windows (1 = the first window is the last).
    unsigned rounds{3};
    // The pair has no repair left: the adapter's GnsPairDiagnostic::relay_failed. With no TURN configured the direct failure
    // is the whole evidence, so a closed link is final. Empty means every closed link counts as final.
    std::function<bool(PeerIndex)> relay_failed;
    // Smoothed round trip to a remote seat, milliseconds. Empty reads MessageTransport::link_stats.
    std::function<std::optional<std::uint32_t>(PeerIndex)> rtt_ms;
};

// What a finished or abandoned incident asks of the session.
struct MeshIncidentResult {
    enum class Kind : std::uint8_t { restored, departure, leave } kind{Kind::leave};
    std::uint64_t incident{};
    MeshSeats survivors{}, removed{};
    // For every seat, the seats it could not reach in the decision's reports (the survivors' "left: connection to X failed").
    std::array<MeshSeats, max_match_seats> lost{};
    std::string code; // mesh_errors for leave
    std::string message;
};

// Wire. Little-endian, bounded; a decode failure is EAWR-NET-1783.
struct MeshReportFrame { MeshLinkReport report; std::uint32_t elapsed_ms{}; MeshSeats active{}; };
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_mesh_report(const MeshLinkReport&, std::uint32_t elapsed_ms, MeshSeats active);
[[nodiscard]] core::Result<MeshReportFrame> decode_mesh_report(std::span<const std::uint8_t>);
// kind is mesh_proposal_kind or mesh_pledge_kind (a pledge echoes the exact topology snapshot, RTT excluded).
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_mesh_decision(std::uint8_t kind, const MeshLossDecision&);
[[nodiscard]] core::Result<MeshLossDecision> decode_mesh_decision(std::uint8_t kind, std::span<const std::uint8_t>);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_mesh_confirmation(const MeshLossConfirmation&);
[[nodiscard]] core::Result<MeshLossConfirmation> decode_mesh_confirmation(std::span<const std::uint8_t>);

// One seat's view of the pair-loss agreement over a fixed active set. It owns the pure N1 MeshLossPolicy, feeds it only
// what this seat's authenticated open pairs deliver, and turns its answers into frames and a MeshIncidentResult. It runs on
// the session's thread with the session's caller-supplied millisecond clock; no sim or network threads are involved.
//
// Contracts (legacy #3697):
//  * Incident ID. The agreed counter "incidents completed so far + 1", starting at 1. A seat finishes an incident only by
//    the agreed restoration or by the settled departures of the confirmed survivors, so every survivor holds the same
//    value. A report for a later ID is ignored (the sender resends every second); a report for an earlier one is stale.
//  * Incident time base. Each seat measures from its own detection, but every report carries the sender's elapsed time in
//    the incident and a receiver moves its start back to the earliest start it hears of. The policy therefore sees one
//    common, incident-relative clock to within a message delay (the policy adds 500 ms tolerance to its give-up timers).
//  * Flapping. A pair lost `flap_limit` times within `flap_window` is held lost for the incident (see options).
//  * Decider loss. The policy has one decider per incident and one pledge per seat, so a decider that dies after its proposal
//    ends the incident for every seat that did not receive a confirmation, and nobody can continue on a different survivor
//    set: a pledged seat (victim or survivor) leaves with EAWR-NET-1781 (at once when its own pair to the decider closes, else
//    at the agreed deadline), a seat that never saw a proposal with 1780, and 1778 is only for a seat a confirmation named. A
//    seat that did receive the confirmation continues; the dead decider's links then close as an ordinary further loss (1782 if
//    they were inside the confirmed survivor set). A re-decision by the next seat is not part of this slice.
//  * Active set. The session supplies the set; a seat leaving by an ordinary departure (a quit whose batch arrived, a timeout
//    or held agreement) is left out as soon as this seat knows of it (see set_active). Every link report carries the sender's
//    set and a seat only begins or feeds an incident from reports of exactly its own set, so seats that learn of a departure
//    at different moments never build different incidents: they wait, converge through the departure agreement and restart
//    over the same set. A set change after a proposal was made or pledged is not applied (the incident then fails closed).
//  * Send failures. A send that fails on a link which still reads open counts, during an incident, as this end's failed pair.
//  * Half-open pairs. Each end proves its own relay failure; nothing is mirrored from a remote report onto an open local link.
class MeshIncident final {
public:
    using Time = std::chrono::milliseconds;
    MeshIncident(MessageTransport& transport, MeshSeats active, MeshIncidentOptions options);
    // The frame is one of the four kinds above, delivered over the authenticated pair `from`.
    [[nodiscard]] core::Result<void> receive(const Received& frame, Time now);
    // Drive links, reports, the policy and the resend schedule. Call every pump after draining receive.
    // `receive_drained`: the session's receive queue is empty. A closed link starts an incident only then, so a quit batch that
    // is still queued is read first.
    [[nodiscard]] core::Result<void> service(Time now, bool receive_drained = true);
    // At most one result per incident; a departure result is followed by settled() from the session.
    [[nodiscard]] std::optional<MeshIncidentResult> take_result();
    // The session finished every agreed departure of the last departure result.
    void settled();
    // The active set changed through an ordinary departure (a quit whose batch arrived, a timeout or held agreement; growth when
    // such an agreement is abandoned). Quiet: taken at once. Running with nothing proposed or pledged yet: the policy restarts
    // over the new set (same incident ID, time base and a monotonic report revision). Otherwise ignored: the incident fails
    // closed on its own deadlines. A set that can no longer hold three seats ends it.
    void set_active(MeshSeats active);
    // This incident owns the closed link to `seat` while three or more seats are active and `seat` is one of them; a seat
    // outside the set (an ordinary quit or timeout departure) stays with the session's ordinary departure.
    [[nodiscard]] bool owns_closed_link(PeerIndex seat) const;
    // A repair window or agreement is running (the session waits instead of timing a missing batch out).
    [[nodiscard]] bool running() const { return phase_ != Phase::quiet && phase_ != Phase::left; }
    [[nodiscard]] bool left() const { return phase_ == Phase::left; }
    // The confirmed removal of `seat` (departure result taken or pending); the only thing that permits a pair-loss proposal.
    [[nodiscard]] bool removal_confirmed(PeerIndex seat) const;
    [[nodiscard]] MeshSeats removed() const { return removed_; }
    [[nodiscard]] MeshSeats active() const { return active_; }
    [[nodiscard]] std::uint64_t incident() const { return incident_; }
    [[nodiscard]] MeshSeats flapping() const { return flapping_; }
    [[nodiscard]] MeshLossState state() const;
    // Host only: its own row plus the latest fresh report of every other active seat, for mesh_rtt_ms.
    [[nodiscard]] std::array<std::optional<MeshLinkReport>, max_match_seats> rtt_reports(Time now) const;
private:
    enum class Phase : std::uint8_t { quiet, running, settling, left };
    [[nodiscard]] Time relative(Time now) const { return now > base_ ? now - base_ : Time{0}; }
    // The policy's own clock: incident time since the current window (round) began.
    [[nodiscard]] Time policy_time(Time now) const { const auto rel = relative(now); return rel > round_offset_ ? rel - round_offset_ : Time{0}; }
    // The first window is the repair grace plus the decision window; a later one only has to decide (the grace is spent).
    [[nodiscard]] Time round_deadline() const;
    void next_round();
    [[nodiscard]] MeshLinkReport local_row(Time now);
    [[nodiscard]] core::Result<void> send(PeerIndex to, const std::vector<std::uint8_t>& frame);
    [[nodiscard]] core::Result<void> broadcast(MeshSeats to, const std::vector<std::uint8_t>& frame);
    [[nodiscard]] core::Result<void> begin(Time now, Time elapsed);
    void note_losses(Time now);
    void finish(MeshIncidentResult result);
    void leave(std::string_view code, std::string message);
    [[nodiscard]] core::Diagnostic malformed(std::string message) const;
    MessageTransport* transport_{};
    MeshIncidentOptions options_;
    PeerIndex self_{};
    MeshSeats active_{}, removed_{}, flapping_{}, was_open_{}, send_failed_{};
    std::uint64_t incident_{1}, revision_{};
    unsigned round_{};
    Time round_offset_{};
    Phase phase_{Phase::quiet};
    Time base_{}, last_report_{}, last_resend_{}, last_steady_{};
    bool have_report_{}, sent_confirmation_{}, confirmed_{};
    // A later incident may start before every pledged seat received an earlier restoration. Each confirmation keeps its
    // own resend window; starting or completing another incident must not strand those seats on the earlier incident id.
    struct Tail { MeshLossConfirmation confirmation; Time until{}, last_sent{}; };
    std::deque<Tail> tails_;
    MeshLinkReport row_;
    std::optional<MeshLossPolicy> policy_;
    std::optional<MeshLossDecision> proposal_;
    std::optional<MeshLossConfirmation> frozen_;
    std::optional<MeshIncidentResult> result_;
    std::array<std::deque<Time>, max_match_seats> losses_;
    struct Heard { MeshLinkReport report; Time at{}; };
    std::array<std::optional<Heard>, max_match_seats> heard_;
    std::uint64_t steady_revision_{};
};

} // namespace eawr::net
