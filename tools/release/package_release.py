#!/usr/bin/env python3
"""Package one platform's release archives from a finished build (docs/releasing.md).

Writes two zips into --out:
  eaw-revised-<version>-<platform>-tools.zip   the headless tools (sim_headless, the scanners, benchmarks)
  eaw-revised-<version>-<platform>-viewer.zip  the Godot viewer project with its built GDExtension
Each carries LICENSE, THIRD_PARTY_NOTICES.md and README.md at its top. No game data,
fonts or Godot binaries go in: players supply their own game installation and the pinned Godot 4.7.2.

Usage:
  python tools/release/package_release.py --build-dir out/build/linux-x64-gcc --platform linux-x64 \\
      --version v0.1.5 --out out/release
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ("sim_headless", "sim_bench", "path_bench", "asset_scan", "asset_validate", "xml_scan", "scene_scan",
         "sky_scan", "unit_scan", "script_smoke", "hud_movie_prepare")
DOCS = ("LICENSE", "THIRD_PARTY_NOTICES.md", "README.md")
VIEWER_PROJECT = ROOT / "apps" / "viewer" / "project"
PLATFORMS = {
    "linux-x64": {"exe": "", "extension": "libeawr_viewer.linux.template_release.x86_64.so"},
    "linux-arm64": {"exe": "", "extension": "libeawr_viewer.linux.template_release.arm64.so"},
    "windows-x64": {"exe": ".exe", "extension": "libeawr_viewer.windows.template_release.x86_64.dll"},
}


def find_tool(build_dir: Path, name: str, suffix: str) -> Path:
    for candidate in (build_dir / "apps" / name / (name + suffix),
                      build_dir / "apps" / name / "Release" / (name + suffix)):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"{name}{suffix} not built under {build_dir}/apps/{name}")


def add_docs(archive: zipfile.ZipFile, top: str) -> None:
    for name in DOCS:
        archive.write(ROOT / name, f"{top}/{name}")


def package(build_dir: Path, platform: str, version: str, out: Path) -> list[Path]:
    spec = PLATFORMS[platform]
    out.mkdir(parents=True, exist_ok=True)
    stem = f"eaw-revised-{version}-{platform}"

    tools_zip = out / f"{stem}-tools.zip"
    with zipfile.ZipFile(tools_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in TOOLS:
            binary = find_tool(build_dir, name, spec["exe"])
            info = zipfile.ZipInfo.from_file(binary, f"{stem}-tools/bin/{binary.name}")
            info.external_attr = 0o755 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, binary.read_bytes())
        add_docs(archive, f"{stem}-tools")

    extension = VIEWER_PROJECT / "bin" / spec["extension"]
    if not extension.is_file():
        raise FileNotFoundError(f"viewer extension not built: {extension}")
    viewer_zip = out / f"{stem}-viewer.zip"
    with zipfile.ZipFile(viewer_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(VIEWER_PROJECT.rglob("*")):
            if not path.is_file() or ".godot" in path.relative_to(VIEWER_PROJECT).parts:
                continue
            relative = path.relative_to(VIEWER_PROJECT).as_posix()
            if relative.startswith("bin/") and path.name != spec["extension"]:
                continue  # another platform's extension, or build leftovers
            archive.write(path, f"{stem}-viewer/project/{relative}")
        add_docs(archive, f"{stem}-viewer")
        for name in ("play-demo.cmd", "play-demo.sh", "demo.py"):
            path = ROOT / "tools" / "release" / name
            info = zipfile.ZipInfo.from_file(path, f"{stem}-viewer/{name}")
            info.external_attr = (0o755 if name.endswith(".sh") else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())
        archive.write(ROOT / "tools/fonts/extract_eaw_fonts.py", f"{stem}-viewer/extract_eaw_fonts.py")
    return [tools_zip, viewer_zip]


def write_sums(files: list[Path], out: Path) -> Path:
    sums = out / "SHA256SUMS.txt"
    lines = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in sorted(files)]
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return sums


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, help="the root CMake build (out/build/<preset>)")
    parser.add_argument("--platform", choices=sorted(PLATFORMS))
    parser.add_argument("--version")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "release")
    parser.add_argument("--sums-only", action="store_true", help="only write SHA256SUMS.txt for the zips in --out")
    args = parser.parse_args(argv)
    try:
        if args.sums_only:
            files = sorted(args.out.glob("*.zip"))
            if not files:
                raise FileNotFoundError(f"no zips in {args.out}")
        else:
            if not (args.build_dir and args.platform and args.version):
                parser.error("--build-dir, --platform and --version are required")
            files = package(args.build_dir, args.platform, args.version, args.out)
        sums = write_sums(files, args.out)
    except (FileNotFoundError, OSError) as error:
        print(f"package_release: {error}", file=sys.stderr)
        return 2
    for path in files + [sums]:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
