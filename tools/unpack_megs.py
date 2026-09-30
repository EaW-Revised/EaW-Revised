"""Build a searchable, provenance-preserving FoC reference tree from a retail install."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import shutil
from dataclasses import dataclass
from pathlib import Path

from inventory.common import canonical_path
from inventory.corpus import Layer, _parse_manifest
from megx import read_index


@dataclass
class Row:
    path: str
    logical: str
    size: int
    sha256: str
    source: str
    priority: tuple[int, int, int]
    active: bool
    overridden_by: str = ""


def copy_range(source, target: Path, size: int) -> str:
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with target.open("xb") as output:
        remaining = size
        while remaining:
            block = source.read(min(1024 * 1024, remaining))
            if not block:
                raise ValueError(f"short read while writing {target}")
            output.write(block)
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def archive_rows(archive: Path, relative: str, layer_name: str, prefix: str,
                 active_index: int, output: Path) -> list[Row]:
    entries = read_index(archive)
    rows: list[Row] = []
    seen: set[str] = set()
    source = f"{layer_name}:Data/{relative}"
    with archive.open("rb") as stream:
        for entry in entries:
            logical = canonical_path((prefix.rstrip("/") + "/" if prefix else "")
                                     + entry.name.replace("\\", "/"))
            if logical in seen:
                raise ValueError(f"duplicate case-insensitive member in {archive}: {logical}")
            seen.add(logical)
            path = f"{layer_name}/{relative}/{logical}"
            stream.seek(entry.offset)
            digest = copy_range(stream, output / path, entry.size)
            rows.append(Row(path, logical, entry.size, digest, source,
                            (1 if layer_name == "foc" else 0, 0, active_index),
                            active_index >= 0))
    return rows


def loose_rows(data_root: Path, layer_name: str, output: Path) -> list[Row]:
    rows: list[Row] = []
    seen: set[str] = set()
    for file in sorted(data_root.rglob("*"), key=lambda p: p.as_posix().lower()):
        if not file.is_file() or file.suffix.lower() == ".meg":
            continue
        logical = canonical_path("Data/" + file.relative_to(data_root).as_posix())
        if logical in seen:
            raise ValueError(f"duplicate case-insensitive loose file: {file}")
        seen.add(logical)
        path = f"{layer_name}/loose/{logical}"
        with file.open("rb") as stream:
            digest = copy_range(stream, output / path, file.stat().st_size)
        rows.append(Row(path, logical, file.stat().st_size, digest,
                        f"{layer_name}:loose:Data/{file.relative_to(data_root).as_posix()}",
                        (1 if layer_name == "foc" else 0, 1, 0), True))
    return rows


def assemble(game_root: Path, output: Path) -> dict[str, int]:
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"output must be empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    rows: list[Row] = []
    archive_count = 0
    order: dict[str, list[str]] = {}
    for layer_name, data_root in (
        ("eaw", game_root / "GameData" / "Data"),
        ("foc", game_root / "corruption" / "Data"),
    ):
        active, missing = _parse_manifest(Layer(layer_name, data_root))
        if missing:
            print(f"{layer_name}: absent localized declarations: {', '.join(missing)}", flush=True)
        active_by_path = {str(path.resolve()).casefold(): (index, prefix)
                          for index, (path, _rel, prefix) in enumerate(active)}
        order[layer_name] = [rel for _path, rel, _prefix in active]
        for archive in sorted(data_root.rglob("*"), key=lambda p: p.as_posix().lower()):
            if not archive.is_file() or archive.suffix.lower() != ".meg":
                continue
            relative = archive.relative_to(data_root).as_posix()
            index, prefix = active_by_path.get(str(archive.resolve()).casefold(), (-1, ""))
            extracted = archive_rows(archive, relative, layer_name, prefix, index, output)
            rows.extend(extracted)
            archive_count += 1
            print(f"{layer_name}/{relative}: {len(extracted)} files", flush=True)
        rows.extend(loose_rows(data_root, layer_name, output))

    winners: dict[str, Row] = {}
    for row in rows:
        if row.active and (row.logical not in winners or row.priority > winners[row.logical].priority):
            winners[row.logical] = row
    effective_rows: list[Row] = []
    for logical, winner in sorted(winners.items()):
        path = f"effective/{logical}"
        target = output / path
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(output / winner.path, target)
        except OSError:
            shutil.copyfile(output / winner.path, target)
        effective_rows.append(Row(path, logical, winner.size, winner.sha256,
                                  winner.path, winner.priority, True))
    for row in rows:
        if not row.active:
            row.overridden_by = "inactive archive"
        elif winners[row.logical] is not row:
            row.overridden_by = winners[row.logical].path
    with (output / "MANIFEST.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(("path", "size", "sha256", "source", "overridden-by"))
        for row in sorted(rows + effective_rows, key=lambda value: value.path):
            writer.writerow((row.path, row.size, row.sha256, row.source, row.overridden_by))
    total_bytes = sum(row.size for row in rows)
    xml = (output / "effective" / "data" / "xml").as_posix()
    manifest = (output / "MANIFEST.tsv").as_posix()
    (output / "README.md").write_text(
        "# FoC / EaW reference files\n\n"
        "Game data from the local retail install, for research only. Do not commit these files.\n\n"
        "`eaw/` and `foc/` retain each archive and loose file. `effective/` contains the "
        "winning FoC view, using hard links where supported. Paths there are ASCII case-folded "
        "like the project VFS. `MANIFEST.tsv` lists every source and effective file; "
        "`overridden-by` names the winning source path for a shadowed entry.\n\n"
        "## Priority, low to high\n\n"
        + "\n".join(f"- {layer}: " + " < ".join(paths) + " < loose"
                    for layer, paths in order.items())
        + "\n\nFoC wins over EaW even when EaW has a loose file. "
        "Each layer follows `Data/MegaFiles.xml` document order, then its three SFX "
        "archives, then Patch, Patch2 and 64Patch. Absent localized declarations are skipped.\n\n"
        "## Search recipes (PowerShell)\n\n"
        "```powershell\n"
        "# XML that defines a type\n"
        f"rg -n -i 'Nebulon_B_Frigate' {xml} -g '*.xml'\n"
        "# All hardpoint references for a unit (find its XML, then the named hardpoints)\n"
        f"rg -n -i -C 15 'Nebulon_B_Frigate' {xml} -g '*.xml'\n"
        f"rg -n -i 'HP_Nebulon' {xml} -g '*.xml'\n"
        "# Particle by name (XML and model references)\n"
        f"rg -n -i 'particle_name' {xml} -g '*.xml'\n"
        "# Provenance of any path\n"
        f"rg -n -i 'nebulon' {manifest}\n"
        "```\n",
        encoding="utf-8",
    )
    return {"archives": archive_count, "source_files": len(rows), "effective_files": len(effective_rows),
            "source_bytes": total_bytes, "effective_bytes": sum(row.size for row in effective_rows)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(assemble(args.game_root, args.output), flush=True)


if __name__ == "__main__":
    main()
