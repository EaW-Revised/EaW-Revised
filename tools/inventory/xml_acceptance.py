#!/usr/bin/env python3
"""Versioned, metadata-only P0-05 catalog acceptance evaluator.

The detailed scanner report and receipt are local inputs.  This tool never writes
them or original XML to the repository. A profile_p0_pass covers one profile's
catalog contract; project-wide full_pass is the conjunction of all three profiles
and belongs to the pinned summary, not to a single-profile evaluation.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

VERSION = 2
PROFILES = ("eaw", "foc", "remake")
CATEGORIES = ("abilities", "campaigns", "factions", "game_objects", "hardpoints", "sfx")
ROOTS = (
    ("game_objects", "data/xml/gameobjectfiles.xml"),
    ("hardpoints", "data/xml/hardpointdatafiles.xml"),
    ("factions", "data/xml/factionfiles.xml"),
    ("campaigns", "data/xml/campaignfiles.xml"),
    ("sfx", "data/xml/sfxeventfiles.xml"),
)
DIAGNOSTIC_CODES = {
    "malformed": "EAWR-XML-0001",
    "missing_registry_include": "EAWR-XML-0004",
    "duplicate_id": "EAWR-XML-0006",
    "unknown": "EAWR-XML-0010",
    "deprecated": "EAWR-XML-0011",
}
HEX = set("0123456789abcdef")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(source_id: str) -> str:
    """Bind VFS source metadata without echoing a possible local host path."""
    return hashlib.sha256(source_id.encode("utf-8")).hexdigest()


def aggregate_p0_pass(profile_results: list[dict]) -> bool:
    """Require all three evaluated profiles before computing project P0 status."""
    require(len(profile_results) == len(PROFILES) and
            {row["profile"] for row in profile_results} == set(PROFILES),
            "project P0 status requires one evaluation of each profile")
    return all(row["profile_p0_pass"] for row in profile_results)


def identity(receipt: dict) -> str:
    parts: list[bytes] = []

    def field(value: str) -> None:
        encoded = value.encode("utf-8")
        parts.append(str(len(encoded)).encode("ascii") + b":" + encoded)

    field("eawr-xml-input-receipt-v1")
    field(receipt["profile"])
    field(receipt["schema_revision"])
    for kind, rows in (("root", receipt["roots"]), ("include", receipt["includes"])):
        for row in rows:
            for value in (kind, row["category"], row["logical_path"],
                          str(row["include_order"]) if kind == "include" else "", row["outcome"],
                          "1" if row["source_id"] is not None else "0", row["source_id"] or "",
                          row["layer"] or "", "1" if row["sha256"] is not None else "0",
                          row["sha256"] or ""):
                field(value)
            if kind == "include":
                field(row["registry_path"])
    return hashlib.sha256(b"".join(parts)).hexdigest()


def evaluate(profile: str, report: dict, receipt: dict, baseline: dict) -> dict:
    """Raise on any unexplained row, count, identity, or exception change."""
    require(profile in PROFILES, "unknown requested profile")
    require(report.get("schema_version") == receipt.get("schema_version") == 1,
            "scanner/receipt version mismatch")
    require(report.get("profile") == receipt.get("profile") == profile, "wrong profile")
    require(report.get("schema_revision") == receipt.get("schema_revision") == baseline["schema_revision"],
            "wrong schema or receipt identity")
    require(receipt.get("identity_algorithm") == "sha256-length-framed-v1" and
            receipt.get("identity_sha256") == identity(receipt), "wrong receipt identity")
    expected = next((row for row in baseline["profiles"] if row["profile"] == profile), None)
    require(expected is not None, "profile absent from baseline")
    require(receipt["identity_sha256"] == expected["input_receipt_identity_sha256"],
            "receipt differs from pinned corpus identity")
    require(baseline["evaluator_version"] == VERSION, "wrong evaluator version")
    require({row["profile"] for row in baseline["profiles"]} == set(PROFILES) and
            len(baseline["profiles"]) == len(PROFILES), "project baseline profiles incomplete")
    require(baseline["full_pass"] == aggregate_p0_pass(baseline["profiles"]),
            "project P0 status disagrees with profile dispositions")
    roots = receipt["roots"]
    includes = receipt["includes"]
    require([(r["category"], r["logical_path"]) for r in roots] == list(ROOTS),
            "missing, reordered, or unexpected registry root")
    require(len(includes) == len(report["registries"]) == report["counts"]["registry_includes"],
            "ordered include count mismatch")
    require(len(includes) == expected["registry_includes"], "include count drift")
    require(len(report["physical_inventory"]) == report["counts"]["physical_xml_records"],
            "physical inventory count mismatch")
    require(len(report["diagnostics"]) == report["counts"]["diagnostics"] and
            len(report["unresolved"]) == report["counts"]["unresolved"],
            "failure or diagnostic rows omitted")
    require(set(report["counts"]["by_category"]) == set(CATEGORIES), "six catalog categories incomplete")
    require(sum(x["raw"] for x in report["counts"]["by_category"].values()) ==
            report["counts"]["definitions"], "category totals differ from definitions")
    for category in CATEGORIES:
        actual = report["counts"]["by_category"][category]
        require(actual == expected["categories"][category] and
                0 < actual["winners"] <= actual["raw"], f"{category} raw/winner drift")
    for key in ("physical_xml_records", "registry_includes", "definitions", "unresolved"):
        require(report["counts"][key] == expected[key], f"{key} drift")

    physical = defaultdict(list)
    for row in report["physical_inventory"]:
        physical[(row["logical_path"].lower(), row["source_id"])].append(row)
    diagnostics = report["diagnostics"]
    diagnostic_paths = defaultdict(list)
    for row in diagnostics:
        diagnostic_paths[(row["code"], (row["logical_path"] or "").lower())].append(row)

    def check_attempt(row: dict, *, root: bool) -> None:
        outcome = row["outcome"]
        require(outcome in ("loaded", "missing", "read_error", "parse_error"), "unknown input outcome")
        digest = row["sha256"]
        require((digest is None or isinstance(digest, str) and len(digest) == 64 and
                 set(digest) <= HEX), "invalid parsed input hash")
        require((row["source_id"] is None) == (row["layer"] is None), "incomplete VFS winner")
        if outcome in ("loaded", "parse_error"):
            require(digest is not None and row["source_id"] is not None, "readable attempt lost hash or winner")
        else:
            require(digest is None, "unreadable attempt has invented hash")
        if row["source_id"] is not None:
            matching = physical[(row["logical_path"].lower(), row["source_id"])]
            require(len(matching) == 1 and matching[0]["active_registry_file"] and
                    matching[0]["layer"] == row["layer"], "receipt winner is not active VFS record")
            if outcome == "loaded":
                require(matching[0]["parsed"], "loaded receipt winner is malformed")
            if outcome == "parse_error":
                require(not matching[0]["parsed"], "parse failure disagrees with physical inventory")
        if outcome != "loaded":
            code = "EAWR-XML-0003" if root else (
                "EAWR-XML-0001" if outcome == "parse_error" else "EAWR-XML-0004")
            require(diagnostic_paths[(code, row["logical_path"].lower())],
                    "failed root/include attempt lacks diagnostic")

    for row in roots:
        check_attempt(row, root=True)
        require(row["sha256"] == baseline["registry_contract"]["registry_file_sha256"][profile][row["category"]],
                "registry XML hash differs from pinned input")
    orders = defaultdict(int)
    for row, recorded in zip(includes, report["registries"]):
        require(row["category"] in dict(ROOTS) and
                row["registry_path"] == dict(ROOTS)[row["category"]] and
                row["include_order"] == orders[row["category"]], "include order or registry mismatch")
        orders[row["category"]] += 1
        require(all((row["category"] == recorded["category"],
                     row["logical_path"].lower() == recorded["included_path"].lower(),
                     row["registry_path"] == recorded["registry_path"],
                     row["include_order"] == recorded["include_order"],
                     (row["outcome"] == "loaded") == recorded["loaded"],
                     row["source_id"] == recorded["source_id"])),
                "receipt does not cover each ordered scanner include")
        check_attempt(row, root=False)
    require(all(row["outcome"] == "loaded" for row in roots), "missing or malformed required root")
    require(all(row["outcome"] != "parse_error" for row in includes), "active malformed include")

    code_counts = Counter(d["code"] for d in diagnostics)
    for label, code in DIAGNOSTIC_CODES.items():
        require(code_counts[code] == expected["diagnostics"][label], f"{label} diagnostic count drift")
    require(set(code_counts) <= set(DIAGNOSTIC_CODES.values()), "unclassified diagnostics")
    malformed = {(d["logical_path"], d["line"]) for d in diagnostics
                 if d["code"] == DIAGNOSTIC_CODES["malformed"]}
    require(malformed == {(r["logical_path"], r["line"])
                          for r in baseline["malformed_physical_xml"][profile]},
            "malformed physical XML rows changed or were omitted")
    require(all(not any(p["active_registry_file"] for p in physical[(path.lower(), d["source_id"])])
                for d in diagnostics if d["code"] == DIAGNOSTIC_CODES["malformed"]
                for path in [d["logical_path"]]), "active malformed XML")
    missing_rows = [r for r in includes if r["outcome"] == "missing"]
    missing = sorted(r["logical_path"].lower() for r in missing_rows)
    require(missing == (baseline["required_failures"]["eaw_missing_registry_includes"]
                        if profile == "eaw" else []), "missing include set broadened or omitted")
    require(sorted(d["logical_path"] for d in diagnostics
                   if d["code"] == DIAGNOSTIC_CODES["missing_registry_include"]) == missing,
            "missing include failure rows omitted")
    missing_dispositions = []
    for row in missing_rows:
        path = row["logical_path"].lower()
        joined = diagnostic_paths[(DIAGNOSTIC_CODES["missing_registry_include"], path)]
        require(len(joined) == 1 and joined[0]["source_id"] is None and
                joined[0]["severity"] == "error", "missing include diagnostic disagrees with receipt")
        root = next(r for r in roots if r["category"] == row["category"])
        missing_dispositions.append({
            "logical_path": path,
            "disposition": "required_include_missing_blocks_p0",
            "registry_path": row["registry_path"],
            "include_order": row["include_order"],
            "registry_sha256": root["sha256"],
            "registry_source_id_sha256": source_digest(root["source_id"]),
            "receipt_outcome": row["outcome"],
            "diagnostic_code": joined[0]["code"],
        })
    require(not any(r["outcome"] == "read_error" for r in includes), "unexplained include read failure")

    unresolved = report["unresolved"]
    approved_cycles = []
    if profile == "remake":
        exception = baseline["required_failures"]["remake_variant_cycles"]
        require(len(exception) == len(unresolved) == 1, "Remake exception broadened")
        item = exception[0]
        failure = unresolved[0]
        require(failure["code"] == "EAWR-XML-0009" and failure["severity"] == "error" and
                failure["logical_path"] == item["logical_path"] and failure["line"] == item["line"] and
                failure["message"] == "variant cycle: " + " -> ".join(item["chain"]),
                "unapproved Remake resolution exception")
        winners = [r for r in includes if r["logical_path"].lower() == item["logical_path"] and
                   r["source_id"] == failure["source_id"] and
                   r["sha256"] == item["input_sha256"] and r["outcome"] == "loaded"]
        require(len(winners) == 1,
                "Remake exception is not bound to exact parsed winner")
        winner = winners[0]
        root = next(r for r in roots if r["category"] == winner["category"])
        approved_cycles.append({
            "logical_path": item["logical_path"],
            "line": item["line"],
            "chain": item["chain"],
            "disposition": "approved_p0_only_source_data_exception",
            "registry_path": winner["registry_path"],
            "include_order": winner["include_order"],
            "registry_sha256": root["sha256"],
            "source_id_sha256": source_digest(winner["source_id"]),
            "input_sha256": winner["sha256"],
            "receipt_outcome": winner["outcome"],
            "diagnostic_code": failure["code"],
        })
    else:
        require(not unresolved, "unapproved resolution failure")
    profile_p0_pass = not missing
    require(profile_p0_pass == expected["profile_p0_pass"], "profile P0 disposition drift")
    return {"evaluator_version": VERSION, "profile": profile, "receipt_identity_sha256":
            receipt["identity_sha256"], "categories": report["counts"]["by_category"],
            "roots": len(roots), "includes": len(includes), "missing_includes": len(missing),
            "malformed_inactive": len(malformed), "unresolved": len(unresolved),
            "profile_p0_pass": profile_p0_pass,
            "missing_include_dispositions": missing_dispositions,
            "approved_cycle_dispositions": approved_cycles}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--summary", type=Path, default=Path("plan/inventories/xml-load-summary.json"))
    args = parser.parse_args()
    try:
        baseline = json.loads(args.summary.read_text(encoding="utf-8"))
        report = json.loads(args.report.read_text(encoding="utf-8"))
        receipt = json.loads(args.receipt.read_text(encoding="utf-8"))
        result = evaluate(args.profile, report, receipt, baseline)
        result["report_sha256"] = sha256(args.report)
        expected = next(row for row in baseline["profiles"] if row["profile"] == args.profile)
        require(result["report_sha256"] == expected["report_sha256"],
                "scanner report differs from pinned corpus report")
        print(json.dumps(result, sort_keys=True))
        return 0
    except (ValueError, KeyError, TypeError, IndexError, OSError) as exc:
        parser.exit(1, f"P0-05 acceptance rejected: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
