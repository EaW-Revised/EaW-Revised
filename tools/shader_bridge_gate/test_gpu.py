"""Opt-in bridge GPU receipt through the production GodotRenderer.

The rig provides a private JSON artifact via EAWR_JOB_BRIDGE_MANIFEST and the
separately built probe through the normal probe-library staging mechanism.
"""
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get("EAWR_SHADER_BRIDGE_GPU_TEST") == "1", "opt-in rig receipt")
class BridgeGpu(unittest.TestCase):
    def test_production_material_route(self):
        from PIL import Image, ImageChops, ImageStat

        destination = Path(os.environ["EAWR_SHADER_BRIDGE_OUTPUT"])
        destination.mkdir(parents=True, exist_ok=True)
        manifest = Path(os.environ["EAWR_JOB_BRIDGE_MANIFEST"])
        specification = json.loads(manifest.read_text(encoding="utf-8"))
        cases = specification["cases"]
        for case in cases:
            for texture in case.get("textures", {}).values():
                if "file" in texture:
                    path = manifest.parent / texture["file"]
                    self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), texture["sha256"])
                    texture["file"] = str(path)
        with tempfile.TemporaryDirectory(prefix="eawr-bridge-") as temporary:
            project = Path(temporary)
            library = Path(os.environ["EAWR_SHADER_BRIDGE_PROBE_LIBRARY"])
            shutil.copy2(library, project / library.name)
            (project / "project.godot").write_text(
                'config_version=5\n[application]\nconfig/name="Shader bridge gate"\nrun/main_scene="res://main.tscn"\n'
                '[display]\nwindow/size/viewport_width=256\nwindow/size/viewport_height=256\n'
                '[rendering]\nrenderer/rendering_method="forward_plus"\n', encoding="utf-8")
            (project / "bridge.gdextension").write_text(
                '[configuration]\nentry_symbol="eawr_shader_bridge_probe_library_init"\n'
                'compatibility_minimum="4.7"\n[libraries]\n'
                f'windows.debug.x86_64="res://{library.name}"\n'
                f'windows.release.x86_64="res://{library.name}"\n', encoding="utf-8")
            (project / "main.tscn").write_text(
                '[gd_scene load_steps=2 format=3]\n'
                '[ext_resource path="res://bridge.gdextension" type="GDExtension" id="1"]\n'
                '[node name="Bridge" type="EawrShaderBridgeProbe"]\n', encoding="utf-8")
            (project / ".godot").mkdir()
            (project / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n", encoding="utf-8")
            staged_manifest = project / "manifest.json"
            staged_manifest.write_text(json.dumps(specification), encoding="utf-8")
            environment = dict(os.environ, EAWR_SHADER_BRIDGE_MANIFEST=str(staged_manifest))
            try:
                result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--path", str(project),
                                         "--rendering-method", "forward_plus", "--rendering-driver", "vulkan"],
                                        env=environment, capture_output=True, text=True, timeout=180)
            except subprocess.TimeoutExpired as error:
                streams = (error.stdout or b"", error.stderr or b"")
                output = "\n".join(value.decode(errors="replace") if isinstance(value, bytes) else value
                                   for value in streams)
                (destination / "engine.log").write_text(output, encoding="utf-8")
                raise
            output = result.stdout + "\n" + result.stderr
            (destination / "engine.log").write_text(output, encoding="utf-8")
            self.assertEqual(result.returncode, 0, output)
            self.assertNotIn("SHADER ERROR:", output)
            self.assertNotIn("ERROR:", output)
            self.assertIn("bridge observed_render_thread=Separate", output)
            self.assertNotIn("bridge observed_render_thread=Safe", output)
            driver = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                                     "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | ConvertTo-Json"],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(driver.returncode, 0, driver.stderr)
            (destination / "gpu-driver.json").write_text(driver.stdout, encoding="utf-8")
            report = (destination / "probe.txt").read_text()
            self.assertIn("status=passed", report)
            self.assertIn(f"panels={len(cases)}", report)
            receipts = []
            if "orientation-stored-colour" in {case["name"] for case in cases}:
                orientation = Image.open(destination / "orientation-stored-colour.png").convert("RGBA")
                samples = [orientation.getpixel(point) for point in ((96,96), (160,96), (96,160), (160,160))]
                self.assertEqual(len(set(samples)), 4, "orientation panel did not sample four distinct texels")
                receipts.append({"case": "orientation-stored-colour", "sample_points": [[96,96],[160,96],[96,160],[160,160]],
                                 "samples_rgba": samples, "alpha_extrema": orientation.getextrema()[3]})
            for name in ("synthetic-ps2", "synthetic-ps1"):
                if name not in {case["name"] for case in cases}:
                    continue
                actual = Image.open(destination / (name + ".png")).convert("RGB")
                reference = Image.open(destination / "synthetic-reference.png").convert("RGB")
                error = max(ImageStat.Stat(ImageChops.difference(actual, reference)).mean)
                self.assertLessEqual(error, 1.0, f"{name} differs from invented reference: {error}")
                colours = actual.getcolors(256 * 256)
                self.assertIsNotNone(colours)
                self.assertGreater(len(colours), 4, "texture sampling was not visible")
                # Non-unit vertex transform places these four probes inside
                # distinct texels. Check stored bytes after varying colour
                # multiplication, independently of the reference shader.
                samples = sorted(actual.getpixel(point) for point in
                                 ((100,110), (190,110), (100,175), (190,175)))
                expected = sorted(((32,96,48), (96,48,32), (64,144,16), (16,72,40)))
                for found, wanted in zip(samples, expected):
                    self.assertLessEqual(max(abs(a-b) for a,b in zip(found,wanted)), 3,
                                         f"stored texel probe {found} != {wanted}")
                receipts.append({"case": name, "mean_error": error, "stored_samples": samples})
            for pair in specification.get("comparisons", []):
                actual = Image.open(destination / (pair["actual"] + ".png")).convert("RGBA")
                reference = Image.open(destination / (pair["reference"] + ".png")).convert("RGBA")
                difference = ImageChops.difference(actual, reference)
                difference.save(destination / (pair["name"] + "-delta.png"))
                extrema = difference.getextrema()
                changed = sum(any(pixel[:3]) for pixel in difference.getdata())
                receipts.append({**pair, "mean_rgba": ImageStat.Stat(difference).mean,
                                 "max_rgba": [extent[1] for extent in extrema], "changed_rgb_pixels": changed,
                                 "actual_sha256": hashlib.sha256((destination / (pair["actual"] + ".png")).read_bytes()).hexdigest(),
                                 "reference_sha256": hashlib.sha256((destination / (pair["reference"] + ".png")).read_bytes()).hexdigest()})
            (destination / "comparison.json").write_text(json.dumps(receipts, indent=2), encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
