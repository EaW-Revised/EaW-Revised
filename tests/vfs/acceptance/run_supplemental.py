#!/usr/bin/env python3
"""Independent P0-02 acceptance probes.

The fixture writer deliberately lays out MEG-v1 bytes instead of calling the
production parser.  The script drives only the public asset_scan/vfs_tests
executables, and keeps all fixture data in a temporary directory.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

from vfs_supplemental_assertions import (
    CheckFailure,
    assert_report_shape,
    check,
    codes,
    load_report,
    report_for,
    run,
)
from vfs_supplemental_fixtures import (
    malformed_bounds,
    malformed_count,
    malformed_duplicate,
    malformed_entry_table,
    malformed_header,
    malformed_name_index,
    malformed_name_table,
    malformed_path,
    manifest,
    meg,
    text,
    write,
)


def run_synthetic(asset_scan: Path, vfs_tests: Path, repo: Path, root: Path) -> list[str]:
    findings: list[str] = []

    # A complete synthetic three-layer profile checks layer-first precedence,
    # loose-over-archive, archive order, raw/effective counts and shadowing.
    game = root / "game"
    base = game / "GameData" / "Data"
    expansion = game / "corruption" / "Data"
    mod = root / "mod" / "Data"
    for directory in (base, expansion, mod):
        directory.mkdir(parents=True)

    manifest(base, ["Base.meg"])
    write(base / "Base.meg", meg([
        ("DATA\\XML\\SHARED.XML", b"base-archive"),
        ("DATA\\XML\\BASEONLY.XML", b"base-only"),
    ]))
    text(base / "XML" / "BaseLoose.xml", "base-loose")

    manifest(expansion, ["Low.meg", "High.meg"])
    write(expansion / "Low.meg", meg([("DATA\\XML\\ORDER.XML", b"low")]))
    write(expansion / "High.meg", meg([("DATA\\XML\\ORDER.XML", b"high")]))
    text(expansion / "XML" / "Shared.xml", "expansion-loose")

    manifest(mod, ["Mod.meg"])
    write(mod / "Mod.meg", meg([
        ("DATA\\XML\\SHARED.XML", b"mod-archive"),
        ("DATA\\XML\\LOOSE.XML", b"mod-archive-shadowed"),
    ]))
    text(mod / "XML" / "Loose.xml", "mod-loose")

    report = root / "precedence.json"
    result = report_for(asset_scan, "remake", game, report, repo, mod)
    check(result.returncode == 0, f"synthetic precedence scan failed: {result.stdout}{result.stderr}")
    precedence = load_report(report)
    assert_report_shape(precedence)
    winners = {item["path"]: item for item in precedence["winners"]}
    check(winners["data/xml/shared.xml"]["source_id"].endswith("mod:Data/Mod.meg"), "mod archive did not beat expansion loose/base archive")
    check(winners["data/xml/loose.xml"]["origin"] == "loose", "same-layer loose file did not beat archive")
    check(winners["data/xml/loose.xml"]["source_id"].endswith("mod:loose:data/XML/Loose.xml"), "loose winner provenance is wrong")
    check(winners["data/xml/order.xml"]["source_id"].endswith("expansion:Data/High.meg"), "later active archive did not win")
    check(precedence["counts"]["raw_records"] > precedence["counts"]["effective_records"], "raw/effective shadowing was not reported")
    check(any(item["record"]["source_id"].endswith("expansion:Data/Low.meg") for item in precedence["shadowed"]), "shadowed archive provenance is missing")
    findings.append("synthetic three-layer precedence/raw-effective accounting passed")

    # G4: the leaf activates an archive that only its parent supplies.
    leaf = root / "leaf" / "Data"
    manifest(leaf, ["ParentOnly.meg"])
    write(mod / "ParentOnly.meg", meg([("DATA/ART/MODELS/SHELL.ALO", b"parent shell")]))
    chain_report = root / "mod-chain.json"
    result = report_for(asset_scan, "remake", game, chain_report, repo,
                        Path(str(leaf.parent) + ";" + str(mod)))
    check(result.returncode == 0, f"mod chain scan failed: {result.stdout}{result.stderr}")
    chain = load_report(chain_report)
    winners = {item["path"]: item for item in chain["winners"]}
    check(winners["data/art/models/shell.alo"]["source_id"] == "mod-parent-1:Data/ParentOnly.meg",
          "parent-only archive did not retain supplying-layer provenance")
    check(not chain["layers"][0]["missing_declared_archives"], "inherited archive still reported missing")
    check(winners["data/xml/shared.xml"]["source_id"] == "mod-parent-1:Data/Mod.meg",
          "parent did not precede expansion")
    findings.append("vfs_mod_chain CLI accepts leaf;parent and resolves parent-only archives")

    # A malformed discovery-only archive must remain visible as its own probe
    # result and must not be silently omitted from metadata.
    eaw = root / "eaw"
    eaw.mkdir()
    manifest(eaw, ["Good.meg"])
    write(eaw / "Good.meg", meg([("DATA\\XML\\GOOD.XML", b"good")]))
    write(eaw / "InactiveBad.meg", malformed_header())
    inactive_report = root / "inactive.json"
    inactive = report_for(asset_scan, "eaw", eaw, inactive_report, repo)
    check(inactive.returncode != 0, "scanner unexpectedly hid malformed discovery-only archive")
    inactive_data = load_report(inactive_report)
    assert_report_shape(inactive_data)
    bad_rows = [row for row in inactive_data["archive_probes"] if row["source_id"].endswith("InactiveBad.meg")]
    check(len(bad_rows) == 1 and bad_rows[0].get("active") is False, "inactive malformed archive is not individually reported")
    check(bad_rows[0].get("error", {}).get("code") == "EAWR-VFS-0005", "inactive malformed archive has wrong diagnostic")
    findings.append("inactive malformed archive was individually reported (scanner exit is non-zero)")

    # A missing manifest declaration is metadata, not a fabricated mount or a
    # probe failure, as required for localized optional archives.
    missing_declared = root / "missing-declared"
    missing_declared.mkdir()
    manifest(missing_declared, ["Good.meg", "MissingLocalized.meg"])
    write(missing_declared / "Good.meg", meg([("DATA\\XML\\GOOD.XML", b"good")]))
    missing_declared_report = root / "missing-declared.json"
    missing_declared_result = report_for(asset_scan, "eaw", missing_declared, missing_declared_report, repo)
    check(missing_declared_result.returncode == 0, "missing declared archive incorrectly failed the scan")
    missing_declared_data = load_report(missing_declared_report)
    declared_missing = [item for layer in missing_declared_data["layers"] for item in layer["missing_declared_archives"]]
    check(any(item.endswith("MissingLocalized.meg") for item in declared_missing), "missing declared archive was not reported in layer metadata")
    check(not missing_declared_data["errors"], "missing declared archive became a probe/runtime error")
    findings.append("missing declared archive stayed metadata-only and did not fabricate a mount")

    # Active malformed archives and hostile logical names must fail closed.
    malformed_cases = {
        "short.meg": (malformed_header(), "EAWR-VFS-0005"),
        "index.meg": (malformed_name_index(), "EAWR-VFS-0007"),
        "bounds.meg": (malformed_bounds(), "EAWR-VFS-0006"),
        "duplicate.meg": (malformed_duplicate(), "EAWR-VFS-0009"),
        "count-limit.meg": (malformed_count(), "EAWR-VFS-0008"),
        "name-table.meg": (malformed_name_table(), "EAWR-VFS-0005"),
        "entry-table.meg": (malformed_entry_table(), "EAWR-VFS-0005"),
        "traversal.meg": (malformed_path("../escape.xml"), "EAWR-VFS-0007"),
        "absolute.meg": (malformed_path("/escape.xml"), "EAWR-VFS-0007"),
        "drive.meg": (malformed_path("C:\\escape.xml"), "EAWR-VFS-0007"),
    }
    for filename, (data, expected_code) in malformed_cases.items():
        case = root / filename.replace(".meg", "")
        case.mkdir()
        manifest(case, [filename])
        write(case / filename, data)
        output = root / f"{filename}.json"
        result = report_for(asset_scan, "eaw", case, output, repo)
        check(result.returncode != 0, f"malformed active archive {filename} was accepted")
        case_report = load_report(output)
        check(expected_code in codes(case_report), f"{filename} did not report {expected_code}: {codes(case_report)}")
    findings.append("active malformed header/index/bounds/duplicate/path archives failed closed with stable codes")

    # Missing roots must be an explicit failure and still produce metadata.
    missing_report = root / "missing.json"
    missing = report_for(asset_scan, "eaw", root / "does-not-exist", missing_report, repo)
    check(missing.returncode != 0, "missing game root unexpectedly succeeded")
    missing_data = load_report(missing_report)
    check("EAWR-VFS-0011" in codes(missing_data), "missing game root lacks mount-invalid diagnostic")
    findings.append("missing-input scan failed explicitly with EAWR-VFS-0011")

    # Manifest selection is structural XML: only direct File children of the
    # intended root activate archives. XML syntax that merely contains tag-like
    # text must not be interpreted as a declaration.
    structural_root = root / "structured-manifest"
    structural_root.mkdir()
    text(
        structural_root / "MegaFiles.xml",
        "<?xml version=\"1.0\"?><Mega_Files>"
        "<!-- <File>Commented.meg</File> -->"
        "<?ignored <File>Instruction.meg</File>?>"
        "<![CDATA[<File>RootCdata.meg</File>]]>"
        "<Group><File>Nested.meg</File></Group>"
        "<file>WrongCase.meg</file>"
        "<File>Good.meg</File>"
        "</Mega_Files>",
    )
    for archive in ("Good.meg", "Commented.meg", "Instruction.meg", "RootCdata.meg", "Nested.meg", "WrongCase.meg"):
        write(structural_root / archive, meg([(f"DATA\\XML\\{archive}.XML", archive.encode("ascii"))]))
    structural_report = root / "structured-manifest.json"
    structural_result = report_for(asset_scan, "eaw", structural_root, structural_report, repo)
    check(structural_result.returncode == 0, f"structured manifest scan failed: {structural_result.stdout}{structural_result.stderr}")
    structural_data = load_report(structural_report)
    structural_active = [item for layer in structural_data["layers"] for item in layer["active_archives"]]
    check(len(structural_active) == 1 and structural_active[0].endswith("Good.meg"),
          "comments, PI, root CDATA, nested or wrong-case File nodes activated archives: " + ",".join(structural_active))
    findings.append("manifest comments/PI/root CDATA/nested and wrong-case File nodes stayed inactive")

    # File text can be split by comments/PI/CDATA and character references are
    # decoded by XML rules before path normalization.
    content_root = root / "manifest-content"
    content_root.mkdir()
    text(
        content_root / "MegaFiles.xml",
        "<Mega_Files><File>Entity&amp;Name.meg</File>"
        "<File>Split<!--ignored--><?ignored value?><![CDATA[Name]]>.meg</File></Mega_Files>",
    )
    write(content_root / "Entity&Name.meg", meg([("DATA\\XML\\ENTITY.XML", b"entity")]))
    write(content_root / "SplitName.meg", meg([("DATA\\XML\\SPLIT.XML", b"split")]))
    content_report = root / "manifest-content.json"
    content_result = report_for(asset_scan, "eaw", content_root, content_report, repo)
    check(content_result.returncode == 0, f"manifest content scan failed: {content_result.stdout}{content_result.stderr}")
    content_data = load_report(content_report)
    content_active = [item for layer in content_data["layers"] for item in layer["active_archives"]]
    check([Path(item).name for item in content_active] == ["Entity&Name.meg", "SplitName.meg"],
          "entity decoding or mixed text/CDATA changed manifest order/content: " + ",".join(content_active))
    findings.append("manifest entities and mixed text/CDATA decoded in document order")

    # XML permits exactly one document element.  A second root must not be
    # silently ignored or treated as another manifest declaration source.
    multiple_root = root / "manifest-multiple-root"
    multiple_root.mkdir()
    write(
        multiple_root / "MegaFiles.xml",
        b"<Mega_Files><File>Good.meg</File></Mega_Files>"
        b"<Mega_Files><File>Unexpected.meg</File></Mega_Files>",
    )
    write(multiple_root / "Good.meg", meg([("DATA\\XML\\GOOD.XML", b"good")]))
    multiple_root_report = root / "manifest-multiple-root.json"
    multiple_root_result = report_for(asset_scan, "eaw", multiple_root, multiple_root_report, repo)
    check(multiple_root_result.returncode != 0, "multiple-root manifest was accepted")
    multiple_root_data = load_report(multiple_root_report)
    check("EAWR-VFS-0010" in codes(multiple_root_data),
          "multiple-root manifest did not produce stable EAWR-VFS-0010: " + repr(codes(multiple_root_data)))
    findings.append("multiple-root manifest was rejected with stable EAWR-VFS-0010")

    # Unicode auto-detection is exercised independently of the UTF-8 fixture
    # writer. Python emits a BOM for this UTF-16 document.
    utf16_root = root / "manifest-utf16"
    utf16_root.mkdir()
    write(utf16_root / "MegaFiles.xml", "<Mega_Files><File>Wide.meg</File></Mega_Files>".encode("utf-16"))
    write(utf16_root / "Wide.meg", meg([("DATA\\XML\\WIDE.XML", b"wide")]))
    utf16_report = root / "manifest-utf16.json"
    utf16_result = report_for(asset_scan, "eaw", utf16_root, utf16_report, repo)
    check(utf16_result.returncode == 0, f"UTF-16 manifest scan failed: {utf16_result.stdout}{utf16_result.stderr}")
    utf16_data = load_report(utf16_report)
    utf16_active = [item for layer in utf16_data["layers"] for item in layer["active_archives"]]
    check(len(utf16_active) == 1 and utf16_active[0].endswith("Wide.meg"), "UTF-16 manifest entry was not activated")
    findings.append("BOM-signalled UTF-16 manifest parsed successfully")

    # UTF-32 is in the documented supported Unicode family.  Exercise a
    # positive BOM-signalled case separately from the malformed encodings.
    utf32_root = root / "manifest-utf32"
    utf32_root.mkdir()
    write(utf32_root / "MegaFiles.xml", "<Mega_Files><File>Wide32.meg</File></Mega_Files>".encode("utf-32"))
    write(utf32_root / "Wide32.meg", meg([("DATA\\XML\\WIDE32.XML", b"wide32")]))
    utf32_report = root / "manifest-utf32.json"
    utf32_result = report_for(asset_scan, "eaw", utf32_root, utf32_report, repo)
    check(utf32_result.returncode == 0, f"UTF-32 manifest scan failed: {utf32_result.stdout}{utf32_result.stderr}")
    utf32_data = load_report(utf32_report)
    utf32_active = [item for layer in utf32_data["layers"] for item in layer["active_archives"]]
    check(len(utf32_active) == 1 and utf32_active[0].endswith("Wide32.meg"), "UTF-32 manifest entry was not activated")
    findings.append("BOM-signalled UTF-32 manifest parsed successfully")

    invalid_manifests: dict[str, bytes] = {
        "malformed": b"<Mega_Files><File>Broken.meg</Mega_Files>",
        "wrong-root": b"<Other><File>Wrong.meg</File></Other>",
        "multiple-roots": (b"<Mega_Files><File>Good.meg</File></Mega_Files>"
                           b"<Mega_Files><File>Unexpected.meg</File></Mega_Files>"),
        "nested-file-content": b"<Mega_Files><File>Bad<Part/>.meg</File></Mega_Files>",
        "doctype": (b"<!DOCTYPE Mega_Files SYSTEM 'https://invalid.example/eawr.dtd'>"
                    b"<Mega_Files><File>Good.meg</File></Mega_Files>"),
        "invalid-utf8": b"<Mega_Files><File>Bad\xff.meg</File></Mega_Files>",
        "malformed-utf16": b"\xff\xfe<\x00M\x00e\x00g\x00a\x00",
        "malformed-declaration": (b"<?xml version='1.0' encoding='UTF-8'? >"
                                   b"<Mega_Files><File>Good.meg</File></Mega_Files>"),
        "mismatched-declaration": (b"<?xml version='1.0' encoding='UTF-16'?>"
                                    b"<Mega_Files><File>Good.meg</File></Mega_Files>"),
        "unsupported-encoding": (b"<?xml version='1.0' encoding='ISO-8859-1'?>"
                                 b"<Mega_Files><File>Good.meg</File></Mega_Files>"),
    }
    for name, manifest_bytes in invalid_manifests.items():
        invalid_root = root / f"manifest-{name}"
        invalid_root.mkdir()
        write(invalid_root / "MegaFiles.xml", manifest_bytes)
        write(invalid_root / "Good.meg", meg([("DATA\\XML\\GOOD.XML", b"good")]))
        invalid_report = root / f"manifest-{name}.json"
        invalid_result = report_for(asset_scan, "eaw", invalid_root, invalid_report, repo)
        check(invalid_result.returncode != 0, f"invalid manifest {name} was accepted")
        invalid_data = load_report(invalid_report)
        check("EAWR-VFS-0010" in codes(invalid_data),
              f"invalid manifest {name} lacks stable EAWR-VFS-0010: {codes(invalid_data)}")

    # The parser input is explicitly bounded at 4 MiB.  Keep the over-limit
    # fixture deterministic and only one byte over that documented boundary.
    over_limit_root = root / "manifest-over-limit"
    over_limit_root.mkdir()
    over_limit = b"<Mega_Files><File>Good.meg</File>" + b" " * (4 * 1024 * 1024) + b"</Mega_Files>"
    write(over_limit_root / "MegaFiles.xml", over_limit)
    write(over_limit_root / "Good.meg", meg([("DATA\\XML\\GOOD.XML", b"good")]))
    over_limit_report = root / "manifest-over-limit.json"
    over_limit_result = report_for(asset_scan, "eaw", over_limit_root, over_limit_report, repo)
    check(over_limit_result.returncode != 0, "over-limit manifest was accepted")
    over_limit_data = load_report(over_limit_report)
    check("EAWR-VFS-0010" in codes(over_limit_data),
          "over-limit manifest did not produce stable EAWR-VFS-0010: " + repr(codes(over_limit_data)))
    findings.append("multiple-root, malformed/mismatched declarations, malformed encodings and 4 MiB limit failed closed with stable EAWR-VFS-0010")

    # The production fixture inspector and a separate Python table reader must
    # agree for a synthetic valid archive. This also checks bounded payload read.
    inspect_archive = root / "inspect.meg"
    write(inspect_archive, meg([("DATA\\XML\\INSPECT.XML", b"inspect-payload")]))
    inspected = run([str(vfs_tests), "--inspect-meg", str(inspect_archive), "data/xml/inspect.xml"], repo)
    check(inspected.returncode == 0, f"synthetic inspect failed: {inspected.stdout}{inspected.stderr}")
    check("size=15 header=696e7370" in inspected.stdout, "synthetic inspect returned wrong payload metadata")
    findings.append("independent synthetic MEG writer and production inspector agreed")
    return findings


def parse_direct_member(archive: Path, wanted: str) -> tuple[int, bytes, int]:
    data = archive.read_bytes()
    check(len(data) >= 8, f"archive too short: {archive}")
    filename_count, entry_count = struct.unpack_from("<II", data, 0)
    offset = 8
    names: list[str] = []
    for _ in range(filename_count):
        check(offset + 2 <= len(data), "independent reader hit truncated name length")
        length = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        check(offset + length <= len(data), "independent reader hit truncated name")
        names.append(data[offset : offset + length].decode("latin1"))
        offset += length
    wanted_key = wanted.replace("/", "\\").upper()
    matches: list[tuple[int, bytes]] = []
    for _ in range(entry_count):
        check(offset + 20 <= len(data), "independent reader hit truncated entry table")
        _, _, size, start, name_index = struct.unpack_from("<IIIII", data, offset)
        offset += 20
        check(name_index < len(names), "independent reader saw invalid name index")
        if names[name_index].upper() == wanted_key:
            check(start + size <= len(data), "independent reader saw out-of-bounds payload")
            matches.append((size, data[start : start + size]))
    check(len(matches) == 1, f"expected one exact member {wanted!r}, got {len(matches)}")
    return matches[0][0], matches[0][1], filename_count


def run_real_comparison(vfs_tests: Path, repo: Path) -> list[str]:
    # The installed archives come from EAWR_EAW_GAME_ROOT and EAWR_REMAKE_MOD_ROOT; either may be unset.
    game = os.environ.get("EAWR_EAW_GAME_ROOT")
    mod = os.environ.get("EAWR_REMAKE_MOD_ROOT")
    candidates = []
    if game:
        candidates += [(Path(game) / "GameData/Data/Config.meg", "DATA/XML/GAMECONSTANTS.XML"),
                       (Path(game) / "corruption/Data/Patch2.meg", "DATA/XML/HARDPOINTS_UNDERWORLD.XML")]
    if mod:
        candidates.append((Path(mod) / "Data/Config.meg", "DATA/XML/AI/PLAYERS/PLAYER_NONE.XML"))
    existing = [(archive, logical) for archive, logical in candidates if archive.exists()]
    if not existing:
        return ["UNEXECUTED: no permitted installed MEG archive was available for real-member comparison"]
    findings: list[str] = []
    for archive, logical in existing:
        size, payload, filename_count = parse_direct_member(archive, logical)
        inspected = run([str(vfs_tests), "--inspect-meg", str(archive), logical.replace("/", "\\")], repo)
        check(inspected.returncode == 0, f"vfs_tests real inspect failed for {archive}: {inspected.stdout}{inspected.stderr}")
        match = re.search(r"size=(\d+) header=([0-9a-f]+)", inspected.stdout)
        check(match is not None, f"unexpected vfs_tests output: {inspected.stdout!r}")
        check(int(match.group(1)) == size, f"production size differs for {logical}")
        check(bytes.fromhex(match.group(2)) == payload[:4], f"production header differs for {logical}")

        # megx.py is the permitted repository reader used as a second table
        # interpretation. It matches a suffix, so use the distinctive filename.
        megx = run([sys.executable, str(repo / "tools" / "megx.py"), str(archive), logical.replace("/", "\\")], repo)
        check(megx.returncode == 0, f"megx.py failed for {archive}: {megx.stderr}")
        megx_match = re.search(r"bytes; header: (.+)$", megx.stdout, re.MULTILINE)
        check(megx_match is not None, f"megx.py did not find {logical}: {megx.stdout}")
        check(f"{size} bytes" in megx.stdout, f"megx.py size differs for {logical}")
        expected_header = repr(payload[:4])
        check(expected_header in megx.stdout, f"megx.py header differs for {logical}: {megx.stdout.splitlines()[0]}")
        findings.append(f"real member {logical} ({archive.name}) matched independent reader, megx.py, and vfs_tests; names={filename_count}")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--asset-scan", type=Path, required=True)
    parser.add_argument("--vfs-tests", type=Path, required=True)
    parser.add_argument("--synthetic-only", action="store_true")
    arguments = parser.parse_args()
    repo = arguments.repo_root.resolve()
    asset_scan = arguments.asset_scan.resolve()
    vfs_tests = arguments.vfs_tests.resolve()
    check(asset_scan.exists(), f"asset_scan executable not found: {asset_scan}")
    check(vfs_tests.exists(), f"vfs_tests executable not found: {vfs_tests}")

    findings: list[str] = []
    with tempfile.TemporaryDirectory(prefix="eawr-vfs-acceptance-") as temporary:
        root = Path(temporary)
        findings.extend(run_synthetic(asset_scan, vfs_tests, repo, root))
    if arguments.synthetic_only:
        findings.append("installed archive comparison intentionally skipped by --synthetic-only")
    else:
        findings.extend(run_real_comparison(vfs_tests, repo))
    print("P0-02 supplemental acceptance:")
    for finding in findings:
        print(f"- {finding}")
    defects = [finding for finding in findings if finding.startswith("DEFECT:")]
    unexecuted = [finding for finding in findings if finding.startswith("UNEXECUTED:")]
    if defects:
        print(f"{len(defects)} contract defect(s) detected", file=sys.stderr)
        return 1
    if unexecuted:
        print("No installed archive comparison was executed", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CheckFailure as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
