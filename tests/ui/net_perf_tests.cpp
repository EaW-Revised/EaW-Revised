// #4043: the overlay's network model: the stall ledger (sub-second waits count), the verdict and
// the stable godot.log summary line. Synthetic samples and a synthetic clock only.

#include "eawr/presentation/ui/net_perf.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

void test_subsecond_waits_count() {
    ui::NetStallLedger ledger;
    // Three waits of 40, 60 and 20 ms inside one second; none reaches the 1 s stall-start threshold.
    ledger.observe(1000, true, 2);
    ledger.observe(1040, false, 0);
    ledger.observe(1300, true, 2);
    ledger.observe(1360, false, 0);
    ledger.observe(1600, true, 1);
    ledger.observe(1620, false, 0);
    const ui::NetStallSnapshot now = ledger.snapshot(1700);
    expect(!now.waiting, "no wait is running");
    expect(now.stalls_last_second == 3, "all three sub-second waits are counted");
    expect(now.stalled_ms_last_second == 120, "their durations add up");
    expect(now.longest_ms_last_second == 60 && now.longest_peer_last_second == 2, "the longest wait names its peer");
    expect(now.longest_ms_10s == 60, "the 10 s longest agrees");
}

void test_per_second_window_and_history() {
    ui::NetStallLedger ledger;
    ledger.observe(0, true, 1);
    ledger.observe(500, false, 0);
    ui::NetStallSnapshot at = ledger.snapshot(900);
    expect(at.stalls_last_second == 1 && at.stalled_ms_last_second == 500, "a fresh wait is in the last second");
    at = ledger.snapshot(1800);
    expect(at.stalls_last_second == 0 && at.stalled_ms_last_second == 0, "it leaves the per-second figures");
    expect(at.longest_ms_10s == 500 && at.longest_peer_10s == 1, "the 10 s longest still remembers it");
    at = ledger.snapshot(11000);
    expect(at.longest_ms_10s == 0, "after ten seconds it is forgotten");
    ledger.observe(20000, false, 0);  // trims
    expect(ledger.snapshot(20000).longest_ms_10s == 0, "trimming keeps the ledger empty");
}

void test_running_wait() {
    ui::NetStallLedger ledger;
    ledger.observe(5000, true, 3);
    ledger.observe(5400, true, 3);
    const ui::NetStallSnapshot now = ledger.snapshot(5640);
    expect(now.waiting && now.wait_peer == 3 && now.wait_ms == 640, "the running wait is measured to now");
    expect(now.stalls_last_second == 1 && now.stalled_ms_last_second == 640 && now.longest_ms_10s == 640,
           "the running wait is in the per-second and 10 s figures");
}

void test_verdict() {
    ui::NetPerfReport report;
    report.frame_ms = 8.0;
    report.sim_ms = 5.0;
    expect(ui::net_verdict(report).text == "ok" && ui::net_verdict(report).blame == ui::NetBlame::ok, "quiet is ok");

    report.frame_ms = 48.0;
    report.sim_ms = 31.0;
    ui::NetVerdict verdict = ui::net_verdict(report);
    expect(verdict.blame == ui::NetBlame::game && verdict.text == "game: frame 48 ms, sim 31 ms", "slow frames blame the game");

    report.stall.waiting = true;
    report.stall.wait_peer = 1;
    report.stall.wait_ms = 640;
    report.stall.stalled_ms_last_second = 640;
    report.stall.longest_ms_last_second = 640;
    report.stall.longest_peer_last_second = 1;
    verdict = ui::net_verdict(report);
    expect(verdict.blame == ui::NetBlame::network && verdict.text == "network: waiting on peer 1 640 ms",
           "a long wait blames the network even when frames are slow too");

    report.stall = {};
    report.stall.stalled_ms_last_second = 99;
    report.frame_ms = 8.0;
    report.sim_ms = 5.0;
    expect(ui::net_verdict(report).blame == ui::NetBlame::ok, "99 ms of waiting in a second is below the blame line");
    report.stall.stalled_ms_last_second = 120;
    report.stall.longest_ms_last_second = 60;
    report.stall.longest_peer_last_second = 2;
    expect(ui::net_verdict(report).text == "network: waiting on peer 2 60 ms", "many short waits blame the network too");
}

void test_summary_line_is_stable() {
    ui::NetPerfReport report;
    report.seat = 0;
    report.frame_ms = 48.04;
    report.sim_ms = 31.0;
    report.stall.waiting = true;
    report.stall.wait_peer = 1;
    report.stall.wait_ms = 640;
    report.stall.stalls_last_second = 3;
    report.stall.stalled_ms_last_second = 700;
    report.stall.longest_ms_10s = 900;
    report.peers.push_back({1, true, 80.4, 5.0, 1.26});
    report.peers.push_back({2, false, 0.0, 0.0, 0.0});
    const std::string expected = "EAWR net summary seat=0 verdict=network wait_peer=1 wait_ms=640 stalls_per_s=3 "
                                 "stalled_ms_1s=700 longest_ms_10s=900 frame_ms=48.0 sim_ms=31.0 "
                                 "peers=1:rtt_ms=80,jitter_ms=5,loss_pct=1.3;2:unmeasured";
    expect(ui::net_summary_line(report) == expected, "the summary line keeps its format: " + ui::net_summary_line(report));

    const ui::NetPerfReport idle;
    expect(ui::net_summary_line(idle) == "EAWR net summary seat=0 verdict=ok wait_peer=- wait_ms=0 stalls_per_s=0 "
                                         "stalled_ms_1s=0 longest_ms_10s=0 frame_ms=0.0 sim_ms=0.0 peers=-",
           "an idle report prints dashes");
    const std::string json = ui::net_report_json(report);
    expect(json.find("\"verdict\": \"network\"") != std::string::npos && json.find("\"wait_peer\": 1") != std::string::npos
               && json.find("\"measured\": false") != std::string::npos,
           "the report object carries the verdict, the gate peer and unmeasured links");
}

} // namespace

int main() {
    test_subsecond_waits_count();
    test_per_second_window_and_history();
    test_running_wait();
    test_verdict();
    test_summary_line_is_stable();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " net perf check(s) failed\n";
        return 1;
    }
    std::cout << "ui net perf passed\n";
    return 0;
}
