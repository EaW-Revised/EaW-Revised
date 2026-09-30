#!/usr/bin/env python3
"""Black-box P0-05 XML scanner acceptance probes.

The fixture is authored here and the assertions consume only the public
``xml_scan`` JSON report.  It deliberately does not reimplement catalog
resolution; the existing public API contract test remains the API-level probe.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


def write(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def build_fixture(root: Path) -> Path:
    data = root / "Data"
    xml = data / "XML"
    write(data / "MegaFiles.xml", "<Mega_Files><File>Absent.meg</File></Mega_Files>")
    write(xml / "GameObjectFiles.xml", """<Game_Object_Files>
  <File>objects-a.xml</File><File>objects-b.xml</File>
  <File>missing-required.xml</File><File>numeric-root.xml</File>
</Game_Object_Files>""")
    write(xml / "HardpointDataFiles.xml", """<Hard_Point_Files>
  <File>missing-hardpoint.xml</File><File>malformed-hardpoint.xml</File><File>hardpoints.xml</File>
</Hard_Point_Files>""")
    write(xml / "FactionFiles.xml", "<Faction_Files><File>factions.xml</File></Faction_Files>")
    write(xml / "CampaignFiles.xml", "<Campaign_Files><File>campaigns.xml</File></Campaign_Files>")
    write(xml / "SFXEventFiles.xml", "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>")

    write(xml / "objects-a.xml", """<Objects>
  <SpaceUnit Name="BASE" zAttr="Z" aAttr="A">
    <TagReplace>base</TagReplace>
    <Death_Clone>Damage_Fire, Base_Clone</Death_Clone>
    <Death_Clone>Damage_Fire, Base_Clone_2</Death_Clone>
    <Empty>base</Empty>
    <Nested mode="base"><Leaf>base</Leaf></Nested>
    <Numeric> 1.5 </Numeric>
    <Mystery>raw</Mystery>
    <Probe_Block zeta="1" alpha="2" Mid="m">
      <!-- a comment is not an element -->
      <Probe_Item Name="FIRST" order="b">
        <Probe_Leaf>one</Probe_Leaf>
        <Probe_Deep kind="x"><Probe_Leaf>two</Probe_Leaf></Probe_Deep>
      </Probe_Item>
      <Probe_Item Name="SECOND"/>
    </Probe_Block>
  </SpaceUnit>
  <SpaceUnit Name="MID"><Variant_Of_Existing_Type>base</Variant_Of_Existing_Type>
    <TagReplace>mid</TagReplace><Death_Clone>Damage_Fire, Mid_Clone</Death_Clone>
  </SpaceUnit>
  <SpaceUnit Name="TOP"><Variant_Of_Existing_Type>mId</Variant_Of_Existing_Type>
    <TagReplace>top</TagReplace><Death_Clone>Damage_Fire, Top_Clone</Death_Clone>
    <Empty></Empty><Nested mode="top"><Leaf>top</Leaf></Nested>
  </SpaceUnit>
  <SpaceUnit Name="MISSING"><Variant_Of_Existing_Type>NO_BASE</Variant_Of_Existing_Type></SpaceUnit>
  <SpaceUnit Name="CYCLE_A"><Variant_Of_Existing_Type>CYCLE_B</Variant_Of_Existing_Type></SpaceUnit>
  <SpaceUnit Name="CYCLE_B"><Variant_Of_Existing_Type>cycle_a</Variant_Of_Existing_Type></SpaceUnit>
  <SpaceUnit Name="DUP"><TagReplace>first</TagReplace></SpaceUnit>
  <SpaceUnit Name="ABILITY_OWNER"><Abilities>
    <Lucky_Shot_Attack_Ability Name="LUCKY"><Damage>9</Damage></Lucky_Shot_Attack_Ability>
  </Abilities></SpaceUnit>
</Objects>""")
    write(xml / "objects-b.xml", """<Objects>
  <SpaceUnit Name="dup"><TagReplace>second</TagReplace></SpaceUnit>
</Objects>""")
    write(xml / "numeric-root.xml", """<?xml version="1.0"?>
<181st><SpaceUnit Name="NUMERIC_ROOT"><Value>accepted</Value></SpaceUnit></181st>""")
    write(xml / "malformed-hardpoint.xml", "<HardPoints><HardPoint Name=\"BROKEN\"><Damage></HardPoints>")
    write(xml / "hardpoints.xml", '<HardPoints><HardPoint Name="HP_A"><Damage>1</Damage></HardPoint></HardPoints>')
    write(xml / "factions.xml", """<Factions><Faction Name="FACTION_A">
  <Standalone_Space_Maps_Special_Weapon_B>OLD</Standalone_Space_Maps_Special_Weapon_B>
</Faction></Factions>""")
    write(xml / "campaigns.xml", '<Campaigns><Campaign Name="CAMPAIGN_A"><Starting_Credits>1000</Starting_Credits></Campaign></Campaigns>')
    write(xml / "sfx.xml", '<SFXEvents><SFXEvent Name="SFX_A"><Samples>a.wav</Samples></SFXEvent></SFXEvents>')

    # These are physically present but deliberately outside all active lists.
    # Their diagnostics must remain visible without creating catalog entries.
    write(xml / "disabled-malformed.xml", "<Disabled><Broken></Disabled>")
    write(xml / "disabled-doctype.xml", "<!DOCTYPE Disabled [<!ENTITY ext SYSTEM 'file:///nope'>]><Disabled>&ext;</Disabled>")
    write(xml / "disabled-numeric-child.xml", "<Disabled><1bad/></Disabled>")
    return data


def run(xml_scan: Path, fixture_data: Path, report: Path, profile: str = "eaw") -> dict:
    receipt = report.with_suffix(".receipt.json")
    result = subprocess.run(
        [str(xml_scan), "--profile", profile, "--game-root", str(fixture_data),
         "--report", str(report), "--input-receipt", str(receipt),
         "--sample", "top", "--sample", "numeric_root"],
        text=True,
        capture_output=True,
        check=False,
    )
    check(report.exists(), f"xml_scan did not write {report}: {result.stdout}{result.stderr}")
    check(receipt.exists(), "xml_scan did not write input receipt")
    payload = json.loads(report.read_text(encoding="utf-8"))
    check(result.returncode == 1, f"fixture should retain hard missing/parser/resolution findings, got {result.returncode}")
    return payload


def receipt_for(report: Path) -> dict:
    return json.loads(report.with_suffix(".receipt.json").read_text(encoding="utf-8"))


def identity(receipt: dict, roots: list | None = None, includes: list | None = None) -> str:
    pieces = []

    def field(value: str) -> None:
        encoded = value.encode("utf-8")
        pieces.append(str(len(encoded)).encode("ascii") + b":" + encoded)

    field("eawr-xml-input-receipt-v1")
    field(receipt["profile"])
    field(receipt["schema_revision"])
    for kind, rows in (("root", receipt["roots"] if roots is None else roots),
                       ("include", receipt["includes"] if includes is None else includes)):
        for row in rows:
            for value in (kind, row["category"], row["logical_path"],
                          str(row["include_order"]) if kind == "include" else "", row["outcome"],
                          "1" if row["source_id"] is not None else "0", row["source_id"] or "",
                          row["layer"] or "", "1" if row["sha256"] is not None else "0",
                          row["sha256"] or ""):
                field(value)
            if kind == "include":
                field(row["registry_path"])
    return hashlib.sha256(b"".join(pieces)).hexdigest()


def meg(entries: list[tuple[str, bytes]]) -> bytes:
    names = b"".join(struct.pack("<H", len(name.encode("latin1"))) + name.encode("latin1")
                     for name, _ in entries)
    offset = 8 + len(names) + 20 * len(entries)
    table = bytearray()
    payload = bytearray()
    for index, (_, data) in enumerate(entries):
        table += struct.pack("<IIIII", 0x12345678, index, len(data), offset + len(payload), index)
        payload += data
    return struct.pack("<II", len(entries), len(entries)) + names + table + payload


def sample_structure(fixture: Path, samples: dict) -> None:
    """--sample occurrences carry ordered attributes and recursive children with file lines.

    Expected lines are located in the authored fixture text, not taken from the scanner."""
    authored = (fixture / "XML" / "objects-a.xml").read_text(encoding="utf-8").splitlines()

    def line_of(marker: str) -> int:
        lines = [number for number, text in enumerate(authored, 1) if marker in text]
        check(len(lines) == 1, f"fixture marker {marker!r} is not unique")
        return lines[0]

    def shape(node: dict) -> tuple:
        return (node["name"], node["raw_text"].strip(), node["line"],
                [(attribute["name"], attribute["value"]) for attribute in node["attributes"]],
                [shape(child) for child in node["children"]])

    earlier_keys = {"name": str, "raw_text": str, "provenance": str, "source_object_id": str, "source": dict}
    for sample in samples.values():
        for value in sample.get("effective_values", []):
            check(all(isinstance(value.get(key), kind) for key, kind in earlier_keys.items())
                  and "displaced_raw_text" in value, "sample value lost or retyped an earlier key")
            check(isinstance(value.get("attributes"), list) and isinstance(value.get("children"), list),
                  "sample value lacks attributes/children")

    top = samples["top"]
    probe = next(value for value in top["effective_values"] if value["name"] == "Probe_Block")
    check(probe["provenance"] == "inherited" and probe["source_object_id"] == "BASE"
          and probe["source"]["line"] == line_of("<Probe_Block"), "inherited nested occurrence lost its origin")
    check([(a["name"], a["value"]) for a in probe["attributes"]] == [("zeta", "1"), ("alpha", "2"), ("Mid", "m")],
          "occurrence attributes are not in authored order")
    expected = [
        ("Probe_Item", "", line_of('Name="FIRST"'), [("Name", "FIRST"), ("order", "b")], [
            ("Probe_Leaf", "one", line_of(">one<"), [], []),
            ("Probe_Deep", "", line_of("<Probe_Deep"), [("kind", "x")], [
                ("Probe_Leaf", "two", line_of(">two<"), [], [])]),
        ]),
        ("Probe_Item", "", line_of('Name="SECOND"'), [("Name", "SECOND")], []),
    ]
    check([shape(child) for child in probe["children"]] == expected,
          f"nested children differ from the authored tree: {[shape(child) for child in probe['children']]}")

    nested = next(value for value in top["effective_values"] if value["name"] == "Nested")
    check([(a["name"], a["value"]) for a in nested["attributes"]] == [("mode", "top")]
          and [shape(child) for child in nested["children"]] == [("Leaf", "top", nested["source"]["line"], [], [])],
          "overriding nested occurrence does not carry its own attributes and children")
    tag_replace = next(value for value in top["effective_values"] if value["name"] == "TagReplace")
    check(tag_replace["attributes"] == [] and tag_replace["children"] == [], "leaf value gained structure")


def acceptance(xml_scan: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="eawr-p005-acceptance-") as temporary:
        root = Path(temporary)
        fixture = build_fixture(root)
        report = root / "scan.json"
        payload = run(xml_scan, fixture, report)
        receipt = receipt_for(report)

        collision = root / "collision.json"
        rejected = subprocess.run(
            [str(xml_scan), "--profile", "eaw", "--game-root", str(fixture),
             "--report", str(collision), "--input-receipt", str(root / "." / "collision.json")],
            text=True, capture_output=True, check=False,
        )
        check(rejected.returncode != 0 and "different paths" in rejected.stderr and not collision.exists(),
              "scanner accepted colliding report and receipt destinations")

        check(receipt["schema_version"] == 1 and receipt["profile"] == "eaw", "receipt header changed")
        check(receipt["schema_revision"] == payload["schema_revision"], "receipt schema revision differs")
        check("enumerated VFS mounts" in receipt["scope"] and "runtime observation" in receipt["scope"],
              "receipt scope omits its limits")
        check(len(receipt["roots"]) == 5 and len(receipt["includes"]) == 10,
              "receipt lost attempted roots or includes")
        check(receipt["identity_sha256"] == identity(receipt), "receipt identity differs from ordered records")
        check(identity(receipt, roots=list(reversed(receipt["roots"]))) != receipt["identity_sha256"],
              "root ordering is absent from identity")
        check(identity(receipt, includes=list(reversed(receipt["includes"]))) != receipt["identity_sha256"],
              "include ordering is absent from identity")
        check(all(row["outcome"] == "loaded" and row["sha256"] for row in receipt["roots"]),
              "normal roots lack loaded outcomes and hashes")
        rows = {row["logical_path"]: row for row in receipt["includes"]}
        for name in ("missing-required.xml", "missing-hardpoint.xml"):
            row = rows[f"data/xml/{name}"]
            check(row["outcome"] == "missing" and row["sha256"] is None,
                  f"missing {name} received a fabricated digest")
        malformed = rows["data/xml/malformed-hardpoint.xml"]
        check(malformed["outcome"] == "parse_error" and
              malformed["sha256"] == hashlib.sha256((fixture / "XML" / "malformed-hardpoint.xml").read_bytes()).hexdigest(),
              "readable malformed input lost its exact-byte digest")
        check(rows["data/xml/hardpoints.xml"]["outcome"] == "loaded",
              "valid sentinel after failed includes disappeared")
        check(rows["data/xml/objects-a.xml"]["sha256"] ==
              hashlib.sha256((fixture / "XML" / "objects-a.xml").read_bytes()).hexdigest(),
              "loaded include digest differs from parse input")
        receipt_text = report.with_suffix(".receipt.json").read_text(encoding="utf-8")
        check(str(root) not in receipt_text and "base-only" not in receipt_text and
              "<Objects>" not in receipt_text and "accepted</Value>" not in receipt_text,
              "receipt contains a host path or XML value")

        run(xml_scan, fixture, root / "repeat.json")
        check(receipt_for(root / "repeat.json") == receipt, "repeat receipt is nondeterministic")
        relocated = root / "relocated" / "Data"
        shutil.copytree(fixture, relocated)
        run(xml_scan, relocated, root / "relocated.json")
        check(receipt_for(root / "relocated.json")["identity_sha256"] == receipt["identity_sha256"],
              "host directory relocation changed receipt identity")
        object_path = relocated / "XML" / "objects-b.xml"
        object_bytes = object_path.read_bytes()
        object_path.write_bytes(object_bytes.replace(b"second", b"newone"))
        check(object_path.stat().st_size == len(object_bytes), "content mutation changed byte count")
        run(xml_scan, relocated, root / "changed.json")
        check(receipt_for(root / "changed.json")["identity_sha256"] != receipt["identity_sha256"],
              "same-size content change did not change identity")

        for case, content in (("missing", None), ("malformed", "<Game_Object_Files><File>"),
                              ("wrong-root", "<Not_Game_Object_Files/>")):
            case_data = root / case / "Data"
            shutil.copytree(fixture, case_data)
            registry = case_data / "XML" / "GameObjectFiles.xml"
            if content is None:
                registry.unlink()
            else:
                write(registry, content)
            case_report = root / f"{case}.json"
            run(xml_scan, case_data, case_report)
            attempted = receipt_for(case_report)["roots"][0]
            check(attempted["logical_path"] == "data/xml/gameobjectfiles.xml" and
                  attempted["outcome"] == ("missing" if content is None else "parse_error"),
                  f"{case} root attempt was omitted")
            check(attempted["sha256"] == (None if content is None else hashlib.sha256(content.encode()).hexdigest()),
                  f"{case} root digest is incorrect")
            check(not any(row["registry_path"] == attempted["logical_path"]
                          for row in receipt_for(case_report)["includes"]),
                  f"{case} root generated nonexistent include attempts")

        native_data = root / "native-path" / "Data"
        shutil.copytree(fixture, native_data)
        registry = native_data / "XML" / "GameObjectFiles.xml"
        registry.write_text(registry.read_text(encoding="utf-8").replace(
            "</Game_Object_Files>", "<File>C:/Private/secret.xml</File></Game_Object_Files>"), encoding="utf-8")
        run(xml_scan, native_data, root / "native-path.json")
        native_receipt = receipt_for(root / "native-path.json")
        check(any(row["logical_path"].startswith("invalid-logical-path:")
                  for row in native_receipt["includes"]), "native-looking include path was not redacted")
        check("C:/Private" not in (root / "native-path.receipt.json").read_text(encoding="utf-8"),
              "native-looking XML include value leaked into receipt")

        archive_data = root / "archive" / "Data"
        shutil.copytree(fixture, archive_data)
        archive_object = archive_data / "XML" / "objects-b.xml"
        archive_bytes = archive_object.read_bytes()
        archive_object.unlink()
        archive_data.joinpath("Objects.meg").write_bytes(meg([("DATA\\XML\\objects-b.xml", archive_bytes)]))
        write(archive_data / "MegaFiles.xml", "<Mega_Files><File>Objects.meg</File></Mega_Files>")
        run(xml_scan, archive_data, root / "archive.json")
        archive_row = next(row for row in receipt_for(root / "archive.json")["includes"]
                           if row["logical_path"] == "data/xml/objects-b.xml")
        check(archive_row["outcome"] == "loaded" and archive_row["sha256"] == hashlib.sha256(archive_bytes).hexdigest()
              and "Objects.meg" in archive_row["source_id"], "MEG winner was not identified and hashed")
        check(archive_row["source_id"] != rows["data/xml/objects-b.xml"]["source_id"] and
              receipt_for(root / "archive.json")["identity_sha256"] != receipt["identity_sha256"],
              "same logical path in archive and loose source shares an identity")

        install = root / "install"
        shutil.copytree(fixture, install / "GameData" / "Data")
        expansion = install / "corruption" / "Data"
        write(expansion / "MegaFiles.xml", "<Mega_Files><File>Absent.meg</File></Mega_Files>")
        winner = b'<Objects><SpaceUnit Name="dup"><TagReplace>strong</TagReplace></SpaceUnit></Objects>'
        winner_path = expansion / "XML" / "objects-b.xml"
        winner_path.parent.mkdir(parents=True)
        winner_path.write_bytes(winner)
        run(xml_scan, install, root / "layered.json", profile="foc")
        layered = receipt_for(root / "layered.json")
        layered_row = next(row for row in layered["includes"] if row["logical_path"] == "data/xml/objects-b.xml")
        check(layered_row["layer"] == "expansion" and layered_row["sha256"] == hashlib.sha256(winner).hexdigest()
              and layered_row["source_id"] != rows["data/xml/objects-b.xml"]["source_id"],
              "stronger layer winner was not bound to parsed bytes")

        check(payload["schema_version"] == 1, "scanner report schema version changed")
        check(payload["schema_revision"] == "3e1b825a124fbc13b2293665f34a36dd4d4be80f", "scanner used the wrong pinned schema")
        check(payload["profile"] == "eaw", "scanner profile is not eaw")
        categories = payload["counts"]["by_category"]
        expected_categories = {"game_objects", "hardpoints", "abilities", "factions", "campaigns", "sfx"}
        check(set(categories) == expected_categories, "report omitted one of the six requested categories")
        check(all(categories[name]["raw"] > 0 and categories[name]["winners"] > 0 for name in expected_categories),
              "an active requested category has no definitions")
        check(payload["counts"]["physical_xml_records"] >= 13, "physical XML inventory is incomplete")
        check(payload["counts"]["registry_includes"] == 10, "registry include order/count was not retained")

        codes = [item["code"] for item in payload["diagnostics"]]
        codes.extend(item["code"] for item in payload["unresolved"])
        for code in ("EAWR-XML-0001", "EAWR-XML-0002", "EAWR-XML-0004", "EAWR-XML-0006",
                     "EAWR-XML-0008", "EAWR-XML-0009", "EAWR-XML-0010", "EAWR-XML-0011"):
            check(code in codes, f"expected retained diagnostic {code} is missing")
        check(any("MISSING -> NO_BASE" in item["message"] for item in payload["unresolved"]),
              "missing-base chain was truncated in scanner output")
        check(any("CYCLE_A -> CYCLE_B -> CYCLE_A" in item["message"] for item in payload["unresolved"]),
              "cycle chain was truncated in scanner output")

        physical = {item["logical_path"]: item for item in payload["physical_inventory"]}
        check("data/xml/disabled-malformed.xml" in physical, "inactive malformed XML disappeared from physical inventory")
        check(not physical["data/xml/disabled-malformed.xml"]["active_registry_file"], "inactive XML was marked active")
        check(any(item["included_path"] == "data/xml/missing-required.xml" and not item["loaded"]
                  for item in payload["registries"]), "missing required include was not retained")
        check(any(item["included_path"] == "data/xml/missing-hardpoint.xml" and not item["loaded"]
                  for item in payload["registries"]), "missing active hardpoint include was not retained")
        check(any(item["included_path"] == "data/xml/malformed-hardpoint.xml" and not item["loaded"]
                  for item in payload["registries"]), "malformed active hardpoint include was treated as loaded")
        check(any(item["included_path"] == "data/xml/hardpoints.xml" and item["loaded"]
                  for item in payload["registries"]), "valid sentinel after missing/malformed includes did not load")
        check(any(item["logical_path"] == "data/xml/malformed-hardpoint.xml" and
                  item["active_registry_file"] and not item["parsed"] for item in payload["physical_inventory"]),
              "active malformed include lost its physical parse failure")

        samples = {item["requested_id"].lower(): item for item in payload["samples"]}
        top = samples["top"]
        check(top["chain"] == ["TOP", "MID", "BASE"], "sample chain is not complete and derived-to-base")
        names = [value["name"] for value in top["effective_values"]]
        check(names.count("Death_Clone") == 4, "repeated merge occurrences were flattened")
        check(next(value for value in top["effective_values"] if value["name"] == "TagReplace")["raw_text"] == "top",
              "case-insensitive variant did not select the authored top definition")
        check(next(value for value in top["effective_values"] if value["name"] == "Empty")["raw_text"] == "",
              "empty derived override was treated as inheritance")
        check(next(value for value in top["effective_values"] if value["name"] == "Nested")["source_object_id"] == "TOP",
              "nested winning occurrence was not retained")
        sample_structure(fixture, samples)
        numeric = samples["numeric_root"]
        check(numeric["object_id"] == "NUMERIC_ROOT", "numeric-leading document root retry did not restore object identity")
        check(any(tag["name"] == "Value" and tag["raw_text"] == "accepted" for tag in numeric["raw_chain"][0]["tags"]),
              "numeric-leading root child was not indexed")

        print("P0-05 black-box scanner acceptance passed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml-scan", required=True, type=Path)
    args = parser.parse_args()
    acceptance(args.xml_scan.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
