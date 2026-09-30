"""Independent P0-03 acceptance checks for the public fixed-point API.

This runner deliberately does not import the production oracle or consume its
expected values.  Integer/Fraction cases are derived here, while trig cases
use a separately generated mpmath vector set.  The C++ probe is built in two
independent Windows build trees and is fed only textual raw operands.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import shutil
import struct
import subprocess
import tempfile
from fractions import Fraction
from pathlib import Path

import mpmath


SCALE = 1 << 24
MIN64 = -(1 << 63)
MAX64 = (1 << 63) - 1
MASK64 = (1 << 64) - 1
OVERFLOW = "EAWR-MATH-0001"
DIVZERO = "EAWR-MATH-0002"
NEG_SQRT = "EAWR-MATH-0003"
MALFORMED = "EAWR-MATH-0004"
DECIMAL_LIMIT = "EAWR-MATH-0005"
ZERO_NORM = "EAWR-MATH-0006"
UNDEFINED_ANGLE = "EAWR-MATH-0007"
NON_UNIT = "EAWR-MATH-0008"

# Keep the acceptance mutation fixtures aligned with the comparator's public
# CI identities.  The evidence root is intentionally not treated as an
# anonymous five-directory bag: a strict baseline must name every target.
EXPECTED_EVIDENCE_TARGETS = (
    "math-evidence-windows-msvc",
    "math-evidence-windows-clang",
    "math-evidence-linux-x64-gcc",
    "math-evidence-linux-x64-clang",
    "math-evidence-linux-arm64-gcc",
)


def nearest_even(num: int, den: int) -> int:
    if den == 0:
        raise ZeroDivisionError
    if den < 0:
        num, den = -num, -den
    negative = num < 0
    num = abs(num)
    q, rem = divmod(num, den)
    if rem * 2 > den or (rem * 2 == den and q & 1):
        q += 1
    return -q if negative else q


def checked(value: int) -> int | str:
    return value if MIN64 <= value <= MAX64 else OVERFLOW


def rounded_fraction(value: Fraction) -> int:
    return nearest_even(value.numerator, value.denominator)


def rounded_sqrt(sum_squares: int) -> int:
    q = math.isqrt(sum_squares)
    remainder = sum_squares - q * q
    twice = 4 * remainder - (4 * q + 1)
    if twice > 0 or (twice == 0 and q & 1):
        q += 1
    return q


def rounded_norm_component(component: int, sum_squares: int) -> int:
    if component == 0:
        return 0
    numerator = abs(component) * SCALE
    low, high = 0, SCALE
    while low <= high:
        middle = (low + high) // 2
        if middle * middle * sum_squares <= numerator * numerator:
            low = middle + 1
        else:
            high = middle - 1
    q = high
    relation = 4 * numerator * numerator - (2 * q + 1) ** 2 * sum_squares
    if relation > 0 or (relation == 0 and q & 1):
        q += 1
    return -q if component < 0 else q


def decimal_expected(text: str) -> int | str:
    if len(text) > 4096:
        return DECIMAL_LIMIT
    match = re.fullmatch(r"([+-]?)(?:(\d+)(?:\.(\d*))?|\.(\d+))(?:[eE]([+-]?\d+))?", text)
    if not match:
        return MALFORMED
    sign, whole, fractional, leading_fraction, exponent_text = match.groups()
    digits = (whole or "") + (fractional if fractional is not None else leading_fraction or "")
    if not digits:
        return MALFORMED
    exponent = int(exponent_text or "0")
    numerator = int(digits) * (1 if sign != "-" else -1)
    denominator_power = len(fractional if fractional is not None else leading_fraction or "")
    power = exponent - denominator_power
    if numerator == 0:
        return 0
    # Avoid materialising an exponent-sized integer for adversarial text.
    if power >= 20:
        return OVERFLOW
    scaled_numerator = abs(numerator) * SCALE
    if power < 0 and -power > len(str(scaled_numerator)) + 1:
        return 0
    if power >= 0:
        numerator *= 10**power
        denominator = 1
    else:
        denominator = 10 ** (-power)
    return checked(nearest_even(numerator * SCALE, denominator))


def expected_scalar(op: str, values: list[int]) -> int | str:
    a = values[0]
    b = values[1] if len(values) > 1 else 0
    if op == "add":
        return checked(a + b)
    if op == "sub":
        return checked(a - b)
    if op == "neg":
        return checked(-a)
    if op == "mul":
        return checked(rounded_fraction(Fraction(a * b, SCALE)))
    if op in ("div", "ratio"):
        if b == 0:
            return DIVZERO
        return checked(rounded_fraction(Fraction(a * SCALE, b)))
    if op == "sqrt":
        if a < 0:
            return NEG_SQRT
        return rounded_sqrt(a * SCALE)
    raise AssertionError(op)


def expected_dot(left: list[int], right: list[int]) -> int | str:
    return checked(rounded_fraction(Fraction(sum(a * b for a, b in zip(left, right)), SCALE)))


def expected_cross(left: list[int], right: list[int]) -> list[int] | str:
    values = [
        expected_dot([left[1], left[2]], [right[2], -right[1]]),
        expected_dot([left[2], left[0]], [right[0], -right[2]]),
        expected_dot([left[0], left[1]], [right[1], -right[0]]),
    ]
    for value in values:
        if isinstance(value, str):
            return value
    return values


def parse_result(line: str) -> int | list[int] | str:
    fields = line.split()
    if not fields:
        raise AssertionError("empty probe result")
    if fields[0] == "ERR":
        return fields[1]
    if fields[0] != "OK":
        raise AssertionError(f"bad probe result: {line!r}")
    return [int(value) for value in fields[1:]]


def assert_result(actual: int | list[int] | str, expected: int | list[int] | str, case: str) -> None:
    actual_value = actual[0] if isinstance(actual, list) and len(actual) == 1 else actual
    if actual_value != expected:
        raise AssertionError(f"{case}: actual={actual!r}, expected={expected!r}")


def splitmix64(state: int) -> tuple[int, int]:
    state = (state + 0x9E3779B97F4A7C15) & MASK64
    value = state
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
    return state, value ^ (value >> 31)


def signed(value: int) -> int:
    return value if value < (1 << 63) else value - (1 << 64)


def sequence_values() -> tuple[str, int]:
    manifest = json.loads(Path("tests/math/sequence-manifest.json").read_text(encoding="utf-8"))
    assert manifest["operation_selector"] == "first output & 7"
    assert manifest["rng_outputs_per_operation"] == 9
    assert manifest["operation_count"] == 1_000_000
    assert manifest["operations"] == [
        "checked add", "checked subtract", "checked negate", "Q24 multiply nearest-even",
        "Q24 divide nearest-even", "signed rational constructor nearest-even",
        "three-product Q24 dot with one rounding", "four-product Q24 sum with one rounding",
    ]
    assert manifest["injection"] == (
        "index modulo 4096 cases 0..7 replace operation/operands with division overflow, "
        "add overflow, division by zero, four maximum positive products, 130-bit cancellation, "
        "and positive/negative halfway products"
    )
    assert manifest["record_encoding"] == (
        "10 bytes per operation: uint8 operation, uint8 status (0 success, 1 overflow, "
        "2 division-by-zero), int64 little-endian payload; failure payload is zero"
    )
    digest = hashlib.sha256()
    state = int(manifest["seed_hex"], 16)
    for index in range(manifest["operation_count"]):
        state, word = splitmix64(state)
        operation = word & 7
        values: list[int] = []
        for _ in range(8):
            state, word = splitmix64(state)
            values.append(signed(word))
        case = index & 4095
        if case == 0:
            operation, values[:2] = 4, [MIN64, -1]
        elif case == 1:
            operation, values[:2] = 0, [MAX64, 1]
        elif case == 2:
            operation, values[:2] = 4, [values[0], 0]
        elif case == 3:
            operation, values = 7, [MIN64] * 8
        elif case == 4:
            operation, values = 7, [MIN64, MIN64, MIN64, MAX64] * 2
        elif case in (5, 6, 7):
            operation = 3
            values[:2] = [(1, SCALE // 2), (3, SCALE // 2), (-3, SCALE // 2)][case - 5]
        a, b, c, d, e, f, g, h = values
        if operation == 0:
            result = checked(a + b)
        elif operation == 1:
            result = checked(a - b)
        elif operation == 2:
            result = checked(-a)
        elif operation == 3:
            result = checked(nearest_even(a * b, SCALE))
        elif operation in (4, 5):
            result = DIVZERO if b == 0 else checked(nearest_even(a * SCALE, b))
        elif operation == 6:
            result = checked(nearest_even(a * b + c * d + e * f, SCALE))
        else:
            result = checked(nearest_even(a * b + c * d + e * f + g * h, SCALE))
        status = 0 if isinstance(result, int) else (2 if result == DIVZERO else 1)
        payload = result if isinstance(result, int) else 0
        digest.update(struct.pack("<BBq", operation, status, payload))
    return digest.hexdigest(), manifest["expected_sha256"]


def mp_nearest(value: mpmath.mpf) -> int:
    negative = value < 0
    value = abs(value)
    lower = int(mpmath.floor(value))
    fraction = value - lower
    if fraction > mpmath.mpf("0.5") or (fraction == mpmath.mpf("0.5") and lower & 1):
        lower += 1
    return -lower if negative else lower


def mp_atan2_raw(y: int, x: int) -> int:
    value = mpmath.atan2(y, x) / (2 * mpmath.pi)
    if value >= mpmath.mpf("0.5"):
        value -= 1
    raw = mp_nearest(value * SCALE)
    if raw >= SCALE // 2:
        raw -= SCALE
    if raw < -SCALE // 2:
        raw += SCALE
    return raw


def matrix_values(rotation: list[int], translation: list[int]) -> list[int]:
    x, y, z, w = rotation
    s2 = SCALE * SCALE
    terms = [
        (s2 - 2 * y * y - 2 * z * z), 2 * x * y - 2 * z * w, 2 * x * z + 2 * y * w,
        2 * x * y + 2 * z * w, s2 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * x * w,
        2 * x * z - 2 * y * w, 2 * y * z + 2 * x * w, s2 - 2 * x * x - 2 * y * y,
    ]
    rounded = [nearest_even(value, SCALE) for value in terms]
    return [rounded[0], rounded[1], rounded[2], translation[0],
            rounded[3], rounded[4], rounded[5], translation[1],
            rounded[6], rounded[7], rounded[8], translation[2]]


def run_probe(executable: Path, cases: list[tuple[str, int | list[int] | str]]) -> str:
    payload = "\n".join(case[0] for case in cases) + "\n"
    completed = subprocess.run(
        [str(executable)], input=payload, text=True, encoding="utf-8", capture_output=True, check=False
    )
    if completed.returncode != 0:
        raise AssertionError(f"probe failed ({executable}): {completed.stderr}")
    output = completed.stdout.splitlines()
    if len(output) != len(cases):
        raise AssertionError(f"probe result count {len(output)} != {len(cases)}")
    for index, ((case, expected), line) in enumerate(zip(cases, output)):
        assert_result(parse_result(line), expected, f"line {index} {case}")
    return completed.stdout


def make_cases() -> list[tuple[str, int | list[int] | str]]:
    cases: list[tuple[str, int | list[int] | str]] = []

    def add(op: str, values: list[int], expected: int | list[int] | str) -> None:
        cases.append((op + " " + " ".join(str(value) for value in values), expected))

    def add_decimal(text: str, expected: int | str | None = None) -> None:
        cases.append(("decimal " + text, decimal_expected(text) if expected is None else expected))

    # Every public scalar error and signed boundary has an explicit case.
    add("integer", [0], 0)
    add("integer", [MIN64 // SCALE], MIN64)
    add("integer", [MAX64 // SCALE], (MAX64 // SCALE) * SCALE)
    for op, values in (("add", [MAX64, 1]), ("sub", [MIN64, 1]), ("neg", [MIN64])):
        add(op, values, OVERFLOW)
    add("div", [1, 0], DIVZERO)
    add("sqrt", [-1], NEG_SQRT)
    add("atan2", [0, 0], UNDEFINED_ANGLE)
    add("norm2", [0, 0], ZERO_NORM)
    add("norm3", [0, 0, 0], ZERO_NORM)
    add("norm4", [0, 0, 0, 0], ZERO_NORM)
    add_decimal("", MALFORMED)
    for text in ("+", "-", ".", "1..0", "1e", "1e+", "1e-", "1e1x", " 1", "1 ", "nan", "Infinity", "١"):
        add_decimal(text, MALFORMED)
    add_decimal("1" * 4097, DECIMAL_LIMIT)
    for text in (
        "0", "+0", "-0", "1", "-1", ".5", "1.", "1e+3", "1e-3",
        "999999999999999.0", "1e-999999999", "1e999999999",
        "0.0000000298023223876953125", "-0.0000000298023223876953125",
        "0.0000000894069671630859375", "-0.0000000894069671630859375",
        "0.000000148?",
        "549755813887.999999940395355224609375", "-549755813888",
    ):
        if "?" not in text:
            add_decimal(text)
    add_decimal("549755813887.9999999701976776123046875", OVERFLOW)

    # Deterministic exact scalar arithmetic, including signed negative ties.
    state = 0x9E3779B97F4A7C15
    for _ in range(700):
        state, word = splitmix64(state); a = signed(word)
        state, word = splitmix64(state); b = signed(word)
        for op in ("add", "sub", "mul"):
            add(op, [a, b], expected_scalar(op, [a, b]))
        add("neg", [a], expected_scalar("neg", [a]))
        add("div", [a, b], expected_scalar("div", [a, b]))
        add("ratio", [a, b], expected_scalar("ratio", [a, b]))
    for a, b in ((1, 2), (-1, 2), (3, 2), (-3, 2), (5, 2), (-5, 2), (MIN64, -SCALE), (MIN64, SCALE)):
        add("div", [a, b], expected_scalar("div", [a, b]))
        add("ratio", [a, b], expected_scalar("ratio", [a, b]))

    for value in [0, 1, 2, 3, 4, SCALE, 2 * SCALE, MAX64, MIN64, SCALE * 3 + 1]:
        add("sqrt", [value], rounded_sqrt(value * SCALE) if value >= 0 else NEG_SQRT)
    for _ in range(350):
        state, word = splitmix64(state); value = signed(word) & MAX64
        add("sqrt", [value], rounded_sqrt(value * SCALE))

    # Exact widened dot/cross, length, and full-domain normalisation.
    edge_vectors = [
        [MIN64, MIN64, MIN64], [MIN64, 1, MAX64], [MAX64, -1, MIN64],
        [0, 0, 0], [1, 2, 3], [SCALE, 0, 0],
    ]
    for left in edge_vectors:
        for right in edge_vectors:
            add("dot3", left + right, expected_dot(left, right))
            cross = expected_cross(left, right)
            add("cross", left + right, cross)
    for _ in range(250):
        values: list[int] = []
        for _ in range(6):
            state, word = splitmix64(state); values.append(signed(word))
        left, right = values[:3], values[3:]
        add("dot3", values, expected_dot(left, right))
        add("cross", values, expected_cross(left, right))
    for left, right in [([MIN64] * 4, [MIN64, MAX64, MIN64, MAX64]), ([1, 2], [3, 4])]:
        if len(left) == 4:
            add("dot4", left + right, expected_dot(left, right))
        else:
            add("dot2", left + right, expected_dot(left, right))
    for dimension, op in ((2, "len2"), (3, "len3"), (4, "len4")):
        for values in edge_vectors[:3] if dimension == 3 else ([MIN64] * dimension, [1] * dimension, [0] * dimension):
            sum_squares = sum(value * value for value in values)
            add(op, list(values), checked(rounded_sqrt(sum_squares)))
    for values, op in [([MIN64, 1, MAX64], "norm3"), ([MIN64] * 4, "norm4"), ([3, 4], "norm2")]:
        sum_squares = sum(value * value for value in values)
        expected = [rounded_norm_component(value, sum_squares) for value in values]
        add(op, values, expected)
    # Normalisation is intentionally expensive (widened exact comparisons); these
    # independent full-domain vectors are still distinct from the fixed fixtures.
    for _ in range(24):
        values = []
        for _ in range(4):
            state, word = splitmix64(state); values.append(signed(word))
        sum_squares = sum(value * value for value in values[:3])
        add("norm3", values[:3], [rounded_norm_component(value, sum_squares) for value in values[:3]])

    # Quaternion, matrix, and affine row conventions.
    unit = [0, 0, SCALE, 0]
    add("composeq", unit + unit, [0, 0, 0, -SCALE])
    add("tomatrix", unit + [2 * SCALE, 3 * SCALE, 4 * SCALE], matrix_values(unit, [2 * SCALE, 3 * SCALE, 4 * SCALE]))
    add("tomatrix", [SCALE, SCALE, 0, 0] + [0, 0, 0], NON_UNIT)
    identity = [SCALE, 0, 0, 0, 0, SCALE, 0, 0, 0, 0, SCALE, 0]
    translated = [SCALE, 0, 0, 2 * SCALE, 0, SCALE, 0, 3 * SCALE, 0, 0, SCALE, 4 * SCALE]
    add("transformv", translated + [SCALE, 0, 0], [SCALE, 0, 0])
    add("transformp", translated + [SCALE, 0, 0], [3 * SCALE, 3 * SCALE, 4 * SCALE])
    add("composem", translated + identity, translated)
    add("composem", identity + translated, translated)
    add("composeq", [0, 0, 0, SCALE] + unit, unit)

    # Exact wraps/cardinals plus 600 independent high-precision vectors.
    quarter, half = SCALE // 4, SCALE // 2
    for angle in [MIN64, -2 * SCALE, -SCALE, -half, -quarter, -1, 0, 1, quarter, half, SCALE, MAX64]:
        wrapped = angle % SCALE
        if wrapped >= half: wrapped -= SCALE
        if wrapped < -half: wrapped += SCALE
        add("wrap", [angle], wrapped)
    for angle in [0, quarter, half, 3 * quarter, -quarter, -half, -3 * quarter, SCALE, MAX64, MIN64]:
        turns = mpmath.mpf(angle) / SCALE
        add("sin", [angle], mp_nearest(mpmath.sin(2 * mpmath.pi * turns) * SCALE))
        add("cos", [angle], mp_nearest(mpmath.cos(2 * mpmath.pi * turns) * SCALE))
    trig_state = 0xC0FFEE1234567890
    angles = [MIN64, MAX64, -1, 0, 1, quarter, half, 3 * quarter]
    for _ in range(292):
        trig_state, word = splitmix64(trig_state); angles.append(signed(word))
    for angle in angles:
        turns = mpmath.mpf(angle) / SCALE
        add("sin", [angle], mp_nearest(mpmath.sin(2 * mpmath.pi * turns) * SCALE))
        add("cos", [angle], mp_nearest(mpmath.cos(2 * mpmath.pi * turns) * SCALE))
    pairs = [(0, 0), (0, 1), (0, -1), (1, 0), (-1, 0), (1, MAX64), (MAX64, 1),
             (1, MIN64), (MIN64, 1), (MIN64, MAX64), (MAX64, MIN64)]
    for _ in range(300):
        trig_state, word = splitmix64(trig_state); y = signed(word)
        trig_state, word = splitmix64(trig_state); x = signed(word)
        if x == 0 and y == 0: x = 1
        pairs.append((y, x))
    for y, x in pairs:
        expected = UNDEFINED_ANGLE if (x == 0 and y == 0) else mp_atan2_raw(y, x)
        add("atan2", [y, x], expected)
    return cases


def run_evidence_checks(root: Path, evidence: Path) -> list[str]:
    findings: list[str] = []
    manifest = json.loads((root / "tests/math/sequence-manifest.json").read_text(encoding="utf-8"))
    independent, frozen = sequence_values()
    if independent != frozen:
        findings.append(f"independent sequence digest {independent} != manifest {frozen}")
    digests = sorted(evidence.rglob("digest.txt"))
    vectors = sorted(evidence.rglob("vectors.csv"))
    if len(digests) != 5 or len(vectors) != 5:
        findings.append(f"local five-artifact evidence count is {len(digests)}/{len(vectors)}")
    else:
        digest_values = [path.read_text(encoding="utf-8").strip() for path in digests]
        vector_bytes = [path.read_bytes() for path in vectors]
        if any(value != frozen for value in digest_values): findings.append("five digest artifacts disagree with manifest")
        if any(value != vector_bytes[0] for value in vector_bytes[1:]): findings.append("five vector artifacts differ")
        rows = list(csv.DictReader(vector_bytes[0].decode("utf-8").splitlines()))
        oracle_rows = list(csv.DictReader((root / "tests/math/oracle-vectors.csv").open(encoding="utf-8")))
        if len(rows) != len(oracle_rows): findings.append("five-artifact vector count differs from frozen oracle")
        if len(rows) != 428: findings.append(f"five-artifact approximate vector count is {len(rows)}, expected 428")
        for index, (row, expected) in enumerate(zip(rows, oracle_rows)):
            if any(row.get(key) != expected.get(key) for key in ("operation", "a", "b", "c")):
                findings.append(f"five-artifact identity mismatch at row {index + 2}"); break
            if abs(int(row["actual_raw"]) - int(expected["expected_raw"])) > 4:
                findings.append(f"five-artifact oracle mismatch at row {index + 2}"); break
    comparator = root / "tools/compare_math_evidence.py"
    manifest_path = root / "tests/math/sequence-manifest.json"
    oracle_path = root / "tests/math/oracle-vectors.csv"

    def compare(path: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python", str(comparator), "--evidence-root", str(path),
             "--manifest", str(manifest_path), "--oracle-vectors", str(oracle_path)],
            text=True, capture_output=True, check=False,
        )

    with tempfile.TemporaryDirectory(prefix="p003-compare-") as temporary:
        temporary_root = Path(temporary)

        target_names = list(EXPECTED_EVIDENCE_TARGETS)
        actual_target_names = sorted(path.name for path in evidence.iterdir() if path.is_dir())
        if actual_target_names != sorted(target_names):
            findings.append(
                "comparison mutation audit requires the exact five named source target directories"
            )
            return findings

        def make_case(name: str, mappings: list[tuple[str, str]]) -> Path:
            case_root = temporary_root / name
            case_root.mkdir()
            for target, source in mappings:
                shutil.copytree(evidence / source, case_root / target)
            return case_root

        baseline = make_case("baseline", [(name, name) for name in target_names])
        if compare(baseline).returncode != 0:
            findings.append("comparison rejects the intact five-artifact evidence set")

        missing = make_case("missing", [(name, name) for name in target_names[:4]])
        if compare(missing).returncode == 0:
            findings.append("comparison accepts missing target output")

        duplicate = make_case(
            "duplicate", [(name, name) for name in target_names[:4]]
            + [("math-evidence-windows-msvc-copy", target_names[0])]
        )
        if compare(duplicate).returncode == 0:
            findings.append("comparison accepts five files with a duplicate target identity")

        wrong_name = make_case(
            "wrong-name", [(name, name) for name in target_names[:4]]
            + [("math-evidence-unexpected-target", target_names[4])]
        )
        if compare(wrong_name).returncode == 0:
            findings.append("comparison accepts an unexpected/wrong target identity")

        digest_mismatch = make_case("digest-mismatch", [(name, name) for name in target_names])
        (digest_mismatch / f"{target_names[2]}/digest.txt").write_text("0" * 64 + "\n", encoding="utf-8")
        if compare(digest_mismatch).returncode == 0:
            findings.append("comparison accepts a digest mismatch")

        vector_mismatch = make_case("vector-mismatch", [(name, name) for name in target_names])
        vector_path = vector_mismatch / f"{target_names[3]}/vectors.csv"
        vector_text = vector_path.read_text(encoding="utf-8")
        vector_path.write_text(vector_text.replace("actual_raw", "actual_raw_bogus", 1), encoding="utf-8")
        if compare(vector_mismatch).returncode == 0:
            findings.append("comparison accepts an approximate-vector mismatch")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--msvc", type=Path, required=True)
    parser.add_argument("--clangcl", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, default=Path("out/p003-hosted-evidence"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    mpmath.mp.dps = 120
    cases = make_cases()
    outputs = [run_probe(executable.resolve(), cases) for executable in (args.msvc, args.clangcl)]
    if outputs[0] != outputs[1]:
        raise AssertionError("MSVC and clang-cl independent probe output differs")
    sequence_digest, expected_digest = sequence_values()
    if sequence_digest != expected_digest:
        raise AssertionError(f"independent million-operation digest mismatch: {sequence_digest}")
    for executable in (
        root / "out/build-p003-root-msvc/tests/math/Release/math_million_ops.exe",
        root / "out/build-p003-root-clangcl/tests/math/Release/math_million_ops.exe",
    ):
        if executable.exists():
            produced = subprocess.run([str(executable), "--expected", expected_digest], text=True,
                                       capture_output=True, check=False)
            if produced.returncode != 0 or produced.stdout.strip() != expected_digest:
                raise AssertionError(f"million-operation executable mismatch: {executable}: {produced.stdout} {produced.stderr}")
    findings = run_evidence_checks(root, args.evidence_root.resolve())
    report = root / "out/p003-independent-findings.txt"
    report.write_text("\n".join(findings) if findings else "independent local evidence checks passed\n", encoding="utf-8")
    print(f"independent public API checks passed: {len(cases)} cases on both Windows compilers")
    print(f"independent million-operation digest: {sequence_digest}")
    if findings:
        print("evidence findings:")
        print("\n".join(findings))
        return 2
    print("five local artifacts and 428-row approximate evidence agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
