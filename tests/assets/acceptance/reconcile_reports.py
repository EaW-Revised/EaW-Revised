#!/usr/bin/env python3
"""Independent metadata-only audit for repaired P0-06 corpus reports.

This checker never opens an asset payload.  It verifies the frozen report
schema, row-level identity/provenance, mechanical aggregates, raw Remake
baseline and the metadata-only inventory projection.  The report directories
are supplied explicitly so a clean checkout cannot accidentally use an
ignored local installation or silently pass with an empty corpus.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


COUNT_KEYS = (
    "meshes", "bones", "materials", "animations",
    "vertices", "indices", "mips", "samples",
)
REPORT_NAMES = {
    ("eaw", "effective"): "eaw-effective.json",
    ("foc", "effective"): "foc-effective.json",
    ("remake", "effective"): "remake-effective.json",
    ("remake", "raw"): "remake-raw.json",
}
HASH_RE = re.compile(r"^[0-9a-f]{64}$")


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def aggregate(rows: list[dict]) -> dict:
    return {
        "discovered": len(rows),
        "loaded": sum(row["outcome"] == "loaded" for row in rows),
        "failed": sum(row["outcome"] == "failed" for row in rows),
        "notices": sum(row["notices"] for row in rows),
        "counts": {
            key: sum(row["counts"][key] for row in rows) for key in COUNT_KEYS
        },
    }


def audit_report(path: Path, expected_profile: str, expected_view: str) -> tuple[dict, list[dict]]:
    report = load(path)
    assert report["schema_version"] == 2, path
    assert (report["profile"], report["view"]) == (expected_profile, expected_view), path
    rows = report["records"]
    assert rows, f"empty corpus report: {path}"
    assert [row["logical_path"] for row in rows] == sorted(row["logical_path"] for row in rows), path
    for row in rows:
        assert row["format"] in {"alo", "ala", "dds", "tga"}, row
        assert row["outcome"] in {"loaded", "failed"}, row
        assert HASH_RE.fullmatch(row["content_sha256"]), row
        assert set(row["counts"]) == set(COUNT_KEYS), row
        assert all(isinstance(row["counts"][key], int) and row["counts"][key] >= 0 for key in COUNT_KEYS), row
        provenance = row["provenance"]
        assert all(key in provenance for key in ("layer", "origin", "source_id", "stored_size")), row
        assert isinstance(provenance["stored_size"], int) and provenance["stored_size"] >= 0, row
        if row["outcome"] == "failed":
            failure = row["failure"]
            assert all(failure.get(key) for key in ("code", "cause", "affected_feature", "follow_up")), row
            assert isinstance(failure.get("byte_offset"), int) and failure["byte_offset"] >= 0, row
        else:
            assert row.get("failure") is None, row
    assert report["total"] == aggregate(rows), path
    for format_name, summary in report["aggregates"].items():
        assert summary == aggregate([row for row in rows if row["format"] == format_name]), (path, format_name)
    return report, rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reports", required=True, type=Path, help="directory containing four report JSON files")
    parser.add_argument("--repeat-reports", required=True, type=Path, help="second complete report generation")
    parser.add_argument("--inventory-summary", required=True, type=Path)
    parser.add_argument("--inventory-failures", required=True, type=Path)
    args = parser.parse_args()

    all_failures: list[dict] = []
    reports: dict[tuple[str, str], dict] = {}
    for identity, name in REPORT_NAMES.items():
        report, rows = audit_report(args.reports / name, *identity)
        repeat = args.repeat_reports / name
        assert (args.reports / name).read_bytes() == repeat.read_bytes(), name
        reports[identity] = report
        for row in rows:
            if row["outcome"] == "failed":
                provenance = row["provenance"]
                all_failures.append({
                    "profile": report["profile"], "view": report["view"],
                    "path": row["logical_path"], "format": row["format"],
                    "source_id": provenance["source_id"],
                    "content_sha256": row["content_sha256"],
                })

    raw = reports[("remake", "raw")]
    assert raw["aggregates"]["alo"]["discovered"] == 4140
    assert raw["aggregates"]["ala"]["discovered"] == 5603
    assert raw["aggregates"]["dds"]["discovered"] == 0
    assert raw["aggregates"]["tga"]["discovered"] == 0

    summary = load(args.inventory_summary)
    assert summary["schema_version"] == 2
    assert summary["report_schema_version"] == 2
    assert len(summary["views"]) == len(REPORT_NAMES)
    for view in summary["views"]:
        identity = (view["profile"], view["view"])
        report = reports[identity]
        report_path = args.reports / REPORT_NAMES[identity]
        assert view["report_sha256"] == hashlib.sha256(report_path.read_bytes()).hexdigest()
        assert view["record_count"] == len(report["records"])
        assert view["successful_record_count"] == report["total"]["loaded"]
        assert view["failed_record_count"] == report["total"]["failed"]
        assert view["aggregates"] == report["aggregates"]

    inventory = load(args.inventory_failures)
    assert inventory["schema_version"] == 2
    assert inventory["failure_count"] == len(all_failures)
    inventory_keys = Counter(
        (row["profile"], row["view"], row["path"], row["source_id"], row["content_sha256"])
        for row in inventory["failures"]
    )
    assert inventory_keys == Counter(
        (row["profile"], row["view"], row["path"], row["source_id"], row["content_sha256"])
        for row in all_failures
    )
    print("P0-06 report reconciliation passed: 4 reports, raw baseline, inventories, and repeat generation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
