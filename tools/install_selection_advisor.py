"""Build the optional Revised game-data layer from private advisor takes."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

VOICES = (("empire", "Empire", "f0_fx"), ("rebel", "Rebel", "plain"),
          ("underworld", "Underworld", "f0_fx"))
SKIP_LOCAL = "964037908"
# sha256 of every earlier shipped version of the layer configuration files. A
# destination file equal to one of these (or to the current template) was never
# hand-edited, so the installer may update it; anything else is the owner's.
KNOWN_TEMPLATES = {
    "RevisedPlanetSearch.xml": frozenset(),
    "SelectionControls.xml": frozenset({
        "1fea04d62f745b14dd6b6687184f0322805c46246d4e481e8856d4129fa98c17"}),
    "SFXEventFilesAdditions.xml": frozenset({
        "3f418c91cd83797506ef992676014df4c852e842976768fb11c84d7801f51eef"}),
    "RevisedGiftSFX.xml": frozenset({
        "20b0e80f3be6c3b0dadc32c6a4f0691bdc716a1da7d6b64c2cfdb7bdfd47c680",
        "83a44237b6679fe082af1f6fd48e2ca7831f44ec5bbc25d30966d03a23894238"}),
}


def install(source: Path, destination: Path, force: bool = False,
            kept: list[str] | None = None) -> dict[str, dict[str, int]]:
    destination = destination.resolve()
    repository = Path(__file__).resolve().parents[1]
    if destination.is_relative_to(repository):
        raise ValueError("Install private voices outside the repository")
    # Accept either the layer folder itself or its parent; its public name is Revised.
    if destination.name != "Revised":
        destination = destination / "Revised"
    events = ET.Element("SFXEvents")
    # Keep the recoverable configuration byte-identical on every platform.
    templates = Path(__file__).with_name("revised_gift_sfx")
    config_bytes = {name: (templates / name).read_bytes() for name in
                    ("SelectionControls.xml", "SFXEventFilesAdditions.xml", "RevisedGiftSFX.xml")}
    config_bytes["RevisedPlanetSearch.xml"] = (templates.parent / "revised_planet_search" / "RevisedPlanetSearch.xml").read_bytes()
    copies = []
    counts = {}
    # Validate the complete selection before changing the destination.
    for voice, faction, suffix in VOICES:
        counts[faction] = {}
        for action, expected in (("allunits", 7), ("localunits", 4)):
            pattern = re.compile(rf"{action}_(\d+)_{suffix}\.wav")
            files = []
            for path in sorted((source / voice).glob("*.wav")):
                match = pattern.fullmatch(path.name)
                if match and not (action == "localunits" and match[1] == SKIP_LOCAL):
                    files.append(path)
            if len(files) != expected:
                raise ValueError(f"{voice}/{action}: expected {expected} approved takes, found {len(files)}")
            counts[faction][action] = len(files)
            event_name = f"Revised_Selection_{faction}_{action}"
            event = ET.SubElement(events, "SFXEvent", Name=event_name)
            for tag, value in (("Is_2D", "Yes"), ("Is_HUD_VO", "Yes"),
                               ("Localize", "Yes"), ("Play_Sequentially", "Yes")):
                ET.SubElement(event, tag).text = value
            samples = []
            for path in files:
                relative = Path("Revised", voice, path.name)
                samples.append((Path("Data/Audio/SFX") / relative).as_posix())
                copies.append((path, destination / "Data" / "Audio" / "SFX" / relative))
            ET.SubElement(event, "Samples").text = "\n".join(samples)
    for source_file, target in copies:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_file, target)
    xml_root = destination / "Data" / "XML"
    xml_root.mkdir(parents=True, exist_ok=True)
    for filename, root in (("RevisedSelectionSFX.xml", events),):
        ET.indent(root)
        temporary = xml_root / (filename + ".tmp")
        ET.ElementTree(root).write(temporary, encoding="utf-8", xml_declaration=True)
        temporary.replace(xml_root / filename)
    # Gift WAVs are optional and owner-managed: configuration only, no audio copies.
    for filename, contents in config_bytes.items():
        target = xml_root / filename
        if not force and target.exists():
            existing = hashlib.sha256(target.read_bytes()).hexdigest()
            if existing != hashlib.sha256(contents).hexdigest()                     and existing not in KNOWN_TEMPLATES[filename]:
                if kept is not None:
                    kept.append(filename)
                continue
        temporary = xml_root / (filename + ".tmp")
        temporary.write_bytes(contents)
        temporary.replace(xml_root / filename)
    return counts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Private voice source root")
    parser.add_argument("--destination", type=Path, required=True, help="Revised layer folder or its parent, outside the repository")
    parser.add_argument("--force", action="store_true", help="Replace hand-edited configuration files with the shipped templates")
    args = parser.parse_args()
    kept: list[str] = []
    for faction, counts in install(args.source, args.destination, args.force, kept).items():
        print(f"{faction}: {counts['allunits']} all-units, {counts['localunits']} local-units takes")
    if kept:
        print(f"Kept your edited {', '.join(kept)} (use --force to reset to the shipped version)")
    print(f"Layer: {args.destination.resolve() if args.destination.name == 'Revised' else args.destination.resolve() / 'Revised'}")


if __name__ == "__main__":
    main()
