#!/usr/bin/env python3
"""Publish scan: the checks every commit of the public tree must pass.

Runs over a source tree (default: the repository root) and fails on any hit of:

- security: private IPv4 addresses, user-profile and home paths, binary addresses in the
  original game's image range, decompiler auto-names, SSH key file names, `.lan` host names;
- clean-room: engine-style names (tools/cleanroom_check.py's pattern and allowlist) in every file
  but vendored `third_party/`;
- assets: game asset file types, images that are not on the reviewed allowlist (by SHA-256), and
  binary files that are not on the allowlist;
- readme: names the README must not carry;
- gitleaks (with --gitleaks): the pinned gitleaks release, fetched and checked by SHA-256.

The rules live in tools/publish_scan/config.json. CI runs this on every push and pull request:
  python tools/publish_scan/publish_scan.py --gitleaks
Exit 0 clean, 1 hits, 2 usage or environment error.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import importlib.util
import io
import json
import platform
import re
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Iterable

HERE = Path(__file__).resolve().parent
CONFIG = HERE / "config.json"
TEXT_LIMIT = 64 * 1024 * 1024


class ScanError(Exception):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_text(path: Path) -> str | None:
    """The file as UTF-8 text, or None for a binary or non-UTF-8 file."""
    if path.stat().st_size > TEXT_LIMIT:
        return None
    data = path.read_bytes()
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def matches(relpath: str, pattern: str) -> bool:
    return relpath.startswith(pattern) if pattern.endswith("/") else fnmatch.fnmatchcase(relpath, pattern)


def tree_files(root: Path) -> list[Path]:
    """Tracked files when root is a git checkout, else every file (an export stage)."""
    if (root / ".git").exists():
        out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True).stdout
        return [root / name for name in out.decode("utf-8").split("\0") if name and (root / name).is_file()]
    return sorted(path for path in root.rglob("*") if path.is_file() and "__pycache__" not in path.parts)


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def load_config(path: Path = CONFIG) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def security_scan(root: Path, config: dict[str, Any], files: Iterable[Path],
                  extra_regex: dict[str, str] | None = None, extra_terms: Iterable[str] = ()) -> list[str]:
    scan = config["security"]
    patterns = [(name, re.compile(regex)) for name, regex in {**scan["regex"], **(extra_regex or {})}.items()]
    terms = sorted(set(extra_terms), key=len, reverse=True)
    term_pattern = re.compile("|".join(re.escape(t) for t in terms), re.I) if terms else None
    allowed = [(item["path"], re.compile(item["regex"])) for item in scan["allow"]]
    hits = []
    for path in files:
        relpath = rel(path, root)
        text = read_text(path)
        if text is None:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            found = [(name, m.group(0)) for name, rx in patterns for m in rx.finditer(line)]
            if term_pattern is not None:
                found += [("private-name", m.group(0)) for m in term_pattern.finditer(line)]
            for name, value in found:
                if any(matches(relpath, p) and rx.search(line) for p, rx in allowed):
                    continue
                hits.append(f"{relpath}:{number}: {name}: {value}")
    return hits


def cleanroom_module(root: Path) -> Any:
    """The scanned tree's own tools/cleanroom_check.py (its pattern, allowlist and Lua API names)."""
    spec = importlib.util.spec_from_file_location("publish_cleanroom_check", root / "tools" / "cleanroom_check.py")
    if spec is None or spec.loader is None:
        raise ScanError("tools/cleanroom_check.py not found in the scanned tree")
    module = importlib.util.module_from_spec(spec)
    previous, sys.dont_write_bytecode = sys.dont_write_bytecode, True  # no __pycache__ in the scanned tree
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def cleanroom_scan(root: Path, config: dict[str, Any], files: Iterable[Path]) -> list[str]:
    checker = cleanroom_module(root)
    allowed = frozenset(checker.lua_api_names())
    skip = config["cleanroom"]["skip"]
    hits = []
    for path in files:
        relpath = rel(path, root)
        if any(matches(relpath, p) for p in skip):
            continue
        text = read_text(path)
        if text is None:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            hits.extend(f"{relpath}:{number}: {token}" for token, _, _ in checker.line_tokens(line, allowed))
    return hits


def asset_scan(root: Path, config: dict[str, Any], files: Iterable[Path],
               retail_hashes: set[str] | None = None) -> list[str]:
    scan = config["assets"]
    banned = {suffix.lower() for suffix in scan["retail_suffixes"]}
    images = {suffix.lower() for suffix in scan["image_suffixes"]}
    hits = []
    for path in files:
        relpath = rel(path, root)
        suffix = path.suffix.lower()
        if suffix in banned and not any(matches(relpath, p) for p in scan["retail_suffix_allow"]):
            hits.append(f"{relpath}: game asset type {suffix}")
            continue
        digest = sha256_file(path) if (retail_hashes is not None or suffix in images) else ""
        if retail_hashes is not None and digest in retail_hashes:
            hits.append(f"{relpath}: byte-identical to a retail file")
        if suffix in images:
            if scan["image_allowlist"].get(relpath) != digest:
                hits.append(f"{relpath}: image not on the reviewed allowlist (sha256 {digest})")
        elif read_text(path) is None and not any(matches(relpath, p) for p in scan["binary_allowlist"]):
            hits.append(f"{relpath}: binary file not on the allowlist")
    return hits


def readme_scan(root: Path, config: dict[str, Any]) -> list[str]:
    readme = root / "README.md"
    if not readme.is_file():
        return ["README.md is missing"]
    text = readme.read_text(encoding="utf-8").casefold()
    return [f"README.md names '{term}'" for term in config["readme"]["forbidden"] if term.casefold() in text]


def gitleaks_binary(config: dict[str, Any], cache: Path) -> Path:
    pin = config["gitleaks"]
    system = platform.system().lower()
    machine = platform.machine().lower()
    key = "windows_x64" if system == "windows" else ("linux_arm64" if machine in {"aarch64", "arm64"} else "linux_x64")
    if system == "darwin":
        key = "darwin_arm64" if machine == "arm64" else "darwin_x64"
    asset = pin["assets"][key]
    tools = cache / f"gitleaks-{pin['version']}-{key}"
    binary = tools / ("gitleaks.exe" if system == "windows" else "gitleaks")
    if binary.is_file():
        return binary
    tools.mkdir(parents=True, exist_ok=True)
    url = f"https://github.com/gitleaks/gitleaks/releases/download/v{pin['version']}/{asset['file']}"
    with urllib.request.urlopen(url, timeout=120) as response:  # noqa: S310 (pinned https URL)
        data = response.read()
    if hashlib.sha256(data).hexdigest() != asset["sha256"]:
        raise ScanError(f"gitleaks {asset['file']} does not match its pinned SHA-256")
    if asset["file"].endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            archive.extract(binary.name, tools)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            archive.extract(binary.name, tools)  # noqa: S202 (pinned release, SHA-256 checked)
    binary.chmod(0o755)
    return binary


def gitleaks_scan(root: Path, config: dict[str, Any], report: Path, cache: Path) -> list[str]:
    binary = gitleaks_binary(config, cache)
    report.parent.mkdir(parents=True, exist_ok=True)
    command = [str(binary), "dir", ".", "--no-banner", "--redact", "--exit-code", "1",
               "--config", str(HERE / "gitleaks.toml"),
               "--report-format", "json", "--report-path", str(report.resolve())]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise ScanError(f"gitleaks failed: {result.stderr.strip()[:400]}")
    findings = json.loads(report.read_text(encoding="utf-8") or "[]") if report.is_file() else []
    return [f"{item.get('File')}:{item.get('StartLine')}: {item.get('RuleID')}" for item in findings]


def scan(root: Path, config: dict[str, Any], *, gitleaks_report: Path | None = None, cache: Path | None = None,
         extra_regex: dict[str, str] | None = None, extra_terms: Iterable[str] = (),
         retail_hashes: set[str] | None = None) -> dict[str, list[str]]:
    files = tree_files(root)
    results = {
        "readme": readme_scan(root, config),
        "security": security_scan(root, config, files, extra_regex, extra_terms),
        "cleanroom": cleanroom_scan(root, config, files),
        "assets": asset_scan(root, config, files, retail_hashes),
    }
    if gitleaks_report is not None:
        results["gitleaks"] = gitleaks_scan(root, config, gitleaks_report, cache or root / "out" / "publish-scan")
    return results


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=HERE.parents[1])
    parser.add_argument("--gitleaks", action="store_true", help="also run the pinned gitleaks release")
    parser.add_argument("--out", type=Path, help="where gitleaks and its report go (default <root>/out/publish-scan)")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    out = (args.out or root / "out" / "publish-scan").resolve()
    try:
        results = scan(root, load_config(), gitleaks_report=out / "gitleaks.json" if args.gitleaks else None,
                       cache=out)
    except (ScanError, OSError, ValueError, KeyError) as error:
        print(f"publish_scan: {error}", file=sys.stderr)
        return 2
    total = 0
    for name, hits in results.items():
        for hit in hits:
            print(f"[{name}] {hit}")
        total += len(hits)
    print(f"publish_scan: {total} hit(s) " + ", ".join(f"{k} {len(v)}" for k, v in results.items()))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
