"""Lit one-viewer co-op battle captures for GC participant colours."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")

DRIVER = '''extends SceneTree
var battle_frames := 0
var early_path := %s
func _initialize(): call_deferred("start")
func start():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(scene)
    current_scene = scene
    RenderingServer.frame_post_draw.connect(drawn)
func drawn():
    if root.find_child("CampaignSpaceBattleReady", true, false) == null: return
    battle_frames += 1
    if battle_frames == 30 and early_path != "":
        root.get_texture().get_image().save_png(early_path)
    if battle_frames == 120:
        RenderingServer.frame_post_draw.disconnect(drawn)
        root.get_texture().get_image().save_png(%s)
        call_deferred("finish")
func finish():
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''


def distance(a, b):
    return sum(abs(x - y) for x, y in zip(a, b))


@unittest.skipUnless(RUNTIME and GAME_ROOT and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "requires installed game and GPU viewer")
class GcBattleColoursRuntime(unittest.TestCase):
    def run_case(self, directory, label, mode, own=False, toggle=None, seat=None, full=False):
        case = directory / label
        case.mkdir()
        settings = case / "settings.cfg"
        settings.write_text(f"eawr-settings 1\ngameplay.gc_battle_colours={mode}\n", encoding="utf-8")
        capture, early, report, driver = case / "battle.png", case / "early.png", case / "report.json", case / "driver.gd"
        driver.write_text(DRIVER % (json.dumps(early.as_posix() if toggle else ""), json.dumps(capture.as_posix())),
                          encoding="utf-8")
        environment = dict(os.environ)
        environment.pop("EAWR_GC_ALLY_OWN_COLOURS", None)
        environment.pop("EAWR_GC_COLOURS_TOGGLE", None)
        environment["EAWR_GC_COLOURS_FOLLOW_MATE"] = "1"  # frame the team mate's ship in the stills
        if own:
            environment["EAWR_GC_ALLY_OWN_COLOURS"] = "1"
        if toggle:
            environment["EAWR_GC_COLOURS_TOGGLE"] = toggle
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-game-root", GAME_ROOT, "--eawr-galactic", "Sandbox_Equal_Footing_Underworld",
            "--eawr-galactic-coop-preview", "yes", "--eawr-galactic-battle", "synthetic",
            "--eawr-galactic-battle-frames", "3600", "--eawr-settings-file", str(settings),
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
            "--eawr-report", str(report), "--eawr-audio", "off"]
            + (["--eawr-galactic-seat", str(seat)] if seat else []), cwd=ROOT, env=environment,
            capture_output=True, text=True, timeout=600, check=False)
        (case / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(capture.is_file(), result.stdout + result.stderr)
        self.assertIn("EAWR handoff phase: tactical-ready", result.stdout + result.stderr)
        campaign = json.loads(report.read_text(encoding="utf-8"))
        live = json.loads((case / "tactical.json").read_text(encoding="utf-8"))["live_session"]
        owner = campaign["local"]["player"]
        mine = {row["entity"]: tuple(row["rgb"]) for row in live["rendered_colours"] if row["player"] == owner}
        others = {row["entity"]: tuple(row["rgb"]) for row in live["rendered_colours"] if row["player"] != owner}
        before = {row["entity"]: tuple(row["rgb"]) for row in live.get("colours_before_toggle", [])
                  if row["entity"] in mine}
        print(f"GC colour capture {label}: {capture}" + (f" early {early}" if toggle else ""))
        if full:
            return campaign, live
        return mine, others, before

    def test_coop_colours(self):
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-colours-"))
        b, b_other, _ = self.run_case(directory, "b", 0)
        shades, shades_other, _ = self.run_case(directory, "c_shades", 1)
        own, own_other, _ = self.run_case(directory, "c_own", 1, own=True)
        on, on_other, on_before = self.run_case(directory, "toggle_on", 0, toggle="60:1")
        off, off_other, off_before = self.run_case(directory, "toggle_off", 1, toggle="60:0")
        self.assertGreaterEqual(len(b), 2, b)
        # B: both of the side's ships keep the colour-off faction look. C (#8359, 11-25): my own ship takes the faction's
        # coloured team colour (Factions.xml Color), the team mate's is a tint; none keeps the colour-off look.
        self.assertEqual(len(set(b.values())), 1, b)
        faction = next(iter(b.values()))
        for label, rows in (("shades", shades), ("own colours", own)):
            self.assertEqual(set(rows), set(b), label)
            self.assertNotIn(faction, rows.values(), (label, rows))
            self.assertGreaterEqual(len(set(rows.values())), 2, (label, rows))
            self.assertTrue(any(distance(rgb, faction) >= 90 for rgb in rows.values()), (label, rows))
        self.assertNotEqual(shades, own)
        # Enemies always keep their faction look, with the option off or on.
        for rows in (shades_other, own_other, on_other, off_other):
            self.assertEqual(rows, b_other)
        # The option applies to units already on the field, in both directions.
        self.assertEqual(on_before, b)
        self.assertEqual(on, shades)
        self.assertEqual(off_before, shades)
        self.assertEqual(off, b)

    def test_main_player_same_from_both_seats(self):
        # GCC-08 (owner decision 11-25, legacy #8359): the main colour goes to #1 of the faction's lobby slot, the same on
        # every peer: the co-op battle seen from the first and from the second human seat renders identical colours.
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-main-"))
        campaign, first = self.run_case(directory, "seat_first", 1, full=True)
        owner = campaign["local"]["player"]
        own_seats = sorted(row["seat"] for row in first["gc_colour_seats"]
                           if row["player"] == owner and row["side"] != 0)
        self.assertGreaterEqual(len(own_seats), 2, first["gc_colour_seats"])
        _, second = self.run_case(directory, "seat_second", 1, seat=own_seats[1], full=True)

        def by_seat(live):
            rows = {}
            for row in live["rendered_colours"]:
                if row["player"] == owner and row["controller_seat"] in own_seats[:2]:
                    rows.setdefault(row["controller_seat"], set()).add(tuple(row["rgb"]))
            return rows

        a, b = by_seat(first), by_seat(second)
        self.assertEqual(set(a), set(own_seats[:2]), first["rendered_colours"])
        self.assertEqual(a, b)  # the same colours on both screens
        self.assertTrue(all(len(colours) == 1 for colours in a.values()), a)
        self.assertNotEqual(a[own_seats[0]], a[own_seats[1]])  # #1 main, #2 a mate shade, whichever seat is local


if __name__ == "__main__":
    unittest.main()
