#include "eawr/net/desync_monitor.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/net/transport.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace eawr::net {
namespace {
constexpr std::size_t hash_size = 64;
constexpr std::uint64_t max_window = 4096;

core::Diagnostic diagnostic(std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-NET-0401";
    result.severity = core::Severity::error;
    result.message = std::move(message);
    return result;
}
bool valid_hash(std::string_view hash) {
    return hash.size() == hash_size && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_hash(std::vector<std::uint8_t>& bytes, const std::string& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset{};
    bool u64(std::uint64_t& value) {
        if (bytes.size() - offset < 8) return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
        return true;
    }
    bool hash(std::string& value) {
        if (bytes.size() - offset < hash_size) return false;
        value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + hash_size));
        offset += hash_size;
        return valid_hash(value);
    }
};
} // namespace

DesyncMonitor::DesyncMonitor(MessageTransport& transport, DesyncOptions options)
    : transport_(transport), options_(std::move(options)) {
    valid_ = options_.exchange_every > 0 && options_.exchange_every <= max_window
        && options_.input_delay <= max_window
        && options_.keep_exchange_points > 0 && options_.keep_exchange_points <= 64
        && options_.max_retained_bytes > 0 && options_.max_retained_bytes <= 256 * 1024 * 1024
        && transport_.peer_count() >= 2 && transport_.peer_count() <= max_match_seats
        && transport_.self() < transport_.peer_count();
    const auto limit = options_.max_retained_bytes;
    if (options_.match_setup.size() > limit || options_.match_identity.size() > limit
        || options_.build_identity.size() > limit) valid_ = false;
    else if (options_.match_setup.size() + options_.match_identity.size() + options_.build_identity.size() > limit)
        valid_ = false;
    retained_bytes_ = options_.match_setup.size() + options_.match_identity.size() + options_.build_identity.size();
    if (valid_) {
        active_peers_.assign(transport_.peer_count(), true);
        departure_ticks_.resize(transport_.peer_count());
    }
}

bool DesyncMonitor::needs_encoding(std::uint64_t tick, bool match_end) const noexcept {
    return valid_ && (tick == 0 || tick % options_.exchange_every == 0 || match_end);
}

core::Result<void> DesyncMonitor::fail(std::string message) {
    failed_ = true;
    frozen_ = true;
    return core::Result<void>::failure(diagnostic(std::move(message)));
}

void DesyncMonitor::keep_encoding(std::uint64_t tick, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return;
    if (!checkpoints_.empty() && checkpoints_.back().tick == tick) return;
    std::size_t existing_encoding_bytes = 0;
    for (const auto& checkpoint : checkpoints_) existing_encoding_bytes += checkpoint.bytes.size();
    // If this encoding cannot fit even after evicting all others, preserve the
    // last usable point instead of discarding it for an unavailable new point.
    if (bytes.size() > options_.max_retained_bytes - (retained_bytes_ - existing_encoding_bytes)) return;
    while (!checkpoints_.empty() && (checkpoints_.size() >= options_.keep_exchange_points
        || bytes.size() > options_.max_retained_bytes - retained_bytes_)) {
        retained_bytes_ -= checkpoints_.front().bytes.size();
        checkpoints_.pop_front();
    }
    if (bytes.size() > options_.max_retained_bytes - retained_bytes_) return;
    checkpoints_.push_back({tick, {bytes.begin(), bytes.end()}});
    retained_bytes_ += bytes.size();
}

core::Result<void> DesyncMonitor::record(std::uint64_t tick, std::string_view hash,
    std::span<const std::uint8_t> encoding, std::span<const std::uint8_t> input, bool end) {
    if (!valid_) return fail("invalid desync options or peer roster");
    // LAN-6 notifies a local sealed quit before recording its completed frame.
    // This peer has left hash agreement; that final hook call is a harmless tail.
    if (!active_peers_[transport_.self()]) return core::Result<void>::success();
    if (failed_ || frozen_ || ended_ || waiting_) return fail("cannot record while the monitor gates advancement");
    if ((!started_ && tick != 0) || (started_ && (completed_ == std::numeric_limits<std::uint64_t>::max()
        || tick != completed_ + 1))) return fail("completed ticks must start at zero and be contiguous");
    if (!valid_hash(hash)) return fail("state hash must be a canonical SHA-256 hex digest");
    // Input payloads are bounded as well as checkpoint encodings. Refuse a window
    // that exceeds the configured diagnostic budget rather than grow without bound.
    while (!checkpoints_.empty() && input.size() > options_.max_retained_bytes - retained_bytes_) {
        retained_bytes_ -= checkpoints_.front().bytes.size();
        checkpoints_.pop_front();
    }
    if (input.size() > options_.max_retained_bytes - retained_bytes_) return fail("canonical input window exceeds diagnostic budget");
    started_ = true;
    completed_ = tick;
    rows_.push_back({tick, std::string(hash), core::sha256_hex(input)});
    inputs_[tick] = {input.begin(), input.end()};
    retained_bytes_ += input.size();
    if (needs_encoding(tick, end)) return exchange(end, encoding);
    return core::Result<void>::success();
}

core::Result<void> DesyncMonitor::finish(std::span<const std::uint8_t> encoding) {
    if (valid_ && !active_peers_[transport_.self()]) return core::Result<void>::success();
    if (!valid_ || !started_ || frozen_ || failed_) return fail("cannot finish an inactive or failed monitor");
    if (ended_) return core::Result<void>::success();
    if (waiting_) return fail("finish must wait for the pending exchange to agree");
    // An exact exchange boundary has already been compared; there is no new row.
    if (rows_.empty()) { ended_ = true; return core::Result<void>::success(); }
    return exchange(true, encoding);
}

bool DesyncMonitor::ready_for_resync() const {
    return valid_ && !failed_ && frozen_ && report_ && !report_->diagnostic_only
        && local_summary_ && summaries_.size() >= active_remote_count()
        && waiting_for_peers().empty();
}

core::Result<void> DesyncMonitor::on_resync(std::uint64_t tick, std::string_view hash) {
    if (!ready_for_resync() || tick != completed_ || !valid_hash(hash))
        return fail("resync requires a diagnosed completed barrier");
    const bool end = ended_;
    recovered_report_ = std::move(report_);
    report_.reset(); frozen_ = false; ended_ = false;
    auto reset = on_reconnect(tick, hash, {});
    ended_ = end;
    return reset;
}

core::Result<void> DesyncMonitor::on_reconnect(std::uint64_t tick, std::string_view hash,
    std::span<const PeerIndex> inactive) {
    if (!valid_ || failed_ || frozen_ || report_ || ended_ || !valid_hash(hash)
        || (started_ && tick < completed_) || !active_peers_[transport_.self()])
        return fail("cannot reset a failed/frozen monitor at reconnect");
    for (const auto peer : inactive) {
        if (peer >= active_peers_.size() || peer == transport_.self()) return fail("invalid recovered monitor roster");
        active_peers_[peer] = false;
        departure_ticks_[peer] = tick;
    }
    // The independent all-peer proof is the new hash frontier. Existing departure
    // membership is preserved; no suspected divergent state is cleared here.
    completed_ = tick;
    started_ = true;
    setup_agreed_ = true;
    waiting_ = false;
    rows_sent_ = false;
    rows_.clear(); inputs_.clear(); checkpoints_.clear(); retained_bytes_ = 0;
    summaries_.clear(); remote_rows_.clear(); future_frames_.clear();
    local_summary_.reset(); previous_.reset(); previous_frozen_ = false;
    next_first_ = tick + 1;
    reconnect_floor_ = tick;
    return core::Result<void>::success();
}

std::size_t DesyncMonitor::active_remote_count() const noexcept {
    const auto count = static_cast<std::size_t>(std::count(active_peers_.begin(), active_peers_.end(), true));
    return count - (active_peers_[transport_.self()] ? 1U : 0U);
}

std::vector<PeerIndex> DesyncMonitor::waiting_for_peers() const {
    std::vector<PeerIndex> peers;
    if (!valid_ || !waiting_ || failed_) return peers;
    for (PeerIndex peer = 0; peer < active_peers_.size(); ++peer) {
        if (peer == transport_.self() || !active_peers_[peer]) continue;
        if (!summaries_.contains(peer) || (rows_sent_ && !remote_rows_.contains(peer))) peers.push_back(peer);
    }
    return peers;
}

core::Result<void> DesyncMonitor::on_input_delay(std::uint64_t delay, std::uint64_t retained_delay) {
    if (!valid_ || failed_ || delay == 0 || delay > max_window || retained_delay < delay || retained_delay > max_window)
        return fail("invalid agreed input delay");
    options_.input_delay = delay;
    retained_input_delay_ = retained_delay;
    return core::Result<void>::success();
}

core::Result<void> DesyncMonitor::on_departure(PeerIndex peer, std::uint64_t tick) {
    if (!valid_ || failed_ || peer >= active_peers_.size()) return fail("invalid desync membership change");
    if (departure_ticks_[peer]) {
        if (*departure_ticks_[peer] != tick) return fail("conflicting desync departure tick");
        return core::Result<void>::success();
    }
    if (tick < completed_ || tick - completed_ > sealed_input_horizon(std::max(options_.input_delay, retained_input_delay_)))
        return fail("departure cutoff is past or exceeds the sealed-input horizon");
    departure_ticks_[peer] = tick;
    active_peers_[peer] = false;
    // Retain evidence, but a departed peer no longer participates in agreement.
    future_frames_.erase(peer);
    if (report_) report_->departures.push_back({peer, tick});
    if (peer == transport_.self()) {
        ended_ = true;
        waiting_ = false;
        return core::Result<void>::success();
    }
    if (report_ && report_->remote_peer == peer) {
        report_->diagnostic_only = true;
        if (local_summary_) build_bundle();
        else if (previous_) build_bundle();
    }
    frozen_ = false;
    if (auto result = evaluate_previous(); !result) return result;
    return evaluate();
}

core::Result<void> DesyncMonitor::broadcast(std::span<const std::uint8_t> frame) {
    for (std::uint32_t peer = 0; peer < transport_.peer_count(); ++peer) {
        if (peer == transport_.self() || !active_peers_[peer]) continue;
        auto sent = transport_.send(peer, frame);
        if (!sent) {
            // Closure can be discovered while the core drains a delivered hash
            // frame, before its membership check. Leave this voter pending: the
            // core will agree departure (opt-in) or return typed link_closed.
            if (transport_.link(peer) == LinkState::closed) continue;
            failed_ = true; frozen_ = true; return sent;
        }
    }
    return core::Result<void>::success();
}

core::Result<void> DesyncMonitor::exchange(bool end, std::span<const std::uint8_t> encoding) {
    keep_encoding(completed_, encoding);
    std::vector<std::uint8_t> history;
    const std::string domain = "eawr-net-desync-v1";
    history.insert(history.end(), domain.begin(), domain.end());
    for (const auto& row : rows_) {
        append_u64(history, row.tick);
        append_hash(history, row.state);
        append_hash(history, row.input);
    }
    local_summary_ = Summary{rows_.front().tick, completed_, core::sha256_hex(history), end};
    waiting_ = true;
    ended_ = end;
    std::vector<std::uint8_t> frame{desync_combined_kind};
    append_u64(frame, local_summary_->first);
    append_u64(frame, completed_);
    append_hash(frame, local_summary_->hash);
    frame.push_back(end ? 1 : 0);
    if (auto sent = broadcast(frame); !sent) return sent;
    return evaluate();
}

core::Result<void> DesyncMonitor::receive(std::uint32_t from, std::span<const std::uint8_t> frame) {
    if (!valid_ || failed_) return fail("inactive desync monitor");
    if (from >= transport_.peer_count() || from == transport_.self() || frame.empty())
        return fail("invalid desync sender or empty frame");
    if (frame[0] != desync_combined_kind && frame[0] != desync_rows_kind) return fail("unknown desync message kind");
    Reader reader{frame, 1};
    Summary summary;
    if (!reader.u64(summary.first) || !reader.u64(summary.last) || summary.last < summary.first
        || summary.last - summary.first >= options_.exchange_every
        || (summary.first == 0 && summary.last != 0)) return fail("malformed desync window");
    std::vector<Row> decoded;
    if (frame[0] == desync_combined_kind) {
        if (!reader.hash(summary.hash) || reader.offset + 1 != frame.size() || frame[reader.offset] > 1)
            return fail("malformed combined hash");
        summary.end = frame[reader.offset] != 0;
        if (!summary.end && summary.last != 0 && summary.last % options_.exchange_every != 0)
            return fail("unexpected desync exchange point");
    } else {
        const auto count = summary.last - summary.first + 1;
        if (frame.size() - reader.offset != count * (2 * hash_size)) return fail("malformed desync row count");
        for (std::uint64_t tick = summary.first; ; ++tick) {
            Row row{tick, {}, {}};
            if (!reader.hash(row.state) || !reader.hash(row.input)) return fail("malformed desync row hash");
            decoded.push_back(std::move(row));
            if (tick == summary.last) break;
        }
        std::vector<std::uint8_t> history;
        const std::string domain = "eawr-net-desync-v1";
        history.insert(history.end(), domain.begin(), domain.end());
        for (const auto& row : decoded) { append_u64(history, row.tick); append_hash(history, row.state); append_hash(history, row.input); }
        summary.hash = core::sha256_hex(history);
    }
    // Even an ignored in-flight tail must be structurally valid.
    if (reconnect_floor_ && summary.last <= *reconnect_floor_) return core::Result<void>::success();
    if (!active_peers_[transport_.self()]) return core::Result<void>::success();
    const bool previous = previous_ && summary.first == previous_->local.first;
    if (!active_peers_[from] && !previous && summary.first != next_first_)
        return core::Result<void>::success();
    if (!previous && summary.first != next_first_ && local_summary_
        && local_summary_->last != std::numeric_limits<std::uint64_t>::max()
        && summary.first == local_summary_->last + 1) {
        auto& queued = future_frames_[from];
        if (queued.size() >= 2) return fail("future desync window exceeds its bound");
        queued.emplace_back(frame.begin(), frame.end());
        return core::Result<void>::success();
    }
    if (!previous && summary.first != next_first_) return fail("out-of-window desync frame");
    auto& summaries = previous ? previous_->summaries : summaries_;
    auto& remote = previous ? previous_->remote : remote_rows_;
    if (frame[0] == desync_combined_kind) {
        if (!summaries.emplace(from, std::move(summary)).second) return fail("duplicate combined hash");
    } else {
        if (!summaries.contains(from)) {
            if (!active_peers_[from]) return core::Result<void>::success();
            return fail("unsolicited desync hash rows");
        }
        if (summary.last != summaries.at(from).last || summary.hash != summaries.at(from).hash)
            return fail("hash rows do not authenticate the combined hash");
        if (!remote.emplace(from, std::move(decoded)).second) return fail("duplicate desync hash rows");
    }
    if (previous) return evaluate_previous();
    return evaluate();
}

core::Result<void> DesyncMonitor::evaluate() {
    if (!local_summary_) return core::Result<void>::success();
    bool mismatch = false;
    for (const auto& [peer, summary] : summaries_) {
        if (!active_peers_[peer]) continue;
        if (summary.first != local_summary_->first || summary.last != local_summary_->last || summary.end != local_summary_->end)
            return fail("peers disagree on the desync exchange frontier or match end");
        mismatch = mismatch || summary.hash != local_summary_->hash;
    }
    const bool notice = std::any_of(remote_rows_.begin(), remote_rows_.end(),
        [&](const auto& entry) { return active_peers_[entry.first]; });
    frozen_ = previous_frozen_ || mismatch || notice;
    diagnose(rows_, remote_rows_, false);
    // MP-24: freeze advancement on the first mismatch, keeping the transport live.
    if (mismatch || frozen_) {
        frozen_ = true;
        if (!rows_sent_) {
            rows_sent_ = true;
            std::vector<std::uint8_t> frame{desync_rows_kind};
            append_u64(frame, local_summary_->first);
            append_u64(frame, local_summary_->last);
            for (const auto& row : rows_) { append_hash(frame, row.state); append_hash(frame, row.input); }
            if (auto sent = broadcast(frame); !sent) return sent;
        }
    }
    const auto active_summaries = static_cast<std::size_t>(std::count_if(summaries_.begin(), summaries_.end(),
        [&](const auto& entry) { return active_peers_[entry.first]; }));
    if (active_summaries != active_remote_count()) return core::Result<void>::success();
    const auto active_rows = static_cast<std::size_t>(std::count_if(remote_rows_.begin(), remote_rows_.end(),
        [&](const auto& entry) { return active_peers_[entry.first]; }));
    if (mismatch && active_rows != active_remote_count()) return core::Result<void>::success();
    diagnose(rows_, remote_rows_);
    if (!mismatch) {
        // Equal active summaries fully authenticate agreement. A detail notice
        // alone cannot change that result; late rows are checked against the
        // retained agreed window rather than holding an unnecessary row gate.
        frozen_ = previous_frozen_;
        if (frozen_) return core::Result<void>::success();
        if (completed_ == 0) setup_agreed_ = true;
        next_first_ = completed_ + 1;
        previous_ = AgreedWindow{*local_summary_, std::move(rows_), std::move(summaries_), std::move(remote_rows_), rows_sent_};
        rows_.clear();
        remote_rows_.clear();
        rows_sent_ = false;
        // Keep the two most recent input batches for context preceding the next window.
        while (inputs_.size() > 2) { retained_bytes_ -= inputs_.begin()->second.size(); inputs_.erase(inputs_.begin()); }
        local_summary_.reset();
        summaries_.clear();
        waiting_ = false;
        auto future = std::move(future_frames_);
        future_frames_.clear();
        for (auto& [peer, frames] : future) {
            for (const auto& frame : frames) {
                if (auto received = receive(peer, frame); !received) return received;
            }
        }
        return core::Result<void>::success();
    }
    if (report_ && !report_->diagnostic_only) return core::Result<void>::success();
    return fail("combined hash mismatch without a divergent row");
}

core::Result<void> DesyncMonitor::evaluate_previous() {
    previous_frozen_ = false;
    if (!previous_) return core::Result<void>::success();
    for (const auto& [peer, remote] : previous_->remote) {
        if (!active_peers_[peer]) continue;
        for (std::size_t i = 0; i < previous_->rows.size(); ++i)
            previous_frozen_ = previous_frozen_ || previous_->rows[i].state != remote[i].state
                || previous_->rows[i].input != remote[i].input;
    }
    diagnose(previous_->rows, previous_->remote);
    frozen_ = frozen_ || previous_frozen_;
    // An honest active peer's rows authenticate an already equal summary, so
    // this defensive branch is reachable only if the digest's binding fails.
    if (previous_frozen_ && !previous_->rows_sent) {
        previous_->rows_sent = true;
        std::vector<std::uint8_t> frame{desync_rows_kind};
        append_u64(frame, previous_->local.first); append_u64(frame, previous_->local.last);
        for (const auto& row : previous_->rows) { append_hash(frame, row.state); append_hash(frame, row.input); }
        return broadcast(frame);
    }
    return core::Result<void>::success();
}

void DesyncMonitor::diagnose(const std::vector<Row>& rows, const std::map<std::uint32_t, std::vector<Row>>& remotes, bool allow_active) {
    bool notes_changed = false;
    for (const auto& [peer, remote] : remotes) {
        if (active_peers_[peer]) continue;
        for (std::size_t i = 0; i < std::min(rows.size(), remote.size()); ++i) {
            if (remote[i].tick > *departure_ticks_[peer]) break;
            const bool world = rows[i].state != remote[i].state;
            if (!world && rows[i].input == remote[i].input) continue;
            auto old = std::find_if(departed_divergences_.begin(), departed_divergences_.end(),
                [&](const auto& note) { return note.peer == peer; });
            DepartedDivergence note{peer, rows[i].tick, world ? rows[i].state : rows[i].input,
                world ? remote[i].state : remote[i].input, world ? "world/script" : "canonical input"};
            if (old == departed_divergences_.end()) { departed_divergences_.push_back(std::move(note)); notes_changed = true; }
            else if (note.tick < old->tick) { *old = std::move(note); notes_changed = true; }
            break;
        }
    }
    if (report_ && notes_changed) {
        report_->departed_divergences = departed_divergences_;
        build_bundle();
    }
    // Prefer an active disagreement; departed evidence is an informational note.
    for (bool active : {true, false}) for (std::size_t i = 0; i < rows.size(); ++i) {
        for (const auto& [peer, remote] : remotes) {
            if ((active && !allow_active) || active_peers_[peer] != active || i >= remote.size()
                || (!active && remote[i].tick > *departure_ticks_[peer])) continue;
            const bool world = rows[i].state != remote[i].state;
            if (!world && rows[i].input == remote[i].input) continue;
            if (report_ && !report_->diagnostic_only && !active) return;
            if (report_ && (!report_->diagnostic_only || !active)
                && report_->first_divergent_tick <= rows[i].tick) return;
            DesyncReport report;
            report.diagnostic_only = !active;
            report.departed_divergences = departed_divergences_;
            report.first_divergent_tick = rows[i].tick;
            report.detected_at = completed_;
            report.local_peer = transport_.self(); report.remote_peer = peer;
            report.local_hash = world ? rows[i].state : rows[i].input;
            report.remote_hash = world ? remote[i].state : remote[i].input;
            report.channel = world ? "world/script" : "canonical input";
            for (PeerIndex departed = 0; departed < departure_ticks_.size(); ++departed)
                if (departure_ticks_[departed]) report.departures.push_back({departed, *departure_ticks_[departed]});
            report_ = std::move(report);
            build_bundle();
            return;
        }
    }
}

void DesyncMonitor::build_bundle() {
    const auto contains = [&](const std::vector<Row>& rows) {
        return !rows.empty() && rows.front().tick <= report_->first_divergent_tick
            && report_->first_divergent_tick <= rows.back().tick;
    };
    const bool current = contains(rows_);
    const bool previous = !current && previous_ && contains(previous_->rows);
    const bool available = current || previous;
    const auto& local_rows = current ? rows_ : (previous ? previous_->rows : rows_);
    const auto& remote_rows = current ? remote_rows_ : (previous ? previous_->remote : remote_rows_);
    auto& report = *report_;
    if (available) report.bundle = {};
    report.bundle_location.clear();
    report.bundle_error.reset();
    report.bundle.id = "tick-" + std::to_string(report.first_divergent_tick) + "-peer-" + std::to_string(report.local_peer);
    const auto binary = [&](const char* name, std::span<const std::uint8_t> bytes) {
        auto old = std::find_if(report.bundle.files.begin(), report.bundle.files.end(),
            [&](const auto& file) { return file.name == name; });
        if (old != report.bundle.files.end()) {
            report.bundle.size_bytes -= old->bytes.size();
            old->bytes.assign(bytes.begin(), bytes.end());
        } else report.bundle.files.push_back({name, {bytes.begin(), bytes.end()}});
        report.bundle.size_bytes += bytes.size();
    };
    const auto text_file = [&](const char* name, const std::string& value) {
        binary(name, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    };
    std::string notes;
    for (const auto& note : report.departed_divergences)
        notes += "departed peer " + std::to_string(note.peer) + " diverged at tick " + std::to_string(note.tick)
            + " channel=" + note.channel + " local_hash=" + note.local_hash + " remote_hash=" + note.remote_hash + "\n";
    text_file("report.txt", "first_divergent_tick=" + std::to_string(report.first_divergent_tick)
        + "\ndiagnostic_only=" + (report.diagnostic_only ? std::string("yes") : std::string("no"))
        + "\ndetected_at=" + std::to_string(report.detected_at) + "\nchannel=" + report.channel
        + "\nlocal_peer=" + std::to_string(report.local_peer) + "\nremote_peer=" + std::to_string(report.remote_peer)
        + "\nlocal_hash=" + report.local_hash + "\nremote_hash=" + report.remote_hash + "\n" + notes);
    std::string departures = "peer,input_cutoff_tick\n";
    for (const auto& departed : report.departures)
        departures += std::to_string(departed.peer) + "," + std::to_string(departed.input_cutoff_tick) + "\n";
    text_file("departures.csv", departures);
    if (!available) {
        // The incident has aged beyond both retained hash windows. Its bundle
        // is the durable snapshot; update notes without replacing that evidence.
        if (options_.diagnostic_sink) {
            auto published = options_.diagnostic_sink(report.bundle);
            if (published) report.bundle_location = std::move(published).value();
            else report.bundle_error = published.error();
        }
        return;
    }
    const Checkpoint* nearest = nullptr;
    const auto distance = [&](std::uint64_t tick) {
        return tick > report.first_divergent_tick ? tick - report.first_divergent_tick : report.first_divergent_tick - tick;
    };
    for (const auto& point : checkpoints_) {
        if (!nearest || distance(point.tick) < distance(nearest->tick)) nearest = &point;
    }
    if (nearest) { report.encoding_tick = nearest->tick; binary("session.bin", nearest->bytes); }
    else binary("session.bin", {});
    text_file("encoding.txt", "completed_tick=" + std::to_string(report.encoding_tick)
        + "\nretained=" + (nearest ? std::string("yes") : std::string("no (encoding unavailable or exceeded budget)")) + "\n");
    std::vector<std::uint8_t> input_bytes;
    // Versioned framing: magic, then repeated LE u64 completed tick, LE u64 byte count, canonical batch.
    const std::string magic = "eawr-desync-input-v1\n";
    input_bytes.insert(input_bytes.end(), magic.begin(), magic.end());
    for (const auto& [tick, bytes] : inputs_) {
        append_u64(input_bytes, tick); append_u64(input_bytes, bytes.size());
        input_bytes.insert(input_bytes.end(), bytes.begin(), bytes.end());
    }
    binary("inputs.bin", input_bytes);
    binary("setup.bin", options_.match_setup);
    text_file("identity.txt", "match=" + options_.match_identity + "\nbuild=" + options_.build_identity + "\n");
    std::string csv = "peer,completed_tick,state_hash,input_hash\n";
    const auto add_rows = [&](std::uint32_t peer, const std::vector<Row>& rows) {
        for (const auto& row : rows) csv += std::to_string(peer) + "," + std::to_string(row.tick) + "," + row.state + "," + row.input + "\n";
    };
    add_rows(transport_.self(), local_rows);
    for (const auto& [peer, rows] : remote_rows) add_rows(peer, rows);
    text_file("hashes.csv", csv);
    text_file("README.txt", "EAWR desync diagnostics v1\nActive disagreement freezes the match; diagnostic_only=yes records departed-peer evidence without stopping survivors.\n"
        "report.txt names the first divergent completed tick (zero means setup).\n"
        "hashes.csv contains authenticated local and remote rows since the last agreed exchange.\n"
        "session.bin is this peer's nearest retained canonical session encoding; encoding.txt gives its tick.\n"
        "inputs.bin contains canonical batches framed by LE u64 completed tick and LE u64 byte count, after its magic line.\n"
        "setup.bin and identity.txt contain the supplied setup, match and build identities.\n"
        "departures.csv records agreed input cutoffs; later departed-peer rows do not qualify the search.\n"
        "Payload retention is capped by max_retained_bytes; hash rows are capped by exchange_every and the agreed peer count.\n"
        "No automatic recovery or corrective world transfer is performed.\n");
    if (options_.diagnostic_sink) {
        auto published = options_.diagnostic_sink(report.bundle);
        if (published) report.bundle_location = std::move(published).value();
        else report.bundle_error = published.error();
    }
}

} // namespace eawr::net
