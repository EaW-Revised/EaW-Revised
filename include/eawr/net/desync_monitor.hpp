#pragma once

#include "eawr/core/result.hpp"
#include "eawr/net/session_monitor.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::net {

class MessageTransport;

// Net kinds 32..39 are coordinator-reserved for desync diagnostics.
// Wire v1: kind 32 = LE u64 first/last completed tick, 64 lowercase ASCII
// SHA-256 hex bytes, u8 match-end flag. Kind 33 = LE u64 first/last tick,
// then contiguous pairs of 64-byte state/input digests. The combined digest
// hashes "eawr-net-desync-v1" followed by LE u64 tick + both digests per row.
inline constexpr std::uint8_t desync_combined_kind = 32;
inline constexpr std::uint8_t desync_rows_kind = 33;
// Kind 34 = incident evidence, sent once per peer after a diagnosed disagreement when
// DesyncOptions::share_evidence is on: LE u64 first/last tick of the window, LE u32 count, then per entry
// LE u64 tick, LE u32 length and the sender's canonical input batch of that tick. Only ticks whose input
// hash differs from a peer's are sent; the receiver checks every batch against that peer's authenticated row.
inline constexpr std::uint8_t desync_evidence_kind = 34;

struct DiagnosticFile {
    std::string name; // Fixed relative filename; no paths supplied by a peer.
    std::vector<std::uint8_t> bytes;
};

struct DiagnosticBundle {
    std::string id;
    std::vector<DiagnosticFile> files;
    std::size_t size_bytes{};
};

// Native publication belongs to a platform caller. Return its durable location;
// without a sink the complete bounded bundle remains available in DesyncReport.
using DiagnosticSink = std::function<core::Result<std::string>(const DiagnosticBundle&)>;

struct AgreedDeparture {
    PeerIndex peer{};
    std::uint64_t input_cutoff_tick{};
};
struct DepartedDivergence {
    PeerIndex peer{};
    std::uint64_t tick{};
    std::string local_hash, remote_hash, channel;
};

struct DesyncReport {
    // Departed-peer evidence is informational and does not stop surviving peers.
    bool diagnostic_only{};
    std::uint64_t first_divergent_tick{};
    std::uint64_t detected_at{};
    std::uint32_t local_peer{};
    std::uint32_t remote_peer{};
    std::string local_hash;
    std::string remote_hash;
    std::string channel{"world/script"};
    std::uint64_t encoding_tick{};
    DiagnosticBundle bundle;
    std::string bundle_location;
    std::optional<core::Diagnostic> bundle_error;
    std::vector<AgreedDeparture> departures;
    std::vector<DepartedDivergence> departed_divergences;
};

struct DesyncOptions {
    std::uint64_t initial_tick{}; // Agreed restored boundary; the first row always exchanges.
    std::uint64_t exchange_every{30}; // 1..4096 completed ticks; zero barrier and end always exchange
    std::uint64_t input_delay{3}; // Match SessionOptions; bounds an agreed departure cutoff by 2*D+2.
    std::size_t keep_exchange_points{2};
    // Payload budget; row metadata additionally bounded by interval/roster. Sized so keep_exchange_points canonical encodings
    // of a campaign galaxy plus a child battle (about 12 MB each) are retained; the retained encodings are the bundle's
    // session.bin and cost at most this much memory per peer.
    std::size_t max_retained_bytes{64 * 1024 * 1024};
    DiagnosticSink diagnostic_sink; // caller chooses the user-data directory or another destination
    std::string match_identity;
    std::vector<std::uint8_t> match_setup;
    std::string build_identity;
    // After a diagnosed disagreement every peer ships its differing canonical input batches to the others, so
    // each peer's bundle can name what differed without the other machine's folder. Off keeps the v1 wire exchange.
    bool share_evidence{};
    // Per evidence frame; the receiver refuses more. It must fit the match transport's frame limit (1 MiB by
    // default) with its header, so the default leaves room; batches over the budget are listed in evidence-notes.txt.
    std::size_t max_evidence_bytes{512 * 1024};
    // Optional replayable record of the match so far (setup plus sealed inputs and per-tick hashes), written
    // to the bundle as session-journal.bin when it fits max_journal_bytes. Called on the monitor's thread.
    std::function<std::vector<std::uint8_t>()> replay_journal;
    std::size_t max_journal_bytes{32 * 1024 * 1024};
};

// Driven by LockstepSession: never polls the transport itself, sleeps or starts threads.
// Route kinds 32/33 to receive(), including while waiting/frozen. Before the first
// step record tick zero; after each step record the row hash (state_hash() on needs_encoding
// ticks, the adapter's CompletedTick::state_sha256 otherwise) and the canonical input
// encoding. Capture the session encoding only when needs_encoding(tick) is true.
// can_step() gates only simulation advancement; transport service must continue.
class DesyncMonitor final : public SessionMonitor {
public:
    DesyncMonitor(MessageTransport& transport, DesyncOptions options = {});
    [[nodiscard]] bool needs_encoding(std::uint64_t tick, bool match_end = false) const noexcept override;
    [[nodiscard]] core::Result<void> record(std::uint64_t completed_tick, std::string_view state_hash,
        std::span<const std::uint8_t> canonical_encoding = {},
        std::span<const std::uint8_t> canonical_inputs = {}, bool match_end = false) override;
    // Flush the last partial window when a match ends after an already recorded tick.
    // full_state_hash (when given) replaces the last row's hash, which a world-only adapter left cheap.
    [[nodiscard]] core::Result<void> finish(std::span<const std::uint8_t> canonical_encoding = {},
        std::string_view full_state_hash = {});
    [[nodiscard]] core::Result<void> on_link_repair(PeerIndex peer, std::function<bool(PeerIndex)> ownership = {}) override;
    [[nodiscard]] core::Result<void> on_departure(PeerIndex peer, std::uint64_t tick) override;
    [[nodiscard]] core::Result<void> on_input_delay(std::uint64_t delay, std::uint64_t retained_delay) override;
    [[nodiscard]] core::Result<void> on_reconnect(std::uint64_t tick, std::string_view hash,
        std::span<const PeerIndex> inactive) override;
    std::optional<std::uint64_t> reconnect_floor_;
    [[nodiscard]] std::vector<PeerIndex> waiting_for_peers() const override;
    [[nodiscard]] core::Result<void> receive(std::uint32_t from, std::span<const std::uint8_t> frame);
    [[nodiscard]] core::Result<void> receive(const Received& received) override { return receive(received.from, received.frame); }
    [[nodiscard]] bool may_step() const noexcept override { return can_step(); }
    [[nodiscard]] bool can_step() const noexcept { return started_ && setup_agreed_ && !waiting_ && !frozen_ && !ended_ && !failed_
        && active_peers_[transport_.self()]; }
    [[nodiscard]] bool frozen() const noexcept override { return frozen_; }
    [[nodiscard]] bool ready_for_resync() const override;
    [[nodiscard]] core::Result<void> on_resync(std::uint64_t tick, std::string_view hash) override;
    [[nodiscard]] const std::optional<DesyncReport>& recovered_report() const noexcept { return recovered_report_; }
    // True while an active incident still lacks a peer's evidence (DesyncOptions::share_evidence). Deliberately not part
    // of waiting_for_peers(): a closed link must not turn a diagnosed desync into a connection-loss error.
    [[nodiscard]] bool waiting_for_evidence() const noexcept {
        if (!options_.share_evidence || !report_ || report_->diagnostic_only || failed_) return false;
        for (PeerIndex peer = 0; peer < active_peers_.size(); ++peer)
            if (peer != transport_.self() && active_peers_[peer] && !evidence_received_.contains(peer)) return true;
        return false;
    }
    [[nodiscard]] bool waiting() const noexcept { return waiting_; }
    [[nodiscard]] bool setup_agreed() const noexcept { return setup_agreed_; }
    [[nodiscard]] std::optional<std::uint64_t> peer_confirmed_tick() const noexcept {
        if (!previous_ || frozen_ || failed_ || previous_->summaries.empty()
            || previous_->summaries.size() + 1 != active_peers_.size()) return {};
        for (const auto& [peer, summary] : previous_->summaries) {
            static_cast<void>(peer);
            if (summary.hash != previous_->local.hash) return {};
        }
        return previous_->local.last;
    }
    // Read-only GC checkpoint source election: only the diagnosed minority
    // chooses a source, from a strict majority of at least three active peers.
    [[nodiscard]] std::optional<PeerIndex> checkpoint_recovery_source() const;
    [[nodiscard]] bool agreed_end() const noexcept { return ended_ && !waiting_ && !frozen_ && !failed_; }
    [[nodiscard]] const std::optional<DesyncReport>& report() const noexcept { return report_; }
    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }

private:
    struct Row { std::uint64_t tick{}; std::string state; std::string input; };
    struct Summary { std::uint64_t first{}, last{}; std::string hash; bool end{}; };
    struct Checkpoint { std::uint64_t tick{}; std::vector<std::uint8_t> bytes; };
    struct AgreedWindow {
        Summary local;
        std::vector<Row> rows;
        std::map<std::uint32_t, Summary> summaries;
        std::map<std::uint32_t, std::vector<Row>> remote;
        bool rows_sent{};
    };
    std::optional<AgreedWindow> previous_;
    // A retry can carry the sender's previous window after we have already
    // agreed its current one. Keep only the last retired digest per peer, no rows.
    std::map<std::uint32_t, Summary> retired_summaries_;
    bool previous_frozen_{}, repair_enabled_{};
    std::function<bool(PeerIndex)> repair_ownership_;
    MessageTransport& transport_;
    DesyncOptions options_;
    bool valid_{true}, started_{}, waiting_{}, frozen_{}, ended_{}, failed_{}, rows_sent_{}, setup_agreed_{};
    std::uint64_t completed_{};
    std::uint64_t next_first_{};
    std::vector<Row> rows_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> inputs_;
    std::deque<Checkpoint> checkpoints_;
    std::size_t retained_bytes_{};
    std::optional<Summary> local_summary_;
    std::map<std::uint32_t, Summary> summaries_;
    std::map<std::uint32_t, std::vector<Row>> remote_rows_;
    // A faster peer can enter the next window while a different ordered stream
    // still holds our current agreement. At most one summary and one detail frame
    // per sender can get ahead: all-peer exchange gating bounds this queue.
    std::map<std::uint32_t, std::deque<std::vector<std::uint8_t>>> future_frames_;
    std::optional<DesyncReport> report_;
    std::optional<DesyncReport> recovered_report_;
    std::vector<bool> active_peers_;
    std::vector<std::optional<std::uint64_t>> departure_ticks_;
    std::vector<DepartedDivergence> departed_divergences_;
    std::uint64_t retained_input_delay_{};
    // Incident evidence: the remote peers' authenticated differing input batches and who has sent theirs.
    std::map<std::uint32_t, std::map<std::uint64_t, std::vector<std::uint8_t>>> remote_inputs_;
    std::set<std::uint32_t> evidence_received_;
    bool evidence_sent_{};
    std::string evidence_notes_; // what this peer could not ship, listed in the bundle
    std::optional<std::vector<std::uint8_t>> journal_cache_; // one materialisation per incident

    [[nodiscard]] core::Result<void> exchange(bool end, std::span<const std::uint8_t> encoding);
    [[nodiscard]] core::Result<void> evaluate();
    [[nodiscard]] core::Result<void> broadcast(std::span<const std::uint8_t> frame);
    [[nodiscard]] core::Result<void> fail(std::string message);
    [[nodiscard]] std::size_t active_remote_count() const noexcept;
    void keep_encoding(std::uint64_t tick, std::span<const std::uint8_t> bytes);
    void diagnose(const std::vector<Row>& rows, const std::map<std::uint32_t, std::vector<Row>>& remote, bool active = true);
    [[nodiscard]] core::Result<void> evaluate_previous();
    void build_bundle();
    void send_evidence(const std::vector<Row>& rows, const std::map<std::uint32_t, std::vector<Row>>& remotes);
    [[nodiscard]] core::Result<void> receive_evidence(std::uint32_t from, std::span<const std::uint8_t> frame);
};

} // namespace eawr::net
