"""Pinned-Godot synthetic pixel check for the two space environment shaders.

The shader text comes from the compiled CPU adapter. No retail files are used.
Set EAWR_GODOT_EXECUTABLE and EAWR_SPACE_TEST_EXE to run this check.
"""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from test_space_map_mode import decode_png  # noqa: E402
SCRIPT = r'''
extends SceneTree

func _initialize() -> void:
    call_deferred("capture")

func make_texture(color: Color) -> ImageTexture:
    var image := Image.create(2, 2, false, Image.FORMAT_RGBA8)
    image.fill(color)
    return ImageTexture.create_from_image(image)

func save_frame(viewport: SubViewport, name: String) -> void:
    await process_frame
    await process_frame
    await process_frame
    var image := viewport.get_texture().get_image()
    image.save_png("res://" + name + ".png")

func capture() -> void:
    var viewport := SubViewport.new()
    viewport.size = Vector2i(64, 64)
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    viewport.own_world_3d = true
    root.add_child(viewport)
    var scene := Node3D.new()
    viewport.add_child(scene)
    var camera := Camera3D.new()
    camera.position = Vector3(0, 0, 3)
    scene.add_child(camera)
    camera.current = true
    var mesh := MeshInstance3D.new()
    var quad := QuadMesh.new()
    quad.size = Vector2(2, 2)
    mesh.mesh = quad
    scene.add_child(mesh)
    var planet := ShaderMaterial.new()
    planet.shader = load("res://planet.gdshader")
    planet.set_shader_parameter("BaseTexture", make_texture(Color(1, 0, 0, 1)))
    planet.set_shader_parameter("CloudTexture", make_texture(Color(0, 0, 1, 0)))
    planet.set_shader_parameter("Emissive", Vector3.ZERO)
    planet.set_shader_parameter("Diffuse", Vector3.ONE)
    planet.set_shader_parameter("Specular", Vector3.ZERO)
    planet.set_shader_parameter("eawr_effect_time", 0.0)
    planet.set_shader_parameter("eawr_effect_light_scale", Vector4.ONE)
    planet.set_shader_parameter("eawr_effect_light_direction", Vector3(0, 0, 1))
    planet.set_shader_parameter("eawr_effect_ambient", Vector3(0.25, 0.25, 0.25))
    planet.set_shader_parameter("eawr_effect_diffuse", Vector3.ZERO)
    planet.set_shader_parameter("eawr_effect_specular", Vector3.ZERO)
    mesh.material_override = planet
    await save_frame(viewport, "planet_base")
    planet.set_shader_parameter("CloudTexture", make_texture(Color(0, 0, 1, 1)))
    await save_frame(viewport, "planet_cloud")
    planet.shader = load("res://planet_blended.gdshader")
    planet.set_shader_parameter("eawr_effect_light_scale", Vector4(1, 1, 1, 0.5))
    await save_frame(viewport, "planet_blended")
    var nebula := ShaderMaterial.new()
    nebula.shader = load("res://nebula.gdshader")
    nebula.set_shader_parameter("BaseTexture", make_texture(Color(0, 1, 0, 1)))
    nebula.set_shader_parameter("UVScrollRate", Vector4.ZERO)
    nebula.set_shader_parameter("DistortionScale", 0.0)
    nebula.set_shader_parameter("SFreq", 0.1)
    nebula.set_shader_parameter("TFreq", 0.2)
    nebula.set_shader_parameter("eawr_effect_time", 0.25)
    nebula.set_shader_parameter("eawr_effect_light_scale", Vector4.ZERO)
    mesh.material_override = nebula
    await save_frame(viewport, "nebula_off")
    nebula.set_shader_parameter("eawr_effect_light_scale", Vector4(0.2, 0.2, 0.2, 1))
    await save_frame(viewport, "nebula_on")
    mesh.rotation = Vector3(0, PI, 0)
    await save_frame(viewport, "nebula_back")
    quit()
'''


@unittest.skipUnless(os.environ.get("EAWR_GODOT_EXECUTABLE") and os.environ.get("EAWR_SPACE_TEST_EXE"),
                     "set EAWR_GODOT_EXECUTABLE and EAWR_SPACE_TEST_EXE")
class EnvironmentEffectGraphical(unittest.TestCase):
    def test_planet_cloud_mix_and_nebula_addition(self):
        godot = os.environ["EAWR_GODOT_EXECUTABLE"]
        cpu = os.environ["EAWR_SPACE_TEST_EXE"]
        with tempfile.TemporaryDirectory(prefix="eawr-wp20-") as temp:
            project = pathlib.Path(temp)
            (project / "project.godot").write_text('config_version=5\n[rendering]\nrenderer/rendering_method="forward_plus"\n', encoding="utf-8")
            (project / "capture.gd").write_text(SCRIPT, encoding="utf-8")
            for name, arg in (("planet", "--dump-planet-opaque"),
                              ("planet_blended", "--dump-planet-blended"),
                              ("nebula", "--dump-nebula")):
                source = subprocess.run([cpu, arg], check=True, capture_output=True, text=True).stdout
                self.assertIn("shader_type spatial;", source)
                (project / f"{name}.gdshader").write_text(source, encoding="utf-8")
            run = subprocess.run([godot, "--rendering-method", "forward_plus", "--path", str(project),
                                  "--script", "res://capture.gd"],
                                 cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=90, check=False)
            self.assertEqual(run.returncode, 0, run.stdout)
            colors = {}
            for name in ("planet_base", "planet_cloud", "planet_blended", "nebula_off", "nebula_on", "nebula_back"):
                capture = project / f"{name}.png"
                self.assertTrue(capture.is_file(), run.stdout)
                width, height, rows = decode_png(capture.read_bytes())
                self.assertEqual((width, height), (64, 64))
                colors[name] = rows[32][32]
            self.assertGreater(colors["planet_base"][0], colors["planet_base"][2] + 40, colors)
            self.assertGreater(colors["planet_cloud"][2], colors["planet_cloud"][0] + 40, colors)
            self.assertLess(colors["planet_blended"][2], colors["planet_cloud"][2] - 20, colors)
            self.assertGreater(colors["nebula_on"][1], colors["nebula_off"][1] + 30, colors)
            # Nebula.fx squares view-space normal z, so a shell's far side
            # (normal z = -1 here) gets the same edge factor as its near side.
            self.assertLessEqual(abs(colors["nebula_back"][1] - colors["nebula_on"][1]), 2, colors)


if __name__ == "__main__":
    unittest.main()
