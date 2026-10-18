#include "eawr/presentation/ui/net_perf.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace eawr::presentation::ui {

namespace {

[[nodiscard]] std::string fixed(const char* pattern, const double value) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

[[nodiscard]] const char* blame_name(const NetBlame blame) {
    switch (blame) {
    case NetBlame::network: return "network";
    case NetBlame::game: return "game";
    case NetBlame::ok: break;
    }
    return "ok";
}

} // namespace

void NetStallLedger::observe(const std::uint64_t now_ms, const bool waiting, const std::uint32_t peer) {
    if (waiting && !waiting_) {
        waiting_ = true;
        since_ = now_ms;
        peer_ = peer;
    } else if (!waiting && waiting_) {
        waiting_ = false;
        episodes_.push_back({since_, now_ms, peer_});
        if (episodes_.size() > net_perf_max_episodes) episodes_.pop_front();
    }
    trim(now_ms);
}

void NetStallLedger::trim(const std::uint64_t now_ms) {
    while (!episodes_.empty() && now_ms > episodes_.front().end && now_ms - episodes_.front().end > net_perf_history_ms)
        episodes_.pop_front();
}

void NetStallLedger::reset() {
    episodes_.clear();
    waiting_ = false;
}

NetStallSnapshot NetStallLedger::snapshot(const std::uint64_t now_ms) const {
    NetStallSnapshot out;
    const std::uint64_t second_from = now_ms > net_perf_second_ms ? now_ms - net_perf_second_ms : 0;
    const std::uint64_t history_from = now_ms > net_perf_history_ms ? now_ms - net_perf_history_ms : 0;
    const auto add = [&](const Episode& episode) {
        if (episode.end < history_from) return;
        const std::uint64_t length = episode.end - episode.start;
        if (length > 0 && length >= out.longest_ms_10s) { out.longest_ms_10s = length; out.longest_peer_10s = episode.peer; }
        if (episode.end < second_from) return;
        ++out.stalls_last_second;
        out.stalled_ms_last_second += episode.end - std::max(episode.start, second_from);
        if (length >= out.longest_ms_last_second) { out.longest_ms_last_second = length; out.longest_peer_last_second = episode.peer; }
    };
    for (const Episode& episode : episodes_) add(episode);
    if (waiting_) {
        out.waiting = true;
        out.wait_peer = peer_;
        out.wait_ms = now_ms > since_ ? now_ms - since_ : 0;
        add({since_, std::max(now_ms, since_), peer_});
    }
    return out;
}

NetVerdict net_verdict(const NetPerfReport& report) {
    const NetStallSnapshot& stall = report.stall;
    NetVerdict out;
    if ((stall.waiting && stall.wait_ms >= net_perf_blame_ms) || stall.stalled_ms_last_second >= net_perf_blame_ms) {
        out.blame = NetBlame::network;
        const bool running = stall.waiting && stall.wait_ms >= stall.longest_ms_last_second;
        const std::uint32_t peer = running ? stall.wait_peer : stall.longest_peer_last_second;
        const std::uint64_t ms = running ? stall.wait_ms : stall.longest_ms_last_second;
        out.text = "network: waiting on peer " + std::to_string(peer) + " " + std::to_string(ms) + " ms";
    } else if (report.frame_ms > net_perf_game_ms || report.sim_ms > net_perf_game_ms) {
        out.blame = NetBlame::game;
        out.text = "game: frame " + fixed("%.0f", report.frame_ms) + " ms, sim " + fixed("%.0f", report.sim_ms) + " ms";
    } else {
        out.text = "ok";
    }
    return out;
}

std::string net_summary_line(const NetPerfReport& report) {
    const NetStallSnapshot& stall = report.stall;
    std::ostringstream out;
    out << "EAWR net summary seat=" << report.seat << " verdict=" << blame_name(net_verdict(report).blame) << " wait_peer=";
    if (stall.waiting) out << stall.wait_peer;
    else out << '-';
    out << " wait_ms=" << (stall.waiting ? stall.wait_ms : 0) << " stalls_per_s=" << stall.stalls_last_second
        << " stalled_ms_1s=" << stall.stalled_ms_last_second << " longest_ms_10s=" << stall.longest_ms_10s
        << " frame_ms=" << fixed("%.1f", report.frame_ms) << " sim_ms=" << fixed("%.1f", report.sim_ms) << " peers=";
    if (report.peers.empty()) out << '-';
    for (std::size_t index = 0; index < report.peers.size(); ++index) {
        const NetPeerLink& peer = report.peers[index];
        out << (index == 0 ? "" : ";") << peer.peer << ':';
        if (!peer.measured) out << "unmeasured";
        else out << "rtt_ms=" << fixed("%.0f", peer.rtt_ms) << ",jitter_ms=" << fixed("%.0f", peer.jitter_ms)
                 << ",loss_pct=" << fixed("%.1f", peer.loss_pct);
    }
    return out.str();
}

std::string net_report_json(const NetPerfReport& report) {
    const NetStallSnapshot& stall = report.stall;
    const NetVerdict verdict = net_verdict(report);
    std::ostringstream out;
    out << "{\"seat\": " << report.seat << ", \"verdict\": \"" << blame_name(verdict.blame) << "\", \"verdict_text\": \""
        << verdict.text << "\", \"waiting\": " << (stall.waiting ? "true" : "false") << ", \"wait_peer\": ";
    if (stall.waiting) out << stall.wait_peer;
    else out << "null";
    out << ", \"wait_ms\": " << (stall.waiting ? stall.wait_ms : 0) << ", \"stalls_per_s\": " << stall.stalls_last_second
        << ", \"stalled_ms_1s\": " << stall.stalled_ms_last_second << ", \"longest_ms_10s\": " << stall.longest_ms_10s
        << ", \"longest_peer_10s\": " << stall.longest_peer_10s << ", \"frame_ms\": " << fixed("%.3f", report.frame_ms)
        << ", \"sim_ms\": " << fixed("%.3f", report.sim_ms) << ", \"peers\": [";
    for (std::size_t index = 0; index < report.peers.size(); ++index) {
        const NetPeerLink& peer = report.peers[index];
        out << (index == 0 ? "" : ", ") << "{\"peer\": " << peer.peer << ", \"measured\": " << (peer.measured ? "true" : "false")
            << ", \"rtt_ms\": " << fixed("%.1f", peer.rtt_ms) << ", \"jitter_ms\": " << fixed("%.1f", peer.jitter_ms)
            << ", \"loss_pct\": " << fixed("%.1f", peer.loss_pct) << "}";
    }
    out << "]}";
    return out.str();
}

} // namespace eawr::presentation::ui
