"""Every unbound Lua name in the scan baseline has a planned row (plan/lua-gap-plan.json), and no row is stale.

The scan's own --check-baseline fails on a NEW unbound call. This test makes the existing ones owned: each
(host, kind, name) in tools/lua_api_scan/baseline.json sits in exactly one planned family row with an owner lane
and a work order. A binding fix that drops a name from the baseline drops it from its row in the same change.
"""
from __future__ import annotations

import json
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "tools/lua_api_scan/baseline.json"
PLAN = ROOT / "plan/lua-gap-plan.json"
CONFIG = ROOT / "tools/lua_api_scan/scan_config.json"
SEVERITY_RANK = {"critical": 0, "high": 1}


def baseline_triples() -> set[tuple[str, str, str]]:
    keys = json.loads(BASELINE.read_text(encoding="utf-8"))["unbound"]
    return {tuple(k.split("|", 3)[1:]) for k in keys}


class LuaGapPlanTests(unittest.TestCase):
    def setUp(self):
        self.plan = json.loads(PLAN.read_text(encoding="utf-8"))
        self.rows = self.plan["rows"]
        self.planned = Counter((r["host"], r["kind"], n) for r in self.rows for n in r["names"])

    def test_every_baseline_name_has_a_planned_row(self):
        missing = sorted(baseline_triples() - set(self.planned))
        self.assertEqual([], missing, "baseline names without a row in plan/lua-gap-plan.json (add them to a family)")

    def test_no_planned_name_left_the_baseline(self):
        stale = sorted(set(self.planned) - baseline_triples())
        self.assertEqual([], stale, "planned names no longer in the baseline (drop them from their row)")

    def test_each_name_is_planned_once(self):
        self.assertEqual([], sorted(t for t, c in self.planned.items() if c > 1))

    def test_rows_are_well_formed(self):
        hosts = {h["id"]: h["lane"] for h in json.loads(CONFIG.read_text(encoding="utf-8"))["hosts"]}
        ids = [r["id"] for r in self.rows]
        self.assertEqual(len(ids), len(set(ids)), "duplicate row id")
        orders: dict[str, list[int]] = {}
        for r in self.rows:
            self.assertIn(r["host"], hosts, r["id"])
            self.assertEqual(hosts[r["host"]], r["owner_lane"], r["id"])
            self.assertIn(r["kind"], ("global", "member"), r["id"])
            self.assertTrue(r["family"] and r["names"], r["id"])
            self.assertEqual(sorted(r["names"]), r["names"], r["id"])
            self.assertIn(r["severity"], SEVERITY_RANK, r["id"])
            orders.setdefault(r["owner_lane"], []).append(r["order"])
        for lane, found in orders.items():
            self.assertEqual(list(range(1, len(found) + 1)), sorted(found), f"order of lane {lane} is not 1..n")
            lane_rows = sorted((r for r in self.rows if r["owner_lane"] == lane), key=lambda r: r["order"])
            keys = [(SEVERITY_RANK[r["severity"]], -r["score"]) for r in lane_rows]
            self.assertEqual(sorted(keys), keys, f"lane {lane} is not ordered by severity, then score")


if __name__ == "__main__":
    unittest.main()
