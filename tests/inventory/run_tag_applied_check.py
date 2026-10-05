#!/usr/bin/env python3
"""The tag perturbation check end to end on the FoC data (docs/tag-applied-check.md).

Runs tag_applied_check.py on a small registry: ship Max_Speed, Tech_Level (PL-20),
SpaceBehavior and MaxRotationsSpace must change the M2 battle. An unread
Score_Cost_Credits must be a finding, and presentation rows are not checkable headless.
Prints SKIPPED: without EAWR_EAW_GAME_ROOT.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = {
    ("SpaceUnit/Max_Speed", "ship"): ("changes", None),
    ("GameConstants/MaxRotationsSpace", "constants"): ("changes", None),
    ("SpaceUnit/Score_Cost_Credits", "ship"): ("no change", "no change on an applied type"),
    ("Faction/Color", "faction"): ("not checkable", None),
    ("GameConstants/AI_SpaceAreaThreatScaleFactor", "constants"): ("not checkable", None),
    ("SpaceUnit/Behavior", "ship"): ("not checkable", None),
    ("SpaceUnit/Damage_Type", "ship"): ("no change", "no change on an applied type"),
    ("SpaceUnit/Tech_Level", "ship"): ("changes", None),
    ("SpaceUnit/SpaceBehavior", "ship"): ("changes", None),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    if not os.environ.get("EAWR_EAW_GAME_ROOT"):
        print("SKIPPED: set EAWR_EAW_GAME_ROOT to a FoC installation")
        return 0
    completed = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "inventory" / "tag_applied_check.py"), "run",
         "--registry", str(ROOT / "tests" / "inventory" / "fixtures" / "tag-applied-smoke.json"),
         "--binary", args.binary, "--out", str(args.work), "--ticks", "450", "--jobs", "2", "--check-every", "1"],
        capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    sys.stdout.write(completed.stdout)
    sys.stdout.write(completed.stderr)
    if completed.returncode != 0:
        print(f"FAIL: tag_applied_check.py run exit {completed.returncode}")
        return 1
    report = json.loads((args.work / "report.json").read_text(encoding="utf-8"))
    failures = []
    seen = {(row["tag"], row["type"]): row for row in report["rows"]}
    for key, (verdict, finding) in EXPECTED.items():
        row = seen.get(key)
        if row is None:
            failures.append(f"{key}: not in the report")
        elif (row["verdict"], row["finding"]) != (verdict, finding):
            failures.append(f"{key}: {row['verdict']} / {row['finding']} ({row['reason']}), expected {verdict} / {finding}")
    tech = seen[("SpaceUnit/Tech_Level", "ship")]
    if not tech.get("tables_changed") or tech["evidence"] != "proven":
        failures.append("Tech_Level must change full tables and prove its PL-20 battle consumer")
    unauthored = seen[("SpaceUnit/Damage_Type", "ship")]
    if not unauthored["reason"].startswith("not authored; read unknown") or unauthored["evidence"] != "unproven":
        failures.append("unauthored Damage_Type must not claim a baseline read")
    if not report["baseline"].get("deterministic"):
        failures.append("the baseline is not deterministic")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    print("tag perturbation check: applied rows change the battle, an unread one is a finding")
    return 0


if __name__ == "__main__":
    sys.exit(main())
