"""Core roster policy: determinism, optional abilities, recursive launch gates."""
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.inventory import roster_gate as gate


class RosterGateTests(unittest.TestCase):
    def setUp(self):
        self.census = json.loads(gate.CENSUS.read_text(encoding="utf-8"))
        self.review = json.loads(gate.REVIEW.read_text(encoding="utf-8"))

    def test_deterministic_and_current(self):
        first = gate.encode(gate.generate(self.census, self.review)).encode()
        self.census["units"].reverse()
        for u in self.census["units"]:
            u["dependencies"].reverse()
        self.assertEqual(first, gate.encode(gate.generate(self.census, self.review)).encode())
        self.assertEqual(first, gate.OUTPUT.read_bytes())

    def test_minor_tags_and_optional_abilities_do_not_block(self):
        data = gate.generate(self.census, self.review)
        disabled = {u["unit"] for u in data["disabled_units"]}
        self.assertNotIn("Interdictor_Cruiser", disabled)
        self.assertNotIn("MC30_Frigate", disabled)
        self.assertTrue(any(a["unit"] == "Interdictor_Cruiser" and a["ability"] == "INTERDICT"
                            for a in data["disabled_abilities"]))
        for u in self.census["units"]:
            u["status"] = "missing"
            u["need_status_including_dependencies"] = {"unregistered:minor": "missing"}
        self.assertEqual(data, gate.generate(self.census, self.review))

    def test_recursive_craft_gate_and_cycles(self):
        craft = next(u for u in self.census["units"] if u["id"] == "X-Wing")
        craft["locomotors"] = ["UNSUPPORTED_LOCOMOTOR"]
        craft["dependencies"].append({"object": "Rebel_X-Wing_Squadron", "relation": "team_container"})
        disabled = {u["unit"]: u["reason"] for u in gate.generate(self.census, self.review)["disabled_units"]}
        self.assertIn("X-Wing", disabled)
        self.assertIn("Rebel_X-Wing_Squadron", disabled)
        self.assertIn("X-Wing", disabled["Rebel_X-Wing_Squadron"])

    def test_mass_driver_only_units_reenable_with_data(self):
        self.review["supported_capabilities"] = [c for c in self.review["supported_capabilities"] if c != "mass-driver"]
        before = gate.generate(self.census, self.review)
        blocked = {u["unit"] for u in before["disabled_units"]}
        self.assertIn("Kedalbe_Battleship", blocked)
        self.assertIn("Vengeance_Frigate", blocked)
        stations = {u["unit"] for u in before["infrastructure_gaps"] if "MASS_DRIVER" in u["reason"]}
        self.assertEqual(stations, {f"Skirmish_Underworld_Star_Base_{level}" for level in range(1, 6)})
        self.review["supported_capabilities"].append("mass-driver")
        after = gate.generate(self.census, self.review)
        disabled = {u["unit"] for u in after["disabled_units"]}
        self.assertNotIn("Kedalbe_Battleship", disabled)
        self.assertNotIn("Vengeance_Frigate", disabled)
        self.assertIn("Krayt_Class_Destroyer", disabled)
        self.assertEqual(before["disabled_abilities"], after["disabled_abilities"])
        self.assertTrue(before["infrastructure_gaps"])
        self.assertTrue(all("MASS_DRIVER" not in u["reason"] for u in after["infrastructure_gaps"]))
        self.assertTrue(any("WEAPON_SPECIAL" in u["reason"] for u in after["infrastructure_gaps"]))

    def test_empty_generated_tables_compile_shape(self):
        self.assertIn("std::array<DisabledUnit, 0>", gate.header({"disabled_units": [], "disabled_abilities": []}))

    def test_player_tooltips_do_not_expose_diagnostic_identifiers(self):
        for u in gate.generate(self.census, self.review)["disabled_units"]:
            self.assertLess(len(u["tooltip"]), 90)
            self.assertNotIn("#", u["tooltip"])
            self.assertNotIn("_", u["tooltip"])


if __name__ == "__main__":
    unittest.main()
