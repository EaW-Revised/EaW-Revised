#!/usr/bin/env python3
"""Execute the translated TerrainWater t1 SPIR-V stages through a pinned wgpu.

Requires an ignored virtual environment with ``wgpu==0.23.0`` (wgpu-native 25.0.2.1)
and the private outputs of ``python -m tools.shaders.terrainwater_t1``.  Prints one
JSON document; it never writes shader bodies.

``pixel`` mode drives the translated fragment stage with a harness pass-through vertex
stage fed by the synthetic D3D9-style pretransformed vertices from ``cases.py``.
``full`` mode drives the translated vertex *and* fragment stages from object-space
vertices and compares against the evaluator fed with an independently written
restatement of the vertex program.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))

import cases as case_data  # noqa: E402
import ps11_eval  # noqa: E402
from tools.shaders.terrainwater_t1 import bind_external_state  # noqa: E402

import wgpu  # noqa: E402

WGPU_PIN = "0.23.0"
SIZE = case_data.RT_SIZE

PASSTHROUGH_WGSL = """
struct VIn {
  @location(0) pos: vec4f, @location(1) diff: vec4f, @location(2) spec: vec4f,
  @location(3) tc0: vec2f, @location(4) tc1: vec2f, @location(5) tc2: vec2f,
};
struct VOut {
  @builtin(position) p: vec4f,
  @location(0) tc0: vec2f, @location(1) tc1: vec2f, @location(2) tc2: vec2f,
  @location(8) v0: vec4f, @location(9) v1: vec4f,
};
@vertex fn main(i: VIn) -> VOut {
  var o: VOut;
  let w = 1.0 / i.pos.w;
  let ndc = vec2f(i.pos.x / 8.0 - 1.0, 1.0 - i.pos.y / 8.0);
  o.p = vec4f(ndc * w, i.pos.z * w, w);
  o.tc0 = i.tc0; o.tc1 = i.tc1; o.tc2 = i.tc2; o.v0 = i.diff; o.v1 = i.spec;
  return o;
}
"""

FILTER = {case_data.FILTER_POINT: "nearest", case_data.FILTER_LINEAR: "linear"}
ADDRESS = {case_data.ADDRESS_WRAP: "repeat", case_data.ADDRESS_CLAMP: "clamp-to-edge"}
BINDING_ADDRESS = {case_data.ADDRESS_WRAP: "wrap", case_data.ADDRESS_CLAMP: "clamp"}
BINDING_FILTER = {case_data.FILTER_POINT: "point", case_data.FILTER_LINEAR: "linear"}


def binding_for(case: dict, convention: str, max_value: float) -> dict:
    stages = {}
    for index, (texture, stage) in enumerate(zip(case["textures"], case["stages"])):
        entry = {"format": texture["format"], "min_filter": BINDING_FILTER[stage["min"]],
                 "mag_filter": BINDING_FILTER[stage["mag"]], "mip_levels": 1, "mip_filter": "none",
                 "srgb_read": False, "texture_transform": "disable"}
        if index != 1:  # stage 1 addressing is owned by the effect pass (CLAMP)
            entry["address_u"] = BINDING_ADDRESS[stage["address_u"]]
            entry["address_v"] = BINDING_ADDRESS[stage["address_v"]]
        stages[str(index)] = entry
    m00, m01, m10, m11 = case["bumpenv"]
    return {"usage": "synthetic", "fog": {"enabled": False}, "stages": stages,
            "pipeline": {"cull_mode": "ccw", "alpha_test": {"enabled": False, "func": "always", "ref": 0},
                         "separate_alpha_blend": False, "blend_op": "add", "srgb_write": False,
                         "color_write_mask": "rgba", "render_target_format": "A8R8G8B8"},
            "pixel_shader_1x_max_value": max_value,
            "bump_env": {"1": {"m00": m00, "m01": m01, "m10": m10, "m11": m11, "convention": convention,
                               "provenance": f"synthetic case {case['name']}"}}}


class Runner:
    def __init__(self, manifest_dir: Path):
        self.manifest = json.loads((manifest_dir / "manifest.json").read_text(encoding="utf-8"))
        adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        self.adapter_info = dict(adapter.info)
        self.device = adapter.request_device_sync()
        self.frag = self.device.create_shader_module(code=(manifest_dir / "frag.spv").read_bytes())
        self.vert = self.device.create_shader_module(code=(manifest_dir / "vert.spv").read_bytes())
        self.passthrough = self.device.create_shader_module(code=PASSTHROUGH_WGSL)

    def texture(self, data: dict):
        if data["format"] == "V8U8":
            fmt, texels = wgpu.TextureFormat.rg8snorm, b"".join(struct.pack("<2b", *t) for t in data["texels"])
            bpp = 2
        else:
            fmt, texels = wgpu.TextureFormat.rgba8unorm, b"".join(struct.pack("<4B", *t) for t in data["texels"])
            bpp = 4
        texture = self.device.create_texture(size=(data["width"], data["height"], 1), format=fmt,
                                             usage=wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST)
        self.device.queue.write_texture({"texture": texture}, texels,
                                        {"bytes_per_row": data["width"] * bpp, "rows_per_image": data["height"]},
                                        (data["width"], data["height"], 1))
        return texture

    def bind_group(self, case: dict, bound: dict):
        cbuffer = bytes.fromhex(bound["cbuffer"]["bytes"])
        uniform = self.device.create_buffer_with_data(data=cbuffer, usage=wgpu.BufferUsage.UNIFORM)
        entries = [{"binding": self.manifest["pixel"]["bindings"]["cbuffer"],
                    "resource": {"buffer": uniform, "offset": 0, "size": len(cbuffer)}}]
        for stage in range(3):
            sampler_state = bound["samplers"][str(stage)]
            address = {"wrap": "repeat", "clamp": "clamp-to-edge"}
            filters = {"point": "nearest", "linear": "linear"}
            sampler = self.device.create_sampler(
                address_mode_u=address[sampler_state["address_u"]], address_mode_v=address[sampler_state["address_v"]],
                mag_filter=filters[sampler_state["mag_filter"]], min_filter=filters[sampler_state["min_filter"]])
            view = self.texture(case["textures"][stage]).create_view()
            entries.append({"binding": int(self.manifest["pixel"]["bindings"]["textures"][str(stage)]), "resource": view})
            entries.append({"binding": int(self.manifest["pixel"]["bindings"]["samplers"][str(stage)]), "resource": sampler})
        return entries

    def render(self, case: dict, draws: list[dict], bound: dict, vertex_module, vertex_layout, globals_bytes=None):
        colour = self.device.create_texture(size=(SIZE, SIZE, 1), format=wgpu.TextureFormat.rgba8unorm,
                                            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
        depth = self.device.create_texture(size=(SIZE, SIZE, 1), format=wgpu.TextureFormat.depth24plus,
                                           usage=wgpu.TextureUsage.RENDER_ATTACHMENT)
        encoder = self.device.create_command_encoder()
        first = True
        for draw in draws:
            blend = ({"color": {"src_factor": "src-alpha", "dst_factor": "one-minus-src-alpha", "operation": "add"},
                      "alpha": {"src_factor": "src-alpha", "dst_factor": "one-minus-src-alpha", "operation": "add"}}
                     if draw["blend"] else None)
            pipeline = self.device.create_render_pipeline(
                layout="auto",
                vertex={"module": vertex_module, "entry_point": "main" if vertex_module is self.passthrough
                        else self.manifest["vertex"]["entry"], "buffers": [vertex_layout]},
                primitive={"topology": "triangle-list", "front_face": "cw", "cull_mode": "back"},
                depth_stencil={"format": "depth24plus", "depth_write_enabled": bool(draw["zwrite"]),
                               "depth_compare": "less-equal"},
                fragment={"module": self.frag, "entry_point": self.manifest["pixel"]["entry"],
                          "targets": [{"format": "rgba8unorm", "blend": blend}]},
            )
            entries = self.bind_group(case, bound)
            if globals_bytes is not None:
                buffer = self.device.create_buffer_with_data(data=globals_bytes, usage=wgpu.BufferUsage.UNIFORM)
                entries.append({"binding": 0, "resource": {"buffer": buffer, "offset": 0, "size": len(globals_bytes)}})
            group = self.device.create_bind_group(layout=pipeline.get_bind_group_layout(0), entries=entries)
            vertex_bytes = draw["vertex_bytes"]
            buffer = self.device.create_buffer_with_data(data=vertex_bytes, usage=wgpu.BufferUsage.VERTEX)
            clear = [c / 255.0 for c in case["clear_rgba"]]
            render_pass = encoder.begin_render_pass(
                color_attachments=[{"view": colour.create_view(), "load_op": "clear" if first else "load",
                                    "store_op": "store", "clear_value": clear}],
                depth_stencil_attachment={"view": depth.create_view(), "depth_load_op": "clear" if first else "load",
                                          "depth_store_op": "store", "depth_clear_value": case["clear_z"]})
            render_pass.set_pipeline(pipeline)
            render_pass.set_bind_group(0, group)
            render_pass.set_vertex_buffer(0, buffer)
            render_pass.draw(draw["count"])
            render_pass.end()
            first = False
        self.device.queue.submit([encoder.finish()])
        data = self.device.queue.read_texture({"texture": colour}, {"bytes_per_row": SIZE * 4, "rows_per_image": SIZE},
                                              (SIZE, SIZE, 1))
        return list(bytes(data))


PASSTHROUGH_LAYOUT = {
    "array_stride": 4 * (4 + 4 + 4 + 6), "step_mode": "vertex",
    "attributes": [{"format": "float32x4", "offset": 0, "shader_location": 0},
                   {"format": "float32x4", "offset": 16, "shader_location": 1},
                   {"format": "float32x4", "offset": 32, "shader_location": 2},
                   {"format": "float32x2", "offset": 48, "shader_location": 3},
                   {"format": "float32x2", "offset": 56, "shader_location": 4},
                   {"format": "float32x2", "offset": 64, "shader_location": 5}]}


def passthrough_bytes(vertices: list[dict]) -> bytes:
    out = b""
    for v in vertices:
        out += struct.pack("<18f", v["x"], v["y"], v["z"], v["rhw"], *[c / 255.0 for c in v["diffuse"]],
                           *[c / 255.0 for c in v["specular"]], *v["tc0"], *v["tc1"], *v["tc2"])
    return out


def run_pixel(runner: Runner, d3d9: dict | None) -> list[dict]:
    results = []
    variants = [("documented", math.inf, 3.0e38), ("transposed", math.inf, 3.0e38), ("documented", 1.0, 1.0)]
    for case in case_data.build_cases():
        if any(draw["kind"] != case_data.KIND_T1 for draw in case["draws"]):
            continue  # stage-1 probe draws are D3D9/evaluator-only (cleanup semantics)
        for convention, evaluator_max, bound_max in variants:
            if bound_max == 1.0 and case["name"] != "range_escape_specular":
                continue
            bound = bind_external_state(runner.manifest, binding_for(case, convention, bound_max))
            draws = [{**draw, "vertex_bytes": passthrough_bytes(draw["vertices"]), "count": len(draw["vertices"])}
                     for draw in case["draws"]]
            actual = runner.render(case, draws, bound, runner.passthrough, PASSTHROUGH_LAYOUT)
            expected, ambiguous = ps11_eval.render_case(case, convention=convention, max_value=evaluator_max)
            record = {"case": case["name"], "convention": convention, "max_value": bound_max,
                      "ambiguous": ambiguous,
                      "vs_evaluator": ps11_eval.compare(expected, actual, case_data.TOLERANCE_LSB)}
            if d3d9 and convention == "transposed" and case["name"] in d3d9:
                record["vs_d3d9_readback"] = direct_compare(actual, d3d9[case["name"]], expected)
            results.append(record)
    return results


def direct_compare(actual: list[int], readback: list[int], expected: list) -> dict:
    worst, failures, compared = 0, 0, 0
    for index, pixel in enumerate(expected):
        if pixel is None:
            continue
        compared += 1
        delta = max(abs(a - b) for a, b in zip(actual[index * 4:index * 4 + 4], readback[index * 4:index * 4 + 4]))
        worst = max(worst, delta)
        failures += delta > case_data.TOLERANCE_LSB
    return {"compared": compared, "max_abs_lsb": worst, "failures": failures}


# --- full pipeline -------------------------------------------------------------

FULL_GLOBALS = {
    "WaterColor": [1.6, 0.9, 0.5, 1.3],       # > 1/0.7 and > 1/0.95: exercises the oD0 clamp
    "Bump0TexU": [0.061, 0.004, 0.0, 0.03],
    "Bump0TexV": [-0.007, 0.053, 0.0, 0.11],
    "m_FOWTexU": [0.029, 0.0, 0.0, 0.06],
    "m_FOWTexV": [0.0, 0.031, 0.0, 0.02],
}


def full_view_projection() -> list[list[float]]:
    """Row-vector matrix M (clip = [x y z 1] * M) giving perspective-varying w."""
    return [[0.12, 0.0, 0.0, 0.03],
            [0.0, -0.11, 0.0, 0.015],
            [0.0, 0.0, 0.02, 0.0],
            [-0.95, 0.9, 0.3, 1.0]]


def full_vertices() -> list[tuple[list[float], float]]:
    """Object-space positions (x, y, z) with vertex alpha; two clockwise triangles."""
    quad = [([-1.0, -1.0, 2.0], 0.15), ([17.0, -1.0, 4.0], 0.9), ([-1.0, 18.0, 1.0], 0.55),
            ([17.0, 18.0, 3.0], 1.0)]
    return [quad[0], quad[1], quad[2], quad[1], quad[3], quad[2]]


def reference_vertex(position: list[float], alpha: float, clamp_colour: bool = True,
                     alpha_factor: float = 0.95, rgb_factor: float = 0.7) -> dict:
    """Independent restatement of the t1 vertex program (not compiled from it)."""
    m = full_view_projection()
    p = position + [1.0]
    clip = [sum(p[i] * m[i][j] for i in range(4)) for j in range(4)]
    world = p  # m_world is identity in this harness
    dot = lambda a, b: sum(x * y for x, y in zip(a, b))
    colour = FULL_GLOBALS["WaterColor"]
    diffuse = [colour[0] * rgb_factor, colour[1] * rgb_factor, colour[2] * rgb_factor,
               colour[3] * alpha * alpha_factor]
    if clamp_colour:
        diffuse = [min(max(c, 0.0), 1.0) for c in diffuse]  # vs_1_1 oD0 clamp, per vertex
    diffuse = [c * 255.0 for c in diffuse]
    ndc_x, ndc_y = clip[0] / clip[3], clip[1] / clip[3]
    return {"x": (ndc_x + 1.0) * SIZE / 2.0, "y": (1.0 - ndc_y) * SIZE / 2.0, "z": clip[2] / clip[3],
            "rhw": 1.0 / clip[3], "diffuse": diffuse, "specular": [0.0, 0.0, 0.0, 0.0],
            "tc0": [dot(FULL_GLOBALS["Bump0TexU"], world), dot(FULL_GLOBALS["Bump0TexV"], world)],
            "tc1": [0.5 * (ndc_x + 1.0), 1.0 - 0.5 * (ndc_y + 1.0)],
            "tc2": [dot(FULL_GLOBALS["m_FOWTexU"], world), dot(FULL_GLOBALS["m_FOWTexV"], world)]}


def globals_buffer(offsets: dict, size: int) -> bytes:
    data = bytearray(size)
    m = full_view_projection()
    identity = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    # HLSL default column_major packing: register j holds column j of the matrix.
    for name, matrix in (("m_worldViewProj", m), ("m_world", identity)):
        values = [matrix[i][j] for j in range(4) for i in range(4)]
        struct.pack_into("<16f", data, offsets[name], *values)
    for name, value in FULL_GLOBALS.items():
        struct.pack_into("<4f", data, offsets[name], *value)
    return bytes(data)


def run_full(runner: Runner, offsets: dict, size: int) -> list[dict]:
    template = next(case for case in case_data.build_cases() if case["name"] == "bem_nonidentity_point")
    vertices = full_vertices()
    object_bytes = b"".join(struct.pack("<3f4f", *pos, 1.0, 1.0, 1.0, alpha) for pos, alpha in vertices)
    layout = {"array_stride": 28, "step_mode": "vertex",
              "attributes": [{"format": "float32x3", "offset": 0, "shader_location": 0},
                             {"format": "float32x4", "offset": 12, "shader_location": 1}]}
    reference = [reference_vertex(pos, alpha) for pos, alpha in vertices]
    results = []
    for convention in ("documented", "transposed"):
        case = dict(template, name="full_pipeline", draws=[{"kind": case_data.KIND_T1, "zfunc": case_data.ZFUNC_LESSEQUAL,
                                                            "zwrite": True, "blend": True, "cleanup_after": False,
                                                            "vertices": reference}])
        bound = bind_external_state(runner.manifest, binding_for(case, convention, 3.0e38))
        draw = dict(case["draws"][0], vertex_bytes=object_bytes, count=len(vertices))
        actual = runner.render(case, [draw], bound, runner.vert, layout, globals_buffer(offsets, size))
        expected, ambiguous = ps11_eval.render_case(case, convention=convention)
        drawn = sum(1 for index in range(SIZE * SIZE)
                    if actual[index * 4:index * 4 + 4] != list(case["clear_rgba"]))
        results.append({"case": "full_pipeline", "convention": convention, "ambiguous": ambiguous,
                        "drawn_pixels": drawn,
                        "vs_evaluator": ps11_eval.compare(expected, actual, case_data.TOLERANCE_LSB),
                        "negative_controls": negative_controls(case, vertices, convention, actual)})
    return results


def negative_controls(case: dict, vertices, convention: str, actual: list[int]) -> dict:
    """Plausible wrong semantics; each must fail to match the translated output."""
    controls = {}
    specs = {
        "colour_clamp_after_interpolation": ({"clamp_colour": False}, {"saturate_colours": True}, None),
        "missing_0_95_alpha_factor": ({"alpha_factor": 1.0}, {}, None),
        "missing_0_7_rgb_factor": ({"rgb_factor": 1.0}, {}, None),
        "exact_screen_space_reflection_uv": ({}, {"affine": frozenset({"tc1"})}, None),
        "fow_modulates_alpha": ({}, {}, "full_mask"),
        "texbem_as_unsigned_offset": ({}, {}, "unsigned_bump"),
    }
    for name, (vertex_options, render_options, program_change) in specs.items():
        reference = [reference_vertex(pos, alpha, **vertex_options) for pos, alpha in vertices]
        variant = dict(case, draws=[dict(case["draws"][0], vertices=reference)])
        saved_program, saved_decode = ps11_eval.PROGRAMS[0], ps11_eval.decode_texel
        try:
            if program_change == "full_mask":
                ps11_eval.PROGRAMS[0] = tuple((op, dst, None if mask == "rgb" else mask, src)
                                              for op, dst, mask, src in saved_program)
            elif program_change == "unsigned_bump":
                def unsigned(texture, index, _decode=saved_decode):
                    if texture["format"] == "V8U8":
                        return tuple((c & 0xFF) / 255.0 for c in texture["texels"][index]) + (1.0, 1.0)
                    return _decode(texture, index)
                ps11_eval.decode_texel = unsigned
            expected, _ = ps11_eval.render_case(variant, convention=convention, **render_options)
        finally:
            ps11_eval.PROGRAMS[0], ps11_eval.decode_texel = saved_program, saved_decode
        controls[name] = len(ps11_eval.compare(expected, actual, case_data.TOLERANCE_LSB)["failures"])
    return controls


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest-dir", type=Path, required=True)
    parser.add_argument("--globals-reflection", type=Path, required=True,
                        help="JSON {name: offset, '$size': bytes} from glslang -q")
    parser.add_argument("--d3d9-readback", type=Path)
    args = parser.parse_args()
    if wgpu.__version__ != WGPU_PIN:
        print(json.dumps({"error": f"wgpu {wgpu.__version__} is not the pinned {WGPU_PIN}"}))
        return 2
    runner = Runner(args.manifest_dir)
    d3d9 = None
    if args.d3d9_readback:
        d3d9 = {c["name"]: c["rgba"] for c in json.loads(args.d3d9_readback.read_text())["cases"]}
    reflection = json.loads(args.globals_reflection.read_text())
    output = {"wgpu": wgpu.__version__,
              "wgpu_native": ".".join(map(str, wgpu.backends.wgpu_native.lib_version_info)),
              "adapter": {k: runner.adapter_info.get(k) for k in ("device", "description", "backend_type", "vendor")},
              "pixel": run_pixel(runner, d3d9),
              "full": run_full(runner, reflection, reflection["$size"])}
    print(json.dumps(output, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
