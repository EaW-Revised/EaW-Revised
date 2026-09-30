"""Decoded-pixel verifier and opt-in graphical run for the synthetic hull probe.

Graphical run: EAWR_GODOT_LIGHTING_RENDERER_RUNTIME_TEST=1,
EAWR_GODOT_EXECUTABLE=<pinned Godot console>,
EAWR_LIGHTING_RENDERER_PROBE_LIBRARY=<built extension>.
EAWR_LIGHTING_RENDERER_PROBE_OUTPUT optionally retains the report and PNGs.
"""

import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[3]
PROJECT = Path(__file__).resolve().parent / "renderer" / "project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert SPEC is not None and SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)

NAMES = ("on", "off", "restored_on", "missing_caster", "casting_disabled", "receiving_disabled")
RSKIN_NAMES = NAMES + ("caster_palette_translated", "caster_palette_restored")
SELECTORS = {
    "rskin_gloss": {"program": "RSkinGloss.fx", "technique": "sph_t1",
                    "pass_name": "sph_t1_p0", "render_pass": "opaque"},
    "mesh_bump_colorize": {"program": "MeshBumpColorize.fx", "technique": "t0",
                           "pass_name": "t0_p0", "render_pass": "opaque"},
}
IDENTITY_SKIN = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
REGIONS = {"receiver": {"x": [-0.7, 0.7], "y": [1.3, 2.4]},
           "control": {"x": [2.1, 2.8], "y": [1.3, 2.4]}}
WIDTH = HEIGHT = 480
HALF_SPAN = 18.0 * 0.41421356237


_DECODED = {}


def read_png(path):
    """Decoded RGBA8 rows; the same bytes decode once per process (the verifier re-reads its PNGs)."""
    raw = path.read_bytes()
    key = hashlib.sha256(raw).digest()
    if key not in _DECODED:
        _DECODED[key] = bytes(_decode_png(raw))
    return bytearray(_DECODED[key])


def _decode_png(raw):
    if not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("PNG signature")
    offset = 8
    compressed = bytearray()
    dimensions = None
    while offset + 12 <= len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        end = offset + 12 + length
        if end > len(raw):
            raise ValueError("truncated PNG chunk")
        kind = raw[offset + 4:offset + 8]
        payload = raw[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", raw, offset + 8 + length)[0]
        if zlib.crc32(kind + payload) != expected_crc:
            raise ValueError("PNG chunk CRC")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            dimensions = (width, height)
            if depth != 8 or color != 6 or compression or filtering or interlace:
                raise ValueError("expected noninterlaced RGBA8 PNG")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
        offset = end
    if dimensions != (WIDTH, HEIGHT):
        raise ValueError("PNG dimensions")
    scanlines = zlib.decompress(compressed)
    stride = WIDTH * 4
    output = bytearray(HEIGHT * stride)
    position = 0
    prior = bytearray(stride)
    for row in range(HEIGHT):
        if position + 1 + stride > len(scanlines):
            raise ValueError("truncated PNG scanlines")
        filter_type = scanlines[position]
        position += 1
        current = bytearray(scanlines[position:position + stride])
        position += stride
        if filter_type > 4:
            raise ValueError("unsupported PNG filter")
        if filter_type == 0:
            output[row * stride:(row + 1) * stride] = current
            prior = current
            continue
        if filter_type == 2:
            current = bytearray((value + above) & 255 for value, above in zip(current, prior))
            output[row * stride:(row + 1) * stride] = current
            prior = current
            continue
        for column in range(stride):
            left = current[column - 4] if column >= 4 else 0
            above = prior[column]
            upper_left = prior[column - 4] if column >= 4 else 0
            if filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                base = left + above - upper_left
                distances = (abs(base - left), abs(base - above), abs(base - upper_left))
                predictor = (left, above, upper_left)[distances.index(min(distances))]
            else:
                predictor = 0
            current[column] = (current[column] + predictor) & 255
        output[row * stride:(row + 1) * stride] = current
        prior = current
    if position != len(scanlines):
        raise ValueError("extra PNG scanlines")
    return output


def write_png(path, pixels):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    stride = WIDTH * 4
    scanlines = b"".join(b"\x00" + pixels[row * stride:(row + 1) * stride] for row in range(HEIGHT))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def source_y(py):
    return (HEIGHT * 0.5 - py - 0.5) * (2 * HALF_SPAN / HEIGHT)


def source_x(px):
    return (px + 0.5 - WIDTH * 0.5) * (2 * HALF_SPAN / WIDTH)


def column_runs(inside):
    """[start, end) pixel runs of the columns for which inside(px) holds."""
    runs, start = [], None
    for px in range(WIDTH + 1):
        if px < WIDTH and inside(px):
            start = px if start is None else start
        elif start is not None:
            runs.append((start, px))
            start = None
    return runs


def region_mean(data, bounds):
    # Integer channel sums over the mask's rows and column runs: the same pixels as a per-pixel walk.
    rows = [py for py in range(HEIGHT) if bounds["y"][0] < source_y(py) < bounds["y"][1]]
    runs = column_runs(lambda px: bounds["x"][0] < source_x(px) < bounds["x"][1])
    totals = [0, 0, 0]
    count = 0
    for py in rows:
        for start, end in runs:
            span = data[(py * WIDTH + start) * 4:(py * WIDTH + end) * 4]
            count += end - start
            for channel in range(3):
                totals[channel] += sum(span[channel::4])
    return {"pixels": count, "rgb": [value / count for value in totals] if count else [0, 0, 0]}


def luma(sample):
    return sum(a * b for a, b in zip(sample["rgb"], (0.2126, 0.7152, 0.0722)))


def verify_report(report, directory, selector="static", expected_identities=None):
    """Return every failed invariant; PNGs are the authority for means."""
    errors = []
    def require(condition, reason):
        if not condition:
            errors.append(reason)

    rskin = selector != "static"
    names = RSKIN_NAMES if rskin else NAMES
    require(report.get("schema") == ("eawr-rskin-shadow-probe-v1" if rskin else "eawr-hull-shadow-probe-v1"), "schema")
    require(report.get("status") == "passed" and report.get("failures") == [], "probe status")
    require(report.get("source") == "alo_viewer_default" and report.get("policy") == "sh", "SH source/policy")
    require(report.get("material") == (SELECTORS[selector] if rskin else
            {"program": "BatchMeshGloss.fx", "technique": "sph_t1",
             "pass_name": "sph_t1_p0", "render_pass": "opaque"}), "legacy selector/pass")
    if rskin:
        require(report.get("probe_mode") == "rskin" and report.get("selector_key") == selector,
                "RSKIN mode/selector")
        identities = report.get("identities", {})
        require(isinstance(identities, dict) and all(isinstance(identities.get(key), str)
                and len(identities[key]) == 64 and all(c in "0123456789abcdef" for c in identities[key])
                for key in ("source_sha256", "library_sha256"))
                and bool(identities.get("build")), "source/build/library identities")
        if expected_identities is not None:
            require(identities == expected_identities, "built source/library identity")
        require(report.get("skeleton") == {"deck_bones": 1, "bridge_bones": 1,
                "rest": "identity", "skin_bones": [0], "vertex_indices": [0, 0, 0, 0],
                "vertex_weights": [1, 0, 0, 0]}, "one-bone skeleton declaration")
        require(report.get("instance_transform") == "identity_fixed", "fixed instance transform")
        require(report.get("skin_bindings") == [
            {"entity_id": 1, "asset_id": 1, "bone_count": 1},
            {"entity_id": 2, "asset_id": 2, "bone_count": 1}], "live deck/bridge skin bindings")
    require(report.get("receiving_materials") == {"normal": 4, "receiver_disabled": 3,
                                                   "variant_failures": 0}, "receiving variants")
    require(report.get("submitted_instances") == 3, "submitted instances")
    require(report.get("space_background") == {
        "selector": "BatchMeshGloss.fx/sph_t1/sph_t1_p0", "casts_shadows": False,
        "geometry": "64x64 authored starfield plate"}, "space background")
    lighting = report.get("lighting", {})
    toward = lighting.get("toward_light")
    constant = lighting.get("sh_constant")
    require(isinstance(toward, list) and len(toward) == 3 and all(isinstance(v, (int, float))
            and math.isfinite(v) for v in toward) and abs(toward[0]) < 0.01
            and 0.70 < toward[1] < 0.72 and 0.70 < toward[2] < 0.72,
            "source-backed sun direction")
    require(isinstance(constant, list) and len(constant) == 3
            and all(isinstance(v, (int, float)) and math.isfinite(v) and v > 0 for v in constant),
            "source-backed SH matrices")
    require(report.get("regions") == REGIONS, "predeclared masks")
    require(report.get("camera") == {"width": 480, "height": 480, "eye": [0, 18, 0],
                                      "target": [0, 0, 0], "up": [0, 0, -1], "fov": 45,
                                      "near": 0.5, "far": 80}, "fixed camera")
    shadows = report.get("shadows", {})
    require(shadows.get("projection") == "orthogonal" and shadows.get("atlas_size") == 2048
            and shadows.get("max_distance") == 22 and shadows.get("floor") == [0.5, 0.5, 0.5],
            "orthogonal coverage/settings")
    require(shadows.get("bias") == 2 and shadows.get("normal_bias") == 5 and isinstance(shadows.get("filter"), str),
            "production space bias/filter declaration")
    backend = report.get("backend", {})
    require(bool(backend.get("version")) and bool(backend.get("driver"))
            and backend.get("method") == "forward_plus"
            and bool(backend.get("vendor")) and bool(backend.get("adapter"))
            and bool(backend.get("api")), "backend")
    captures = report.get("captures", [])
    if not isinstance(captures, list) or len(captures) != len(names) or not all(
            isinstance(c, dict) for c in captures) or [c["name"] for c in captures] != list(names):
        return errors + ["capture sequence"]

    measured = {}
    for capture in captures:
        name = capture["name"]
        if rskin:
            require(capture.get("bridge_skin_source_x") == (8 if name == "caster_palette_translated" else 0),
                    name + " palette evidence")
            require(capture.get("deck_skin_asset") == IDENTITY_SKIN, name + " deck palette")
            expected_bridge = None if name == "missing_caster" else IDENTITY_SKIN.copy()
            if expected_bridge is not None and name == "caster_palette_translated":
                expected_bridge[12] = 8
            require("bridge_skin_asset" in capture and capture["bridge_skin_asset"] == expected_bridge,
                    name + " bridge palette")
        require(capture.get("png") == name + ".png", name + " filename")
        path = directory / (name + ".png")
        if not path.is_file():
            errors.append(name + " missing PNG")
            continue
        try:
            pixels = read_png(path)
            metrics = {key: region_mean(pixels, bounds) for key, bounds in REGIONS.items()}
        except (OSError, ValueError, zlib.error) as exc:
            errors.append(name + " corrupt PNG: " + str(exc))
            continue
        for region, values in metrics.items():
            declared = capture.get(region, {})
            require(values["pixels"] == declared.get("pixels"), name + " " + region + " pixel count")
            listed = declared.get("rgb")
            require(isinstance(listed, list) and len(listed) == 3 and all(
                isinstance(value, (int, float)) and math.isfinite(value)
                and abs(value - actual) <= 0.02 for value, actual in zip(listed, values["rgb"])),
                name + " " + region + " decoded mean")
        require(metrics["receiver"]["pixels"] > 500 and metrics["control"]["pixels"] > 250,
                name + " mask coverage")
        measured[name] = metrics

    if len(measured) == len(names):
        on, off = measured["on"], measured["off"]
        require(luma(off["receiver"]) - luma(on["receiver"]) >= 6,
                "on/off hull darkening")
        require(abs(luma(off["control"]) - luma(on["control"])) <= 2,
                "on/off unaffected control")
        for name in names[2:]:
            reference = on if name in ("restored_on", "caster_palette_restored") else off
            require(abs(luma(measured[name]["receiver"]) - luma(reference["receiver"])) <= 2,
                    name + " receiver control")
            require(abs(luma(measured[name]["control"]) - luma(off["control"])) <= 2,
                    name + " unaffected control")
    return errors


def sample_report(directory, selector="static"):
    captures = []
    rskin = selector != "static"
    background = bytes((18, 24, 40, 255))
    for name in (RSKIN_NAMES if rskin else NAMES):
        dark = name in ("on", "restored_on", "caster_palette_restored")
        receiver = bytes((90, 90, 90, 255) if dark else (140, 140, 140, 255))
        # Every row inside 1.3 < y < 2.4 is the same: receiver, control and background columns.
        band = b"".join(
            receiver if REGIONS["receiver"]["x"][0] < source_x(px) < REGIONS["receiver"]["x"][1]
            else bytes((140, 140, 140, 255)) if REGIONS["control"]["x"][0] < source_x(px) < REGIONS["control"]["x"][1]
            else background
            for px in range(WIDTH))
        pixels = bytearray(b"".join(band if 1.3 < source_y(py) < 2.4 else background * WIDTH
                                    for py in range(HEIGHT)))
        write_png(directory / (name + ".png"), pixels)
        capture = {"name": name, "png": name + ".png",
                   **{key: region_mean(pixels, bounds) for key, bounds in REGIONS.items()}}
        if rskin:
            capture["bridge_skin_source_x"] = 8 if name == "caster_palette_translated" else 0
            capture["deck_skin_asset"] = IDENTITY_SKIN.copy()
            capture["bridge_skin_asset"] = None if name == "missing_caster" else IDENTITY_SKIN.copy()
            if name == "caster_palette_translated":
                capture["bridge_skin_asset"][12] = 8
        captures.append(capture)
    report = {"schema": "eawr-rskin-shadow-probe-v1" if rskin else "eawr-hull-shadow-probe-v1",
            "status": "passed", "failures": [],
            "source": "alo_viewer_default", "policy": "sh",
            "material": SELECTORS[selector] if rskin else
                {"program": "BatchMeshGloss.fx", "technique": "sph_t1",
                 "pass_name": "sph_t1_p0", "render_pass": "opaque"},
            "receiving_materials": {"normal": 4, "receiver_disabled": 3, "variant_failures": 0},
            "submitted_instances": 3,
            "space_background": {"selector": "BatchMeshGloss.fx/sph_t1/sph_t1_p0",
                                 "casts_shadows": False, "geometry": "64x64 authored starfield plate"},
            "lighting": {"toward_light": [0, 0.7071068, 0.7071068],
                         "sh_constant": [0.2, 0.2, 0.2]},
            "camera": {"width": 480, "height": 480, "eye": [0, 18, 0], "target": [0, 0, 0],
                       "up": [0, 0, -1], "fov": 45, "near": 0.5, "far": 80},
            "shadows": {"projection": "orthogonal", "atlas_size": 2048, "max_distance": 22,
                        "floor": [0.5, 0.5, 0.5], "bias": 2, "normal_bias": 5, "filter": "engine default"},
            "backend": {"version": "4.7.2", "driver": "vulkan", "method": "forward_plus",
                        "vendor": "sample", "adapter": "sample", "api": "sample"},
            "regions": REGIONS, "captures": captures}
    if rskin:
        report.update({"probe_mode": "rskin", "selector_key": selector,
                       "identities": {"source_sha256": "a" * 64, "library_sha256": "b" * 64,
                                      "build": "Release/MSVC-x64"},
                       "skeleton": {"deck_bones": 1, "bridge_bones": 1, "rest": "identity",
                                    "skin_bones": [0], "vertex_indices": [0, 0, 0, 0],
                                    "vertex_weights": [1, 0, 0, 0]},
                       "instance_transform": "identity_fixed",
                       "skin_bindings": [{"entity_id": 1, "asset_id": 1, "bone_count": 1},
                                         {"entity_id": 2, "asset_id": 2, "bone_count": 1}]})
    return report


class VerifierTests(unittest.TestCase):
    def test_positive_and_negative_evidence(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-verifier-") as temporary:
            directory = Path(temporary)
            report = sample_report(directory)
            self.assertEqual(verify_report(report, directory), [])
            mutations = [
                lambda r: r["captures"][0]["receiver"]["rgb"].__setitem__(0, 91),
                lambda r: r["captures"][1].__setitem__("name", "on"),
                lambda r: r["receiving_materials"].__setitem__("variant_failures", 1),
                lambda r: r["regions"]["receiver"]["x"].__setitem__(0, -0.2),
                lambda r: r["shadows"].__setitem__("max_distance", 1),
                lambda r: r["backend"].__setitem__("method", "dummy"),
                lambda r: r.__setitem__("status", "failed"),
            ]
            for index, mutate in enumerate(mutations):
                changed = copy.deepcopy(report)
                mutate(changed)
                self.assertTrue(verify_report(changed, directory), f"mutation {index}")
            missing = directory / "on.png"
            missing.unlink()
            self.assertIn("on missing PNG", verify_report(report, directory))
            missing.write_bytes(b"not PNG")
            self.assertTrue(any("corrupt PNG" in error for error in verify_report(report, directory)))

    def test_rskin_selector_palette_and_decoded_pixel_negatives(self):
        for selector in SELECTORS:
            with self.subTest(selector=selector), tempfile.TemporaryDirectory(prefix="eawr-rskin-verifier-") as temporary:
                directory = Path(temporary)
                report = sample_report(directory, selector)
                self.assertEqual(verify_report(report, directory, selector), [])
                mutations = [
                    lambda r: r["material"].__setitem__("program", "BatchMeshGloss.fx"),
                    lambda r: r.__setitem__("selector_key", "static"),
                    lambda r: r.pop("skin_bindings"),
                    lambda r: r["skin_bindings"][1].__setitem__("bone_count", 2),
                    lambda r: r["captures"][6].pop("bridge_skin_source_x"),
                    lambda r: r["captures"][6].__setitem__("bridge_skin_source_x", 0),
                    lambda r: r["captures"][6]["bridge_skin_asset"].__setitem__(12, 0),
                    lambda r: r["captures"][7].pop("deck_skin_asset"),
                    lambda r: r["receiving_materials"].__setitem__("normal", 3),
                    lambda r: r.__setitem__("submitted_instances", 2),
                ]
                for index, mutate in enumerate(mutations):
                    changed = copy.deepcopy(report)
                    mutate(changed)
                    self.assertTrue(verify_report(changed, directory, selector), f"mutation {index}")
                def replace_capture(name, source):
                    path = directory / (name + ".png")
                    original = path.read_bytes()
                    path.write_bytes((directory / (source + ".png")).read_bytes())
                    changed = copy.deepcopy(report)
                    capture = next(c for c in changed["captures"] if c["name"] == name)
                    pixels = read_png(path)
                    for key, bounds in REGIONS.items():
                        capture[key] = region_mean(pixels, bounds)
                    self.assertTrue(verify_report(changed, directory, selector), name + " pixel negative")
                    path.write_bytes(original)
                replace_capture("caster_palette_translated", "on")
                replace_capture("caster_palette_restored", "off")
                missing = directory / "caster_palette_translated.png"
                missing.unlink()
                self.assertIn("caster_palette_translated missing PNG", verify_report(report, directory, selector))
                missing.write_bytes(b"not PNG")
                self.assertTrue(any("corrupt PNG" in error for error in verify_report(report, directory, selector)))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_LIGHTING_RENDERER_RUNTIME_TEST"),
                     "set EAWR_GODOT_LIGHTING_RENDERER_RUNTIME_TEST for graphical probe")
class GraphicalProbe(unittest.TestCase):
    def run_probe(self, selector="static"):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        library = os.environ.get("EAWR_LIGHTING_RENDERER_PROBE_LIBRARY")
        self.assertTrue(executable and Path(executable).is_file(), "pinned Godot console missing")
        self.assertTrue(library and Path(library).is_file(), "built lighting extension missing")
        with tempfile.TemporaryDirectory(prefix="eawr-hull-runtime-") as temporary:
            directory = Path(temporary)
            project = directory / "project"
            shutil.copytree(PROJECT, project)
            (project / "bin").mkdir()
            shutil.copy2(library, project / "bin" / Path(library).name)
            (project / ".godot").mkdir()
            (project / ".godot" / "extension_list.cfg").write_text(
                "res://eawr_lighting_renderer_probe.gdextension\n", encoding="utf-8")
            report_path = directory / "hull-shadow.json"
            identities = None
            args = [executable, "--path", str(project), "--rendering-driver", "vulkan", "--",
                    "--eawr-lighting-report", str(report_path), "--eawr-lighting-output", str(directory)]
            if selector != "static":
                identities = {"source_sha256": hashlib.sha256(
                    (PROJECT.parent / "lighting_renderer_probe.cpp").read_bytes()).hexdigest(),
                    "library_sha256": hashlib.sha256(Path(library).read_bytes()).hexdigest(),
                    "build": "Release/MSVC-x64"}
                args += ["--eawr-lighting-selector", selector,
                         "--eawr-lighting-source-sha256", identities["source_sha256"],
                         "--eawr-lighting-library-sha256", identities["library_sha256"],
                         "--eawr-lighting-build-identity", identities["build"]]
            result = subprocess.run(
                args,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300, check=False)
            output = result.stdout + "\n" + result.stderr
            destination = os.environ.get("EAWR_LIGHTING_RENDERER_PROBE_OUTPUT")
            if destination:
                target = Path(destination) / selector if selector != "static" else Path(destination)
                target.mkdir(parents=True, exist_ok=True)
                (target / "hull-shadow.log").write_text(output, encoding="utf-8")
                if report_path.is_file():
                    shutil.copy2(report_path, target / report_path.name)
                for name in (RSKIN_NAMES if selector != "static" else NAMES):
                    if (directory / (name + ".png")).is_file():
                        shutil.copy2(directory / (name + ".png"), target / (name + ".png"))
            self.assertIsNotNone(HARNESS.engine_banner(result.stdout, HARNESS.PINNED_GODOT_IDENTITY), output)
            self.assertEqual(result.returncode, 0, output)
            self.assertIn("EAWR hull shadow probe passed", result.stdout)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(verify_report(report, directory, selector, identities), [], json.dumps(report, indent=2))

    def test_production_hull_shadow(self):
        self.run_probe()

    def test_rskin_hull_shadow(self):
        for selector in SELECTORS:
            with self.subTest(selector=selector):
                self.run_probe(selector)


if __name__ == "__main__":
    unittest.main()
