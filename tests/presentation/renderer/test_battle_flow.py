"""Local Begin and session recreation contracts (WBF-05/10/49/50)."""

import os
import pathlib
import tempfile
import unittest

from live_session_test_support import LiveSessionRunner
from test_space_map_mode import decode_png


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires the viewer GPU host and game data")
class BattleFlowGpu(LiveSessionRunner, unittest.TestCase):
    def test_pause_uses_the_native_shell_and_resume_button(self):
        with tempfile.TemporaryDirectory(prefix="battle-pause-shell-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "pause", (
                "--eawr-live-ticks", "80", "--eawr-live-step", "1", "--eawr-live-workers", "2",
                "--eawr-live-input", "f5:click:hud=pause", "--eawr-live-input", "f35:click:hud=resume",
                "--eawr-live-capture-frames", "10,30,45", "--eawr-live-ai", "off", "--eawr-audio", "off"),
                engine_args=("--quit-after", "600"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            overlay = result["hud"]["battle_overlay"]
            self.assertTrue(overlay["authored_pause_shell"])
            self.assertIsNone(overlay["paused_banner"])
            self.assertEqual(overlay["resume_presses"], 1)
            self.assertGreater(overlay["resume_rect"][1], 30)
            self.assertLess(overlay["resume_rect"][1], 70)
            track = result["live_session"]["time"]["track"]
            held = next(row for row in track if row["state"] == "paused")
            resumed = next(row for row in track if row["cause"] == "resume")
            self.assertEqual(held["tick"], resumed["tick"], "the battle tick stays held while the UI animates")
            for frame in (10, 30, 45):
                self.assertGreater((directory / f"pause_f{frame:04d}.png").stat().st_size, 10000)
            images = [decode_png((directory / f"pause_f{frame:04d}.png").read_bytes()) for frame in (10, 30, 45)]
            rect = result["hud"]["time_panel"]["pause"]["rect"]
            x, y, width, height = [int(value) for value in rect]
            crops = [[pixels[row][x:x + width] for row in range(y, y + height)] for _, _, pixels in images[:2]]
            self.assertNotEqual(crops[0], crops[1], "the pause flash continues while the battle tick is held")
            red = lambda image: sum(r > 150 and r > 2 * g and r > 2 * b
                for row in image[2][:75] for r, g, b in row[450:830])
            self.assertGreater(red(images[0]), 20, "the native top-edge banner and Resume caption are red")
            self.assertEqual(red(images[2]), 0, "Resume hides the native pause shell")

    def test_local_faction_outcome_sound_plays_once_at_the_deciding_frame(self):
        for station, outcome, event in ((8, "victory", "RHD_Battle_End"), (1, "defeat", "RHD_Defeated")):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory(prefix="battle-outcome-sound-") as temporary:
                code, result = self._battle_end(pathlib.Path(temporary), outcome, station, outcome,
                    ("--eawr-live-ai", "off", "--eawr-audio", "off"))
                self.assertEqual(code, 0, result.get("failure"))
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                audio = result["battle_audio"]
                key = "battle_" + outcome
                self.assertEqual(audio["requested"].get(key + ":" + event), 1, audio["requested"])
                starts = [row for row in audio["ability_starts"] if row["reason"] == key]
                self.assertEqual(len(starts), 1, starts)
                decided = result["live_session"]["outcome"]["decided_tick"]
                self.assertLessEqual(abs(starts[0]["tick"] - decided), 3)
                self.assertFalse(any(event in item for item in audio["missing_events"]), audio["missing_events"])

    def test_results_resolve_authored_win_and_loss_summary_music(self):
        with tempfile.TemporaryDirectory(prefix="battle-summary-music-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "mod"
            xml = mod / "Data/XML"
            xml.mkdir(parents=True)
            (mod / "Data/MegaFiles.xml").write_text('<Mega_Files><File>Absent.meg</File></Mega_Files>', encoding="utf-8")
            (xml / "Audio.xml").write_text('''<Audio>
<Music_Event_Battle_End_Summary_Screen_Win>ResultsWinTest</Music_Event_Battle_End_Summary_Screen_Win>
<Music_Event_Battle_End_Summary_Screen_Lose>ResultsLossTest</Music_Event_Battle_End_Summary_Screen_Lose>
</Audio>''', encoding="utf-8")
            filename = "UND_Zann_Consortium_Theme.MP3"  # opened from the installed game, never copied into the fixture
            (xml / "MusicEvents.xml").write_text('<MusicEvents>' + ''.join(
                f'<MusicEvent Name="{name}"><Files>{filename}</Files><Volume_Percent>35</Volume_Percent>'
                '<Fade_In_Seconds>0.2</Fade_In_Seconds><Fade_Out_Previous_Seconds>0.3</Fade_Out_Previous_Seconds>'
                '<Loop>No</Loop></MusicEvent>' for name in ("ResultsWinTest", "ResultsLossTest"))
                + '</MusicEvents>', encoding="utf-8")
            for station, outcome, event in ((8, "victory", "ResultsWinTest"), (1, "defeat", "ResultsLossTest")):
                with self.subTest(outcome=outcome):
                    code, result = self._battle_end(directory, outcome, station, outcome, (
                        "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-audio", "off"))
                    self.assertEqual(code, 0, result.get("failure"))
                    audio = result["battle_audio"]
                    selected = [row for row in audio["music"] if " summary " in row]
                    self.assertEqual(len(selected), 1, selected)
                    self.assertIn(f" summary {event} {filename}", selected[0])
                    self.assertNotIn(filename, audio["missing_samples"])
                    self.assertFalse(any("summary music event" in item for item in audio["problems"]), audio["problems"])

    def test_authored_results_preserve_early_losses_for_win_and_loss(self):
        for station, outcome in ((8, "victory"), (1, "defeat")):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory(prefix="battle-results-") as temporary:
                directory = pathlib.Path(temporary)
                code, result = self._battle_end(directory, outcome, station, outcome, (
                    "--eawr-live-order", "5:damage:11@1000000", "--eawr-live-ai", "off", "--eawr-audio", "off"))
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(live["outcome"]["local_result"], outcome)
                self.assertEqual(live["results"]["scoring_diagnostic"], "")
                sides = live["results"]["sides"]
                self.assertTrue(any(row["name"] == "Tartan_Patrol_Cruiser" for row in sides[1]["losses"]), sides)
                for side in sides:
                    self.assertEqual(side["total"], sum(row["count"] for row in side["losses"] + side["heroes"]))
                    self.assertEqual(len(side["statistics"]), 5)
                panel = result["hud"]["battle_overlay"]["end_panel"]
                self.assertTrue(panel["authored_dialog"])
                self.assertEqual(panel["rect"], [0, 0, 1280, 720])
                self.assertGreater((directory / f"{outcome}_f0080.png").stat().st_size, 10000)

    def test_results_run_the_mounted_scoring_module(self):
        with tempfile.TemporaryDirectory(prefix="battle-scoring-mod-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "mod"
            script = mod / "Data/Scripts/Miscellaneous/GameScoring.lua"
            script.parent.mkdir(parents=True)
            (mod / "Data/MegaFiles.xml").write_text('<Mega_Files><File>Absent.meg</File></Mega_Files>', encoding="utf-8")
            script.write_text('''require("PGBase")
ServiceRate = 0
function Base_Definitions() losses = 0; produced = 0 end
function Game_Mode_Starting_Event(mode, map) mode_seen = mode end
function Tactical_Unit_Destroyed_Event(object, killer)
  losses = losses + 1
  cost_seen = object.Get_Game_Scoring_Type().Get_Score_Cost_Credits()
end
function Tactical_Production_End_Event(kind, player, location) produced = produced + 1 end
function Player_Quit_Event(player) quit_seen = player.Get_ID() end
function main() while true do PumpEvents() end end
function Get_Game_Stat_For_Control_ID(player, control, tactical)
  return "mounted:" .. mode_seen .. ":" .. tostring(losses) .. ":" .. tostring(produced) .. ":" .. control
end
''', encoding="utf-8")
            code, result = self._battle_end(directory, "mod", 8, "victory", (
                "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-audio", "off"))
            self.assertEqual(code, 0, result.get("failure"))
            model = result["live_session"]["results"]
            self.assertEqual(model["scoring_diagnostic"], "")
            self.assertGreater(model["scoring_pumps"], 0)
            for side in model["sides"]:
                for control, value in side["statistics"].items():
                    self.assertTrue(value.startswith("mounted:Space:"), value)
                    self.assertTrue(value.endswith(":0:" + control), value)

    def test_intentional_quit_while_paused_opens_results_before_exit(self):
        for pending_victory in (False, True):
            with self.subTest(pending_victory=pending_victory), tempfile.TemporaryDirectory(prefix="battle-quit-") as temporary:
                directory = pathlib.Path(temporary)
                orders = ("--eawr-live-order", "1:damage:8@1000000") if pending_victory else ()
                code, result = self._run(directory, "quit", (
                    "--eawr-live-ticks", "100", "--eawr-live-step", "1",
                    "--eawr-live-input", "f5:click:hud=pause", "--eawr-live-input", "f10:click:hud=quit",
                    "--eawr-live-input", "f25:click:hud=quit", "--eawr-live-capture-frames", "8,20",
                    "--eawr-live-workers", "2", "--eawr-live-ai", "off", "--eawr-audio", "off", *orders),
                    engine_args=("--quit-after", "600"))
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(live["phase"], "returning")
                self.assertEqual(live["outcome"]["condition"], "intentional_quit")
                self.assertEqual(live["outcome"]["local_result"], "defeat")
                self.assertEqual(live["completed_ticks"], live["outcome"]["end_tick"])
                self.assertEqual(live["intentional_quits"], [{"player": 1, "tick": live["outcome"]["decided_tick"]}])
                self.assertLess(live["outcome"]["end_tick"] - live["outcome"]["decided_tick"], 210)
                self.assertEqual(result["hud"]["battle_overlay"]["quit_presses"], 2)
                self.assertIsNotNone(live["battle_end"]["ended_frame"])
                for frame in (8, 20):
                    self.assertGreater((directory / f"quit_f{frame:04d}.png").stat().st_size, 10000)

    def test_selected_all_units_keeps_battle_running_after_station_loss(self):
        for selection in ("starbase", "all-units"):
            with self.subTest(selection=selection), tempfile.TemporaryDirectory(prefix="battle-condition-") as temporary:
                directory = pathlib.Path(temporary)
                code, result = self._run(directory, "condition", (
                    "--eawr-skirmish-victory", selection,
                    "--eawr-live-order", "1:damage:8@1000000",
                    "--eawr-live-ticks", "30", "--eawr-live-step", "3",
                    "--eawr-live-workers", "2", "--eawr-live-ai", "off", "--eawr-audio", "off"),
                    session=("--eawr-live-session", "skirmish"))
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(live["victory_condition"],
                                 "enemy_starbase_destroyed" if selection == "starbase" else "all_enemy_units_destroyed")
                if selection == "starbase":
                    self.assertEqual(live["outcome"]["winner"], 1)
                else:
                    self.assertIsNone(live["outcome"])

    def test_begin_holds_tick_zero_until_button_or_key(self):
        for gesture in ("f5:click:hud=begin", "f5:key:ENTER"):
            with self.subTest(gesture=gesture), tempfile.TemporaryDirectory(prefix="battle-begin-") as temporary:
                directory = pathlib.Path(temporary)
                code, result = self._run(directory, "begin", (
                    "--eawr-live-begin", "manual", "--eawr-live-input", gesture,
                    "--eawr-live-ticks", "9", "--eawr-live-step", "1",
                    "--eawr-live-workers", "2", "--eawr-live-ai", "off", "--eawr-audio", "off",
                    "--eawr-live-capture-frames", "2,4,9"), engine_args=("--quit-after", "600"))
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                # Mouse injection moves to the button first, then dispatches its click;
                # the key gesture dispatches directly on its requested frame.
                begin_frame = 6 if gesture == "f5:click:hud=begin" else 5
                self.assertEqual((live["ready_tick"], live["begin_tick"], live["begin_frame"]), (0, 0, begin_frame))
                self.assertTrue(live["headless_hashes_equal"])
                self.assertGreaterEqual(live["completed_ticks"], 9)
                self.assertFalse(result["hud"]["battle_overlay"]["begin_visible"])
                for frame in (2, 4, 9):
                    self.assertGreater((directory / f"begin_f{frame:04d}.png").stat().st_size, 10000)


if __name__ == "__main__":
    unittest.main()
