#!/usr/bin/env python3
"""Stage only release tools, launcher and required Debug symbols between jobs."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from package_release import PLATFORMS, TOOLS, find_tool


def stage(build_dir: Path, platform: str, configuration: str, out: Path) -> None:
    suffix = PLATFORMS[platform]["exe"]
    copies = []
    for name in TOOLS:
        source = find_tool(build_dir, name, suffix, configuration)
        destination = out / "apps" / name / configuration / source.name
        copies.append((source, destination))
        if configuration == "Debug":
            copies.append((source.with_suffix(".pdb"), destination.with_suffix(".pdb")))
    if platform == "windows-x64":
        launcher = build_dir / "apps" / "launcher" / "Release" / "EAWR.exe"
        copies.append((launcher, out / "apps" / "launcher" / "Release" / "EAWR.exe"))
    # Validate the complete payload before writing any of it.
    for source, _ in copies:
        if not source.is_file():
            raise FileNotFoundError(source)
    for source, destination in copies:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--config", required=True, choices=("Release", "Debug"))
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    stage(args.build_dir, args.platform, args.config, args.out)


if __name__ == "__main__":
    main()
