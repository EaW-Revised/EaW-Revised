"""Validate the geometry evidence and bounded Lua commands without the game."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("fog_ghost_debugger", ROOT / "tools/validation/p1_capture/foc_fog_ghost_debugger.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class FogGhostEvidenceContracts(unittest.TestCase):
    def state(self):
        return {"station_valid": True, "local_station_valid": True, "camera": True,
                "station": [4500, -4500, 0], "local_station": [-4500, 4500, 0], "now": 10,
                "scout_valid": True, "scout": [4000, -4500, 0], "placed": "T",
                "station_owner_local": False, "scout_owner_local": True, "station_invulnerable": True}

    def test_near_scout_can_establish_discovery(self):
        driver.check_state(self.state(), "seen")

    def test_near_scout_cannot_prove_withdrawal(self):
        with self.assertRaisesRegex(AssertionError, "sight geometry"):
            driver.check_state(self.state(), "fogged")

    def test_failed_teleport_and_missing_camera_are_rejected(self):
        for key, value in (("placed", "M"), ("camera", False), ("scout_valid", False),
                           ("station_owner_local", True), ("scout_owner_local", False), ("station_invulnerable", False)):
            state = self.state()
            state[key] = value
            with self.assertRaises(AssertionError):
                driver.check_state(state, "seen")

    def test_commands_fit_the_original_buffer_with_wrapper_space(self):
        for chunk in (*driver.PREPARE, *driver.SPAWN, *driver.WITHDRAW):
            self.assertLessEqual(len(chunk.encode("utf-8")), 190)


if __name__ == "__main__":
    unittest.main()
