"""CLI: python -m tools.lua_api_scan --roots ... --out-dir ... (see docs/lua-api-coverage.md)."""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

from . import collect, report, scan

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = Path(__file__).with_name("scan_config.json")
DEFAULT_BASELINE = Path(__file__).with_name("baseline.json")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="lua_api_scan", description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO, help="repository root (binding tables are read from its src/)")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--roots", type=Path, help="local JSON: layer id -> installed directory (stock = the unpacked FoC view)")
    parser.add_argument("--workshop-root", type=Path, help="folder holding one directory per workshop id")
    parser.add_argument("--stock-root", type=Path, help="unpacked stock view (contains data/scripts)")
    parser.add_argument("--profiles", help="comma-separated profile ids (default: every profile with an available root)")
    parser.add_argument("--out-dir", type=Path, help="write matrix.json and matrix.md here (absolute path)")
    parser.add_argument("--markdown-doc", type=Path, help="write the committed summary table here")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--check-baseline", action="store_true", help="exit 1 when an unbound call is not in the baseline")
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--baseline-scope", default="all", choices=["all", "present"],
                        help="'present': only profiles scanned now take part in the comparison (fixture runs)")
    args = parser.parse_args(argv)

    roots_file = args.roots or (Path(os.environ["EAWR_LUA_SCAN_ROOTS"]) if os.environ.get("EAWR_LUA_SCAN_ROOTS") else None)
    config = scan.load_config(args.config)
    profile_rows = collect.load_profiles(args.repo, config)
    layers = {layer for row in profile_rows for layer in row["layers_leaf_first"]}
    roots = collect.resolve_roots(roots_file, args.workshop_root, args.stock_root, layers | {"stock"})
    wanted = [p.strip() for p in args.profiles.split(",")] if args.profiles else None
    start = time.monotonic()
    result = scan.run_scan(args.repo, config, roots, wanted, progress=lambda m: print(m, file=sys.stderr))
    result["elapsed_seconds"] = round(time.monotonic() - start, 1)
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "matrix.json").write_text(json.dumps(result, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        (args.out_dir / "matrix.md").write_text(report.full_markdown(result, config), encoding="utf-8", newline="\n")
        print(f"wrote {args.out_dir / 'matrix.json'} and matrix.md", file=sys.stderr)
    if args.markdown_doc:
        args.markdown_doc.write_text(report.summary_markdown(result, config), encoding="utf-8", newline="\n")
    keys = report.baseline_keys(result)
    status = 0
    if args.update_baseline:
        args.baseline.write_text(json.dumps({"schema_version": 1, "unbound": sorted(keys)}, indent=1) + "\n",
                                 encoding="utf-8", newline="\n")
    elif args.check_baseline:
        known = set(json.loads(args.baseline.read_text(encoding="utf-8"))["unbound"])
        scanned = {p["id"] for p in result["profiles"] if p.get("available")}
        if args.baseline_scope == "present":
            known = {k for k in known if k.split("|", 1)[0] in scanned}
        new, gone = sorted(keys - known), sorted(known - keys)
        for key in new:
            print(f"NEW unbound call: {key}")
        for key in gone:
            print(f"fixed (drop from baseline): {key}")
        status = 1 if new else 0
    missing = [c for c in result["runtime_crosscheck"] if c["static"] == "MISSING"]
    for item in missing:
        print(f"runtime unbound not in the static result: {item['name']}", file=sys.stderr)
    print(json.dumps({"profiles": [p["id"] for p in result["profiles"] if p.get("available")],
                      "unbound_rows": len(result["rows"]), "elapsed_seconds": result["elapsed_seconds"]}))
    return status


if __name__ == "__main__":
    raise SystemExit(main())
