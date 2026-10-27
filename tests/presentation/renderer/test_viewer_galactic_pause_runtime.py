"""WGM-07 project pause policy: lit native-input captures of admit/refuse/running."""
import json
import pathlib
import unittest

try:
    from . import test_viewer_galactic_fleet_drag_runtime as drag
except ImportError:
    import test_viewer_galactic_fleet_drag_runtime as drag


@unittest.skipUnless(drag.support.RUNTIME and drag.support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPauseRuntime(unittest.TestCase):
    capture = drag.GalacticFleetDragRuntime.capture
    center = staticmethod(drag.GalacticFleetDragRuntime.center)
    project = staticmethod(drag.GalacticFleetDragRuntime.project)
    native_drag = drag.GalacticFleetDragRuntime.native_drag

    def test_policy_three_states(self):
        for label, fixture, paused, admitted in [
            ("paused-admit", "fleet-drag-paused-admit", True, True),
            ("paused-refuse", "fleet-drag-paused-refuse", True, False),
            ("running", "fleet-drag-paused-admit", False, True),
        ]:
            with self.subTest(state=label):
                data = self.native_drag("member-radar", fixture=fixture, paused=paused)
                self.assertEqual(data["fleet_hud"]["drag"]["orders"], int(admitted))
                self.assertEqual(data["live_session"]["pause_depth"], int(paused))
                self.assertEqual(data["live_session"]["departure_hint_visible"], paused and admitted)
                moved = [row for row in data["fleets"] if row["fleet"] == 601 and row["transit"]]
                self.assertEqual(bool(moved), admitted)
                if paused:
                    self.assertEqual(data["live_session"]["galactic_frame"], 0)
                if admitted:
                    self.assertFalse(any(row["fleet"] == 601 for row in data["fleet_hud"]["cards"]))
                directory = pathlib.Path(data["evidence_directory"])
                (directory / "pause-state.json").write_text(json.dumps({"state": label, "look": "galactic camera lighting (WGP-01); neutral synthetic axis",
                    "paused": paused, "admitted": admitted}), encoding="utf-8")
