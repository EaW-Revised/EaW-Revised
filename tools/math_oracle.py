#!/usr/bin/env python3
"""Independent P0-03 integer/Fraction and pinned-mpmath oracle."""

from __future__ import annotations

import argparse
import csv
import hashlib
import math
import struct
from fractions import Fraction
from pathlib import Path

import mpmath


SCALE = 1 << 24
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
MASK64 = (1 << 64) - 1
SEED = 0xD1CE_B00C_5EED_0024
COUNT = 1_000_000


def splitmix64(state: int) -> tuple[int, int]:
    state = (state + 0x9E3779B97F4A7C15) & MASK64
    value = state
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
    return state, value ^ (value >> 31)


def signed(value: int) -> int:
    return value if value < (1 << 63) else value - (1 << 64)


def nearest_even(numerator: int, denominator: int) -> int:
    if denominator == 0:
        raise ZeroDivisionError
    negative = (numerator < 0) != (denominator < 0)
    numerator, denominator = abs(numerator), abs(denominator)
    quotient, remainder = divmod(numerator, denominator)
    twice = remainder * 2
    if twice > denominator or (twice == denominator and quotient & 1):
        quotient += 1
    return -quotient if negative else quotient


def checked(value: int) -> tuple[int, int]:
    return (0, value) if INT64_MIN <= value <= INT64_MAX else (1, 0)


def rounded_fraction(value: Fraction) -> int:
    return nearest_even(value.numerator, value.denominator)


def evaluate(operation: int, values: list[int]) -> tuple[int, int]:
    a, b, c, d, e, f, g, h = values
    if operation == 0:
        return checked(a + b)
    if operation == 1:
        return checked(a - b)
    if operation == 2:
        return checked(-a)
    if operation == 3:
        return checked(rounded_fraction(Fraction(a * b, SCALE)))
    if operation == 4:
        if b == 0:
            return 2, 0
        return checked(rounded_fraction(Fraction(a * SCALE, b)))
    if operation == 5:
        if b == 0:
            return 2, 0
        return checked(rounded_fraction(Fraction(a * SCALE, b)))
    if operation == 6:
        return checked(rounded_fraction(Fraction(a * b + c * d + e * f, SCALE)))
    return checked(rounded_fraction(Fraction(a * b + c * d + e * f + g * h, SCALE)))


def injected(index: int, operation: int, values: list[int]) -> tuple[int, list[int]]:
    case = index & 4095
    if case == 0:
        return 4, [INT64_MIN, -1, *values[2:]]
    if case == 1:
        return 0, [INT64_MAX, 1, *values[2:]]
    if case == 2:
        return 4, [values[0], 0, *values[2:]]
    if case == 3:
        return 7, [INT64_MIN] * 8
    if case == 4:
        return 7, [INT64_MIN, INT64_MIN, INT64_MIN, INT64_MAX] * 2
    if case == 5:
        return 3, [1, SCALE // 2, *values[2:]]
    if case == 6:
        return 3, [3, SCALE // 2, *values[2:]]
    if case == 7:
        return 3, [-3, SCALE // 2, *values[2:]]
    return operation, values


def sequence_digest() -> str:
    digest = hashlib.sha256()
    state = SEED
    for index in range(COUNT):
        state, word = splitmix64(state)
        operation = word & 7
        values: list[int] = []
        for _ in range(8):
            state, word = splitmix64(state)
            values.append(signed(word))
        operation, values = injected(index, operation, values)
        status, payload = evaluate(operation, values)
        digest.update(struct.pack("<BBq", operation, status, payload))
    return digest.hexdigest()


def nearest_mpf(value: mpmath.mpf) -> int:
    negative = value < 0
    value = abs(value)
    lower = int(mpmath.floor(value))
    fraction = value - lower
    if fraction > mpmath.mpf("0.5") or (fraction == mpmath.mpf("0.5") and lower & 1):
        lower += 1
    return -lower if negative else lower


def write_vectors(path: Path) -> None:
    if mpmath.__version__ != "1.3.0":
        raise SystemExit(f"expected mpmath 1.3.0, found {mpmath.__version__}")
    mpmath.mp.dps = 100
    rows: list[tuple[str, int, int, int, int]] = []
    angles = [
        INT64_MIN, -3 * SCALE // 4, -SCALE // 2, -SCALE // 4, -1, 0, 1,
        SCALE // 8, SCALE // 4, SCALE // 3, SCALE // 2, INT64_MAX,
    ]
    vector_state = 0xA11C_E5E1_24
    for _ in range(128):
        vector_state, word = splitmix64(vector_state)
        angles.append(signed(word))
    for raw in angles:
        turns = mpmath.mpf(raw) / SCALE
        rows.append(("sin", raw, 0, 0, nearest_mpf(mpmath.sin(2 * mpmath.pi * turns) * SCALE)))
        rows.append(("cos", raw, 0, 0, nearest_mpf(mpmath.cos(2 * mpmath.pi * turns) * SCALE)))
    pairs = [
        (1, 1), (1, -1), (-1, 1), (-1, -1), (INT64_MAX, 1),
        (1, INT64_MAX), (INT64_MIN, INT64_MAX), (INT64_MAX, INT64_MIN),
        (1, INT64_MIN), (-1, INT64_MIN),
        (SCALE, 0), (-SCALE, 0), (0, SCALE), (0, -SCALE),
    ]
    for _ in range(128):
        vector_state, y_word = splitmix64(vector_state)
        vector_state, x_word = splitmix64(vector_state)
        y, x = signed(y_word), signed(x_word)
        if x == 0 and y == 0:
            x = 1
        pairs.append((y, x))
    for y, x in pairs:
        turns = mpmath.atan2(y, x) / (2 * mpmath.pi)
        if turns >= mpmath.mpf("0.5"):
            turns -= 1
        raw = nearest_mpf(turns * SCALE)
        if raw >= SCALE // 2:
            raw -= SCALE
        elif raw < -SCALE // 2:
            raw += SCALE
        rows.append(("atan2", y, x, 0, raw))
    vectors = [
        ("norm2x", (3, 4)), ("norm2y", (3, 4)),
        ("norm3x", (INT64_MIN, 1, INT64_MAX)),
        ("norm3y", (INT64_MIN, 1, INT64_MAX)),
        ("norm3z", (INT64_MIN, 1, INT64_MAX)),
        ("norm4x", (INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN)),
    ]
    for name, components in vectors:
        index = {"x": 0, "y": 1, "z": 2}.get(name[-1], 0)
        norm = mpmath.sqrt(sum(mpmath.mpf(value) ** 2 for value in components))
        expected = nearest_mpf(mpmath.mpf(components[index]) * SCALE / norm)
        padded = tuple(components) + (0,) * (3 - len(components))
        rows.append((name, padded[0], padded[1], padded[2], expected))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("operation", "a", "b", "c", "expected_raw"))
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--digest", action="store_true")
    parser.add_argument("--vectors", type=Path)
    args = parser.parse_args()
    if args.digest:
        print(sequence_digest())
    if args.vectors:
        write_vectors(args.vectors)
    if not args.digest and not args.vectors:
        parser.error("select --digest and/or --vectors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
