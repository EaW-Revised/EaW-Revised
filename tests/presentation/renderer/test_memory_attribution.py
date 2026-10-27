"""Execute the production opt-in entry and report guards without a GPU/engine."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain


class MemoryAttributionContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "apps/viewer/src/memory_attribution.hpp").read_text(encoding="utf-8")
        # Execute the production path before the first engine probe. A probe
        # moved ahead of the opt-in guard is included, rather than ignored.
        entry = source.split("void capture() {", 1)[1].split("auto* performance =", 1)[0]
        startup = (ROOT / "apps/viewer/src/startup_trace.hpp").read_text(encoding="utf-8")
        start = startup.index("if (MemoryAttribution::enabled()) {")
        end = startup.index("\n                }", start) + len("\n                }")
        report_guard = startup[start:end]
        fixture = r'''
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
bool requested = false;
bool enabled() { return requested; }
struct MemoryAttribution { static bool enabled() { return requested; } };
int probe_work = 0, report_work = 0;
void capture() {
''' + entry + r'''
    std::vector<int> allocation(8, 1);
    probe_work += allocation.front();
}
struct Probe {
    void write(std::ostream& output) const { ++report_work; output << "{\"stub\":true}"; }
};
struct Snapshot { Probe attribution; };
std::string report() {
    std::ostringstream output;
    Snapshot snapshot;
    const char* phases[] = {"post_load", "first_frame", "tick_1200"};
    output << '[';
    bool first = true;
    for (const auto* phase : phases) {
        output << (first ? "" : ",") << "{\"phase\":\"" << phase << "\",\"existing_counter\":7";
        first = false;
''' + report_guard + r'''
        output << '}';
    }
    output << ']';
    return output.str();
}
int main() {
    capture();
    const auto off = report();
    const auto off_probes = probe_work, off_reports = report_work;
    requested = true;
    capture();
    const auto on = report();
    std::cout << "{\"off\":{\"probe_calls\":" << off_probes << ",\"report_calls\":" << off_reports
              << ",\"snapshots\":" << off << "},\"on\":{\"probe_calls\":" << probe_work
              << ",\"report_calls\":" << report_work << ",\"snapshots\":" << on << "}}";
}
'''
        toolchain = select_toolchain()
        if toolchain is None:
            raise unittest.SkipTest("requires a C++ compiler on the validation host")
        with tempfile.TemporaryDirectory(prefix="eawr-memory-opt-in-") as temporary:
            path = Path(temporary)
            cpp, executable = path / "guard.cpp", path / "guard.exe"
            cpp.write_text(fixture, encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            completed = subprocess.run([str(executable)], check=True, capture_output=True,
                                       text=True, timeout=10)
            cls.evidence = json.loads(completed.stdout)

    def test_flag_off_skips_probe_work(self):
        self.assertEqual(self.evidence["off"]["probe_calls"], 0)
        self.assertEqual(self.evidence["on"]["probe_calls"], 1)  # Positive control.

    def test_flag_off_omits_only_the_attribution_output(self):
        off, on = self.evidence["off"], self.evidence["on"]
        self.assertEqual(off["report_calls"], 0)
        self.assertEqual(on["report_calls"], 3)
        self.assertEqual([row["phase"] for row in off["snapshots"]],
                         ["post_load", "first_frame", "tick_1200"])
        for disabled, enabled in zip(off["snapshots"], on["snapshots"]):
            self.assertEqual(disabled["existing_counter"], 7)
            self.assertNotIn("attribution", disabled)
            self.assertEqual(enabled["attribution"], {"stub": True})


class ContainerCapacityContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "apps/viewer/src/container_memory.hpp").read_text(encoding="utf-8")
        entry = source.split("void LiveSessionView::capture_memory_capacity(", 1)[1]
        entry = entry.split("{", 1)[1].split("memory_capacity_report_ =", 1)[0]
        load_entry = source.split("void LiveSessionView::capture_memory_capacity_post_load(", 1)[1]
        load_entry = load_entry.split("{", 1)[1].split("memory_capacity_post_load_report_ =", 1)[0]
        middle_entry = source.split("void LiveSessionView::capture_memory_capacity_600(", 1)[1]
        middle_entry = middle_entry.split("{", 1)[1].split("memory_capacity_600_report_ =", 1)[0]
        header = (ROOT / "include/eawr/diagnostics/memory_capacity.hpp").as_posix()
        fixture = '#include "' + header + '"\n' + r'''
#include <iostream>
#include <sstream>
#include <string>
struct Mesh { std::vector<int> triangles, nodes; };
struct Profile {
    std::vector<Mesh> meshes;
    std::vector<int> weapons, target_bones, hardpoints, aimed_routes, hardpoint_meshes, death_projectiles;
};
struct Table { std::vector<Profile> profiles, retaliation_team_types; };
bool memory_capacity_probe_ = false;
std::string memory_capacity_report_;
double presented_tick_ = 1200;
int captures = 0, load_captures = 0, middle_captures = 0, session_queries = 0;
std::string memory_capacity_600_report_;
std::string memory_capacity_post_load_report_;
struct Options { bool borrowed_feed = false; } options_;
struct Session {
    bool held = true;
    unsigned tick = 0;
    bool paused() const { ++session_queries; return held; }
    unsigned completed_tick() const { ++session_queries; return tick; }
} session_value;
Session* session_ = &session_value;
void capture_middle() {
''' + middle_entry + r'''
    ++middle_captures;
    memory_capacity_600_report_ = "recorded";
}
void capture_load() {
''' + load_entry + r'''
    ++load_captures;
    memory_capacity_post_load_report_ = "recorded";
}
void capture() {
''' + entry + r'''
    ++captures;
    memory_capacity_report_ = "recorded";
}
int main() {
    capture();
    capture_load();
    capture_middle();
    const auto middle_off = middle_captures;
    const auto load_off_queries = session_queries;
    const auto off = captures;
    memory_capacity_probe_ = true;
    options_.borrowed_feed = true;
    capture_load();
    options_.borrowed_feed = false;
    session_value.held = false;
    capture_load();
    session_value.held = true;
    session_value.tick = 1;
    capture_load();
    const auto rejected_load = load_captures;
    session_value.tick = 0;
    capture_load();
    capture_load();
    presented_tick_ = 599;
    capture_middle();
    const auto middle_before = middle_captures;
    presented_tick_ = 600;
    capture_middle();
    presented_tick_ = 601;
    capture_middle();
    presented_tick_ = 1199;
    capture();
    const auto before = captures;
    presented_tick_ = 1200;
    capture();
    presented_tick_ = 1201;
    capture();
    Table table;
    table.profiles.resize(1);
    table.profiles.front().meshes.resize(1);
    table.profiles.front().meshes.front().triangles.reserve(100);
    table.profiles.front().meshes.front().triangles.resize(2);
    table.profiles.front().meshes.front().nodes.reserve(20);
    table.retaliation_team_types = table.profiles;
    const auto expected = (table.profiles.front().meshes.front().triangles.capacity()
        + table.retaliation_team_types.front().meshes.front().triangles.capacity()) * sizeof(int);
    eawr::diagnostics::MemoryCapacity capacities;
    eawr::diagnostics::combat_capacity(capacities, "synthetic", table);
    std::uint64_t actual = 0;
    for (std::size_t i = 0; i < capacities.count; ++i)
        if (std::string(capacities.rows[i].name) == "triangles") actual = capacities.rows[i].bytes;
    std::vector<bool> packed(65);
    const auto packed_bytes = eawr::diagnostics::MemoryCapacity::bytes(packed);
    eawr::diagnostics::MemoryCapacity bounded;
    for (std::size_t i = 0; i < bounded.rows.size() + 1; ++i) bounded.add("g", "n", 1);
    std::ostringstream output;
    capacities.write(output);
    std::cout << "{\"off\":" << off << ",\"before\":" << before << ",\"captures\":" << captures
              << ",\"load_off_queries\":" << load_off_queries << ",\"rejected_load\":" << rejected_load
              << ",\"load_captures\":" << load_captures
              << ",\"middle_off\":" << middle_off << ",\"middle_before\":" << middle_before
              << ",\"middle_captures\":" << middle_captures
              << ",\"actual\":" << actual << ",\"expected\":" << expected
              << ",\"packed_bytes\":" << packed_bytes << ",\"packed_capacity\":" << packed.capacity()
              << ",\"bounded_count\":" << bounded.count << ",\"limit\":" << bounded.rows.size()
              << ",\"bounded_complete\":" << (bounded.complete ? "true" : "false")
              << ",\"inventory\":" << output.str() << '}';
}
'''
        toolchain = select_toolchain()
        if toolchain is None:
            raise unittest.SkipTest("requires a C++ compiler on the validation host")
        with tempfile.TemporaryDirectory(prefix="eawr-container-capacity-") as temporary:
            path = Path(temporary)
            cpp, executable = path / "capacity.cpp", path / "capacity.exe"
            cpp.write_text(fixture, encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            completed = subprocess.run([str(executable)], check=True, capture_output=True,
                                       text=True, timeout=10)
            cls.evidence = json.loads(completed.stdout)

    def test_capacity_counts_unused_storage_and_both_owned_profile_copies(self):
        self.assertEqual(self.evidence["actual"], self.evidence["expected"])
        self.assertGreaterEqual(self.evidence["actual"], 408)
        self.assertEqual(self.evidence["packed_bytes"], (self.evidence["packed_capacity"] + 7) // 8)
        self.assertTrue(self.evidence["inventory"]["rows_complete"])

    def test_load_probe_requires_registered_standalone_paused_tick_zero_and_captures_once(self):
        self.assertEqual(self.evidence["load_off_queries"], 0)
        self.assertEqual(self.evidence["rejected_load"], 0)
        self.assertEqual(self.evidence["load_captures"], 1)

    def test_tick_600_probe_is_off_or_early_then_captures_once(self):
        self.assertEqual(self.evidence["middle_off"], 0)
        self.assertEqual(self.evidence["middle_before"], 0)
        self.assertEqual(self.evidence["middle_captures"], 1)

    def test_row_bound_is_explicit(self):
        self.assertEqual(self.evidence["bounded_count"], self.evidence["limit"])
        self.assertFalse(self.evidence["bounded_complete"])

    def test_unregistered_probe_and_early_frame_do_no_work_then_capture_once(self):
        self.assertEqual(self.evidence["off"], 0)
        self.assertEqual(self.evidence["before"], 0)
        self.assertEqual(self.evidence["captures"], 1)


if __name__ == "__main__":
    unittest.main()
