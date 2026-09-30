#!/usr/bin/env python3
"""Regenerate the checked-in Q62 CORDIC constants (requires mpmath==1.3.0)."""

from __future__ import annotations

import mpmath


if mpmath.__version__ != "1.3.0":
    raise SystemExit(f"expected mpmath 1.3.0, found {mpmath.__version__}")

mpmath.mp.dps = 100
SCALE = 1 << 62
ITERATIONS = 48
gain = mpmath.mpf(1)
for index in range(ITERATIONS):
    gain *= mpmath.sqrt(1 + mpmath.power(2, -2 * index))

print(f"gain_inverse_q62={int(mpmath.nint(SCALE / gain))}")
for index in range(ITERATIONS):
    value = mpmath.atan(mpmath.power(2, -index)) / (2 * mpmath.pi)
    print(f"atan_turn_q62[{index}]={int(mpmath.nint(value * SCALE))}")
