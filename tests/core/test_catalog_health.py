#!/usr/bin/env python3
"""#793: sim_headless acts on what load_catalog found wrong (R-10, R-13, C-06).

A wholly invented install: the five registries, one hardpoint registry that lists a
malformed file, a missing file and a good file. The run cannot get as far as a skirmish
(there are no units), so the contracts are about the catalog stage that comes first:

* a malformed admitted hardpoint include fails the run there, naming the file;
* a registry with the wrong root fails the run there;
* a missing include is a warning on stderr and the run goes on to its later failure.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

REGISTRIES = {
    "GameObjectFiles.xml": "<Game_Object_Files></Game_Object_Files>",
    "HardpointDataFiles.xml": "<Hard_Point_Files><File>eawr_health_a.xml</File>{extra}</Hard_Point_Files>",
    "FactionFiles.xml": "<Faction_Files></Faction_Files>",
    "CampaignFiles.xml": "<Campaign_Files></Campaign_Files>",
    "SFXEventFiles.xml": "<SFXEvent_Files></SFXEvent_Files>",
}
GOOD_HARDPOINTS = "<HardPoints><HardPoint Name=\"EAWR_HEALTH_HP\"><Damage>1</Damage></HardPoint></HardPoints>"


def write_install(root: Path, *, extra_include: str, files: dict[str, str]) -> None:
    for folder in ("GameData", "corruption"):
        data = root / folder / "Data"
        (data / "XML").mkdir(parents=True, exist_ok=True)
        (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    xml = root / "GameData" / "Data" / "XML"
    for name, body in REGISTRIES.items():
        (xml / name).write_text(body.format(extra=extra_include), encoding="utf-8")
    (xml / "eawr_health_a.xml").write_text(GOOD_HARDPOINTS, encoding="utf-8")
    for name, body in files.items():
        (xml / name).write_text(body, encoding="utf-8")


def run(program: str, root: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run([program, "--skirmish", "m2", "--game-root", str(root), "--census-out", str(out)],
                          check=False, capture_output=True, text=True)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="eawr-catalog-health-") as temporary:
        base = Path(temporary)

        malformed = base / "malformed"
        write_install(malformed, extra_include="<File>eawr_health_b.xml</File>",
                      files={"eawr_health_b.xml": "<HardPoints><HardPoint Name=\"BROKEN\"></HardPoints>"})
        result = run(args.program, malformed, base / "malformed.json")
        output = result.stdout + result.stderr
        expect(result.returncode != 0, "a malformed admitted hardpoint include must fail the run")
        expect("eawr_health_b.xml" in output and "EAWR-XML-0001" in output,
               f"the failure names the malformed hardpoint include: {output}")

        wrong_root = base / "wrong_root"
        write_install(wrong_root, extra_include="", files={})
        (wrong_root / "GameData" / "Data" / "XML" / "FactionFiles.xml").write_text(
            "<Wrong_Root></Wrong_Root>", encoding="utf-8")
        result = run(args.program, wrong_root, base / "wrong_root.json")
        output = result.stdout + result.stderr
        expect(result.returncode != 0 and "EAWR-XML-0003" in output,
               f"a registry with the wrong root must fail the run on the registry: {output}")

        missing = base / "missing"
        write_install(missing, extra_include="<File>eawr_health_absent.xml</File>", files={})
        result = run(args.program, missing, base / "missing.json")
        output = result.stdout + result.stderr
        expect("catalog load: include_missing=1" in output,
               f"a missing include is a warning, not silence: {output}")
        expect("EAWR-XML-0003" not in output and "EAWR-XML-0001" not in output,
               f"a missing include is not a registry or parse failure: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
