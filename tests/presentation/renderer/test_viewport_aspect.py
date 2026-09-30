"""The 3D view keeps its aspect when the window changes shape (#195).

The base game squashes models on ultrawide screens until the resolution is
changed. The viewer must not: every 3D view takes its horizontal extent from
the viewport's current size. That holds because

1. the project sets no stretch mode or aspect, so the root viewport of an
   interactive run has the window's size and shape;
2. the only content-scale call is the capture pin, which keeps the aspect
   (letterbox); test_capture_size.py owns that check;
3. the renderer's only camera projection is a vertical-FOV perspective
   (RenderingServer camera_set_perspective), for which Godot takes the aspect
   from the viewport every frame. Nothing sets a frustum, orthogonal or
   keep-width projection, and there is no Camera3D node;
4. the main scene is the host alone, so the 3D view is the root viewport and
   not a fixed-size SubViewport, and no shader takes its own aspect uniform.

The tactical camera republishes its frame on every resize with the vertical
FOV and pose unchanged (tests/presentation/camera/controller_main.cpp,
test_viewport_aspect). These checks are structural and run everywhere.
"""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
PROJECT = ROOT / "apps/viewer/project"
SOURCES = sorted([*(ROOT / "apps/viewer/src").glob("*.[ch]pp"),
                  *(ROOT / "src/presentation/godot").rglob("*.[ch]pp")])


def source_texts() -> dict[str, str]:
    return {path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8") for path in SOURCES}


class ViewportAspect(unittest.TestCase):
    def test_sources_are_found(self):
        names = source_texts()
        self.assertIn("src/presentation/godot/renderer.cpp", names)
        self.assertIn("apps/viewer/src/capture_viewport.hpp", names)

    def test_project_sets_no_stretch(self):
        settings = (PROJECT / "project.godot").read_text(encoding="utf-8")
        self.assertNotRegex(settings, r"(?m)^\s*window/stretch/", "the root viewport must follow the window")

    def test_main_scene_is_the_host_alone(self):
        scene = (PROJECT / "main.tscn").read_text(encoding="utf-8")
        nodes = re.findall(r'(?m)^\[node name="([^"]+)" type="([^"]+)"', scene)
        self.assertEqual(nodes, [("ViewerHost", "ViewerHost")])

    def test_only_a_vertical_fov_perspective_projects(self):
        forbidden = ("camera_set_frustum", "camera_set_orthogonal", "camera_set_use_vertical_aspect",
                     "set_keep_aspect", "KEEP_WIDTH", "CONTENT_SCALE_ASPECT_IGNORE")
        # A Camera3D node is a second projection path unless it is the audio listener: the battle audio
        # parks one in a never-drawn viewport because Godot's 3D players hear only through a camera
        # (#443). It draws nothing, so it must stay outside every projection setter and never render.
        listener_files = ("apps/viewer/src/battle_audio.cpp", "apps/viewer/src/battle_audio.hpp")
        camera_node_setters = re.compile(
            r"\b(set_perspective|set_orthogonal|set_frustum|set_projection|set_fov|set_keep_aspect_mode)\s*\(|->\s*(?:fov|keep_aspect)\b")
        perspective = []
        for name, text in source_texts().items():
            for token in forbidden:
                self.assertNotIn(token, text, f"{name} uses {token}")
            if name in listener_files:
                self.assertNotRegex(text, r"camera_set_", f"{name} must not set a projection")
                self.assertNotRegex(text, camera_node_setters,
                                    f"{name}: the listener camera must not set a projection")
                if name.endswith(".cpp"):
                    self.assertIn("UPDATE_DISABLED", text, f"{name}: the listener viewport must never draw")
            else:
                self.assertNotIn("Camera3D", text, f"{name} uses Camera3D")
            if "camera_set_perspective(" in text:
                perspective.append(name)
        self.assertEqual(perspective, ["src/presentation/godot/renderer.cpp"])
        renderer = (ROOT / "src/presentation/godot/renderer.cpp").read_text(encoding="utf-8")
        self.assertRegex(renderer, r"camera_set_perspective\(\s*camera_,\s*camera\.vertical_fov_degrees,",
                         "the renderer projects with the frame's vertical FOV")

    def test_no_shader_takes_its_own_aspect(self):
        for name, text in source_texts().items():
            self.assertNotRegex(text, r"uniform\s+\w+\s+\w*aspect", name)


if __name__ == "__main__":
    unittest.main()
