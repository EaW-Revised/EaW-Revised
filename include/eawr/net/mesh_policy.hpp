#pragma once

#include "eawr/net/transport.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::net {
using MeshSeats = std::uint16_t;
constexpr MeshSeats mesh_seat(PeerIndex seat) {
    return seat < max_match_seats ? static_cast<MeshSeats>(1U << seat) : MeshSeats{};
}
enum class MeshFormationAction : std::uint8_t { none, regrant, refuse, close_room };
struct MeshRelayFailure { bool a_failed{}, b_failed{}; };
struct MeshFormationResult {
    MeshFormationAction action{};
    PeerIndex seat{}, unreachable{};
    // EAWR-NET-1762 denotes pair-unreachable, with the remote seat for curated text.
    bool rejoin_allowed{};
};
// A failed pair needs a report from both ends, but the unreachable end may never send one. After the grace (five
// seconds, at most half the pair deadline) with one end's failure reported and the other's not, the silent end counts as
// failed too. Nothing is inferred for a pair nobody reported on. Pure; the caller supplies the wait.
std::chrono::milliseconds mesh_report_grace(std::chrono::milliseconds pair_timeout);
MeshRelayFailure mesh_silent_end_failed(MeshRelayFailure reported, std::chrono::milliseconds waited,
    std::chrono::milliseconds pair_timeout);
std::string mesh_newcomer_refusal_text(std::string_view unreachable_name, bool relay_configured = true);
class MeshFormation final {
public:
    explicit MeshFormation(std::uint32_t capacity);
    bool attach(PeerIndex seat, std::uint64_t admission_revision);
    void vacate(PeerIndex seat);
    bool pair_open(PeerIndex a, PeerIndex b);
    // A direct failure clears readiness but cannot refuse/regrant until both
    // endpoints have exhausted this ICE attempt. Nonhost pairs get two fresh
    // grants per occupancy before the later admission is refused.
    MeshFormationResult pair_failed(PeerIndex a, PeerIndex b, MeshRelayFailure relay);
    bool complete() const;
    bool connected(PeerIndex seat) const;
    MeshSeats occupied() const { return occupied_; }
private:
    std::uint32_t capacity_{};
    MeshSeats occupied_{};
    std::array<MeshSeats, max_match_seats> links_{};
    std::array<std::array<unsigned, max_match_seats>, max_match_seats> retries_{};
    std::array<std::uint64_t, max_match_seats> admitted_{};
    bool closed_{};
};

struct MeshLinkReport {
    // revision versions topology only; RTT updates do not change it.
    std::uint64_t incident{}, revision{};
    MeshSeats open{}, relay_failed{};
    std::array<std::uint32_t, max_match_seats> rtt_ms{};
    bool operator==(const MeshLinkReport&) const = default;
};
enum class MeshLossRule : std::uint8_t { all_open, majority_clique };
enum class MeshLossOutcome : std::uint8_t { departure, restored };
struct MeshLossDecision {
    std::uint64_t incident{};
    PeerIndex decider{};
    MeshSeats active{}, reported{}, survivors{};
    std::array<MeshLinkReport, max_match_seats> reports{};
    bool rejoin_allowed{true};
    MeshLossRule rule{MeshLossRule::all_open};
    MeshLossOutcome outcome{MeshLossOutcome::departure};
    bool operator==(const MeshLossDecision&) const = default;
};
struct MeshLossConfirmation {
    MeshLossDecision proposal;
    MeshSeats pledges{}, survivors{};
    bool operator==(const MeshLossConfirmation&) const = default;
};
enum class MeshLossState : std::uint8_t { repairing, awaiting_decision, agreeing, restored, settling, settled, leave };
inline constexpr auto mesh_receiver_tolerance = std::chrono::milliseconds{500};
// An incident uses a fixed agreed active set and incident ID supplied by the
// session. The caller delivers reports only over their authenticated open pair,
// never forwards them, and supplies its initial local row, then updates it on
// each local pair change. Its own row is local truth and does not age out.
// Election uses fresh reports received within three seconds (the one-second report
// cadence plus two seconds of stability). Time is an injected common
// incident-relative duration, NOT an absolute local steady-clock reading.
// The session supplies the same agreed pledge deadline to every seat, allowing the
// full repair/announcement window despite different local detection times.
// Before that cutoff confirmation requires every active pledge. At cutoff a
// majority of the original active set can confirm, excluding missing pledgers.
// Majority intersection and one pledge per seat forbid two effective proposals.
// Pledged seats wait at most another ten seconds for confirmation delivery.
// Receivers add the fixed 500 ms tolerance to their give-up timers. The decider
// alone fixes the pledge cutoff; small clock skew never gates a received certificate.
// Inputs are clamped to a monotonic high-water time, including message stamps.
// N4 MUST resend the unchanged proposal once per second until the pledge cutoff,
// permitting a receiver to accept later when a delayed report catches up. Echo
// only after accept returns true, only to that proposal's decider, as a distinct
// authenticated pledge control (never route a proposal to acknowledge).
// N4 MUST resend the frozen confirmation once per second through cutoff + 10 s.
// No network/sim work lives here.
// Restoration is a no-removal proposal through the same pledge/confirmation
// protocol. Only its commit enters restored, with no local continuation shortcut.
// An authenticated, verified exclusion confirmation stops even a non-pledger.
// restored completes this agreed incident; a later loss starts a
// new policy instance with the session's next agreed incident ID.
class MeshLossPolicy final {
public:
    using Time = std::chrono::milliseconds;
    MeshLossPolicy(PeerIndex self, MeshSeats active, std::uint64_t incident, Time local_repair_started, Time agreed_deadline);
    bool report(PeerIndex from, const MeshLinkReport& report, Time now);
    std::optional<MeshLossDecision> decide(Time now);
    bool accept(PeerIndex authenticated_sender, const MeshLossDecision& decision, Time now);
    // accept/decide pledge one candidate per incident, without closing links or
    // removing seats. Every active seat echoes that exact topology snapshot to
    // the decider, including prospective victims. RTT is excluded from it.
    bool acknowledge(PeerIndex authenticated_sender, const MeshLossDecision& decision, Time now);
    std::optional<MeshLossConfirmation> confirmation(Time now);
    // Deliver only the decider's authenticated confirmation control here.
    // The decider enforces all pledges before cutoff, majority at cutoff.
    // true means the control was accepted; an excluded receiver still leaves.
    // Continuing requires restored, or completion of the bounded departure
    // protocol followed by settled(). N4 owns that protocol's timeout.
    bool commit(PeerIndex authenticated_sender, const MeshLossConfirmation& confirmation, Time now);
    void advance(Time now);
    // Called on a real additional pair loss while departures settle. Expected
    // closures to removed seats are excluded; losing a survivor fails closed.
    void pair_lost(PeerIndex a, PeerIndex b);
    void settled();
    MeshLossState state() const { return state_; }
    MeshLossState state(Time now) { advance(now); return state_; }
    // Use state(now) for polling; a bare state() read does not advance time.
    const std::optional<MeshLossDecision>& decision() const { return decision_; }
    bool rejoin_allowed() const { return rejoin_allowed_; }
private:
    Time monotonic(Time now);
    MeshSeats fresh_reports(Time now) const;
    PeerIndex self_{};
    MeshSeats active_{}, received_{}, acknowledged_{};
    std::uint64_t incident_{};
    Time started_{}, changed_{}, deadline_{}, last_now_{};
    std::array<MeshLinkReport, max_match_seats> reports_{};
    std::array<Time, max_match_seats> seen_{};
    MeshLossState state_{MeshLossState::repairing};
    std::optional<MeshLossDecision> decision_;
    std::optional<MeshLossConfirmation> confirmed_;
    std::optional<MeshLossConfirmation> issued_;
    bool rejoin_allowed_{};
};

// Host's own measurements for its pairs; minimum of the two reports for other
// pairs, then maximum across the agreed active set. Missing reports are unknown.
std::optional<std::uint32_t> mesh_rtt_ms(MeshSeats active,
    const std::array<std::optional<MeshLinkReport>, max_match_seats>& reports);
// Freeze proposals only: already agreed future changes remain the session's
// responsibility and still apply. No timing authority is elected on host loss.
constexpr bool mesh_timing_frozen(MeshSeats active, LinkState host_link) {
    return !(active & mesh_seat(0)) || host_link != LinkState::open;
}
} // namespace eawr::net
