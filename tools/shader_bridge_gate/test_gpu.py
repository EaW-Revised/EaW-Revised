"""Opt-in bridge GPU receipt through the production GodotRenderer.

The rig provides a private JSON artifact via EAWR_JOB_BRIDGE_MANIFEST and the
separately built probe through the normal probe-library staging mechanism.
"""
import json
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
        cases = json.loads(manifest.read_text(encoding="utf-8"))["cases"]
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
            environment = dict(os.environ, EAWR_SHADER_BRIDGE_MANIFEST=str(manifest))
            try:
                result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "safe", "--path", str(project),
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
            report = (destination / "probe.txt").read_text()
            self.assertIn("status=passed", report)
            self.assertIn(f"panels={len(cases)}", report)
            receipts = []
            for name in ("synthetic-ps2", "synthetic-ps1"):
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
            (destination / "comparison.json").write_text(json.dumps(receipts, indent=2), encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
