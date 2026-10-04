"""FL-12: a purchased carrier renders its fighters after its hyperspace arrival."""

import os
import pathlib
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from battle_input_test_support import BattleInputRunner, ROOT

sys.path.insert(0, str(ROOT))
from tools.inventory.corpus import Corpus


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game")
class CarrierArrivalGraphical(BattleInputRunner, unittest.TestCase):
    def test_bought_victory_launches_after_landing(self):
        with tempfile.TemporaryDirectory(prefix="eawr-carrier-arrival-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "menu"
            xml = mod / "Data/XML"
            xml.mkdir(parents=True)
            (mod / "Data/MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            carrier_source = corpus.read_effective("foc", "data/xml/spaceunitsfrigates.xml")
            self.assertIsNotNone(carrier_source)
            carrier = next(node for node in ET.fromstring(carrier_source.data) if node.attrib.get("Name") == "Victory_Destroyer")
            population = int(carrier.findtext("Population_Value"))
            # Isolate the real carrier purchase without spending the GPU run on station upgrades.
            for filename in ("starbases.xml", "gameconstants.xml"):
                source = corpus.read_effective("foc", "data/xml/" + filename)
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                if filename == "starbases.xml":
                    station = next(node for node in tree if node.attrib.get("Name") == "Skirmish_Empire_Star_Base_1")
                    for child in list(station):
                        if child.tag == "Tactical_Buildable_Objects_Multiplayer":
                            station.remove(child)
                    ET.SubElement(station, "Tactical_Buildable_Objects_Multiplayer").text = "Empire, Victory_Destroyer"
                else:
                    level = next(node for node in tree.iter() if node.tag == "MP_Default_Start_Tech_Level")
                    level.text = "3"
                (xml / filename).write_bytes(ET.tostring(tree, encoding="utf-8"))
            common = ["--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-player", "1",
                      "--eawr-live-follow", "1", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                      "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                      "--eawr-skirmish-slot", "1:Empire:0:human", "--eawr-skirmish-slot", "2:Rebel:1:ai",
                      "--eawr-skirmish-fleet", "1:Victory_Destroyer", "--eawr-skirmish-fleet", "2:none",
                      "--eawr-live-input", "10:click:unit=1"]
            code, menu = self._run(directory, "menu", common, camera=None, end_tick=30, session="skirmish")
            self.assertEqual(code, 0, menu.get("failure"))
            card = menu["hud"]["unit_cards"]["drawn"][0]
            self.assertEqual(card["type"], "Victory_Destroyer")
            ready = 20 + card["build_frames"] + 60
            station = next(row["position"] for row in menu["live_session"]["own_units"] if row["entity"] == 1)
            point = (station[0] + 600, station[1] - 500)
            placement = ready + 20
            end = placement + 600
            extra = common + ["--eawr-live-input", "20:click:card=0",
                              "--eawr-live-input", f"{ready}:click:hud=b_reinforcement",
                              "--eawr-live-input", f"{ready + 10}:press:hud=r_0000",
                              "--eawr-live-input", f"{ready + 15}:hover:@{point[0]},{point[1]},0",
                              "--eawr-live-input", f"{placement}:release:@{point[0]},{point[1]},0",
                              "--eawr-live-capture-ticks", ",".join(str(tick) for tick in
                                  (placement + 40, placement + 155, placement + 185, placement + 335, placement + 485)),
                              "--eawr-live-workers", "4"]
            code, result = self._run(directory, "arrival", extra, camera=None, end_tick=end, session="skirmish")
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(live["rejected"], [])
            self.assertEqual(live["economy_requests"]["buys"], 1)
            self.assertEqual(live["economy_requests"]["reinforcements"], 1)
            arrivals = [row for row in live["arrivals"] if row["owner"] == 1]
            self.assertEqual(len(arrivals), 1, arrivals)
            landed = arrivals[0]["landed_tick"]
            launched = [row for row in live["squadrons"] if row["owner"] == 1 and row["launched"] and row["seen_tick"] >= landed]
            self.assertEqual(len(launched), 3, launched)
            times = sorted(row["seen_tick"] for row in launched)
            self.assertLessEqual(times[0] - landed, 30)
            self.assertTrue(all(abs(second - first - 150) <= 2 for first, second in zip(times, times[1:])), times)
            drawn = {row["entity"] for row in live["own_units"]}
            self.assertTrue(all(set(row["members"]) <= drawn for row in launched))
            self.assertEqual(live["economy"]["population"], population)
            # Frame the observed bought ship for the eye check after its arrival.
            framed = extra.copy()
            fixed_follow = framed.index("--eawr-live-follow")
            del framed[fixed_follow:fixed_follow + 2]
            code, followed = self._run(directory, "carrier", framed + [
                "--eawr-live-follow-group", f"{landed}:{arrivals[0]['unit']}"],
                camera=None, end_tick=end, session="skirmish")
            self.assertEqual(code, 0, followed.get("failure"))
            self.assertTrue(followed["live_session"]["headless_hashes_equal"])
            self.assertEqual(followed["live_session"]["rejected"], [])


if __name__ == "__main__":
    unittest.main()
