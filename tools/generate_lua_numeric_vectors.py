#!/usr/bin/env python3
"""Generate binary64 reference data for the authoritative Lua numeric profile.

Offline tool (not run by CTest). Expected results come from the dev box's
hardware binary64 through numpy/CPython (x86-64 SSE2, round to nearest even,
no flush-to-zero) and CPython's correctly rounded float parsing and
formatting. Every NaN result is the profile's canonical quiet NaN
0xFFF8000000000000 (the x86-64 indefinite value the hardware produces for
invalid operations); IEEE 754 leaves payload propagation open. Integer conversion and Q24 boundary expectations are computed
from their specifications with exact integers and Fractions.

Writes tests/script/numeric/vectors.txt (explicit edge cases) and
tests/script/numeric/sequence-manifest.json (SHA-256 digests of long
SplitMix64-driven sequences, replayed by lua_numeric_tests without floats).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np

MASK64 = (1 << 64) - 1
SIGN = 0x8000000000000000
EXPONENT = 0x7FF0000000000000
FRACTION = 0x000FFFFFFFFFFFFF
QUIET = 0x0008000000000000
DEFAULT_NAN = 0xFFF8000000000000
INT64_MIN = -(1 << 63)

SPECIALS = [
    0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
    0xFFF8000000000000, 0x7FF8000000000000, 0x7FF8000000000001, 0x7FF0000000000001,
    0xFFF4000000000000, 0x0000000000000001, 0x8000000000000001, 0x000FFFFFFFFFFFFF,
    0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x3FF0000000000000,
    0xBFF0000000000000, 0x3FE0000000000000, 0x4000000000000000, 0x4008000000000000,
    0x3FB999999999999A, 0x4340000000000000, 0x43E0000000000000, 0xC3E0000000000000,
]

SEEDS = {
    "arithmetic": 0x5EED0246A0000001,
    "compare": 0x5EED0246A0000002,
    "integer": 0x5EED0246A0000003,
    "q24": 0x5EED0246A0000004,
    "parse": 0x5EED0246A0000005,
    "format": 0x5EED0246A0000006,
}

COUNTS = {
    "arithmetic": 1_000_000,
    "compare": 250_000,
    "integer": 250_000,
    "q24": 250_000,
    "parse": 200_000,
    "format": 50_000,
}

FORMATS = ["%.14g", "%.17g", "%.3f", "%.12e", "%g", "%.0f", "%#.3g", "%+.5e", "%.20f", "%.1f"]


class SplitMix64:
    def __init__(self, seed: int):
        self.state = seed & MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)


def to_float(bits: int) -> float:
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def to_bits(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def is_nan(bits: int) -> bool:
    return (bits & ~SIGN & MASK64) > EXPONENT


def signed(value: int) -> int:
    return value - (1 << 64) if value & SIGN else value


def draw_operand(rng: SplitMix64) -> int:
    selector = rng.next() & 15
    x = rng.next()
    sign = x & SIGN
    fraction = x & FRACTION
    field = (x >> 52) & 0x7FF
    if selector <= 4:
        return x
    if selector == 5:
        return SPECIALS[x % len(SPECIALS)]
    if selector == 6:
        return to_bits(float((x % 2001) - 1000))
    if selector == 7:
        return sign | fraction
    if selector in (8, 9):
        return sign | ((1013 + (field & 15)) << 52) | fraction
    if selector == 10:
        return sign | ((2046 - (field & 15)) << 52) | fraction
    if selector == 11:
        return sign | ((1 + (field & 15)) << 52) | fraction
    if selector == 12:
        return sign | ((1013 + (field & 31)) << 52) | (fraction & 0x000FF00000000000)
    return sign | ((899 + field % 248) << 52) | fraction


def draw_pair(rng: SplitMix64) -> tuple[int, int]:
    a = draw_operand(rng)
    mode = rng.next() & 3
    if mode == 0:
        return a, a ^ (rng.next() & 0x3F)
    if mode == 1:
        b = draw_operand(rng)
        shift = rng.next() % 5
        exponent = (((a >> 52) & 0x7FF) + shift - 2) & 0x7FF
        return a, (b & ~EXPONENT & MASK64) | (exponent << 52)
    return a, draw_operand(rng)


def canonical(bits: int) -> int:
    return DEFAULT_NAN if is_nan(bits) else bits


def hardware(op: str, a_bits: np.ndarray, b_bits: np.ndarray | None) -> np.ndarray:
    a = a_bits.view(np.float64)
    with np.errstate(all="ignore"):
        if op == "sqrt":
            result = np.sqrt(a)
        else:
            b = b_bits.view(np.float64)
            result = {"add": np.add, "sub": np.subtract, "mul": np.multiply, "div": np.divide}[op](a, b)
    return result.view(np.uint64).copy()


def binary_results(op: str, pairs: list[tuple[int, int]]) -> list[int]:
    if not pairs:
        return []
    a = np.array([p[0] for p in pairs], dtype=np.uint64)
    b = np.array([p[1] for p in pairs], dtype=np.uint64)
    return [canonical(value) for value in hardware(op, a, b).tolist()]


def sqrt_results(values: list[int]) -> list[int]:
    if not values:
        return []
    return [canonical(value) for value in hardware("sqrt", np.array(values, dtype=np.uint64), None).tolist()]


def compare_flags(a: int, b: int) -> int:
    x, y = to_float(a), to_float(b)
    return int(x == y) | (int(x < y) << 1) | (int(x <= y) << 2)


def truncate_int64(bits: int) -> int:
    value = to_float(bits)
    if math.isnan(value) or math.isinf(value) or not (-(2.0**63) <= value < 2.0**63):
        return INT64_MIN
    return int(value)


def round_half_even(value: Fraction) -> int:
    floor = value.numerator // value.denominator
    rest = value - floor
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and floor % 2 == 1):
        return floor + 1
    return floor


def to_q24(bits: int) -> tuple[int, int]:
    value = to_float(bits)
    if math.isnan(value) or math.isinf(value):
        return 2, 0
    exact = Fraction(value) * (1 << 24)
    raw = round_half_even(exact)
    if raw < INT64_MIN or raw > (1 << 63) - 1:
        return 3, 0
    return (0 if raw == exact else 1), raw


def from_q24(raw: int) -> int:
    return to_bits(float(Fraction(raw, 1 << 24)))


def le64(value: int) -> bytes:
    return struct.pack("<Q", value & MASK64)


# ------------------------------------------------------------------ sequences


def arithmetic_sequence() -> tuple[str, int]:
    rng = SplitMix64(SEEDS["arithmetic"])
    ops: list[int] = []
    operands: list[tuple[int, int]] = []
    for _ in range(COUNTS["arithmetic"]):
        op = rng.next() % 5
        ops.append(op)
        operands.append((draw_operand(rng), 0) if op == 4 else draw_pair(rng))
    results = [0] * len(ops)
    for op_index, name in enumerate(("add", "sub", "mul", "div")):
        positions = [i for i, op in enumerate(ops) if op == op_index]
        for position, value in zip(positions, binary_results(name, [operands[i] for i in positions])):
            results[position] = value
    positions = [i for i, op in enumerate(ops) if op == 4]
    for position, value in zip(positions, sqrt_results([operands[i][0] for i in positions])):
        results[position] = value
    digest = hashlib.sha256()
    for op, value in zip(ops, results):
        digest.update(bytes([op]) + le64(value))
    return digest.hexdigest(), len(ops)


def compare_sequence() -> tuple[str, int]:
    rng = SplitMix64(SEEDS["compare"])
    digest = hashlib.sha256()
    for _ in range(COUNTS["compare"]):
        a, b = draw_pair(rng)
        digest.update(bytes([compare_flags(a, b)]))
    return digest.hexdigest(), COUNTS["compare"]


def integer_sequence() -> tuple[str, int]:
    rng = SplitMix64(SEEDS["integer"])
    digest = hashlib.sha256()
    for _ in range(COUNTS["integer"]):
        shift = rng.next() & 63
        integer = signed(rng.next()) >> shift
        digest.update(le64(to_bits(float(integer))))
        digest.update(le64(truncate_int64(draw_operand(rng))))
    return digest.hexdigest(), COUNTS["integer"]


def q24_sequence() -> tuple[str, int]:
    rng = SplitMix64(SEEDS["q24"])
    digest = hashlib.sha256()
    for _ in range(COUNTS["q24"]):
        status, raw = to_q24(draw_operand(rng))
        digest.update(bytes([status]) + le64(raw))
        shift = rng.next() & 63
        digest.update(le64(from_q24(signed(rng.next()) >> shift)))
    return digest.hexdigest(), COUNTS["q24"]


def decimal_text(rng: SplitMix64) -> str:
    count = 1 + rng.next() % 24
    digits = "".join(str(rng.next() % 10) for _ in range(count))
    point = rng.next() % (count + 1)
    text = digits if point == count else digits[:point] + "." + digits[point:]
    if rng.next() & 1:
        exponent = rng.next() % 700 - 350
        text += ("e", "E")[rng.next() & 1] + str(exponent)
    if rng.next() & 1:
        text = "-" + text
    return text


def parse_sequence() -> tuple[str, int]:
    rng = SplitMix64(SEEDS["parse"])
    digest = hashlib.sha256()
    for _ in range(COUNTS["parse"]):
        digest.update(le64(to_bits(float(decimal_text(rng)))))
    return digest.hexdigest(), COUNTS["parse"]


def format_sequence() -> tuple[str, int]:
    rng = SplitMix64(SEEDS["format"])
    digest = hashlib.sha256()
    for _ in range(COUNTS["format"]):
        bits = draw_operand(rng)
        if (bits & EXPONENT) == EXPONENT:
            bits &= ~0x4000000000000000 & MASK64
        value = to_float(bits)
        for spec in FORMATS:
            digest.update((spec % value).encode("ascii") + b"\n")
        digest.update(shortest(value).encode("ascii") + b"\n")
    return digest.hexdigest(), COUNTS["format"]


def shortest(value: float) -> str:
    for precision in range(1, 18):
        text = "%.*g" % (precision, value)
        if to_bits(float(text)) == to_bits(value):
            return text
    raise AssertionError("no round trip")


# -------------------------------------------------------------- explicit file

# UCRT strtod (the statically linked CRT of x64 FoC): decimal with e/E
# exponents, C99 hexadecimal floats, inf/infinity and nan[(chars)].
SPACE = r"[ \t\n\v\f\r]*"
DECIMAL = re.compile(SPACE + r"([+-]?)((?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)")
HEX = re.compile(SPACE + r"([+-]?)0[xX]((?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?[0-9]+)?)")
INFINITY = re.compile(SPACE + r"([+-]?)(?i:(infinity|inf))")
NOT_A_NUMBER = re.compile(SPACE + r"([+-]?)(?i:nan)(?:\([0-9A-Za-z_]*\))?")


def reference_prefix(text: str) -> tuple[int, int]:
    match = INFINITY.match(text)
    if match:
        return to_bits(float(match.group(1) + "inf")), match.end()
    match = NOT_A_NUMBER.match(text)
    if match:
        return DEFAULT_NAN, match.end()
    match = HEX.match(text)
    if match:
        try:
            value = float.fromhex(match.group(1) + "0x" + match.group(2))
        except OverflowError:
            value = float(match.group(1) + "inf")
        return to_bits(value), match.end()
    match = DECIMAL.match(text)
    if match is None:
        return 0, 0
    return to_bits(float(match.group(1) + match.group(2))), match.end()


PARSE_CASES = [
    "0", "-0", "+0", "1", "-1", "0.5", ".5", "5.", "-.5", "  12  ", "\t\n 7", "1e", "1e+", "1e-2", "1E2",
    "1d2", "1D-2", "2.5d+1", "0x10", "inf", "nan", "infinity", ".", "-", "+", "", "e5", "1..2", "12abc",
    "0X1p4", "0x1.8p1", "0x.8", "-0x10", "0x", "0xg", "0x1p", "0x1p-1074", "0x1p-1075", "0x1.8p-1074",
    "0x1p1024", "0x1.fffffffffffff8p1023", "0x1.fffffffffffff7p1023", "0x123456789abcdef123",
    "0x0.000000000000000000000000001p0", "0x1.00000000000008p0", "0x1.00000000000008000000001p0", "INF",
    "-Infinity", "infinit", "nan(123)", "nan(", "NaN(abc_1)", "-nan", " +inf ", "0x1P+3",
    "9007199254740993", "9007199254740995", "4.9406564584124654e-324", "2.4703282292062327e-324",
    "2.4703282292062328e-324", "2.2250738585072011e-308", "2.2250738585072014e-308",
    "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1e309", "-1e309",
    "1e-400", "-1e-400", "0.1", "0.2", "0.3", "3.14159265358979323846E7", "123456789012345678901234567890",
    "0.000000000000000000000000000000000000001", "1" + "0" * 400 + "e-400",
    "0." + "0" * 330 + "24703282292062327208828439643411068618252990130716238221279284125033775364",
    "1" * 800, "0." + "1" * 900 + "e1", "8.98846567431158e307", "4.35689e-311", "1e22", "1e23", "9e15",
    "1e999999999999999", "1e-999999999999999", "000000000000000000000000001.5", "-0.0e10",
]

FORMAT_CASES = [
    ("%.14g", [0x3FF0000000000000, 0x3FB999999999999A, 0x3EC0000000000000, 0x3E90000000000000,
               0x4340000000000000, 0x4341C37937E08000, 0x44B52D02C7E14AF6, 0x8000000000000000, 0x0000000000000001,
               0x7FEFFFFFFFFFFFFF, 0x3FD5555555555555, 0x4059000000000000, 0xC0FE240C9FBE76C9]),
    ("%.0f", [0x3FE0000000000000, 0x3FF8000000000000, 0x4004000000000000, 0x400C000000000000,
              0xBFE0000000000000, 0x3FE0000000000001]),
    ("%.1f", [0x3FD0000000000000, 0x3FE4000000000000, 0x3FB999999999999A]),
    ("%.2f", [0x3F847AE147AE147B, 0x4000000000000000, 0x7FEFFFFFFFFFFFFF]),
    ("%.3f", [0x400921FB54442D18, 0x0000000000000001]),
    ("%e", [0x0000000000000000, 0x7FEFFFFFFFFFFFFF, 0x0000000000000001]),
    ("%g", [0x0000000000000000, 0x3F1A36E2EB1C432D, 0x3EE4F8B588E368F1, 0x412E848000000000, 0x4415AF1D78B58C40]),
    ("%#g", [0x0000000000000000, 0x3FF0000000000000]),
    ("%#.0e", [0x3FF0000000000000]), ("%#.0f", [0x3FF0000000000000]),
    ("%+.3e", [0x3FF0000000000000, 0xBFF0000000000000]),
    ("% .2f", [0x3FF0000000000000]), ("%010.3f", [0xBFF0000000000000]), ("%-10.3f", [0x3FF0000000000000]),
    ("%10.3e", [0x3FF0000000000000]), ("%010g", [0x3FF0000000000000]), ("%.99f", [0x0000000000000001]),
    ("%G", [0x3E90000000000000]), ("%E", [0x3E90000000000000]), ("%.20g", [0x3FB999999999999A]),
]

SPECIAL_FORMATS = [
    ("%g", 0x7FF0000000000000, "inf"), ("%g", 0xFFF0000000000000, "-inf"), ("%g", 0x7FF8000000000000, "nan"),
    ("%g", 0xFFF8000000000000, "-nan(ind)"), ("%E", 0x7FF0000000000000, "INF"),
    ("%G", 0xFFF8000000000000, "-NAN(IND)"), ("%.14g", 0x7FF0000000000000, "inf"),
    ("%.14g", 0xFFF8000000000000, "-nan(ind)"), ("%+f", 0x7FF0000000000000, "+inf"),
    ("%08.2f", 0xFFF0000000000000, "    -inf"), ("%-6f", 0x7FF8000000000000, "nan   "),
    ("%g", 0x7FF0000000000001, "nan(snan)"), ("%g", 0xFFF8000000000001, "-nan"),
]


def explicit_vectors() -> list[str]:
    lines = ["# Generated by tools/generate_lua_numeric_vectors.py; hexadecimal binary64 bits."]
    pairs = [(a, b) for a in SPECIALS for b in SPECIALS]
    for name in ("add", "sub", "mul", "div"):
        for (a, b), result in zip(pairs, binary_results(name, pairs)):
            lines.append(f"{name} {a:016x} {b:016x} {result:016x}")
    for a, result in zip(SPECIALS, sqrt_results(SPECIALS)):
        lines.append(f"sqrt {a:016x} {result:016x}")
    for a, b in pairs:
        lines.append(f"cmp {a:016x} {b:016x} {compare_flags(a, b)}")
    for a in SPECIALS:
        lines.append(f"f2i {a:016x} {truncate_int64(a)}")
        status, raw = to_q24(a)
        lines.append(f"q24 {a:016x} {status} {raw}")
    for integer in (0, 1, -1, 2**53, 2**53 + 1, 2**53 + 3, 2**63 - 1, INT64_MIN, -(2**53) - 1, 123456789):
        lines.append(f"i2f {integer} {to_bits(float(integer)):016x}")
    for raw in (0, 1, -1, 1 << 24, -(1 << 24), (1 << 63) - 1, INT64_MIN, (1 << 53) + 1, 3 << 22):
        lines.append(f"fromq24 {raw} {from_q24(raw):016x}")
    inventory = Path(__file__).resolve().parents[1] / "plan/inventories/lua-numeric.json"
    foc_literals = list(json.loads(inventory.read_text(encoding="utf-8"))["foc_effective"]["distinct_literals"])
    for text in PARSE_CASES + foc_literals:
        value, consumed = reference_prefix(text)
        lines.append(f"parse {json.dumps(text)} {value:016x} {consumed}")
    for spec, values in FORMAT_CASES:
        for bits in values:
            lines.append(f"format {json.dumps(spec)} {bits:016x} {json.dumps(spec % to_float(bits))}")
    for spec, bits, text in SPECIAL_FORMATS:
        lines.append(f"format {json.dumps(spec)} {bits:016x} {json.dumps(text)}")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1] / "tests/script/numeric")
    args = parser.parse_args()
    with np.errstate(all="ignore"):
        probe = np.array([to_bits(1.0)], dtype=np.uint64).view(np.float64) / np.float64(0.0) * 0.0
    if int(probe.view(np.uint64)[0]) != DEFAULT_NAN:
        print("hardware default NaN is not 0xFFF8000000000000; run on x86-64", file=sys.stderr)
        return 1
    if to_bits(to_float(2) * 0.5) != 1:
        print("flush-to-zero is active", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "vectors.txt").write_text("\n".join(explicit_vectors()) + "\n", encoding="ascii", newline="\n")
    sequences = {}
    for name, function in (("arithmetic", arithmetic_sequence), ("compare", compare_sequence),
                           ("integer", integer_sequence), ("q24", q24_sequence), ("parse", parse_sequence),
                           ("format", format_sequence)):
        digest, count = function()
        sequences[name] = {"seed_hex": f"{SEEDS[name]:016x}", "count": count, "sha256": digest}
        print(f"{name}: {count} {digest}")
    manifest = {
        "schema_version": 1,
        "generator": "tools/generate_lua_numeric_vectors.py",
        "reference": "x86-64 hardware binary64 (numpy/CPython), CPython correctly rounded parse/format; "
                     "every NaN result is the canonical NaN 0xfff8000000000000",
        "numeric_abi": {"name": "eawr-lua-binary64-soft", "version": 1},
        "rng": "SplitMix64, uint64 wrapping",
        "formats": FORMATS + ["shortest"],
        "sequences": sequences,
    }
    (args.out / "sequence-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii",
                                                      newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
