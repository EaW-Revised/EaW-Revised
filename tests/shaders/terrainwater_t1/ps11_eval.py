"""Independent pure-Python evaluator for the TerrainWater ``t1`` ps_1_1 draw.

Written from the public Direct3D 9 documentation of the pixel-shader 1.x instruction
set and fixed pipeline, not from the translator: this module imports nothing from
``tools.shaders``.  The program is a hand-written instruction list mirroring the
token stream in ``d3d9_oracle.cpp``.  Semantics pinned here:

* ``tex tN``         samples stage N at texture-coordinate set N (non-projected u, v).
* ``texbem tM, tN``  treats ``tN.r``/``tN.g`` as signed (du, dv) and samples stage M at
                     ``u' = uM + BUMPENVMAT00*du + BUMPENVMAT10*dv`` and
                     ``v' = vM + BUMPENVMAT01*du + BUMPENVMAT11*dv`` using stage M's
                     matrix, as the Microsoft ``texbem - ps`` reference states; no
                     luminance term (that is ``texbeml``).  ``convention="transposed"``
                     instead reproduces ``u' = uM + M00*du + M01*dv``,
                     ``v' = vM + M10*du + M11*dv``, which is what the recorded native
                     D3D9 driver readback does for off-diagonal entries.
* ``mad``/``mul``/``mov`` operate per component; a write mask leaves unmasked
  components of the destination unchanged.
* ``v0``/``v1`` are the interpolated diffuse/specular colours, each in [0, 1].
* ``r0`` is saturated to [0, 1] when it becomes the output colour.  Each arithmetic
  result is clamped to +/-``max_value`` (the device's PixelShader1xMaxValue; default
  unbounded, which is what the recorded driver reports).
* Signed ``V8U8`` texels decode as ``max(c / 127, -1)``.
* Rasterisation: modern pixel centres, perspective-correct attributes (weights
  ``lambda_i * rhw_i``), screen-linear depth stored as 24-bit UNORM, CULL_CCW with
  y pointing down.
* Output merger: ZFUNC LESSEQUAL, optional depth write, SRCALPHA/INVSRCALPHA applied
  to all four channels of an 8-bit target, rounded to nearest.
"""

from __future__ import annotations

import math
from typing import Optional

ADDRESS_WRAP = 1
ADDRESS_CLAMP = 3
FILTER_POINT = 1
FILTER_LINEAR = 2

# Pixels whose point-sample coordinate or edge function lies this close to a
# discontinuity are reported as ambiguous instead of compared.
BOUNDARY_EPSILON = 1e-3
DEPTH_MAX = (1 << 24) - 1

T1_PROGRAM = (
    ("tex", ("t", 0), None, ()),
    ("texbem", ("t", 1), None, (("t", 0),)),
    ("tex", ("t", 2), None, ()),
    ("mad", ("r", 0), None, (("v", 0), ("t", 1), ("v", 1))),
    ("mul", ("r", 0), "rgb", (("r", 0), ("t", 2))),
)
STAGE1_PROBE_PROGRAM = (
    ("tex", ("t", 1), None, ()),
    ("mov", ("r", 0), None, (("t", 1),)),
)
PROGRAMS = {0: T1_PROGRAM, 1: STAGE1_PROBE_PROGRAM}
MASKS = {None: (0, 1, 2, 3), "rgb": (0, 1, 2), "a": (3,)}


class Ambiguous(Exception):
    """A sample or coverage decision sits on a discontinuity."""


def decode_texel(texture: dict, index: int) -> tuple[float, float, float, float]:
    texel = texture["texels"][index]
    if texture["format"] == "V8U8":
        du, dv = (max(c / 127.0, -1.0) for c in texel)
        # Missing channels of a two-channel format read as 1.0 (blue, alpha).
        return du, dv, 1.0, 1.0
    return tuple(c / 255.0 for c in texel)


def _address(index: int, size: int, mode: int) -> int:
    if mode == ADDRESS_WRAP:
        return index % size
    if mode == ADDRESS_CLAMP:
        return min(max(index, 0), size - 1)
    raise ValueError(f"unsupported address mode {mode}")


def sample(texture: dict, stage: dict, u: float, v: float, strict: bool) -> tuple[float, ...]:
    width, height = texture["width"], texture["height"]
    x, y = u * width, v * height
    if stage["mag"] != stage["min"]:
        raise ValueError("synthetic cases use one filter per stage")
    if stage["mag"] == FILTER_POINT:
        if strict:
            for coordinate, size, mode in ((x, width, stage["address_u"]), (y, height, stage["address_v"])):
                fraction = coordinate - math.floor(coordinate)
                clamped_edge = mode == ADDRESS_CLAMP and (coordinate <= 0 or coordinate >= size)
                if not clamped_edge and min(fraction, 1 - fraction) < BOUNDARY_EPSILON:
                    raise Ambiguous("point sample on a texel boundary")
        ix = _address(math.floor(x), width, stage["address_u"])
        iy = _address(math.floor(y), height, stage["address_v"])
        return decode_texel(texture, iy * width + ix)
    if stage["mag"] == FILTER_LINEAR:
        x -= 0.5
        y -= 0.5
        x0, y0 = math.floor(x), math.floor(y)
        fx, fy = x - x0, y - y0
        result = [0.0, 0.0, 0.0, 0.0]
        for dx, dy, weight in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)),
                               (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
            ix = _address(x0 + dx, width, stage["address_u"])
            iy = _address(y0 + dy, height, stage["address_v"])
            texel = decode_texel(texture, iy * width + ix)
            for c in range(4):
                result[c] += weight * texel[c]
        return tuple(result)
    raise ValueError(f"unsupported filter {stage['mag']}")


def run_program(program, inputs: dict, textures: list[dict], stages: list[dict],
                bumpenv: list[float], strict: bool, convention: str = "documented",
                max_value: float = math.inf) -> tuple[float, float, float, float]:
    """Execute one pixel.  ``inputs`` holds ``tc0..tc2`` (u, v) and ``v0``/``v1`` RGBA."""
    regs: dict[tuple[str, int], list[float]] = {
        ("v", 0): list(inputs["v0"]), ("v", 1): list(inputs["v1"]),
        ("r", 0): [0.0] * 4, ("r", 1): [0.0] * 4,
    }
    m00, m01, m10, m11 = bumpenv
    if convention == "transposed":
        m01, m10 = m10, m01
    elif convention != "documented":
        raise ValueError(f"unknown bump-matrix convention {convention}")
    for opcode, dst, mask, sources in program:
        if opcode == "tex":
            stage = dst[1]
            u, v = inputs[f"tc{stage}"]
            value = sample(textures[stage], stages[stage], u, v, strict)
        elif opcode == "texbem":
            stage = dst[1]
            du, dv = regs[sources[0]][0], regs[sources[0]][1]
            u, v = inputs[f"tc{stage}"]
            value = sample(textures[stage], stages[stage],
                           u + m00 * du + m10 * dv, v + m01 * du + m11 * dv, strict)
        else:
            args = [regs[source] for source in sources]
            if opcode == "mov":
                value = list(args[0])
            elif opcode == "mul":
                value = [args[0][c] * args[1][c] for c in range(4)]
            elif opcode == "mad":
                value = [args[0][c] * args[1][c] + args[2][c] for c in range(4)]
            else:
                raise ValueError(f"unsupported opcode {opcode}")
            # ps_1_x registers hold at least [-1, 1]; beyond that the device's
            # PixelShader1xMaxValue clamps each written result.
            value = [min(max(c, -max_value), max_value) for c in value]
        target = regs.setdefault(dst, [0.0] * 4)
        for c in MASKS[mask]:
            target[c] = value[c]
    return tuple(min(max(c, 0.0), 1.0) for c in regs[("r", 0)])


def quantize_depth(z: float) -> int:
    """Depth is stored as a 24-bit unsigned-normalized value (D24S8 / depth24plus)."""
    return int(math.floor(min(max(z, 0.0), 1.0) * DEPTH_MAX + 0.5))


def _edge(ax, ay, bx, by, px, py) -> float:
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def _interpolate(values, weights) -> list[float]:
    return [sum(w * value[k] for w, value in zip(weights, values)) for k in range(len(values[0]))]


def render_case(case: dict, strict: bool = True, convention: str = "documented",
                max_value: float = math.inf, affine: frozenset = frozenset(),
                saturate_colours: bool = False) -> tuple[list[Optional[tuple[int, int, int, int]]], int]:
    """Return per-pixel RGBA8 (``None`` where ambiguous) and the ambiguous count.

    ``affine`` names attributes interpolated linearly in screen space and
    ``saturate_colours`` clamps colours after interpolation; both exist only so tests
    can build negative controls that a correct executor must fail to match.
    """
    width, height = case["rt_width"], case["rt_height"]
    colour = [tuple(case["clear_rgba"]) for _ in range(width * height)]
    depth = [quantize_depth(case["clear_z"])] * (width * height)
    ambiguous = [False] * (width * height)
    stages = [dict(stage) for stage in case["stages"]]
    for draw in case["draws"]:
        program = PROGRAMS[draw["kind"]]
        vertices = draw["vertices"]
        for t in range(0, len(vertices), 3):
            a, b, c = vertices[t:t + 3]
            area = _edge(a["x"], a["y"], b["x"], b["y"], c["x"], c["y"])
            if area <= 0:
                # y points down: a positive area is clockwise on screen.  CULL_CCW
                # discards counter-clockwise (non-positive) triangles.
                continue
            for py in range(height):
                for px in range(width):
                    cx, cy = px + 0.5, py + 0.5
                    e = (_edge(b["x"], b["y"], c["x"], c["y"], cx, cy),
                         _edge(c["x"], c["y"], a["x"], a["y"], cx, cy),
                         _edge(a["x"], a["y"], b["x"], b["y"], cx, cy))
                    index = py * width + px
                    if min(e) < -BOUNDARY_EPSILON * area:
                        continue
                    if min(e) < BOUNDARY_EPSILON * area:
                        ambiguous[index] = True
                        continue
                    screen = [value / area for value in e]
                    z = quantize_depth(sum(w * v["z"] for w, v in zip(screen, (a, b, c))))
                    if not z <= depth[index]:
                        continue
                    persp = [w * v["rhw"] for w, v in zip(screen, (a, b, c))]
                    total = sum(persp)
                    persp = [w / total for w in persp]
                    tri = (a, b, c)
                    weights = {name: (screen if name in affine else persp)
                               for name in ("tc0", "tc1", "tc2", "diffuse", "specular")}
                    inputs = {f"tc{s}": _interpolate([v[f"tc{s}"] for v in tri], weights[f"tc{s}"]) for s in range(3)}
                    for register, name in (("v0", "diffuse"), ("v1", "specular")):
                        values = [x / 255.0 for x in _interpolate([v[name] for v in tri], weights[name])]
                        inputs[register] = [min(max(x, 0.0), 1.0) for x in values] if saturate_colours else values
                    try:
                        out = run_program(program, inputs, case["textures"], stages, case["bumpenv"], strict,
                                          convention, max_value)
                    except Ambiguous:
                        ambiguous[index] = True
                        continue
                    if draw["blend"]:
                        alpha = out[3]
                        dst = [channel / 255.0 for channel in colour[index]]
                        out = tuple(out[k] * alpha + dst[k] * (1.0 - alpha) for k in range(4))
                    colour[index] = tuple(int(math.floor(channel * 255.0 + 0.5)) for channel in out)
                    if draw["zwrite"]:
                        depth[index] = z
        if draw["cleanup_after"]:
            stages[1]["address_u"] = ADDRESS_WRAP
            stages[1]["address_v"] = ADDRESS_WRAP
    pixels = [None if ambiguous[i] else colour[i] for i in range(width * height)]
    return pixels, sum(ambiguous)


def compare(expected: list, actual_flat: list[int], tolerance: int) -> dict:
    """Compare evaluator pixels to a flat RGBA8 readback; ambiguous pixels are skipped."""
    worst = 0
    failures = []
    compared = 0
    for index, pixel in enumerate(expected):
        if pixel is None:
            continue
        compared += 1
        got = actual_flat[index * 4:index * 4 + 4]
        delta = max(abs(p - g) for p, g in zip(pixel, got))
        worst = max(worst, delta)
        if delta > tolerance:
            failures.append({"pixel": index, "expected": list(pixel), "actual": list(got)})
    return {"compared": compared, "max_abs_lsb": worst, "failures": failures}
