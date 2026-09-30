"""UI-07 (#304) input routing, focus and command sink in the viewer.

`--eawr-input-routing` builds a synthetic HUD (a faceplate opaque on its left
half, Stop and Move buttons on component rects), an edit box and a modal
layer, drives them with synthetic events through Godot's real dispatch
(Input.parse_input_event) and reports what reached the world layer, the host's
_unhandled_input after the routing policy. The rules: UI-I1 (GUI before world,
modal dialogs block the world, a world hold keeps its pointer over the HUD),
UI-I2 (only component rects and opaque faceplate texels stop the pointer, the
wheel included), UI-I3 (a focused edit box suppresses hotkeys and camera keys)
and UI-C1 (HUD and world orders give the same commands). The headless command
stream and hash checks (UI-C1, UI-C2) are the root CTest `ui_input_command_contracts`.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE. It reads no game data.
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))

from viewer_mode_sources import mode_source  # noqa: E402

SRC = ROOT / "apps/viewer/src"
GODOT_UI = ROOT / "src/presentation/godot/ui"
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))

RULES = {"UI-I1", "UI-I2", "UI-I3", "UI-C1"}


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated body for {signature}")


class InputRoutingStructure(unittest.TestCase):
    def test_world_input_runs_after_the_gui(self):
        source = mode_source("viewer_host")
        before_gui = function_body(source, "void ViewerHost::_input(")
        # Before the GUI only a world hold takes the pointer.
        self.assertIn("world_takes_before_gui(", before_gui)
        # A modal opening or closing ends world holds before the event is routed, and every
        # frame (#314 review P1).
        self.assertLess(before_gui.index("sync_modal_holds();"), before_gui.index("world_takes_before_gui("))
        sync = function_body(source, "void ViewerHost::sync_modal_holds(")
        self.assertIn("world_capture_.clear();", sync)
        self.assertIn("cancel_held_world_input();", sync)
        self.assertIn("sync_modal_holds();", function_body(source, "void ViewerHost::_process("))
        self.assertIn("set_input_as_handled()", before_gui)
        after_gui = function_body(source, "void ViewerHost::_unhandled_input(")
        self.assertIn("world_accepts(", after_gui)
        self.assertIn("input_focus(", after_gui)
        route = function_body(source, "void ViewerHost::route_world_input(")
        # The world layer comes before the camera.
        self.assertLess(route.index("input_routing_mode_"), route.index("camera_interaction_"))
        self.assertIn('"gui_focus_changed"', source)
        for name in ("map_mode.cpp", "space_environment.cpp", "space_environment_view.cpp"):
            text = (SRC / name).read_text(encoding="utf-8")
            self.assertEqual(text.count("set_process_input(true);"), text.count("set_process_unhandled_input(true);"), name)

    def test_hud_roots_stop_the_wheel_and_modals_join_their_group(self):
        routing = (GODOT_UI / "input_routing.cpp").read_text(encoding="utf-8")
        self.assertEqual(routing.count("set_force_pass_scroll_events(false);"), 2)
        self.assertIn("add_to_group(group);", routing)
        self.assertIn("register_ui_input_classes()", (SRC / "register_types.cpp").read_text(encoding="utf-8"))

    def test_the_sink_is_the_only_order_path_and_writes_no_sim_state(self):
        header = (ROOT / "include/eawr/presentation/ui/command_sink.hpp").read_text(encoding="utf-8")
        self.assertIn("class CommandSink", header)
        self.assertIn("virtual core::Result<void> issue(const TacticalIntent& intent) = 0;", header)
        for path in (ROOT / "src/presentation/ui/command_sink.cpp", SRC / "input_routing_mode.cpp"):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("TacticalSession", text, path)
            self.assertNotIn(".submit(", text, path)

    def test_the_mode_and_sources_are_in_the_build(self):
        build = (ROOT / "apps/viewer/CMakeLists.txt").read_text(encoding="utf-8")
        for source in ("src/input_routing_mode.cpp", "src/presentation/godot/ui/input_routing.cpp",
                       "src/presentation/ui/input_routing.cpp", "src/presentation/ui/command_sink.cpp",
                       "src/presentation/ui/hud.cpp", "src/presentation/ui/shell_alpha.cpp"):
            self.assertIn(source, build)
        host = (SRC / "viewer_host.cpp").read_text(encoding="utf-8")
        self.assertIn("InputRoutingMode::requested()", host)
        self.assertIn("input_routing_mode_->process()", host)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the viewer routing self-test")
class InputRoutingRuntime(unittest.TestCase):
    def test_routing_rules_hold_through_godot_dispatch(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-input-routing-"))
        report = directory / "input_routing.json"
        completed = subprocess.run(
            [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-input-routing", "--eawr-report", str(report)],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=300)
        self.assertTrue(report.is_file(), completed.stdout)
        result = json.loads(report.read_text(encoding="utf-8"))
        failed = [f"{step['rule']} {step['name']}: {step['problem']}" for step in result["steps"] if not step["passed"]]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["viewport"], [1280, 720])
        self.assertEqual({step["rule"] for step in result["steps"]}, RULES)
        self.assertGreaterEqual(len(result["steps"]), 21)
        names = {step["name"] for step in result["steps"]}
        for name in ("opening a modal ends the world's holds", "closing the modal resurrects no hold"):
            self.assertIn(name, names)
        self.assertGreaterEqual(result["cancels"], 2)
        commands = result["commands"]
        self.assertEqual([(c["origin"], c["verb"]) for c in commands],
                         [("hud_button", "stop"), ("world_click", "move"), ("hotkey", "stop"), ("hud_button", "move")])
        strip = lambda c: {k: v for k, v in c.items() if k not in ("tick", "sequence", "origin")}  # noqa: E731
        self.assertEqual(strip(commands[0]), strip(commands[2]), "HUD Stop and the stop hotkey differ")
        self.assertEqual(strip(commands[1]), strip(commands[3]), "a world right click and HUD move mode differ")
        self.assertEqual([c["sequence"] for c in commands], [0, 1, 2, 3])


if __name__ == "__main__":
    unittest.main()
