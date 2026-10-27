"""Execute complete production capture/snapshot bodies with engine-boundary adapters."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class OwnerCensusSnapshotContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "apps/viewer/src/container_memory.hpp").read_text(encoding="utf-8")
        bodies = "\n".join(function(source, signature) for signature in (
            "void LiveSessionView::capture_memory_capacity(",
            "void LiveSessionView::capture_memory_capacity_600(",
            "void LiveSessionView::capture_memory_capacity_post_load(",
            "std::string LiveSessionView::memory_capacity_snapshot("))
        report = (ROOT / "apps/viewer/src/live_session_report.cpp").read_text(encoding="utf-8")
        guards = "\n".join(function(report, "if (!" + member + ".empty())")
                           if "if (!" + member + ".empty()) {" in report else
                           report[report.index("if (!" + member + ".empty())"):
                                  report.index(";", report.index("if (!" + member + ".empty())")) + 1]
                           for member in ("memory_capacity_report_", "memory_capacity_600_report_",
                                          "memory_capacity_post_load_report_"))
        fixture = r'''
#include "eawr/diagnostics/owner_census.hpp"
#include "eawr/diagnostics/memory_capacity.hpp"
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <new>
#include <set>
#include <sstream>
namespace diagnostics = eawr::diagnostics;
using Census = diagnostics::OwnerCensus;
unsigned inventory_work = 0, owner_work = 0, callback_work = 0;
unsigned allocations = 0;
void* operator new(std::size_t size) {
    ++allocations;
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
struct Boundary {
    bool paused() const { return true; }
    std::uint64_t completed_tick() const { return 0; }
    diagnostics::MemoryCapacity memory_capacity() const { ++inventory_work; return {}; }
    void owner_census(Census&) const { ++owner_work; }
};
using BattleScene = Boundary;
struct Triangles {
    std::vector<int> data;
    bool empty() const { return data.empty(); }
    const auto& vertices() const { return data; }
    std::uint64_t capacity_bytes() const { return data.capacity() * sizeof(int); }
    std::size_t size() const { return data.size(); }
};
struct Mesh { Triangles triangles; };
struct Unit { std::vector<Mesh> collision_meshes; };
struct Tables { std::vector<Unit> units; };
struct CombatMesh { std::vector<int> triangles, nodes; };
struct Profile {
    std::vector<CombatMesh> meshes;
    std::vector<int> weapons, target_bones, hardpoints, aimed_routes, hardpoint_meshes, death_projectiles;
};
struct Content { struct { std::vector<Profile> profiles, retaliation_team_types; } combat; };
struct Pose { std::vector<int> hardpoints, hardpoint_turrets; };
struct Fog { std::vector<std::shared_ptr<std::vector<int>>> values; };
struct LiveSessionView {
    std::function<diagnostics::MemoryCapacity()> memory_capacity_probe_;
    std::function<void(Census&)> owner_census_probe_;
    std::string memory_capacity_report_, memory_capacity_600_report_, memory_capacity_post_load_report_;
    double presented_tick_ = 1200;
    struct { bool borrowed_feed = false; } options_;
    Boundary* session_ = nullptr;
    Boundary* projectile_models_ = nullptr;
    struct { Boundary* latest = nullptr; Fog* fog = nullptr; } battle_frame_;
    Tables* tables_ = nullptr;
    Content* content_ = nullptr;
    std::vector<int> placed_ships_, session_records_, unfogged_map_props_, draw_visibility_, launch_slots_,
        placement_clones_, visible_, poses_, clip_poses_, fog_immediate_, fog_observations_,
        fog_ghost_instances_, launch_clones_, active_clones_, retiring_clones_;
    std::vector<Pose> live_poses_;
    void capture_memory_capacity(const BattleScene&);
    void capture_memory_capacity_600(const BattleScene&);
    void capture_memory_capacity_post_load(const BattleScene&);
    std::string memory_capacity_snapshot(const BattleScene&, const char*, double, std::uint64_t, bool) const;
    std::string report() const {
        std::ostringstream output;
        output << "{\"existing\":true";
''' + guards + r'''
        output << '}'; return output.str();
    }
};
''' + bodies + r'''
int main() {
    Boundary population, session;
    LiveSessionView view;
    view.session_ = &session;
    // This is the normal flag-off registration state. Every capture is eligible
    // by tick/session; only the actual production opt-in guard prevents work.
    view.owner_census_probe_ = [](Census&) { ++callback_work; };
    const auto before_off = allocations;
    view.capture_memory_capacity_post_load(population);
    view.capture_memory_capacity_600(population);
    view.capture_memory_capacity(population);
    const auto off_allocations = allocations - before_off;
    std::cout << "{\"off\":" << view.report() << ",\"off_work\":"
        << inventory_work + owner_work + callback_work << ",\"off_allocations\":" << off_allocations;
    view.memory_capacity_probe_ = [] { ++inventory_work; return diagnostics::MemoryCapacity{}; };
    view.capture_memory_capacity_600(population);
    view.capture_memory_capacity_600(population);
    std::cout << ",\"on\":" << view.report() << ",\"owner_work\":" << owner_work
        << ",\"callback_work\":" << callback_work;
    const auto before = owner_work + callback_work;
    std::cout << ",\"inventory_only\":" << view.memory_capacity_snapshot(population, "test", 1200, 1200, false)
        << ",\"inventory_owner_work\":" << owner_work + callback_work - before << "}\n";
}
'''
        with tempfile.TemporaryDirectory(prefix="owner-snapshot-") as directory:
            cpp = Path(directory) / "snapshot.cpp"
            cpp.write_text(fixture, encoding="utf-8")
            executable = Path(directory) / "snapshot.exe"
            toolchain = select_toolchain()
            if toolchain is None:
                raise RuntimeError("Owner snapshot contract requires a native compiler")
            key = "INCLUDE" if toolchain.msvc else "CPLUS_INCLUDE_PATH"
            toolchain.environment[key] = str(ROOT / "include") + os.pathsep + toolchain.environment.get(key, "")
            compile_fixture(toolchain, cpp, executable)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=30)
            cls.evidence = json.loads(result.stdout)

    def test_unregistered_probe_emits_no_snapshot_or_owner_census_and_does_no_work(self):
        self.assertEqual(self.evidence["off"], {"existing": True})
        self.assertNotIn("owner_census", json.dumps(self.evidence["off"]))
        self.assertEqual(self.evidence["off_work"], 0)
        self.assertEqual(self.evidence["off_allocations"], 0)

    def test_full_snapshot_positive_control_and_inventory_only_serialization(self):
        snapshot = self.evidence["on"]["memory_capacity_tick_600"]
        self.assertIn("owner_census", snapshot)
        self.assertTrue(snapshot["owner_census"]["complete"])
        self.assertEqual(self.evidence["owner_work"], 1)
        self.assertEqual(self.evidence["callback_work"], 1)
        self.assertNotIn("owner_census", self.evidence["inventory_only"])
        self.assertEqual(self.evidence["inventory_owner_work"], 0)


if __name__ == "__main__":
    unittest.main()
