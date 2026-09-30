"""Synthetic draw cases for the TerrainWater ``t1`` ps_1_1 oracle.

Every texel, vertex and state here is original synthetic data.  The same case list is
consumed by three independent executors:

* ``ps11_eval.py``      - a pure-Python ps_1_1 evaluator written from the public D3D9
                          instruction documentation (no translator imports);
* ``d3d9_oracle.cpp``   - a legacy Direct3D 9 HAL draw with hand-encoded ps.1.1 tokens;
* ``wgpu_harness.py``   - the translated SPIR-V executed through a pinned wgpu.

Screen coordinates use the modern convention: pixel ``(i, j)`` has its centre at
``(i + 0.5, j + 0.5)``.  The D3D9 executor subtracts 0.5 because Direct3D 9 places
pixel centres on integers.  Quads are split along a diagonal that never passes through
a pixel centre, and vertex attributes are generated from a single projective mapping
so both triangles agree.
"""

from __future__ import annotations

FORMAT_HEADER = "EAWR_T1_CASES 1"
RT_SIZE = 16

# Draw kinds.  ``T1`` is the TerrainWater t1 pixel program; ``STAGE1_PLAIN`` is a
# probe program ("tex t1; mov r0, t1") used only to observe stage-1 sampler state
# before and after the effect's cleanup pass.
KIND_T1 = 0
KIND_STAGE1_PLAIN = 1

ADDRESS_WRAP = 1
ADDRESS_CLAMP = 3
FILTER_POINT = 1
FILTER_LINEAR = 2

ZFUNC_LESSEQUAL = 4

# Declared comparison tolerance, in 8-bit output units per channel.
TOLERANCE_LSB = 2


def bump_texel(x: int, y: int) -> tuple[int, int]:
    """Signed (du, dv) in [-127, 127]; asymmetric in x/y and between channels."""
    du = (x * 37 + y * 11 + 3) % 255 - 127
    dv = (x * 13 + y * 71 + 91) % 255 - 127
    return du, dv


def reflection_texel(x: int, y: int) -> tuple[int, int, int, int]:
    return (16 + x * 30, 16 + y * 30, (x * 53 + y * 97) % 256, 255 - (x * 7 + y * 23) % 200)


def fow_texel(x: int, y: int) -> tuple[int, int, int, int]:
    # Alpha deliberately varies: the program's FOW multiply is RGB-only.
    return (64 + x * 40, 64 + y * 40, 255 - x * 20 - y * 30, (x * 50 + y * 13 + 7) % 256)


def probe_texel(x: int, y: int) -> tuple[int, int, int, int]:
    return (x * 60 + 15, y * 60 + 15, 128, 255)


def _texture(fmt: str, size: int, fn) -> dict:
    return {"format": fmt, "width": size, "height": size,
            "texels": [fn(x, y) for y in range(size) for x in range(size)]}


def _stages(filter_mode: int) -> list[dict]:
    return [
        {"address_u": ADDRESS_WRAP, "address_v": ADDRESS_WRAP, "min": filter_mode, "mag": filter_mode},
        # The effect pass sets AddressU[1]/AddressV[1] = CLAMP.
        {"address_u": ADDRESS_CLAMP, "address_v": ADDRESS_CLAMP, "min": filter_mode, "mag": filter_mode},
        {"address_u": ADDRESS_WRAP, "address_v": ADDRESS_WRAP, "min": filter_mode, "mag": filter_mode},
    ]


def _affine(c0: float, cx: float, cy: float):
    return lambda x, y: c0 + cx * x + cy * y


def _vertex(x: float, y: float, z: float, rhw_fn, diffuse, specular, tc0, tc1, tc2) -> dict:
    rhw = rhw_fn(x, y)

    def attr(pair):
        # Projective mapping: attribute * rhw is affine in screen space.
        return [round(fn(x, y) / rhw, 6) for fn in pair]

    def colour(quad):
        return [max(0, min(255, round(fn(x, y) / rhw))) for fn in quad]

    return {"x": x, "y": y, "z": z, "rhw": round(rhw, 6),
            "diffuse": colour(diffuse), "specular": colour(specular),
            "tc0": attr(tc0), "tc1": attr(tc1), "tc2": attr(tc2)}


def _quad(x0, y0, x1, y1, z, rhw_fn, diffuse, specular, tc0, tc1, tc2, clockwise=True) -> list[dict]:
    corners = {name: _vertex(px, py, z, rhw_fn, diffuse, specular, tc0, tc1, tc2)
               for name, (px, py) in {"a": (x0, y0), "b": (x1, y0), "c": (x0, y1), "d": (x1, y1)}.items()}
    # Screen y points down: a->b->c is clockwise on screen.
    tris = [("a", "b", "c"), ("b", "d", "c")]
    if not clockwise:
        tris = [(p, r, q) for p, q, r in tris]
    return [corners[n] for tri in tris for n in tri]


def _projective(rhw_fn, fn):
    # Express a perspective-interpolated attribute through its pre-divided numerator.
    return lambda x, y: fn(x, y) * rhw_fn(x, y)


def _draw(kind: int, vertices: list[dict], zwrite=True, blend=True, cleanup_after=False) -> dict:
    return {"kind": kind, "zfunc": ZFUNC_LESSEQUAL, "zwrite": zwrite, "blend": blend,
            "cleanup_after": cleanup_after, "vertices": vertices}


PERSPECTIVE = _affine(1.0, -0.03, -0.015)
FLAT_W = _affine(1.0, 0.0, 0.0)


def _full_quad(rhw_fn, z=0.5, diffuse=None, specular=None, tc1_span=(0.0, 1.0),
               x0=-1.0, y0=-1.0, x1=17.0, y1=18.0, clockwise=True) -> list[dict]:
    lo, hi = tc1_span
    scale = (hi - lo) / RT_SIZE

    def p(fn):
        return _projective(rhw_fn, fn)

    diffuse = diffuse or (p(_affine(200, 0, 0)), p(_affine(160, 2, 0)), p(_affine(90, 0, 3)), p(_affine(40, 6, 5)))
    specular = specular or tuple(p(_affine(0, 0, 0)) for _ in range(4))
    tc0 = (p(_affine(0.03, 0.061, 0.004)), p(_affine(0.11, -0.007, 0.053)))
    tc1 = (p(_affine(lo, scale, 0.0)), p(_affine(lo + 0.02, 0.003, scale)))
    tc2 = (p(_affine(0.06, 0.029, 0.0)), p(_affine(0.02, 0.0, 0.031)))
    return _quad(x0, y0, x1, y1, z, rhw_fn, diffuse, specular, tc0, tc1, tc2, clockwise)


def build_cases() -> list[dict]:
    textures = [_texture("V8U8", 8, bump_texel), _texture("A8R8G8B8", 8, reflection_texel),
                _texture("A8R8G8B8", 4, fow_texel)]
    probe_textures = [textures[0], _texture("A8R8G8B8", 4, probe_texel), textures[2]]
    asymmetric = [0.25, -0.125, 0.0625, 0.375]  # m00, m01, m10, m11
    cases = []

    def case(name, draws, bumpenv, filter_mode=FILTER_POINT, tex=None, clear=(32, 48, 64, 96), purpose=""):
        cases.append({"name": name, "purpose": purpose, "rt_width": RT_SIZE, "rt_height": RT_SIZE,
                      "clear_rgba": list(clear), "clear_z": 1.0,
                      "textures": tex or textures, "stages": _stages(filter_mode),
                      "bumpenv": bumpenv, "draws": draws})

    case("bem_nonidentity_point", [_draw(KIND_T1, _full_quad(PERSPECTIVE))], asymmetric,
         purpose="asymmetric non-identity bump matrix, perspective-varying rhw, point sampling")
    case("bem_nonidentity_unblended", [_draw(KIND_T1, _full_quad(PERSPECTIVE), blend=False)], asymmetric,
         purpose="same program with blending disabled so RGBA program output is read back directly")
    case("bem_zero_matrix_point", [_draw(KIND_T1, _full_quad(PERSPECTIVE))], [0.0, 0.0, 0.0, 0.0],
         purpose="explicit zero matrix: texbem degenerates to an unperturbed stage-1 lookup")
    case("bem_transposed_probe", [_draw(KIND_T1, _full_quad(FLAT_W))], [0.0, 0.5, 0.0, 0.0],
         purpose="only m01 non-zero: du must perturb v, detecting a transposed matrix")
    case("bem_clamp_edges", [_draw(KIND_T1, _full_quad(PERSPECTIVE, tc1_span=(-0.35, 1.4)))],
         [0.5, 0.0, 0.0, -0.5], purpose="stage-1 coordinates and perturbation beyond [0,1] under CLAMP")
    case("bem_nonidentity_linear", [_draw(KIND_T1, _full_quad(PERSPECTIVE))], asymmetric,
         filter_mode=FILTER_LINEAR, purpose="bilinear filtering on all three stages")

    p = lambda fn: _projective(FLAT_W, fn)
    half = (p(_affine(110, 1, 0)), p(_affine(90, 0, 1)), p(_affine(70, 2, 1)), p(_affine(30, 7, 6)))
    spec = (p(_affine(60, 2, 0)), p(_affine(20, 0, 3)), p(_affine(100, 1, 1)), p(_affine(200, 0, 0)))
    case("specular_add_alpha_fow", [_draw(KIND_T1, _full_quad(FLAT_W, diffuse=half, specular=spec))],
         asymmetric, purpose="non-zero v1 (kept inside [0,1]), varying vertex alpha, RGB-only FOW")

    bright = (p(_affine(250, 0, 0)), p(_affine(230, 1, 0)), p(_affine(240, 0, 1)), p(_affine(90, 8, 4)))
    hot = (p(_affine(200, 3, 0)), p(_affine(180, 0, 4)), p(_affine(220, 1, 1)), p(_affine(160, 2, 2)))
    case("range_escape_specular", [_draw(KIND_T1, _full_quad(FLAT_W, diffuse=bright, specular=hot), blend=False)],
         asymmetric, purpose="v0*t1+v1 exceeds 1: result depends on PixelShader1xMaxValue")

    front = _full_quad(PERSPECTIVE, z=0.3, x0=-1.0, y0=-1.0, x1=9.0, y1=18.0)
    behind = _full_quad(PERSPECTIVE, z=0.7)
    equal = _full_quad(PERSPECTIVE, z=0.3, x0=5.0, y0=-1.0, x1=17.0, y1=9.0,
                       diffuse=(p(_affine(255, 0, 0)), p(_affine(255, 0, 0)), p(_affine(255, 0, 0)), p(_affine(180, 0, 0))))
    case("depth_lessequal_overlap",
         [_draw(KIND_T1, front), _draw(KIND_T1, behind), _draw(KIND_T1, equal)], asymmetric,
         purpose="overlapping draws: farther rejected, equal depth accepted, depth written, alpha blended")

    cw = _full_quad(FLAT_W, x0=-1.0, y0=-1.0, x1=17.0, y1=7.0)
    ccw = _full_quad(FLAT_W, x0=-1.0, y0=9.0, x1=17.0, y1=18.0, clockwise=False)
    case("cull_ccw_baseline", [_draw(KIND_T1, cw + ccw)], asymmetric,
         purpose="renderer-baseline CULL_CCW: counter-clockwise triangles are discarded")

    left = _full_quad(FLAT_W, x0=-1.0, y0=-1.0, x1=8.0, y1=18.0, tc1_span=(-0.5, 2.5))
    right = _full_quad(FLAT_W, x0=8.0, y0=-1.0, x1=17.0, y1=18.0, tc1_span=(-0.5, 2.5))
    case("cleanup_restores_wrap",
         [_draw(KIND_T1, _full_quad(FLAT_W, z=0.9), cleanup_after=False),
          _draw(KIND_STAGE1_PLAIN, left, zwrite=False, blend=False, cleanup_after=True),
          _draw(KIND_STAGE1_PLAIN, right, zwrite=False, blend=False)],
         asymmetric, tex=probe_textures,
         purpose="stage 1 samples CLAMP during the pass and WRAP after the t1_cleanup pass")
    return cases


def _fmt(value) -> str:
    if isinstance(value, float):
        return repr(round(value, 6))
    return str(value)


def to_text(cases: list[dict]) -> str:
    """Whitespace-separated serialization read by ``d3d9_oracle.cpp`` via fscanf."""
    out = [FORMAT_HEADER, str(len(cases))]
    formats = {"V8U8": 0, "A8R8G8B8": 1}
    for item in cases:
        out.append(item["name"])
        out.append(f"{item['rt_width']} {item['rt_height']}")
        out.append(" ".join(map(_fmt, item["clear_rgba"])) + " " + _fmt(item["clear_z"]))
        for texture in item["textures"]:
            out.append(f"{formats[texture['format']]} {texture['width']} {texture['height']}")
            out.append(" ".join(" ".join(map(str, texel)) for texel in texture["texels"]))
        for stage in item["stages"]:
            out.append(f"{stage['address_u']} {stage['address_v']} {stage['min']} {stage['mag']}")
        out.append(" ".join(_fmt(float(v)) for v in item["bumpenv"]))
        out.append(str(len(item["draws"])))
        for draw in item["draws"]:
            out.append(f"{draw['kind']} {draw['zfunc']} {int(draw['zwrite'])} {int(draw['blend'])} "
                       f"{int(draw['cleanup_after'])} {len(draw['vertices'])}")
            for v in draw["vertices"]:
                fields = [v["x"], v["y"], v["z"], v["rhw"], *v["diffuse"], *v["specular"],
                          *v["tc0"], *v["tc1"], *v["tc2"]]
                out.append(" ".join(_fmt(float(f)) if isinstance(f, float) else str(f) for f in fields))
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    import sys
    sys.stdout.write(to_text(build_cases()))
