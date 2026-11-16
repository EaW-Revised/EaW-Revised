"""WST-58/68, H088: the stock campaign's intro goal opens the authored story summary window once, at the authored chapter;
the Active and Completed tabs switch and Close shuts it."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
CAMPAIGN = "Sandbox_Equal_Footing_Underworld"
# The original's victory-condition rows: TEXTCOLOR 255 024 039 255 in the stock intro chapter.
VICTORY_RED = [255, 24, 39, 255]


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class StorySummaryRuntime(unittest.TestCase):
    def arguments(self, report, *extra):
        return ["--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT, "--eawr-report", str(report), *extra]

    def view(self, *, driver=None, capture=None, ticks=True, resolution="1280x720"):
        """One viewer run at 1280x720; returns the report. A driver script sends native input, else the viewer's own
        tick capture is used."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-story-summary-"))
        report = directory / "summary.json"
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", resolution, "--path", str(ROOT / "apps/viewer/project")]
        if driver:
            script = directory / "driver.gd"
            script.write_text(driver, encoding="utf-8")
            command += ["--script", str(script)]
        command.append("--")
        extra = ["--eawr-galactic-paused", "no"]
        if ticks:
            extra += ["--eawr-galactic-ticks", "75"]
        if ticks:
            # The viewer's tick run ends after it writes its capture, so every such run names one.
            capture = capture or directory / "tick.png"
        if capture:
            extra += ["--eawr-capture", str(capture)]
        command += self.arguments(report, *extra)
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        self.assertNotIn("ERROR:", log)
        return json.loads(report.read_text(encoding="utf-8")), directory

    @staticmethod
    def control(summary, name):
        return next(row for row in summary["controls"] if row["name"] == name)

    @staticmethod
    def click(rect, at):
        x, y, w, h = rect
        return '''    if elapsed > %s and sent < %d:
        sent += 1
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = Vector2(%s, %s)
            event.global_position = event.position
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = pressed
            Input.parse_input_event(event)
''' % (at[0], at[1], x + w / 2, y + h / 2)

    @staticmethod
    def driver(clicks, close_at):
        body = "".join(clicks)
        return '''extends SceneTree
var elapsed = 0.0
var sent = 0
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
%s
    if elapsed > %s:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (body, close_at)

    def test_equal_footing_opens_the_intro_summary_with_tabs_goal_and_recap(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-story-capture-"))
        capture = directory / "summary.png"
        data, _ = self.view(capture=capture)
        self.assertTrue(capture.is_file())
        from space_map_test_support import decode_png
        width, height, _rows = decode_png(capture.read_bytes())
        self.assertEqual((width, height), (1280, 720))
        summary = data["story_summary"]
        self.assertTrue(summary["visible"] and summary["mounted"], "the intro goal opened the window")
        self.assertEqual(summary["tab"], "active")
        self.assertEqual(summary["build_failures"], 0)
        self.assertEqual(len(summary["goals"]), 1, "the stock intro adds one goal")
        goal = summary["goals"][0]
        self.assertEqual(summary["selected"], goal["tag"])
        self.assertFalse(goal["completed"])
        self.assertTrue(goal["viewed"], "reading the chapter marked the goal viewed")
        self.assertTrue(summary["header"], "the incoming-transmission header names the speaker")
        names = {row["name"] for row in summary["controls"]}
        for authored in ("IDC_RADIO_ACTIVE", "IDC_RADIO_COMPLETED", "IDC_GOAL_LIST", "IDC_STORY_RECAP",
                         "IDC_SUMMARY_DIALOG_CLOSE_BUTTON", "IDC_HINTS"):
            self.assertIn(authored, names)
        self.assertTrue(self.control(summary, "IDC_RADIO_ACTIVE")["pressed"])
        self.assertFalse(self.control(summary, "IDC_RADIO_COMPLETED")["pressed"])
        self.assertFalse(self.control(summary, "IDC_SUMMARY_DIALOG_ABANDON")["visible"], "no goal can be abandoned")
        goal_rows = self.control(summary, "IDC_GOAL_LIST")["items"]
        self.assertEqual(len(goal_rows), 1)
        self.assertTrue(goal_rows[0]["selected"] and goal_rows[0]["text"], "the goal's name is its chapter's first text line")
        recap = self.control(summary, "IDC_STORY_RECAP")["items"]
        self.assertGreater(len(recap), 3)
        self.assertEqual(recap[0]["text"], goal_rows[0]["text"], "the recap starts with the same first line")
        red = [row for row in recap if row["colour"] == VICTORY_RED]
        self.assertGreaterEqual(len(red), 2, "the victory conditions are drawn in the authored TEXTCOLOR")
        self.assertTrue(any(not row["text"].strip() for row in recap), "NEWLINE leaves a blank row before them")
        self.assertTrue(all(row["colour"] != VICTORY_RED for row in recap[:2]))

    def test_tabs_switch_and_close_shuts_the_window(self):
        first, _ = self.view()
        summary = first["story_summary"]
        completed = self.control(summary, "IDC_RADIO_COMPLETED")["rect"]
        close = self.control(summary, "IDC_SUMMARY_DIALOG_CLOSE_BUTTON")["rect"]
        # The window opens about thirty ticks in; the scripted presses come well after.
        switched, _ = self.view(driver=self.driver([self.click(completed, (14.0, 1))], 17.0), ticks=False)
        after = switched["story_summary"]
        self.assertTrue(after["visible"])
        self.assertEqual(after["tab"], "completed")
        self.assertTrue(self.control(after, "IDC_RADIO_COMPLETED")["pressed"])
        self.assertEqual(len(self.control(after, "IDC_GOAL_LIST")["items"]), 0, "the intro goal is not completed")
        self.assertEqual(after["header"], "", "no goal on the shown tab: no chapter header")
        closed, _ = self.view(driver=self.driver([self.click(close, (14.0, 1))], 17.0), ticks=False)
        done = closed["story_summary"]
        self.assertFalse(done["visible"])
        self.assertEqual(len(done["goals"]), 1, "the intro goal is tagged, so it is kept when the window closes")

    def test_summary_holds_galactic_clock_until_close(self):
        initial, _ = self.view()
        close = self.control(initial["story_summary"], "IDC_SUMMARY_DIALOG_CLOSE_BUTTON")["rect"]
        held, _ = self.view(driver=self.driver([], 17.0), ticks=False)
        self.assertTrue(held["story_summary"]["visible"])
        self.assertEqual(held["live_session"]["pause_depth"], 1)
        # The authored intro is due at one second. A seventeen-second run must
        # remain near that boundary while the summary is open (allow the async publication).
        frame = held["live_session"]["galactic_frame"]
        self.assertLess(frame, 90)
        resumed, _ = self.view(driver=self.driver([self.click(close, (14.0, 1))], 17.0), ticks=False)
        self.assertFalse(resumed["story_summary"]["visible"])
        self.assertEqual(resumed["live_session"]["pause_depth"], 0)
        self.assertGreater(resumed["live_session"]["galactic_frame"], frame + 30)

    @staticmethod
    def wheel_driver(steps, minimum_end=17.0):
        """Native input, one event per frame from 14 s on: each step is [x, y, button]; a wheel detent arrives as the press
        and release pair the engine sends. The run ends a second after the last step (and not before `minimum_end`)."""
        return '''extends SceneTree
var elapsed = 0.0
var sent = 0
var done_at = -1.0
var steps = %s
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed > 14.0 and sent < steps.size():
        var step = steps[sent]
        sent += 1
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = Vector2(step[0], step[1])
            event.global_position = event.position
            event.button_index = step[2]
            event.pressed = pressed
            Input.parse_input_event(event)
    if sent >= steps.size() and done_at < 0.0:
        done_at = elapsed
    if done_at >= 0.0 and elapsed > max(done_at + 1.0, %s):
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(steps), minimum_end)

    @staticmethod
    def centre(rect):
        x, y, w, h = rect
        return [x + w / 2, y + h / 2]

    def test_the_wheel_over_the_menu_never_reaches_the_galaxy(self):
        """#6758: while the pointer is over the Objectives menu the wheel scrolls the menu and nothing else, at the top of the
        list, in its middle and at its bottom, over the list and over the rest of the window. The galaxy camera and selection
        end up exactly as in a run with no wheel at all; the same wheel with the menu closed does move the camera."""
        WHEEL_UP, WHEEL_DOWN, LEFT = 4, 5, 1
        # The stock intro's recap fits the list at 1280x720 (both ends of its range are then one state); a smaller
        # window may make it scroll, so the middle of the range is exercised at the first size where it does.
        resolution, first = "1280x720", None
        for size in ("1280x720", "960x540", "800x450", "640x360"):
            candidate, _ = self.view(resolution=size)
            recap = self.control(candidate["story_summary"], "IDC_STORY_RECAP")
            if first is None or recap["scroll_max"] > 0:
                resolution, first = size, candidate
            if recap["scroll_max"] > 0:
                break
        summary = first["story_summary"]
        recap = self.control(summary, "IDC_STORY_RECAP")
        scrollable = recap["scroll_max"] > 0
        at_list = self.centre(recap["rect"])
        at_button = self.centre(self.control(summary, "IDC_SUMMARY_DIALOG_CLOSE_BUTTON")["rect"])

        baseline, _ = self.view(driver=self.wheel_driver([]), ticks=False, resolution=resolution)
        # Top end, then a notch into the middle, then well past the bottom end, then the rest of the window.
        notches = ([at_list + [WHEEL_UP]] * 3 + [at_list + [WHEEL_DOWN]] * 2
                   + [at_list + [WHEEL_DOWN]] * 60 + [at_list + [WHEEL_UP]] * 3 + [at_list + [WHEEL_DOWN]] * 60
                   + [at_button + [WHEEL_UP], at_button + [WHEEL_DOWN]] * 5)
        pressed, _ = self.view(driver=self.wheel_driver(notches), ticks=False, resolution=resolution)
        self.assertTrue(pressed["story_summary"]["visible"], "the window is still open")
        scrolled = self.control(pressed["story_summary"], "IDC_STORY_RECAP")
        self.assertEqual(scrolled["scroll"], scrolled["scroll_max"], "the wheel left the list at its bottom end")
        if scrollable:
            self.assertGreater(scrolled["scroll"], 0, "the wheel scrolled the list")
        self.assertEqual(pressed["camera"], baseline["camera"], "the wheel over the menu moved the galaxy camera")
        self.assertEqual(pressed["map_input"], baseline["map_input"], "the wheel over the menu changed the selection")

        # The same detents with the menu shut do reach the galaxy: the check above could otherwise pass for want of a zoom.
        shut, _ = self.view(driver=self.wheel_driver([at_button + [LEFT]] + [at_list + [WHEEL_DOWN]] * 12),
                            ticks=False, resolution=resolution)
        self.assertFalse(shut["story_summary"]["visible"])
        self.assertNotEqual(shut["camera"], baseline["camera"], "the wheel with the menu closed zooms the galaxy")


if __name__ == "__main__":
    unittest.main()
