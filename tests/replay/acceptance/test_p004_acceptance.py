#!/usr/bin/env python3
"""Independent P0-04 acceptance probes (temporary adversarial inputs only).

Registered in root CTest, this checks the frozen
fixture and command-line boundary without regenerating or changing any checked-in
fixture.  The C++ contract executable remains the API-level probe; this script
adds independent byte, CLI, comparator, and hosted-artifact checks.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


ORIGINAL_SHA256 = "fcf7f050a4ae8be540def3e4609fc7ff68a21e4ed4fce59e46c1fc62e59a4f90"
ZERO_SHA256 = "f92cda7a6f86d6a321a1f0a8858b35318959d54b6f35a6457f353d0dbeab9345"
ORIGINAL_BYTES = 1480
ZERO_BYTES = 96
TARGETS = (
    "windows-msvc",
    "windows-clang",
    "linux-x64-gcc",
    "linux-x64-clang",
    "linux-arm64-gcc",
)
WORKERS = (1, 2, 4)
REPLAY_CONTRACTS = {
    "replay_contracts",
    "sim_headless_replay_contract",
    "replay_hash_comparator_contracts",
}


def fail(message: str) -> "NoReturn":
    raise AssertionError(message)


def invoke(program: pathlib.Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(program), *args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def invoke_comparator(
    comparator: pathlib.Path,
    root: pathlib.Path,
    audit: pathlib.Path,
    targets: tuple[str, ...],
) -> subprocess.CompletedProcess[str]:
    arguments = [
        sys.executable,
        str(comparator),
        "--evidence-root",
        str(root),
        "--audit",
        str(audit),
    ]
    for target in targets:
        arguments.extend(("--target", target))
    return subprocess.run(
        arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False
    )


def write_replay(root: pathlib.Path, name: str, payload: bytes) -> pathlib.Path:
    path = root / name
    path.write_bytes(payload)
    return path


def run_cli_case(
    program: pathlib.Path,
    root: pathlib.Path,
    payload: bytes,
    name: str,
    *arguments: str,
) -> tuple[int, str, bool]:
    replay = write_replay(root, f"{name}.eawr-replay", payload)
    output = root / f"{name}.csv"
    completed = invoke(
        program,
        "--replay",
        str(replay),
        "--hash-out",
        str(output),
        *arguments,
    )
    return completed.returncode, completed.stdout, output.exists()


def expected_rows(audit: pathlib.Path) -> str:
    data = json.loads(audit.read_text(encoding="utf-8"))
    rows = data["ticks"]
    return "tick,sha256\n" + "".join(
        f"{row['completed_tick']},{row['state_sha256']}\n" for row in rows
    )


def check_frozen_fixtures(fixtures: pathlib.Path) -> tuple[bytes, bytes]:
    original = (fixtures / "original-v1.eawr-replay").read_bytes()
    zero = (fixtures / "zero-tick-v1.eawr-replay").read_bytes()
    if len(original) != ORIGINAL_BYTES or hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        fail("original-v1.eawr-replay is not the frozen 1480-byte fixture")
    if len(zero) != ZERO_BYTES or hashlib.sha256(zero).hexdigest() != ZERO_SHA256:
        fail("zero-tick-v1.eawr-replay is not the frozen 96-byte fixture")
    return original, zero


def check_cli(program: pathlib.Path, fixtures: pathlib.Path, audit: pathlib.Path) -> None:
    original, zero = check_frozen_fixtures(fixtures)
    expected = expected_rows(audit)
    with tempfile.TemporaryDirectory(prefix="p004-acceptance-") as temporary:
        root = pathlib.Path(temporary)
        outputs: list[bytes] = []
        for workers in (*WORKERS, 8):
            code, output, exists = run_cli_case(
                program, root, original, f"valid-{workers}", "--workers", str(workers)
            )
            if code != 0 or not exists:
                fail(f"workers={workers} valid replay failed: {output}")
            data = (root / f"valid-{workers}.csv").read_bytes()
            if data != expected.encode("utf-8"):
                fail(f"workers={workers} output differs from independent audit")
            if b"\r" in data or data.startswith(b"\xef\xbb\xbf"):
                fail(f"workers={workers} output is not BOM-free LF CSV")
            outputs.append(data)
        if len(set(outputs)) != 1:
            fail("1/2/4/8 worker CSV outputs differ")

        code, output, exists = run_cli_case(program, root, zero, "zero", "--workers", "4")
        if code != 0 or not exists or (root / "zero.csv").read_bytes() != b"tick,sha256\n":
            fail(f"zero-tick replay is not header-only: {code}: {output}")

        def malformed(name: str, payload: bytes, fragment: str | None = None) -> None:
            code, output, exists = run_cli_case(program, root, payload, name, "--workers", "4")
            if code == 0 or exists or "EAWR-SIM-" not in output:
                fail(f"{name} was accepted or published output: {code}: {output}")
            if fragment is not None and fragment not in output:
                fail(f"{name} diagnostic omitted {fragment!r}: {output}")

        bad = bytearray(original)
        struct.pack_into("<I", bad, 224, 0)
        malformed("length-zero", bytes(bad), "command")
        bad = bytearray(original)
        # Version 2 is supported now; use a version outside the format registry.
        struct.pack_into("<H", bad, 8, 0xFFFF)
        malformed("version", bytes(bad), "version")
        bad = bytearray(original)
        bad[248] = 99
        malformed("unknown-opcode", bytes(bad), "opcode")
        bad = bytearray(original)
        bad[249] = 1
        malformed("flags", bytes(bad), "flags")
        bad = bytearray(original)
        struct.pack_into("<H", bad, 250, 1)
        malformed("reserved", bytes(bad), "flags")
        bad = bytearray(original)
        struct.pack_into("<Q", bad, 228, 5)
        malformed("after-final", bytes(bad), "not less than final")
        malformed("truncated", original[:-1], "length")
        malformed("trailing", original + b"\0", "trailing")
        bad = bytearray(original)
        struct.pack_into("<Q", bad, 56, 21)
        malformed("count", bytes(bad), "command")
        bad = bytearray(original)
        struct.pack_into("<Q", bad, 160, 1)
        malformed("duplicate-initial-id", bytes(bad), "initial entity")
        bad = bytearray(original)
        struct.pack_into("<Q", bad, 252, 999)
        malformed("missing-command-target", bytes(bad), "unknown entity")

        bad = bytearray(original)
        bad[64] ^= 1
        code, output, exists = run_cli_case(program, root, bytes(bad), "content-identity", "--workers", "1")
        if code != 0 or not exists:
            fail(f"opaque content identity mutation was rejected: {output}")
        if (root / "content-identity.csv").read_text(encoding="utf-8") == expected:
            fail("content identity is absent from canonical state hash")

        audit_data = json.loads(audit.read_text(encoding="utf-8"))
        final_command = next(
            item
            for item in audit_data["commands"]
            if item["tick"] == 4 and item["player_id"] == 3 and item["sequence"] == 1
        )
        bad = bytearray(original)
        struct.pack_into("<q", bad, int(final_command["payload_offset"]) + 8, -46)
        code, output, exists = run_cli_case(program, root, bytes(bad), "future-command", "--workers", "1")
        if code != 0 or not exists:
            fail(f"future-command mutation was rejected: {output}")
        if (root / "future-command.csv").read_text(encoding="utf-8").splitlines()[1] == expected.splitlines()[1]:
            fail("future command bytes do not contribute to an earlier canonical hash")

        completed = invoke(program, "--replay", str(fixtures / "original-v1.eawr-replay"))
        if completed.returncode == 0 or "EAWR-CORE-0001" not in completed.stdout:
            fail(f"missing --hash-out did not return argument error: {completed.returncode}: {completed.stdout}")
        # Three workers are supported by the bounded thread pool; zero is not.
        completed = invoke(program, "--replay", str(fixtures / "original-v1.eawr-replay"), "--hash-out", str(root / "invalid.csv"), "--workers", "0")
        if completed.returncode == 0 or "EAWR-CORE-0001" not in completed.stdout:
            fail(f"invalid worker count did not return argument error: {completed.stdout}")
        completed = invoke(program, "--replay", str(fixtures / "original-v1.eawr-replay"), "--hash-out", str(root), "--workers", "1")
        if completed.returncode != 4 or "EAWR-SIM-CLI-0001" not in completed.stdout:
            fail(f"unwritable hash destination did not return I/O error: {completed.returncode}: {completed.stdout}")


def write_lf_text(path: pathlib.Path, value: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(value)


def write_evidence(root: pathlib.Path, target: str, rows: str) -> None:
    directory = root / f"replay-evidence-{target}"
    directory.mkdir(parents=True, exist_ok=True)
    write_lf_text(directory / "target.txt", target + "\n")
    for workers in WORKERS:
        write_lf_text(directory / f"workers-{workers}.csv", rows)


def check_comparator(comparator: pathlib.Path, audit: pathlib.Path) -> None:
    rows = "tick,sha256\n1," + "a" * 64 + "\n2," + "b" * 64 + "\n"
    with tempfile.TemporaryDirectory(prefix="p004-comparator-") as temporary:
        root = pathlib.Path(temporary)
        audit_path = root / "audit.json"
        audit_path.write_text(
            json.dumps({"ticks": [{"completed_tick": 1, "state_sha256": "a" * 64}, {"completed_tick": 2, "state_sha256": "b" * 64}]}),
            encoding="utf-8",
        )
        targets = ("alpha", "beta")
        for target in targets:
            write_evidence(root, target, rows)
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode != 0:
            fail(f"valid comparator evidence rejected: {result.stdout}")

        # Required outputs cannot be substituted by duplicate, unknown, aliased,
        # or nested target names.
        (root / "replay-evidence-beta" / "workers-4.csv").unlink()
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode == 0 or "missing required result" not in result.stdout:
            fail("missing worker result was accepted")
        write_evidence(root, "beta", rows)

        shutil.copytree(root / "replay-evidence-beta", root / "replay-evidence-beta-copy")
        shutil.rmtree(root / "replay-evidence-beta")
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode == 0 or "missing target identity" not in result.stdout:
            fail("duplicate/renamed target was accepted as required beta")
        write_evidence(root, "beta", rows)

        shutil.rmtree(root / "replay-evidence-beta")
        write_evidence(root, "rogue", rows)
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode == 0 or "missing target identity" not in result.stdout:
            fail("unknown target substituted for required beta")
        shutil.rmtree(root / "replay-evidence-rogue")
        beta = root / "replay-evidence-beta"
        beta.mkdir(parents=True)
        (beta / "nested").mkdir()
        for workers in WORKERS:
            (beta / "nested" / f"workers-{workers}.csv").write_text(rows, encoding="utf-8")
        (beta / "target.txt").write_text("beta\n", encoding="utf-8")
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode == 0 or "missing required result" not in result.stdout:
            fail("nested worker paths were accepted as direct results")
        write_evidence(root, "beta", rows)

        (root / "replay-evidence-beta" / "target.txt").write_text("alpha\n", encoding="utf-8")
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode == 0 or "target identity mismatch" not in result.stdout:
            fail("aliased target identity was accepted")
        write_evidence(root, "beta", rows)

        write_lf_text(
            root / "replay-evidence-beta" / "workers-2.csv",
            "tick,sha256\n1," + "a" * 64 + "\n2," + "c" * 64 + "\n",
        )
        result = invoke_comparator(comparator, root, audit_path, targets)
        if result.returncode == 0 or "first divergent tick 2" not in result.stdout:
            fail("corrupt evidence did not report first divergent tick")
        write_evidence(root, "beta", rows)

        result = subprocess.run(
            [sys.executable, str(comparator), "--evidence-root", str(root), "--audit", str(audit_path), "--target", "alpha", "--target", "alpha"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if result.returncode == 0 or "duplicate target" not in result.stdout:
            fail("duplicate target list was accepted")


def check_hosted(hosted: pathlib.Path, audit: pathlib.Path, comparator: pathlib.Path) -> None:
    for target in TARGETS:
        directory = hosted / f"replay-evidence-{target}"
        if (directory / "target.txt").read_text(encoding="utf-8") != target + "\n":
            fail(f"hosted target identity is wrong for {target}")
        for workers in WORKERS:
            data = (directory / f"workers-{workers}.csv").read_bytes()
            if b"\r" in data or data.startswith(b"\xef\xbb\xbf"):
                fail(f"hosted {target}/{workers} CSV is not LF/BOM-free")
            if data.decode("utf-8") != expected_rows(audit):
                fail(f"hosted {target}/{workers} payload differs from frozen audit")
    result = invoke_comparator(comparator, hosted, audit, TARGETS)
    if result.returncode != 0:
        fail(f"hosted evidence comparator rejected complete matrix: {result.stdout}")

    expected_cases = REPLAY_CONTRACTS
    for junit in sorted(hosted.glob("test-results-*/*.xml")):
        suite = ET.parse(junit).getroot()
        if suite.attrib.get("tests") != "15" or suite.attrib.get("failures") != "0" or suite.attrib.get("skipped") != "0" or suite.attrib.get("disabled") != "0":
            fail(f"hosted JUnit gate is not 15/0/0/0: {junit}")
        names = {case.attrib.get("name") for case in suite.findall("testcase")}
        if not expected_cases.issubset(names):
            fail(f"hosted JUnit omits replay cases: {junit}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--fixtures", type=pathlib.Path, required=True)
    parser.add_argument("--audit", type=pathlib.Path, required=True)
    parser.add_argument("--comparator", type=pathlib.Path, required=True)
    parser.add_argument("--hosted-root", type=pathlib.Path)
    args = parser.parse_args()
    try:
        check_cli(args.program, args.fixtures, args.audit)
        check_comparator(args.comparator, args.audit)
        if args.hosted_root is not None:
            check_hosted(args.hosted_root, args.audit, args.comparator)
    except (AssertionError, OSError, ValueError, ET.ParseError) as error:
        print(f"P0-04 independent acceptance FAILED: {error}", file=sys.stderr)
        return 1
    print("P0-04 independent acceptance passed: frozen bytes, CLI/API boundary, comparator, and hosted matrix")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
