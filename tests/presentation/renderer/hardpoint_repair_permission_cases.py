"""Execute the viewer repair admission against small snapshot/type fixtures.

The production method is compiled verbatim without Godot; the GPU case covers
the real selection, input scheduler and service together.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain

SOURCE = ROOT / "apps/viewer/src/live_session_economy.cpp"

HARNESS = r"""
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <vector>
namespace sim { using EntityId = std::uint64_t; }
namespace tactical { struct StationMenu { int station; }; }
struct Health {
    int value;
    int raw() const { return value; }
    bool operator<(Health other) const { return value < other.value; }
};
struct Slot { Health health{50}, max_health{100}; std::vector<int> repairing_players; bool enabled = true; };
struct Durability { std::vector<Slot> hardpoints{Slot{}}; };
struct Instance { int owner = 1, type_id = 10; std::optional<Durability> durability{Durability{}}; };
struct Type { bool station_community_property = true; };
struct Index {
    Instance value;
    const Instance* instance(sim::EntityId id) const { return id == 1 ? &value : nullptr; }
};
struct LiveSessionView {
    Index snapshot_index_;
    int player_ = 1;
    bool has_account = true;
    Type type;
    std::map<int, const Type*> presentation_types_{{10, &type}};
    struct Economy { std::vector<tactical::StationMenu> menus{{10}}; } economy_;
    const void* local_economy() const { return has_account ? this : nullptr; }
    bool is_ally_of_local(int owner) const { return owner == 1 || owner == 3; }
    bool hardpoint_repair_allowed(sim::EntityId station, std::uint32_t hardpoint) const;
};
__METHOD__
int main() {
    LiveSessionView view;
    auto& instance = view.snapshot_index_.value;
    auto& slot = instance.durability->hardpoints[0];
    auto emit = [&](const char* name) { std::cout << name << '=' << view.hardpoint_repair_allowed(1, 0) << '\n'; };
    emit("own");
    instance.owner = 3; emit("ally_community");
    view.type.station_community_property = false; emit("ally_private");
    view.type.station_community_property = true;
    instance.owner = 2; emit("enemy");
    instance.owner = 0; emit("neutral");
    instance.owner = 3;
    slot.health.value = 100; emit("undamaged");
    slot.health.value = 0; emit("destroyed");
    slot.health.value = 50;
    slot.enabled = false; emit("disabled_positive");
    slot.repairing_players = {1}; emit("already_paying");
    slot.repairing_players = {3}; emit("other_payer");
    view.has_account = false; emit("missing_account");
    view.has_account = true;
    view.presentation_types_.clear(); emit("missing_type_ally");
    instance.owner = 1; emit("missing_type_own");
    view.economy_.menus.clear(); emit("not_station");
    std::cout << "missing_entity=" << view.hardpoint_repair_allowed(2, 0) << '\n';
    std::cout << "invalid_slot=" << view.hardpoint_repair_allowed(1, 1) << '\n';
    instance.durability.reset(); emit("missing_durability");
}
"""


class HardpointRepairPermissionCases:
    def test_selection_and_slot_admission(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest("requires a C++ compiler for the verbatim viewer-method contract")
        source = Path(os.environ.get("EAWR_REPAIR_CONTRACT_SOURCE", SOURCE)).read_text(encoding="utf-8")
        begin = source.index("bool LiveSessionView::hardpoint_repair_allowed(")
        end = source.index("\nbool LiveSessionView::repair_hardpoint(", begin)
        with tempfile.TemporaryDirectory(prefix="eawr-repair-contract-") as temporary:
            directory = Path(temporary)
            cpp = directory / "contract.cpp"
            executable = directory / ("contract.exe" if os.name == "nt" else "contract")
            cpp.write_text(HARNESS.replace("__METHOD__", source[begin:end]), encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        actual = dict(line.split("=") for line in ran.stdout.splitlines())
        allowed = {"own", "ally_community", "disabled_positive", "other_payer", "missing_type_own"}
        expected = {name: "1" if name in allowed else "0" for name in (
            "own", "ally_community", "ally_private", "enemy", "neutral", "undamaged", "destroyed",
            "disabled_positive", "already_paying", "other_payer", "missing_account", "missing_type_ally",
            "missing_type_own", "not_station", "missing_entity", "invalid_slot", "missing_durability")}
        self.assertEqual(actual, expected)

