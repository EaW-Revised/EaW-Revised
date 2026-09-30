"""P1-09 camera bindings and host input contract.

Three jobs:

1. The committed binding table is project-authored, explicitly non-retail and
   shaped as schema v2 expects (v1 plus the `free` context and free-camera
   settings). Its full validation lives in the C++ `camera_bindings_contracts`
   test; this guard fails fast on labelling drift.
2. Source guards: the loader/adapter stays engine independent and simulation
   free, input is routed only to the explicit opt-in interaction, interaction
   code never writes the pinned capture camera, and map/space modes are not
   touched.
3. Opt-in graphical runs (same switches as the other viewer runtime tests) that
   drive the real viewer with synthetic input through Godot's event dispatch
   against the wholly synthetic camera fixture.
"""

import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "assets" / "fixtures"))
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))

import camera_fixture as fixture  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402

BINDINGS = ROOT / "apps/viewer/project/config/camera-bindings.json"
EXPORT_PRESETS = ROOT / "apps/viewer/project/export_presets.cfg"
ADAPTER_HEADER = ROOT / "apps/viewer/src/camera_input.hpp"
VIEWER_HEADER = ROOT / "apps/viewer/src/viewer_host.hpp"
FREE_HEADER = ROOT / "include/eawr/presentation/camera/free_camera.hpp"
FREE_SOURCE = ROOT / "src/presentation/camera/free_camera.cpp"

RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
RUNTIME_REASON = "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise"


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


class CameraBindingTable(unittest.TestCase):
    def test_committed_table_is_project_authored_and_non_retail(self):
        table = json.loads(BINDINGS.read_text(encoding="utf-8"))
        self.assertEqual(table["schema"], "eawr-camera-bindings")
        self.assertEqual(table["version"], 2)
        self.assertEqual(table["provenance"], "project-authored")
        self.assertIn("NON-RETAIL", table["notice"])
        self.assertIn("not the original Empire at War defaults", table["notice"])
        contexts = {binding["context"] for binding in table["bindings"]}
        self.assertEqual(contexts, {"land", "space", "free"})
        self.assertEqual(set(table["free_camera"]), {
            "move_speed", "vertical_speed", "look_degrees_per_unit",
            "pitch_min_degrees", "pitch_max_degrees"})
        toggles = {b["context"] for b in table["bindings"] if b["action"] == "free_toggle"}
        self.assertEqual(toggles, {"land", "space", "free"})
        for binding in table["bindings"]:
            if binding["action"] == "free_toggle":
                continue
            free_action = binding["action"].startswith("free_")
            self.assertEqual(free_action, binding["context"] == "free", binding["id"])
        ids = [binding["id"] for binding in table["bindings"]]
        self.assertEqual(len(ids), len(set(ids)))
        chords = [(b["context"], b["device"], b["control"].lower(),
                   tuple(sorted(b.get("modifiers", [])))) for b in table["bindings"]]
        self.assertEqual(len(chords), len(set(chords)), "ambiguous chord in the committed table")

    def test_table_carries_no_map_bounds_or_retail_claims(self):
        # The notice may *name* what is unestablished; no other field may carry it.
        table = json.loads(BINDINGS.read_text(encoding="utf-8"))
        table.pop("notice")
        text = json.dumps(table).lower()
        for token in ("bounds", "retail", "original"):
            self.assertNotIn(token, text)

    def test_binding_table_is_included_in_every_export_preset(self):
        text = EXPORT_PRESETS.read_text(encoding="utf-8")
        filters = re.findall(r'^include_filter="([^"]*)"$', text, flags=re.MULTILINE)
        self.assertEqual(len(filters), 3, "expected the Windows, Linux x86_64 and Linux ARM64 presets")
        self.assertTrue(all("config/*.json" in value.split(",") for value in filters))
        config_jsons = sorted(path.name for path in BINDINGS.parent.glob("*.json"))
        self.assertEqual(config_jsons, ["camera-bindings.json", "map-camera-bindings.json",
                                        "map-camera-free-bindings.json", "space-live-camera-bindings.json",
                                        "space-map-camera-bindings.json"])


class CameraInputSourceContract(unittest.TestCase):
    def test_adapter_is_engine_independent_and_simulation_free(self):
        for text in (ADAPTER_HEADER.read_text(encoding="utf-8"), mode_source("camera_input")):
            self.assertNotIn("godot::", text)
            self.assertNotIn("<godot_cpp", text)
            self.assertNotIn("eawr/sim", text)
            self.assertNotIn("eawr/vfs", text)
            self.assertNotIn("fstream", text)

    def test_input_is_routed_only_to_the_opt_in_interaction(self):
        source = mode_source("viewer_host")
        # UI-07: the GUI sees events first; the world, then the camera, get
        # what it leaves through route_world_input.
        body = function_body(source, "void ViewerHost::route_world_input(")
        self.assertIn("if (!camera_interaction_ || event.is_null()) return;", body)
        for handler in ("void ViewerHost::_input(", "void ViewerHost::_unhandled_input("):
            self.assertIn("route_world_input(event);", function_body(source, handler))
        notification = function_body(source, "void ViewerHost::_notification(")
        self.assertIn("if (!camera_interaction_) return;", notification)
        # The interaction is created only by start_camera_interaction, reached
        # only from the tactical camera path when explicitly requested.
        self.assertEqual(source.count("camera_interaction_ = std::move(run);"), 1)
        self.assertIn("options_->camera_interactive && !start_camera_interaction()", source)

    def test_interaction_never_writes_the_capture_camera(self):
        source = mode_source("viewer_host")
        for signature in ("bool ViewerHost::start_camera_interaction(",
                          "bool ViewerHost::step_camera_interaction(",
                          "bool ViewerHost::toggle_free_camera(",
                          "bool ViewerHost::step_free_camera(",
                          "bool ViewerHost::advance_camera_selftest(",
                          "void ViewerHost::_input(",
                          "void ViewerHost::_unhandled_input(",
                          "void ViewerHost::route_world_input(",
                          "void ViewerHost::_notification("):
            body = function_body(source, signature)
            self.assertIsNone(re.search(r"capture_camera_\s*(\.\w+(\[\d+\])?)?\s*=[^=]", body),
                              f"{signature} writes capture_camera_")
            self.assertNotIn("fixed_capture_snapshot_", body)
        step = function_body(source, "bool ViewerHost::step_camera_interaction(")
        self.assertIn("if (run.adapter.capture_locked()) return true;", step)
        start = function_body(source, "bool ViewerHost::start_camera_interaction(")
        self.assertIn("run->adapter.set_capture_locked(true);", start)

    def test_space_and_effect_modes_are_not_interactive(self):
        source = mode_source("viewer_host")
        self.assertIn("the unlocked mode is not interactive", source)
        effect = mode_source("effect_mode")
        self.assertNotIn("camera_input", effect)
        self.assertNotIn("free_camera", effect)
        self.assertNotIn("FreeCamera", effect)
        # P1 #30: the space map is interactive only through the opt-in
        # space-context MapCameraBridge, and never has free flight.
        space = mode_source("space_environment")
        self.assertIn("std::unique_ptr<viewer::MapCameraBridge> bridge;", space)
        self.assertIn("camera_input::Context::space", space)
        self.assertNotIn("free_camera", space)
        self.assertNotIn("FreeCamera", space)

    def test_free_camera_module_is_pure_and_unbounded(self):
        for path in (FREE_HEADER, FREE_SOURCE):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("godot", text.lower())
            self.assertNotIn("eawr/sim", text)
            self.assertNotIn("eawr/vfs", text)
            self.assertNotIn("eawr/assets", text)
            self.assertNotIn("fstream", text)
        # No map bound is derived here; typed #26 bounds are a separate gate.
        code = "\n".join(line for line in FREE_SOURCE.read_text(encoding="utf-8").splitlines()
                         if not line.lstrip().startswith("//"))
        for token in ("bound_min", "bounds_min", "map_bounds", "ted", "Ted", "TED"):
            self.assertNotIn(token, code)

    def test_free_flight_transitions_restore_and_respect_capture_lock(self):
        source = mode_source("viewer_host")
        step = function_body(source, "bool ViewerHost::step_camera_interaction(")
        # The lock check precedes any toggle handling.
        self.assertLess(step.index("if (run.adapter.capture_locked()) return true;"),
                        step.index("free_toggle_requests"))
        toggle = function_body(source, "bool ViewerHost::toggle_free_camera(")
        for token in ("run.pose = run.saved_pose;", "run.camera = run.saved_camera;",
                      "run.adapter.set_context(run.saved_context);",
                      "run.saved_pose = run.pose;", "run.saved_camera = run.camera;",
                      "run.adapter.set_context(camera_input::Context::free);"):
            self.assertIn(token, toggle)
        # Entry does not rewrite the render camera, so it cannot jump.
        entry = toggle.split("run.saved_pose = run.pose;", 1)[1]
        self.assertNotIn("set_camera", entry)
        self.assertNotIn("run.camera.", entry)

    def test_viewer_options_and_report_fields(self):
        source = mode_source("viewer_host")
        for option in ("--eawr-camera-interactive", "--eawr-camera-input-selftest",
                       "--eawr-camera-bindings"):
            self.assertIn(f'argument == "{option}"', source)
        self.assertIn('"res://config/camera-bindings.json"', source)
        for token in ('\\"camera_input\\"', '\\"bounds\\": null', '\\"capture_locked_for_run\\"',
                      '\\"input_callbacks\\"', "camera_input_selftest_passed",
                      '\\"free_camera\\"', '\\"transitions\\"', "eawr-free-camera-v1"):
            self.assertIn(token, source)
        header = VIEWER_HEADER.read_text(encoding="utf-8")
        self.assertIn("void _input(const godot::Ref<godot::InputEvent>& event) override;", header)
        self.assertIn("void _unhandled_input(const godot::Ref<godot::InputEvent>& event) override;", header)

    def test_bindings_read_is_capped_before_allocation(self):
        source = mode_source("viewer_host")
        start = function_body(source, "bool ViewerHost::start_camera_interaction(")
        self.assertIn("read_bytes_bounded(", start)
        self.assertIn("camera_input::max_binding_document_bytes", start)
        self.assertNotIn("read_bytes(", start)
        bounded = function_body(source, "BoundedRead read_bytes_bounded(")
        godot_branch, native_branch = bounded.split("std::ifstream", 1)
        # The length check precedes every allocation or read in both branches.
        self.assertLess(godot_branch.index("length > max_bytes"),
                        godot_branch.index("get_buffer("))
        self.assertLess(native_branch.index("> max_bytes"), native_branch.index("out.resize("))
        self.assertLess(native_branch.index("> max_bytes"), native_branch.index("input.read("))
        # The generic reader used by other call sites is unchanged in shape.
        self.assertIn("input->get_buffer(input->get_length())",
                      function_body(source, "std::optional<std::vector<std::byte>> read_bytes("))
        header = ADAPTER_HEADER.read_text(encoding="utf-8")
        self.assertIn("max_binding_document_bytes = std::size_t{1} << 20U", header)
        self.assertIn("json.size() > max_binding_document_bytes",
                      mode_source("camera_input"))

    def test_report_strings_use_the_control_escaping_encoder(self):
        source = mode_source("viewer_host")
        body = function_body(source, "std::string json(const std::string_view value)")
        self.assertIn("json_string_literal(value)", body)


class CameraInputGraphicalRun(unittest.TestCase):
    def _run(self, workspace, name, *arguments):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable,
                        "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        mod_root = fixture.write_fixture_root(workspace / "fixture")
        report = workspace / f"{name}.json"
        command = [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                   "--eawr-mod-root", str(mod_root), *arguments, "--eawr-report", str(report)]
        completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False, timeout=180)
        return completed, json.loads(report.read_text(encoding="utf-8"))

    @unittest.skipUnless(RUNTIME, RUNTIME_REASON)
    def test_synthetic_input_self_test_passes_in_both_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = pathlib.Path(directory)
            for mode in ("land", "space"):
                completed, report = self._run(workspace, f"selftest-{mode}",
                                              "--eawr-camera-mode", mode,
                                              "--eawr-camera-input-selftest")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                self.assertEqual(report["status"], "camera_input_selftest_passed")
                section = report["camera_input"]
                self.assertEqual(section["context"], mode)
                self.assertIsNone(section["bounds"])
                self.assertEqual(section["bindings"]["provenance"], "project-authored")
                self.assertGreater(section["input_callbacks"], 0)
                checks = section["selftest"]["checks"]
                self.assertGreaterEqual(len(checks), 14)
                self.assertTrue(all(check["passed"] for check in checks), checks)
                names = {check["name"] for check in checks}
                self.assertIn("eligible mouse drag rotates yaw about a fixed target", names)
                self.assertIn("real size_changed signal cancels held movement", names)
                # UI-07 (#314 review P1): a modal ends held camera input and resurrects none.
                for name in ("held pan moves before the modal opens",
                             "a modal opening stops a held pan and a held rotate grab",
                             "closing the modal resurrects no camera hold"):
                    self.assertIn(name, names)
                for name in ("toggle enters free flight from the current camera without a jump",
                             "held free forward flies along the view direction",
                             "held rise moves only along world up",
                             "grabbed look turns the view without translating",
                             "focus loss froze free flight",
                             "focus regain resurrected no free flight",
                             "real size_changed signal cancels free flight",
                             "toggle exit restores the exact tactical state",
                             "tactical pan resumes after free flight"):
                    self.assertIn(name, names)
                self.assertGreater(section["resize_notifications"], 0)
                # The drag rotated the camera; nothing reset it afterwards.
                self.assertNotEqual(section["pose"]["yaw_degrees"], 0)
                free = section["free_camera"]
                table = json.loads(BINDINGS.read_text(encoding="utf-8"))
                self.assertEqual(free["controller"], "eawr-free-camera-v1")
                self.assertEqual(free["provenance"], "project-authored")
                # The report prints float32 values at 9 significant digits.
                self.assertEqual(set(free["settings"]), set(table["free_camera"]))
                for key, value in table["free_camera"].items():
                    self.assertAlmostEqual(free["settings"][key], value, places=6)
                self.assertFalse(free["active"])
                self.assertEqual((free["entries"], free["exits"], free["rejections"]), (1, 1, 0))
                self.assertEqual([entry["kind"] for entry in free["transitions"]],
                                 ["enter", "exit"])
                self.assertGreater(free["moving_steps"], 0)
                self.assertIsNotNone(free["pose"])

    @unittest.skipUnless(RUNTIME, RUNTIME_REASON)
    def test_capture_lock_keeps_the_exact_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = pathlib.Path(directory)
            for mode in ("land", "space"):
                plain_completed, plain = self._run(
                    workspace, f"plain-{mode}", "--eawr-camera-mode", mode,
                    "--eawr-capture", str(workspace / f"plain-{mode}.png"))
                locked_completed, locked = self._run(
                    workspace, f"locked-{mode}", "--eawr-camera-mode", mode,
                    "--eawr-camera-input-selftest",
                    "--eawr-capture", str(workspace / f"locked-{mode}.png"))
                self.assertEqual(plain_completed.returncode, 0, plain_completed.stdout)
                self.assertEqual(locked_completed.returncode, 0, locked_completed.stdout)
                self.assertEqual(locked["status"], "tactical_camera_exercise_passed")
                plain_png = (workspace / f"plain-{mode}.png").read_bytes()
                locked_png = (workspace / f"locked-{mode}.png").read_bytes()
                self.assertEqual(plain["capture_sha256"], hashlib.sha256(plain_png).hexdigest())
                self.assertEqual(locked["capture_sha256"], hashlib.sha256(locked_png).hexdigest())
                self.assertEqual(plain["capture_sha256"], locked["capture_sha256"])
                self.assertEqual(plain_png, locked_png)
                self.assertEqual(plain["capture_identity"]["camera"],
                                 locked["capture_identity"]["camera"])
                self.assertEqual(plain["tactical_camera"], locked["tactical_camera"])
                section = locked["camera_input"]
                self.assertTrue(section["capture_locked_for_run"])
                self.assertGreater(section["input_callbacks"], 0)
                self.assertEqual(section["counters"]["routed"], 0)
                self.assertEqual(section["moving_steps"], 0)
                self.assertTrue(all(check["passed"] for check in section["selftest"]["checks"]))
                self.assertIn("capture lock ignored the free-flight toggle",
                              {check["name"] for check in section["selftest"]["checks"]})
                free = section["free_camera"]
                self.assertFalse(free["active"])
                self.assertEqual((free["entries"], free["steps"], free["transitions"]), (0, 0, []))
                self.assertIsNone(free["pose"])

    @unittest.skipUnless(RUNTIME, RUNTIME_REASON)
    def test_control_characters_in_notice_keep_the_report_valid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = pathlib.Path(directory)
            table = json.loads(BINDINGS.read_text(encoding="utf-8"))
            notice = "hostile\nline two\u0000nul\u001f\"quoted\"\\ end"
            table["notice"] = notice
            hostile = workspace / "hostile-bindings.json"
            hostile.write_text(json.dumps(table), encoding="utf-8")
            self.assertIn("\\u0000", hostile.read_text(encoding="utf-8"))
            # _run parses the report with the strict default decoder, which
            # rejects raw control characters inside strings.
            completed, report = self._run(workspace, "hostile-notice",
                                          "--eawr-camera-mode", "land",
                                          "--eawr-camera-input-selftest",
                                          "--eawr-camera-bindings", str(hostile))
            self.assertEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(report["camera_input"]["bindings"]["notice"], notice)
            # Only layout line breaks may appear as raw control bytes.
            raw = (workspace / "hostile-notice.json").read_bytes()
            self.assertFalse(any(byte < 0x20 and byte not in b"\r\n" for byte in raw))
            self.assertIn(b'"hostile\\nline two\\u0000nul\\u001f\\"quoted\\"\\\\ end"', raw)

    @unittest.skipUnless(RUNTIME, RUNTIME_REASON)
    def test_interaction_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = pathlib.Path(directory)
            bad = workspace / "bad-bindings.json"
            bad.write_text('{"schema": "eawr-camera-bindings", "version": 3}', encoding="utf-8")
            oversize = workspace / "oversize-bindings.json"
            oversize.write_bytes(b" " * ((1 << 20) + 1))
            cases = {
                "unlocked": ("--eawr-camera-mode", "unlocked", "--eawr-camera-interactive"),
                "no-mode": ("--eawr-camera-interactive",),
                "bad-version": ("--eawr-camera-mode", "land", "--eawr-camera-interactive",
                                "--eawr-camera-bindings", str(bad)),
                "oversize": ("--eawr-camera-mode", "land", "--eawr-camera-interactive",
                             "--eawr-camera-bindings", str(oversize)),
            }
            failures = {}
            for name, arguments in cases.items():
                completed, report = self._run(workspace, name, *arguments)
                self.assertEqual(completed.returncode, 2, f"{name}: {completed.stdout}")
                self.assertEqual(report["status"], "failed", name)
                failures[name] = report["failure"]
            self.assertIn("EAWR-CAMERA-0202", failures["bad-version"])
            self.assertIn("exceed 1 MiB and were not read", failures["oversize"])


if __name__ == "__main__":
    unittest.main()
