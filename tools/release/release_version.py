#!/usr/bin/env python3
"""The one release version (config/release-version.json) and the rules that derive every other version string.

CMake reads the same file (cmake/EawrReleaseVersion.cmake), so the project version, the mods service version and the
default build identity follow it. A kit label is the release version plus an operator suffix; a kit version adds the
first eight characters of the kit commit; a release tag is `v` plus the version.

  python tools/release/release_version.py                # print the version
  python tools/release/release_version.py check-tag v0.3.3
  python tools/release/release_version.py label c-clang   # print the kit label for a suffix
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION_FILE = ROOT / "config" / "release-version.json"
VERSION = re.compile(r"\d+\.\d+\.\d+")
# The rule CMake (EAWR_KIT_LABEL) and package_release.py apply to a kit label.
LABEL = re.compile(r"[A-Za-z0-9._-]{1,32}")
SUFFIX = re.compile(r"[A-Za-z0-9._-]{0,24}")


def read(path: Path = VERSION_FILE) -> str:
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    version = data.get("version") if isinstance(data, dict) else None
    if not isinstance(version, str) or not VERSION.fullmatch(version) or set(data) != {"version"}:
        raise ValueError(f"{path.name} must hold exactly {{\"version\": \"<major>.<minor>.<patch>\"}}")
    return version


def kit_label(suffix: str, version: str | None = None) -> str:
    """The kit label for an operator suffix: the release version followed by the suffix, as one validated string."""
    version = read() if version is None else version
    if not SUFFIX.fullmatch(suffix):
        raise ValueError(f"kit suffix {suffix!r} is invalid: 0-24 characters from A-Z a-z 0-9 . _ -")
    label = version + suffix
    if not LABEL.fullmatch(label):
        raise ValueError(f"kit label {label!r} is invalid: 1-32 characters from A-Z a-z 0-9 . _ -")
    return label


def check_label(label: str, version: str | None = None) -> str:
    """Return the suffix of a kit label, or raise when the label does not start with the release version."""
    version = read() if version is None else version
    if not LABEL.fullmatch(label) or not label.startswith(version):
        raise ValueError(f"kit label {label!r} must start with the release version {version}")
    rest = label[len(version):]
    if rest and rest[0] in "0123456789":
        raise ValueError(f"kit label {label!r} extends the release version {version} with more digits")
    return rest


def kit_version(label: str, commit: str, version: str | None = None) -> str:
    """The version string of one kit: <label>-<first eight characters of the kit commit>."""
    check_label(label, version)
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("the kit commit must be a full lowercase commit hash")
    return f"{label}-{commit[:8]}"


def check_tag(tag: str, version: str | None = None) -> None:
    version = read() if version is None else version
    if tag != "v" + version:
        raise ValueError(f"release tag {tag!r} does not match config/release-version.json ({version}); expected v{version}")


def main(argv: list[str]) -> int:
    try:
        if not argv:
            print(read())
        elif argv[0] == "check-tag" and len(argv) == 2:
            check_tag(argv[1])
        elif argv[0] == "label" and len(argv) == 2:
            print(kit_label(argv[1]))
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except (OSError, ValueError) as error:
        print(f"release_version: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
