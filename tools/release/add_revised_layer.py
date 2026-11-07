#!/usr/bin/env python3
"""Add the private Revised layer to an extracted player package, locally."""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

import package_release as package


def add_layer(folder: Path, source: Path, archive: Path | None = None, public: bool = False) -> None:
    if public:
        raise ValueError("--public refuses to include the Revised layer")
    if not (folder / package.PLAYER_LAUNCHER).is_file() or not (folder / "README.md").is_file():
        raise ValueError("expected an extracted player folder containing EAWR.exe and README.md")
    if (folder / package.REVISED_LAYER_RELPATH).exists():
        raise ValueError("player folder already contains a Revised layer")
    if archive is not None and (not archive.is_file() or archive.resolve().is_relative_to(folder.resolve())):
        raise ValueError("--zip must name an existing zip outside the player folder")
    # Build a sibling copy and archive before touching either original. Keep a
    # folder backup until the archive replacement succeeds, so failures roll back.
    with tempfile.TemporaryDirectory(prefix=".eawr-layer-", dir=folder.parent) as temporary:
        work = Path(temporary)
        staged = work / folder.name
        shutil.copytree(folder, staged)
        package.stage_revised_layer(staged, source, False, False)
        readme = staged / "README.md"
        with readme.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(f"\n{package.REVISED_LAYER_README}\n")
        sums = staged / package.PLAYER_SUMS
        files = sorted(p for p in staged.rglob("*") if p.is_file() and p != sums)
        sums.write_text("".join(f"{package.sha256_file(p)}  {p.relative_to(staged).as_posix()}\n" for p in files),
                        encoding="utf-8", newline="\n")
        zip_temp = None
        try:
            if archive is not None:
                with tempfile.NamedTemporaryFile(prefix=archive.name + ".", suffix=".tmp", dir=archive.parent,
                                                 delete=False) as stream:
                    zip_temp = Path(stream.name)
                with zipfile.ZipFile(zip_temp, "w", zipfile.ZIP_DEFLATED) as output:
                    for path in files + [sums]:
                        output.write(path, f"{folder.name}/{path.relative_to(staged).as_posix()}")
            backup = work / "original"
            os.replace(folder, backup)
            try:
                os.replace(staged, folder)
                if archive is not None:
                    os.replace(zip_temp, archive)
            except BaseException:
                if folder.exists():
                    shutil.rmtree(folder)
                os.replace(backup, folder)
                raise
        finally:
            if zip_temp is not None:
                zip_temp.unlink(missing_ok=True)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--revised-layer", type=Path, required=True)
    parser.add_argument("--public", action="store_true")
    args = parser.parse_args(argv)
    try:
        add_layer(args.folder, args.revised_layer, args.zip, args.public)
    except (OSError, ValueError) as error:
        print(f"add_revised_layer: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
