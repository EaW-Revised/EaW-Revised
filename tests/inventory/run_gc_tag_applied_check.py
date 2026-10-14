"""Installed-game GC perturbation contract, including an identity-only negative control."""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify_stock(report, plan):
    require(len(report["rows"]) == len(plan["checks"]), "every GC row must be reported")
    require(report["baseline"]["campaign_count"] >= 30 and report["baseline"]["deterministic"], "stock baselines must run deterministically")
    require(any(r["verdict"] == "changes" for r in report["rows"]), "stock run must exercise a consumer")
    require(any(c.get("baseline_reused") for r in report["rows"] for c in r.get("campaigns", [])), "reuse must be observable")


def verify_smoke(report):
    require(report["baseline"]["deterministic"] and report["baseline"]["campaign_count"] >= 30, "stock baselines must run deterministically")
    rows = {r["tag"]: r for r in report["rows"]}
    for tag in ("Planet/Galactic_Position", "Campaign/Starting_Credits"):
        row = rows[tag]
        require(row["verdict"] == "changes", f"{tag}: {row['reason']}")
        require(row["hash_changed"] and row["facts_changed"], f"{tag}: state and snapshot must change")
        require(row["first_tick"] == 0, f"{tag}: setup must change at tick zero")
    verify_route(rows["TradeRoute/HS_Speed_Factor"])
    control = rows["Campaign/Description_Text"]
    require(control["verdict"] == "no change", "description must remain an identity-only negative control")
    require(control["finding"] == "no change on an applied type", "unchanged applied control must be a finding")
    require(not control["hash_changed"] and not control["facts_changed"], "description must not change state or snapshot")
    require(control["campaigns"] and all(c.get("baseline_reused") for c in control["campaigns"]), "description runs must reuse identical normalized setups")


def verify_route(row):
    # Retail can author duplicate route Names in one file. The overlay deliberately
    # refuses that ambiguity rather than claiming a partial class perturbation.
    if row["verdict"] == "not reachable":
        require(re.fullmatch(r".+: Name selector matched (?:[2-9]|[1-9][0-9]+) nodes; expected exactly one", row["reason"]),
                f"route refusal must identify duplicate authored Names: {row['reason']}")
        require(not row["hash_changed"] and not row["facts_changed"] and not row["campaigns"],
                "refused route overlays must not claim behavioral evidence")
        return
    require(row["verdict"] == "changes" and row["hash_changed"] and row["facts_changed"] and row["first_tick"] == 0,
            "unambiguous routes must change state and snapshot at tick zero")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--stock", action="store_true", help="bounded full registry audit instead of the smoke contract")
    args = parser.parse_args()
    if not os.environ.get("EAWR_EAW_GAME_ROOT"):
        print("SKIPPED: GC perturbation requires EAWR_EAW_GAME_ROOT")
        return 0
    args.work.mkdir(parents=True, exist_ok=True)
    if args.stock:
        code = subprocess.run([sys.executable, str(ROOT / "tools/inventory/tag_applied_check.py"), "run",
                               "--registry", str(ROOT / "docs/tag-coverage/statuses-galactic.json"),
                               "--binary", args.binary, "--out", str(args.work), "--ticks", "120", "--jobs", "1",
                               "--check-workers", "1", "--max-seconds", "600"], check=False, timeout=720).returncode
        if code:
            return code
        report = json.loads((args.work / "report.json").read_text())
        plan = json.loads((args.work / "plan.json").read_text())
        verify_stock(report, plan)
        return 0
    registry = {"note": "Perturbation scene: galactic.", "rows": [
        {"classes": [element], "tag": tag, "status": "applied", "area": "economy"}
        for element, tag in (("Planet", "Galactic_Position"), ("TradeRoute", "HS_Speed_Factor"),
                             ("Campaign", "Starting_Credits"), ("Campaign", "Description_Text"))]}
    path = args.work / "registry.json"
    path.write_text(json.dumps(registry), encoding="utf-8")
    completed = subprocess.run([sys.executable, str(ROOT / "tools/inventory/tag_applied_check.py"), "run",
                                "--registry", str(path), "--binary", args.binary, "--out", str(args.work),
                                "--ticks", "3", "--jobs", "1", "--check-workers", "1", "--max-seconds", "180"],
                               check=False, timeout=240)
    if completed.returncode:
        return 1
    report = json.loads((args.work / "report.json").read_text())
    verify_smoke(report)
    require((args.work / "findings.md").is_file(), "findings artifact must exist")
    print("GC perturbation: stock positions/credits change; routes change or explicitly refuse duplicate Names; description identity does not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
