"""GPU regressions for hero team cards and parentless fighter locomotors."""

import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from battle_input_test_support import BattleInputRunner


LIT = ("--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")
QUIET = ("--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-audio", "off")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and
                     os.environ.get("EAWR_EAW_GAME_ROOT"), "requires the GPU viewer rig")
class HeroSquadronsGraphical(BattleInputRunner, unittest.TestCase):
    def test_hero_team_has_one_selection_card_and_control_group(self):
        # WHE-SQ-01 / L-2: heterogeneous names do not split an authored homogeneous team.
        for faction, hero, count in (("Empire", "Darth_Vader_TIE_Fighter_Squadron", 7),
                                     ("Rebel", "Rogue_Squadron_Space", 6)):
            with self.subTest(hero=hero), tempfile.TemporaryDirectory(prefix="eawr-hero-card-") as temporary:
                directory = pathlib.Path(temporary)
                fleet = ("--eawr-skirmish-slot", f"1:{faction}:0:human",
                         "--eawr-skirmish-slot", "2:Underworld:1:ai",
                         "--eawr-skirmish-fleet", f"1:{hero}",
                         "--eawr-skirmish-fleet", "2:none", "--eawr-live-step", "1")
                code, probe = self._run(directory, "probe", (*LIT, *QUIET, *fleet),
                                        session="skirmish", camera=None, end_tick=1)
                self.assertEqual(code, 0, probe.get("failure"))
                entity = next(row["entity"] for row in probe["live_session"]["start_fleet"]
                              if row["player"] == 1 and row["type"] == hero)
                code, result = self._run(directory, "selected-lit", (
                    *LIT, *QUIET, *fleet, "--eawr-live-follow", str(entity),
                    "--eawr-live-input", f"20:click:icon={entity}",
                    "--eawr-live-input", "25:key:3+ctrl"),
                    session="skirmish", camera=None, end_tick=60)
                self.assertEqual(code, 0, result.get("failure"))
                live, battle = result["live_session"], result["battle_input"]
                team = next(row for row in live["squadrons"] if row["container"] == entity)
                self.assertEqual(len(team["members"]), count)
                self.assertEqual(battle["selected"], [entity], battle["log"])
                self.assertEqual(battle["groups"]["3"], [entity])
                cards = battle["unit_cards"]["cards"]
                self.assertEqual(len(cards), 1, cards)
                self.assertEqual(cards[0]["unit"], entity)
                self.assertEqual(cards[0]["members"], [entity])
                self.assertTrue(cards[0]["squadron"])
                self.assertTrue(live["headless_hashes_equal"])
                self.assertTrue((directory / "selected-lit.png").is_file())

    def test_slave_i_flies_and_fights_enemy_fighters(self):
        # WHE-SQ-02: one hero entity flies continuously and acquires fighter targets.
        with tempfile.TemporaryDirectory(prefix="eawr-solo-fighter-") as temporary:
            directory = pathlib.Path(temporary)
            fleet = ("--eawr-skirmish-fleet", "1:Rebel_X-Wing_Squadron",
                     "--eawr-skirmish-fleet", "2:Slave_I", "--eawr-live-player", "2",
                     "--eawr-live-step", "2")
            code, probe = self._run(directory, "probe", (*LIT, *QUIET, *fleet),
                                    session="skirmish", camera=None, end_tick=1)
            self.assertEqual(code, 0, probe.get("failure"))
            start = probe["live_session"]["start_fleet"]
            hero = next(row for row in start if row["player"] == 2 and row["type"] == "Slave_I")
            enemy = next(row for row in start if row["player"] == 1 and row["type"] == "Rebel_X-Wing_Squadron")
            entity, target = hero["entity"], enemy["entity"]
            code, result = self._run(directory, "dogfight-lit", (
                *LIT, *QUIET, *fleet, "--eawr-live-follow", str(entity),
                "--eawr-live-order", f"1:move:{target}@-950,900,0",
                "--eawr-live-order", f"1:move:{entity}@-950,900,0",
                "--eawr-live-order", f"1450:attack:{entity}@{target}",
                "--eawr-live-input", f"1500:click:unit={entity}",
                "--eawr-live-capture-ticks", "1450,1500,1550,1600"),
                session="skirmish", camera=None, end_tick=1700)
            self.assertEqual(code, 0, result.get("failure"))
            live, battle = result["live_session"], result["battle_input"]
            self.assertEqual(live["rejected"], [])
            self.assertTrue(live["headless_hashes_equal"])
            final = self._position(result, entity)
            self.assertGreater(sum((final[i] - hero["position"][i]) ** 2 for i in (0, 1)), 100 ** 2)
            enemy_members = next(row["members"] for row in live["squadrons"] if row["container"] == target)
            self.assertTrue(any(row["shooter"] == entity and row["target"] in enemy_members
                                for row in live["first_hits"]), live["first_hits"])
            # The self flight group remains internal: one ordinary hero card and selection.
            self.assertNotIn(entity, [row["container"] for row in live["squadrons"]])
            self.assertEqual(battle["selected"], [entity], battle["log"])
            self.assertEqual(len(battle["unit_cards"]["cards"]), 1)
            self.assertFalse(battle["unit_cards"]["cards"][0]["squadron"])


if __name__ == "__main__":
    unittest.main()
