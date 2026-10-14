#pragma once

#include "eawr/core/result.hpp"
#include "eawr/net/session_monitor.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
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
    std::uint64_t exchange_every{30}; // 1..4096 completed ticks; zero barrier and end always exchange
    std::uint64_t input_delay{3}; // Match SessionOptions; bounds an agreed departure cutoff by 2*D+2.
    std::size_t keep_exchange_points{2};
    std::size_t max_retained_bytes{16 * 1024 * 1024}; // payload budget; row metadata additionally bounded by interval/roster
    DiagnosticSink diagnostic_sink; // caller chooses the user-data directory or another destination
    std::string match_identity;
    std::vector<std::uint8_t> match_setup;
    std::string build_identity;
};

// Driven by LockstepSession: never polls the transport itself, sleeps or starts threads.
// Route kinds 32/33 to receive(), including while waiting/frozen. Before the first
// step record tick zero; after each step reuse state_hash and the canonical input
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
    [[nodiscard]] core::Result<void> finish(std::span<const std::uint8_t> canonical_encoding = {});
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
    [[nodiscard]] bool waiting() const noexcept { return waiting_; }
    [[nodiscard]] bool setup_agreed() const noexcept { return setup_agreed_; }
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
    bool previous_frozen_{};
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

    [[nodiscard]] core::Result<void> exchange(bool end, std::span<const std::uint8_t> encoding);
    [[nodiscard]] core::Result<void> evaluate();
    [[nodiscard]] core::Result<void> broadcast(std::span<const std::uint8_t> frame);
    [[nodiscard]] core::Result<void> fail(std::string message);
    [[nodiscard]] std::size_t active_remote_count() const noexcept;
    void keep_encoding(std::uint64_t tick, std::span<const std::uint8_t> bytes);
    void diagnose(const std::vector<Row>& rows, const std::map<std::uint32_t, std::vector<Row>>& remote, bool active = true);
    [[nodiscard]] core::Result<void> evaluate_previous();
    void build_bundle();
};

} // namespace eawr::net
