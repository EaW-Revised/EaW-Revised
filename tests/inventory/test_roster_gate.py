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
        self.assertFalse(any(a["unit"] == "Interdictor_Cruiser" and a["ability"] in {"INTERDICT", "MISSILE_SHIELD"}
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
        self.assertNotIn("Krayt_Class_Destroyer", disabled)
        self.assertEqual(before["disabled_abilities"], after["disabled_abilities"])
        self.assertTrue(before["infrastructure_gaps"])
        self.assertTrue(all("MASS_DRIVER" not in u["reason"] for u in after["infrastructure_gaps"]))
        self.assertFalse(any("WEAPON_SPECIAL" in u["reason"] for u in after["infrastructure_gaps"]))

    def test_blast_capability_is_required_separately_from_special_admission(self):
        self.assertIn("blast-damage", self.review["supported_capabilities"])
        enabled = gate.generate(self.census, self.review)
        self.review["supported_capabilities"].remove("blast-damage")
        blocked = gate.generate(self.census, self.review)
        before = {u["unit"]: u for u in blocked["disabled_units"]}
        after = {u["unit"]: u for u in enabled["disabled_units"]}
        for ship in ("Broadside_Class_Cruiser", "Marauder_Missile_Cruiser"):
            self.assertIn("Proj_Ship_Diamond_Boron_Missile", before[ship]["reason"])
            self.assertNotIn(ship, after)
            barrage = [a for a in enabled["disabled_abilities"]
                       if a["unit"] == ship and a["ability"] == "BARRAGE"]
            self.assertEqual(barrage, [])
        self.assertIn("blast-area", before["Krayt_Class_Destroyer"]["reason"])
        self.assertNotIn("Krayt_Class_Destroyer", after)
        self.assertEqual(blocked["disabled_abilities"], enabled["disabled_abilities"])

    def test_special_admission_is_unit_scoped_and_independent_of_blast(self):
        enabled = gate.generate(self.census, self.review)
        self.assertNotIn("HARD_POINT_WEAPON_SPECIAL", self.review["weapon_hardpoints"])
        self.assertNotIn("The_Peacebringer", {u["unit"] for u in enabled["disabled_units"]})
        self.assertNotIn("Krayt_Class_Destroyer", {u["unit"] for u in enabled["disabled_units"]})
        self.assertEqual(enabled["infrastructure_gaps"], [])
        self.assertFalse(any(a["unit"] == "The_Peacebringer" and a["ability"] == "BLAST"
                             for a in enabled["disabled_abilities"]))
        self.review["special_weapon_units"] = []
        disabled = gate.generate(self.census, self.review)
        self.assertIn("The_Peacebringer", {u["unit"] for u in disabled["disabled_units"]})
        self.review["abilities"].remove("BLAST")
        self.review["special_weapon_units"] = ["Krayt_Class_Destroyer"]
        without_blast = gate.generate(self.census, self.review)
        self.assertTrue(any(a["unit"] == "The_Peacebringer" and a["ability"] == "BLAST"
                            for a in without_blast["disabled_abilities"]))
        self.assertNotIn("Krayt_Class_Destroyer", {u["unit"] for u in without_blast["disabled_units"]})
        self.assertFalse(any(a["unit"] == "Krayt_Class_Destroyer" for a in without_blast["disabled_abilities"]))
        self.assertEqual({u["unit"] for u in without_blast["infrastructure_gaps"]},
                         {f"Skirmish_Underworld_Star_Base_{level}" for level in range(3, 6)})

    def test_station_special_admission_does_not_hide_other_weapon_gaps(self):
        self.review["supported_capabilities"].remove("mass-driver")
        rows = gate.generate(self.census, self.review)["infrastructure_gaps"]
        self.assertEqual({row["unit"] for row in rows},
                         {f"Skirmish_Underworld_Star_Base_{level}" for level in range(1, 6)})
        self.assertTrue(all("MASS_DRIVER" in row["reason"] and "WEAPON_SPECIAL" not in row["reason"] for row in rows))
        self.review["special_weapon_units"].remove("Skirmish_Underworld_Star_Base_4")
        rows = gate.generate(self.census, self.review)["infrastructure_gaps"]
        self.assertEqual({row["unit"] for row in rows if "WEAPON_SPECIAL" in row["reason"]},
                         {"Skirmish_Underworld_Star_Base_4"})

    def test_barrage_requires_both_reviewed_handler_and_fire_rate_modifier(self):
        for field, value in (("abilities", "BARRAGE"), ("ability_modifiers", "FIRE_RATE_MULTIPLIER")):
            with self.subTest(field=field):
                review = dict(self.review)
                review[field] = [entry for entry in review[field] if entry != value]
                disabled = gate.generate(self.census, review)["disabled_abilities"]
                for ship in ("Broadside_Class_Cruiser", "Marauder_Missile_Cruiser"):
                    self.assertTrue(any(row["unit"] == ship and row["ability"] == "BARRAGE"
                                        for row in disabled))

    def test_empty_generated_tables_compile_shape(self):
        self.assertIn("std::array<DisabledUnit, 0>", gate.header({"disabled_units": [], "disabled_abilities": []}))

    def test_lure_admission_requires_handler_and_authored_damage_modifier(self):
        enabled = gate.generate(self.census, self.review)
        for unit in ("A-Wing", "A_Wing_Squadron"):
            self.assertFalse(any(a["unit"] == unit and a["ability"] == "LURE"
                                 for a in enabled["disabled_abilities"]))
            self.assertFalse(any(u["unit"] == unit for u in enabled["disabled_units"]))
        for field, value, units in (
            ("abilities", "LURE", ("A-Wing", "A_Wing_Squadron")),
            ("ability_modifiers", "TAKE_DAMAGE_MULTIPLIER", ("A-Wing",)),
        ):
            review = dict(self.review)
            review[field] = [entry for entry in review[field] if entry != value]
            data = gate.generate(self.census, review)
            for unit in units:
                self.assertTrue(any(a["unit"] == unit and a["ability"] == "LURE"
                                    for a in data["disabled_abilities"]))
            self.assertEqual(data["disabled_units"], enabled["disabled_units"])

    def test_player_tooltips_do_not_expose_diagnostic_identifiers(self):
        for u in gate.generate(self.census, self.review)["disabled_units"]:
            self.assertLess(len(u["tooltip"]), 90)
            self.assertNotIn("#", u["tooltip"])
            self.assertNotIn("_", u["tooltip"])


if __name__ == "__main__":
    unittest.main()
