"""Build the optional Revised game-data layer from private advisor takes."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

VOICES = (("empire", "Empire", "f0_fx"), ("rebel", "Rebel", "plain"),
          ("underworld", "Underworld", "f0_fx"))
SKIP_LOCAL = "964037908"
ADVISORS = {"Empire": "EHD_Choose_Reinforcements", "Rebel": "RHD_Choose_Reinforcements",
            "Underworld": "UHD_Choose_Reinforcements"}


def install(source: Path, destination: Path) -> dict[str, dict[str, int]]:
    destination = destination.resolve()
    repository = Path(__file__).resolve().parents[1]
    if destination.is_relative_to(repository):
        raise ValueError("Install private voices outside the repository")
    # Accept either the layer folder itself or its parent; its public name is Revised.
    if destination.name != "Revised":
        destination = destination / "Revised"
    events = ET.Element("SFXEvents")
    controls = ET.Element("Selection_Controls")
    ET.SubElement(controls, "Selection_Advisor_Enabled").text = "No"
    actions = {}
    for action, command, key in (("allunits", "SELECT_OWN_ALL", "Ctrl+Q"),
                                 ("localunits", "SELECT_OWN_SCREEN", "Q")):
        actions[action] = ET.SubElement(controls, "Selection_Action", Name=command)
        ET.SubElement(actions[action], "Default_Key").text = key
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
            ET.SubElement(actions[action], "Default_Faction_SFXEvent").text = f"{faction}, {event_name}"
            ET.SubElement(actions[action], "Advisor_SFXEvent").text = f"{ADVISORS[faction]}, {event_name}"
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
    registry = ET.Element("SFXEvent_Files")
    ET.SubElement(registry, "File").text = "RevisedSelectionSFX.xml"
    xml_root = destination / "Data" / "XML"
    xml_root.mkdir(parents=True, exist_ok=True)
    for filename, root in (("SFXEventFilesAdditions.xml", registry), ("RevisedSelectionSFX.xml", events),
                           ("SelectionControls.xml", controls)):
        ET.indent(root)
        temporary = xml_root / (filename + ".tmp")
        ET.ElementTree(root).write(temporary, encoding="utf-8", xml_declaration=True)
        temporary.replace(xml_root / filename)
    return counts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Private voice source root")
    parser.add_argument("--destination", type=Path, required=True, help="Revised layer folder or its parent, outside the repository")
    args = parser.parse_args()
    for faction, counts in install(args.source, args.destination).items():
        print(f"{faction}: {counts['allunits']} all-units, {counts['localunits']} local-units takes")

    print(f"Layer: {args.destination.resolve() if args.destination.name == 'Revised' else args.destination.resolve() / 'Revised'}")


if __name__ == "__main__":
    main()
