"""The runtime message and confirm box (MNU-42, docs/ui/menu-screens.md).

`--eawr-message-box <variant>` opens EawrMessageBox on the gallery's retail skin and scripts input
through Godot's real dispatch: a click beside the box and one on its frame away from its buttons
must reach neither the world under it nor choose a result (UI-I1), each button gives its own result
through the `result_chosen` signal, and Esc gives the cancel result where the box has one. The
engine-free half (variant selection, slots, results) is the root CTest `ui_message_box_contracts`.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT.
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402
import test_ui_gallery  # noqa: E402

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"


class MessageBoxStructure(unittest.TestCase):
    def test_the_box_is_registered_and_built(self):
        register = (ROOT / "apps/viewer/src/register_types.cpp").read_text(encoding="utf-8")
        self.assertIn("register_message_box_classes()", register)
        build = source_text("apps/viewer/CMakeLists.txt")
        self.assertIn("src/ui_gallery_box.cpp", build)
        self.assertIn("src/presentation/godot/ui/message_box_view.cpp", build)
        sources = (ROOT / "src/presentation/ui/sources.cmake").read_text(encoding="utf-8")
        self.assertIn("message_box.cpp", sources)

    def test_the_box_blocks_the_world_and_reports_through_a_signal(self):
        view = (ROOT / "src/presentation/godot/ui/message_box_view.cpp").read_text(encoding="utf-8")
        self.assertIn("EawrUiModalLayer", view)
        self.assertIn('ADD_SIGNAL(MethodInfo("result_chosen"', view)
        self.assertIn("KEY_ESCAPE", view)
        self.assertIn("escape_result()", view)

    def test_the_model_stays_engine_free(self):
        header = (ROOT / "include/eawr/presentation/ui/message_box.hpp").read_text(encoding="utf-8")
        self.assertNotIn("godot", header)


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class MessageBoxGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-message-box-"))
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(cls.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)

    def run_box(self, variant):
        result = test_ui_gallery.run_viewer(self, self.directory, f"message-box-{variant}", self.cache,
                                            "--eawr-message-box", variant)
        box = result["message_box"]
        failed = [f"{step['name']}: {step['problem']}" for step in box["steps"] if not step["passed"]]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual(box["world_presses"], 0)
        return box

    def test_a_two_button_confirm_gives_each_result_and_blocks_clicks(self):
        box = self.run_box("two")
        self.assertEqual(box["dialog"], "IDD_DIALOG_BOX2")
        names = [step["name"] for step in box["steps"]]
        self.assertEqual(len(names), 7, names)
        self.assertIn("the IDC_DIALOG_BOX_BUTTON_LEFT press gives its result", names)
        self.assertIn("the IDC_DIALOG_BOX_BUTTON_RIGHT press gives its result", names)
        self.assertIn("a click beside the box is blocked", names)
        self.assertIn("a click on the frame away from its buttons is blocked", names)

    def test_the_other_variants_pick_their_dialogs(self):
        for variant, dialog in (("quit", "IDD_DIALOG_BOX_QUIT_CONFIRM"), ("three-centred", "IDD_DIALOG_BOX_CENTERED"),
                                ("one", "IDD_DIALOG_BOX1"), ("small", "IDD_DIALOG_BOX_SMALL")):
            with self.subTest(variant=variant):
                self.assertEqual(self.run_box(variant)["dialog"], dialog)


if __name__ == "__main__":
    unittest.main()
