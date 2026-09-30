#!/usr/bin/env python3
"""Original black-box fixtures for the P0-06 asset report contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

from asset_validate_test_support import (
    BAD_TEXTURE_NAME,
    COUNT_KEYS,
    CRC_BUCKETS,
    MODEL_BUCKETS,
    RATE_KEYS,
    add_counts,
    check_environment_candidate_ledger,
    check_environment_ledger,
    check_source_bounds_ledger,
    dds_fixture,
    ted_fixture,
    ted_land_fixture,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validator", required=True, type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="eawr-asset-report-") as temporary:
        root = Path(temporary)
        data = root / "Data"
        textures = data / "Art" / "Textures"
        models = data / "Art" / "Models"
        textures.mkdir(parents=True)
        models.mkdir(parents=True)
        # A declared-but-missing archive is recorded by the accepted manifest
        # contract while the loose layer remains fully enumerable.
        (data / "MegaFiles.xml").write_text(
            "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8"
        )
        valid = dds_fixture()
        invalid = b"original unsupported model fixture"
        (textures / "Valid.DDS").write_bytes(valid)
        (models / "Invalid.ALO").write_bytes(invalid)
        maps = data / "Art" / "Maps"
        maps.mkdir(parents=True)
        ted = ted_fixture()
        (maps / "Fixture.TED").write_bytes(ted)
        land_ted = ted_land_fixture()
        (maps / "FixtureLand.TED").write_bytes(land_ted)

        reports = [root / "report-a.json", root / "report-b.json"]
        for report in reports:
            completed = subprocess.run(
                [
                    str(args.validator),
                    "--profile", "eaw",
                    "--game-root", str(data),
                    "--report", str(report),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            if completed.returncode != 1 or not report.is_file():
                raise AssertionError(
                    f"expected report and honest failure exit 1, got {completed.returncode}: {completed.stderr}"
                )
        if reports[0].read_bytes() != reports[1].read_bytes():
            raise AssertionError("repeated reports are not byte-identical")

        try:
            raw_report = reports[0].read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            raise AssertionError(f"report is not valid UTF-8: {error}") from error
        report = json.loads(raw_report)
        if report["schema_version"] != 2 or report["profile"] != "eaw" or report["view"] != "effective":
            raise AssertionError("report identity fields changed")
        if "environment_candidate_ledger" in raw_report:
            raise AssertionError("the single-map candidate ledger leaked into the schema 2 bulk report")
        records = report["records"]
        if len(records) != 4:
            raise AssertionError(f"expected four complete records, got {len(records)}")
        by_format = {record["format"]: record for record in records}
        dds = by_format["dds"]
        alo = by_format["alo"]
        ted_records = {record["logical_path"]: record for record in records
                       if record["format"] == "ted"}
        ted_record = ted_records["data/art/maps/fixture.ted"]
        land_record = ted_records["data/art/maps/fixtureland.ted"]
        counts = ted_record["counts"]
        if land_record["outcome"] != "loaded" or land_record["counts"]["terrain_samples"] != 2:
            raise AssertionError("synthetic land TED did not load its terrain")
        if land_record["counts"]["texture_references"] != 1 or land_record["counts"]["textures_resolved"]:
            raise AssertionError("an undecodable texture name must stay one unresolved reference")
        if ted_record["outcome"] != "loaded" or counts["maps"] != 1:
            raise AssertionError("synthetic TED did not load as one map")
        if ted_record["content_sha256"] != hashlib.sha256(ted).hexdigest():
            raise AssertionError("TED content identity missing")
        if counts["placements"] != 1 or counts["placements_crc_missing"] != 1:
            raise AssertionError("an unresolvable placement must stay a counted row")
        if sum(counts[key] for key in CRC_BUCKETS) != counts["placements"]:
            raise AssertionError("CRC buckets do not sum to the placement denominator")
        if sum(counts[key] for key in MODEL_BUCKETS) != counts["placements"]:
            raise AssertionError("model buckets do not sum to the placement denominator")
        if counts["placements_model_unknown"] != 1:
            raise AssertionError("an unresolved type must not claim known renderability")

        resolution = report["map_resolution"]
        if set(resolution) != set(RATE_KEYS):
            raise AssertionError(f"map_resolution keys changed: {sorted(resolution)}")
        for key, value in resolution.items():
            if set(value) != {"resolved", "total", "rate", "meets_99_percent"}:
                raise AssertionError(f"{key} rate shape changed")
            if value["total"] == 0:
                if value["rate"] is not None or value["meets_99_percent"]:
                    raise AssertionError(f"{key} invented a rate from an empty denominator")
            elif abs(value["rate"] - value["resolved"] / value["total"]) > 1e-6:
                raise AssertionError(f"{key} rate is not the reported quotient")
            elif value["meets_99_percent"] != (value["resolved"] * 100 >= value["total"] * 99):
                raise AssertionError(f"{key} 99 percent verdict disagrees with its own numbers")
        placement_rate = resolution["placement_type_resolution"]
        if placement_rate != {"resolved": 0, "total": 1, "rate": 0.0, "meets_99_percent": False}:
            raise AssertionError("an honest shortfall was not reported as a shortfall")

        unresolved = report["map_unresolved"]
        if set(unresolved) != {"placement_type_crcs", "model_names", "texture_names", "sky_object_ids"}:
            raise AssertionError(f"map_unresolved keys changed: {sorted(unresolved)}")
        if unresolved["placement_type_crcs"] != [{"crc32": "0xa1b2c3d4", "maps": 1}]:
            raise AssertionError(
                "the unresolved placement CRC was not named with its owning map count: "
                f"{unresolved['placement_type_crcs']}"
            )
        families = report["map_families"]
        expected_keys = {"terrain_effects_used", "terrain_effects_declared_slots",
                         "skydome_objects", "skydome_models", "maps_with_terrain",
                         "maps_with_water_record", "maps_with_sky_reference", "space_maps",
                         "map_issues"}
        if set(families) != expected_keys:
            raise AssertionError(f"map_families keys changed: {sorted(families)}")
        # The land fixture declares one slot with a primary and no secondary
        # diffuse texture, which is the single-diffuse TERRAIN selection.
        if families["terrain_effects_used"] != [{"name": "TerrainRenderBump.fx", "maps": 1}]:
            raise AssertionError(
                f"terrain effect family was not recorded: {families['terrain_effects_used']}")
        if families["terrain_effects_declared_slots"] != families["terrain_effects_used"]:
            raise AssertionError("a used slot must also be a declared slot")
        if families["maps_with_terrain"] != 1 or families["space_maps"] != 1:
            raise AssertionError("land and space maps were not counted separately")
        issues = {entry["name"]: entry for entry in families["map_issues"]}
        # The fixture's missing CRC is a typed notice in the one map that
        # carries it; the synthetic maps omit 1/256/8 and 1/259.
        if issues.get("placement_type_missing") != {"name": "placement_type_missing", "maps": 1, "occurrences": 1}:
            raise AssertionError(f"the missing type CRC was not a typed map notice: {families['map_issues']}")
        if set(issues) != {"placement_type_missing", "optional_section_absent"}:
            raise AssertionError(f"unexpected typed map notices: {sorted(issues)}")
        if any(entry["maps"] > entry["occurrences"] for entry in families["map_issues"]):
            raise AssertionError("a typed map notice counts more maps than occurrences")
        if families["skydome_objects"] or families["skydome_models"]:
            raise AssertionError("no fixture declares a skydome, so none may be recorded")
        if families["maps_with_sky_reference"]:
            raise AssertionError("a map with no environment record has no sky reference")

        for key in ("model_names", "sky_object_ids"):
            if unresolved[key]:
                raise AssertionError(f"{key} invented an unresolved reference")
        expected_name = BAD_TEXTURE_NAME.rstrip(b"\x00").decode("latin-1")
        if unresolved["texture_names"] != [{"name": expected_name, "maps": 1}]:
            raise AssertionError(
                f"the undecodable texture name did not round-trip: {unresolved['texture_names']}"
            )
        if dds["outcome"] != "loaded" or dds["counts"]["mips"] != 1:
            raise AssertionError("valid DDS outcome or mip count missing")
        if dds["content_sha256"] != hashlib.sha256(valid).hexdigest():
            raise AssertionError("valid DDS content hash mismatch")
        if alo["outcome"] != "failed" or alo["content_sha256"] != hashlib.sha256(invalid).hexdigest():
            raise AssertionError("failed ALO content identity missing")
        if set(alo["counts"]) != COUNT_KEYS or any(alo["counts"].values()):
            raise AssertionError("failed record must expose explicit zero counts")
        failure = alo.get("failure", {})
        for field in ("code", "cause", "affected_feature", "follow_up", "byte_offset"):
            if field not in failure:
                raise AssertionError(f"failure metadata lacks {field}")
        provenance = alo.get("provenance", {})
        for field in ("layer", "origin", "source_id", "stored_size"):
            if field not in provenance:
                raise AssertionError(f"logical provenance lacks {field}")

        derived = {
            "discovered": len(records),
            "loaded": sum(record["outcome"] == "loaded" for record in records),
            "failed": sum(record["outcome"] == "failed" for record in records),
            "notices": sum(record["notices"] for record in records),
            "counts": {key: 0 for key in COUNT_KEYS},
        }
        for record in records:
            add_counts(derived["counts"], record["counts"])
        if report["total"] != derived:
            raise AssertionError("top-level aggregate is not the mechanical sum of records")
        for format_name, aggregate in report["aggregates"].items():
            subset = [record for record in records if record["format"] == format_name]
            expected = {
                "discovered": len(subset),
                "loaded": sum(record["outcome"] == "loaded" for record in subset),
                "failed": sum(record["outcome"] == "failed" for record in subset),
                "notices": sum(record["notices"] for record in subset),
                "counts": {key: 0 for key in COUNT_KEYS},
            }
            for record in subset:
                add_counts(expected["counts"], record["counts"])
            if aggregate != expected:
                raise AssertionError(f"{format_name} aggregate is not mechanically derived")

        legacy_summary = root / "legacy-summary.json"
        legacy_failures = root / "legacy-failures.json"
        legacy = subprocess.run(
            [
                str(args.validator),
                "--profile", "eaw",
                "--game-root", str(data),
                "--summary", str(legacy_summary),
                "--failures", str(legacy_failures),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if legacy.returncode != 1 or not legacy_summary.is_file() or not legacy_failures.is_file():
            raise AssertionError("legacy summary/failures mode was not preserved")
        legacy_rows = json.loads(legacy_failures.read_text(encoding="utf-8"))["failures"]
        if len(legacy_rows) != 1 or legacy_rows[0]["message"] != legacy_rows[0]["cause"]:
            raise AssertionError("legacy failure cause compatibility changed")
        if legacy_rows[0]["content_sha256"] != hashlib.sha256(invalid).hexdigest():
            raise AssertionError("legacy failure output lacks content identity")

        check_environment_ledger(args.validator, data, ted)
        check_environment_candidate_ledger(args.validator, data)
        check_source_bounds_ledger(args.validator, data)

    print("asset_validate report contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
