"""Reclassifying registry rows that only ground or galactic objects carry (tools/inventory/tag_registry_reclassify_ground.py)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

import tag_registry_reclassify_ground as rg  # noqa: E402

XML = """<Root>
<HeroUnit Name="Ground_Hero"><CategoryMask>LandHero</CategoryMask><Land_Model_Name>A.alo</Land_Model_Name>
  <Abilities><Ground_Only_Ability><Range>1</Range></Ground_Only_Ability><Shared_Ability><Range>1</Range></Shared_Ability></Abilities></HeroUnit>
<HeroUnit Name="Both_Hero"><CategoryMask>LandHero | SpaceHero</CategoryMask>
  <Abilities><Shared_Ability><Range>1</Range></Shared_Ability></Abilities></HeroUnit>
<HeroUnit Name="Ground_Variant" ><Variant_Of_Existing_Type>Ground_Hero</Variant_Of_Existing_Type></HeroUnit>
<HeroUnit Name="Agent"><CategoryMask>NonCombatHero</CategoryMask><Abilities><Slice_Ability><Range>1</Range></Slice_Ability></Abilities></HeroUnit>
<HeroUnit Name="Dual_Hero"><CategoryMask>LandHero | SpaceHero</CategoryMask><Space_Layer>Land</Space_Layer>
  <Land_Model_Name>B.alo</Land_Model_Name>
  <Abilities><Flame_Ability><Range>1</Range></Flame_Ability><Spaceish_Ability><Range>1</Range></Spaceish_Ability></Abilities></HeroUnit>
<HeroUnit Name="Dual_Hero_Space_Model"><CategoryMask>LandHero | SpaceHero</CategoryMask><Space_Layer>Land</Space_Layer>
  <Land_Model_Name>C.alo</Land_Model_Name><Space_Model_Name>C_S.alo</Space_Model_Name>
  <Abilities><Spaceish_Ability><Range>1</Range></Spaceish_Ability></Abilities></HeroUnit>
<HeroUnit Name="Plain"><Abilities><Odd_Ability><Range>1</Range></Odd_Ability></Abilities></HeroUnit>
</Root>"""


def row(tag: str, status: str = "todo", classes=("HeroUnit",)) -> dict:
    return {"area": "combat", "classes": list(classes), "status": status, "tag": tag, "ticket": 1}


class ReclassifyGroundTests(unittest.TestCase):
    def run_rows(self, rows: list[dict]):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "units.xml").write_text(XML, encoding="utf-8")
            objects, failed = rg.load_objects(Path(tmp))
        self.assertEqual(failed, 0)
        registry = {"schema_version": 2, "evidence": {}, "rows": rows}
        return rg.reclassify(registry, rg.classify(objects))

    def test_ground_only_tag_moves_with_its_carriers(self):
        out, changed, _ = self.run_rows([row("Abilities/Ground_Only_Ability/Range")])
        moved = out["rows"][0]
        self.assertEqual(moved["status"], "land-or-galactic")
        self.assertIn("Ground_Hero", moved["evidence"])
        self.assertIn("Ground_Variant", moved["evidence"])
        self.assertNotIn("ticket", moved)
        self.assertEqual(len(changed), 1)

    def test_galactic_agent_tag_moves(self):
        out, _, _ = self.run_rows([row("Abilities/Slice_Ability/Range")])
        self.assertEqual(out["rows"][0]["status"], "land-or-galactic")
        self.assertIn("SCOPE-GALACTIC", out["rows"][0]["evidence"])

    def test_non_ability_tag_of_agent_is_kept(self):
        out, _, _ = self.run_rows([row("CategoryMask")])
        self.assertEqual(out["rows"][0]["status"], "todo")

    def test_mixed_tag_keeps_status_and_names_space_carrier(self):
        out, changed, _ = self.run_rows([row("Abilities/Shared_Ability/Range")])
        self.assertEqual(out["rows"][0]["status"], "todo")
        self.assertIn("Both_Hero", out["rows"][0]["note"])
        self.assertEqual(changed, [])

    def test_dual_form_hero_without_space_form_is_ground(self):
        out, _, _ = self.run_rows([row("Abilities/Flame_Ability/Range")])
        self.assertEqual(out["rows"][0]["status"], "land-or-galactic")
        self.assertIn("Dual_Hero", out["rows"][0]["evidence"])

    def test_dual_form_hero_with_space_model_stays_space(self):
        out, _, _ = self.run_rows([row("Abilities/Spaceish_Ability/Range")])
        self.assertEqual(out["rows"][0]["status"], "todo")
        self.assertIn("Dual_Hero_Space_Model", out["rows"][0]["note"])

    def test_unknown_carrier_is_kept(self):
        out, _, _ = self.run_rows([row("Abilities/Odd_Ability/Range")])
        self.assertEqual(out["rows"][0]["status"], "todo")

    def test_applied_unjudged_and_unseen_rows_are_untouched(self):
        rows = [row("Abilities/Ground_Only_Ability/Range", "applied"), row("Max_Speed", classes=("GameConstants",)),
                row("Nowhere/Tag")]
        out, changed, _ = self.run_rows(rows)
        self.assertEqual(sorted(r["status"] for r in out["rows"]), ["applied", "todo", "todo"])
        self.assertEqual(changed, [])

    def test_multi_class_row_splits_per_class(self):
        out, _, _ = self.run_rows([row("Abilities/Ground_Only_Ability/Range", classes=("GameConstants", "HeroUnit"))])
        self.assertEqual({(r["status"], tuple(r["classes"])) for r in out["rows"]},
                         {("todo", ("GameConstants",)), ("land-or-galactic", ("HeroUnit",))})


if __name__ == "__main__":
    unittest.main()
