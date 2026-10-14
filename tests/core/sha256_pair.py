"""Paired native/scalar measurements using the same executable and host."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = []
    # ABBA ordering helps reveal drift; each child selects once before its threads start.
    for mode in ("scalar", "automatic", "automatic", "scalar"):
        env = os.environ.copy()
        env.pop("EAWR_SHA256_BACKEND", None)
        if mode == "scalar":
            env["EAWR_SHA256_BACKEND"] = mode
        result = subprocess.run([str(args.program), "--profile"], env=env, capture_output=True,
                                text=True, timeout=600, check=True)
        row = json.loads(result.stdout)
        if mode == "scalar" and row["backend"] != "scalar":
            raise ValueError("forced fallback did not run")
        if row["sse2"] and row["ssse3"] and row["sha"] and mode == "automatic" and row["backend"] != "x86-sha":
            raise ValueError("native x86 route did not run on supported hardware")
        runs.append(row)
    base = runs[0]
    for row in runs[1:]:
        for key in ("canonical_bytes", "canonical_digest", "ticks", "queue_backlog"):
            if row[key] != base[key]:
                raise ValueError(f"canonical queue input changed: {key}")
        if len(row["runs"]) != len(base["runs"]):
            raise ValueError("live/LAN run count changed")
        for actual, expected in zip(row["runs"], base["runs"]):
            for key in ("workers", "lan", "units", "instances", "projectiles", "instructions",
                        "world_rows_digest", "lan_rows_digest"):
                if actual[key] != expected[key]:
                    raise ValueError(f"live/LAN bytes or work counts changed: {key}")
    report = {"processor": platform.processor(), "platform": platform.platform(),
              "ordering": ["scalar", "automatic", "automatic", "scalar"], "runs": runs}
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
