"""plan/inventories/lua-engine-surface.json stays inside the engine registration index and lists every name once."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lua_api_scan import bindings, scan  # noqa: E402


class LuaEngineSurfaceTests(unittest.TestCase):
    def setUp(self):
        self.surface = json.loads((ROOT / "plan/inventories/lua-engine-surface.json").read_text(encoding="utf-8"))
        entries = json.loads((ROOT / "plan/inventories/foc-lua-registrations.json").read_text(encoding="utf-8"))["entries"]
        self.globals = {e["exposed_name"] for e in entries if e["call_kind"] == "global"}
        self.members = {(e["receiver_type"], e["exposed_name"]) for e in entries if e["call_kind"] != "global"}

    def test_every_listed_name_is_registered(self):
        for row in self.surface["never_called_globals"]:
            self.assertIn(row["name"], self.globals)
        for row in self.surface["never_called_members"]:
            self.assertIn((row["receiver"], row["name"]), self.members)

    def test_no_name_is_listed_twice(self):
        names = [r["name"] for r in self.surface["never_called_globals"]]
        self.assertEqual(len(names), len(set(names)))
        members = [(r["receiver"], r["name"]) for r in self.surface["never_called_members"]]
        self.assertEqual(len(members), len(set(members)))

    def baseline_names(self):
        keys = json.loads((ROOT / "tools/lua_api_scan/baseline.json").read_text(encoding="utf-8"))["unbound"]
        globals_, members = set(), set()
        for key in keys:
            _profile, _host, kind, name = key.split("|", 3)
            (globals_ if kind == "global" else members).add(name.split(".")[0] if kind == "global" else name)
        return globals_, members

    def test_baseline_names_are_on_the_engine_surface(self):
        base_globals, base_members = self.baseline_names()
        self.assertEqual([], sorted(base_globals - self.globals), "baseline globals the engine surface lacks")
        self.assertEqual([], sorted(base_members - {n for _r, n in self.members}), "baseline members the engine surface lacks")

    def test_never_called_names_are_not_called_gaps(self):
        base_globals, base_members = self.baseline_names()
        listed_globals = {r["name"] for r in self.surface["never_called_globals"]}
        listed_members = {r["name"] for r in self.surface["never_called_members"]}
        self.assertEqual([], sorted(listed_globals & base_globals), "listed as never called but a script calls it unbound")
        self.assertEqual([], sorted(listed_members & base_members), "listed as never called but a script calls it unbound")

    def test_bound_in_matches_the_host_binding_tables(self):
        config = scan.load_config(ROOT / "tools/lua_api_scan/scan_config.json")
        host_bind = bindings.read_all(ROOT, config["hosts"], config.get("follow"))

        def bound(kind, name):
            found = []
            for host, b in host_bind.items():
                ok = b.knows_global(name) if kind == "global" else b.knows_member(name)
                if ok and name not in b.stubs:
                    found.append(host)
            return sorted(found)

        for row in self.surface["never_called_globals"]:
            self.assertEqual(bound("global", row["name"]), sorted(row["bound_in"]), row["name"])
        for row in self.surface["never_called_members"]:
            self.assertEqual(bound("member", row["name"]), sorted(row["bound_in"]), row["name"])


if __name__ == "__main__":
    unittest.main()
