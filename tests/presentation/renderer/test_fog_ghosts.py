"""FW-25..29: remembered station art after a Rebel scout withdraws."""

import os
import pathlib
import shutil
import statistics
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from live_session_test_support import CAMERA, CORVETTE, EMPIRE_STATION, LiveSessionRunner, decode_png


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed-game GPU lane")
class FogGhostsGraphical(LiveSessionRunner, unittest.TestCase):
    def test_station_seen_then_fogged_remains_half_lit(self):
        with tempfile.TemporaryDirectory(prefix="eawr-fog-ghosts-") as temporary:
            directory = pathlib.Path(temporary)
            code, initial = self._run(directory, "layout", ("--eawr-live-ticks", "3", "--eawr-live-ai", "off"))
            self.assertEqual(code, 0, initial.get("failure"))
            station = next(marker for marker in initial["live_session"]["start_markers"]
                           if marker["player"] == 2 and marker["use"] == "station")
            x, y, _ = station["position"]
            camera = ET.parse(CAMERA)
            camera.getroot().find("initial").attrib.update(target_x=str(x), target_y=str(y), zoom="0.75")
            camera_path = directory / "station-camera.xml"
            camera.write(camera_path, encoding="utf-8", xml_declaration=True)
            shutil.copyfile(CAMERA.parent / "space-live-camera-bindings.json", directory / "space-live-camera-bindings.json")
            # Keep the station intact, disable its weapon/hangar hardpoints and remove the
            # opposing capital ships. Squadrons are containers, not damageable hulls.
            orders = []
            for hardpoint in range(2, 7):
                orders += ["--eawr-live-order", f"1:damage:{EMPIRE_STATION}@100000,{hardpoint}"]
            for enemy in (11, 12):
                orders += ["--eawr-live-order", f"1:damage:{enemy}@1000000"]
            orders += ["--eawr-live-order", f"15:move:{CORVETTE}@{x - 850},{y},0",
                       "--eawr-live-order", f"4200:move:{CORVETTE}@-5057,4700,0"]
            common = (*orders, "--eawr-live-ai", "off", "--eawr-live-ticks", "6000", "--eawr-live-step", "15",
                      "--eawr-live-workers", "2", "--eawr-hud", "off", "--eawr-audio", "off",
                      "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")
            code, hidden = self._run(directory, "hidden", (*common, "--eawr-live-capture-ticks", "0,3600,4635,6000"), camera=camera_path)
            self.assertEqual(code, 0, hidden.get("failure"))
            live = hidden["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = [row for row in live["fog_ghost_log"] if row["entity"] == EMPIRE_STATION]
            self.assertTrue(rows, live)
            self.assertFalse(rows[0]["visible"], rows)
            self.assertFalse(rows[0]["ghost"], rows)
            self.assertEqual(rows[0]["pieces"], 0, rows)
            self.assertTrue(any(row["visible"] for row in rows), rows)
            self.assertFalse(rows[-1]["visible"], rows)
            self.assertTrue(rows[-1]["ghost"], rows)
            self.assertGreater(rows[-1]["pieces"], 0, rows)
            self.assertEqual(rows[-1]["light_scale"], 0.5, rows)
            code, revealed = self._run(directory, "revealed", (*common, "--eawr-live-reveal", "on"), camera=camera_path)
            self.assertEqual(code, 0, revealed.get("failure"))
            self.assertEqual(live["final_state_sha256"], revealed["live_session"]["final_state_sha256"])
            self.assertEqual(revealed["live_session"]["fog_ghost_pieces"], 0)
            # Highest hull bump shaders compute on stored RGB (docs/rendering.md colour
            # policy), so check those captured values directly. An sRGB decode would turn
            # the source's .5 light scale into roughly .25 and test the wrong equation.
            images = [decode_png((directory / name).read_bytes()) for name in
                      ("hidden_t0000.png", "hidden.png", "hidden_t4635.png")]
            self.assertEqual({image[:2] for image in images}, {(1280, 720)})
            sky, ghost, full = [image[2] for image in images]
            ratios = []
            for py in range(120, 600):
                for px in range(260, 1020):
                    a, b, c = sky[py][px], ghost[py][px], full[py][px]
                    if max(abs(c[i] - a[i]) for i in range(3)) < 60: continue
                    channel = max(range(3), key=lambda i: c[i])
                    if not 90 <= c[channel] <= 220: continue
                    ratios.append(b[channel] / c[channel])
            self.assertGreater(len(ratios), 100, "station art missing from GPU comparison")
            ratio = statistics.median(ratios)
            self.assertGreater(ratio, 0.35, ratio)
            self.assertLess(ratio, 0.65, ratio)


if __name__ == "__main__":
    unittest.main()
