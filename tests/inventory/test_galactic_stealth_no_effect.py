"""WGM-215: validated galactic stealth cannot consume its actor."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
import tag_registry as registry


class GalacticStealthNoEffectTests(unittest.TestCase):
    def test_all_authored_classes_have_the_sourced_no_effect_decision(self):
        value = registry.load_registry(registry.DEFAULT_STATUSES)
        rows = [row for row in value["rows"]
                if row["tag"].endswith("Galactic_Stealth_Ability/Causes_Despawn")]
        self.assertTrue(rows)
        self.assertEqual({name for row in rows for name in row["classes"]},
                         {"Abilities", "Container", "GenericHeroUnit", "GroundCompany", "GroundInfantry",
                          "HeroCompany", "HeroUnit", "SpaceUnit", "Squadron", "UniqueUnit"})
        for row in rows:
            with self.subTest(classes=row["classes"]):
                self.assertEqual(row["status"], "foc-ignores")
                self.assertEqual(row["evidence"], "WGM-215")
                self.assertIn("debug build", value["evidence"][row["evidence"]])


if __name__ == "__main__":
    unittest.main()
