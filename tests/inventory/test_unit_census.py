"""Census contracts: repeated lists, variant winners and unsupported handlers."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.inventory import unit_census as census


def catalog(*definitions):
    result = census.Catalog.__new__(census.Catalog)
    result.defs = {}
    result.cache = {}
    result.duplicates = []
    for index, raw in enumerate(definitions):
        result.add(ET.fromstring(raw), "data/xml/fixture.xml", "base", 0, index)
    return result


class UnitCensusTests(unittest.TestCase):
    def test_variant_closest_layer_preserves_repeated_lists(self):
        c = catalog('<SpaceUnit Name="base"><HardPoints>old</HardPoints><Max_Speed>1</Max_Speed></SpaceUnit>',
                    '<SpaceUnit Name="child"><Variant_Of_Existing_Type>base</Variant_Of_Existing_Type>'
                    '<HardPoints>a</HardPoints><HardPoints>b</HardPoints><Max_Speed>2</Max_Speed>'
                    '<Max_Speed>3</Max_Speed></SpaceUnit>')
        obj = c.resolve("CHILD")
        self.assertEqual([census.text(n) for n, _, _ in obj["values"]["hardpoints"]], ["a", "b"])
        self.assertEqual(census.value(obj, "Max_Speed"), "3")
        self.assertEqual(obj["variant_chain"], ["child", "base"])

    def test_variant_cycle_fails_instead_of_losing_reachable_units(self):
        c = catalog('<SpaceUnit Name="a"><Variant_Of_Existing_Type>b</Variant_Of_Existing_Type></SpaceUnit>',
                    '<SpaceUnit Name="b"><Variant_Of_Existing_Type>a</Variant_Of_Existing_Type></SpaceUnit>')
        with self.assertRaisesRegex(ValueError, "variant cycle"):
            c.resolve("a")

    def test_stronger_layer_wins_over_later_registry_order(self):
        c = catalog('<SpaceUnit Name="unit"><Max_Speed>1</Max_Speed></SpaceUnit>')
        c.add(ET.fromstring('<SpaceUnit Name="unit"><Max_Speed>2</Max_Speed></SpaceUnit>'),
              "data/xml/expansion.xml", "expansion", 0, 0)
        c.add(ET.fromstring('<SpaceUnit Name="unit"><Max_Speed>3</Max_Speed></SpaceUnit>'),
              "data/xml/base.xml", "base", 10, 10)
        self.assertEqual(census.value(c.resolve("unit"), "Max_Speed"), "2")

    def test_hero_candidates_keep_disagreement_and_exclude_ground_abilities(self):
        c = catalog('<HeroCompany Name="purchase"><Company_Units>hero</Company_Units>'
                    '<Company_Transport_Unit>transport</Company_Transport_Unit><Unit_Abilities_Data>'
                    '<Unit_Ability><Type>SPRINT</Type></Unit_Ability></Unit_Abilities_Data></HeroCompany>',
                    '<UniqueUnit Name="hero"><Unique_Space_Unit>fighter</Unique_Space_Unit></UniqueUnit>',
                    '<SpaceUnit Name="transport"/>', '<SpaceUnit Name="fighter"/>')
        obj = c.resolve("purchase")
        unit, special = census.abilities(obj)
        self.assertEqual(unit, [])
        edges = census.dependencies(c, obj, unit, special)
        self.assertEqual({e["object"] for e in edges}, {"transport", "fighter"})
        self.assertEqual({e["relation"] for e in edges}, {"hero_deployment_candidate"})

    def test_craft_multiplicity_and_inactive_carrier_lists(self):
        c = catalog('<Squadron Name="s"><Squadron_Units>a,a</Squadron_Units>'
                    '<Squadron_Units>a</Squadron_Units><Starting_Spawned_Units_Tech_0>a,2</Starting_Spawned_Units_Tech_0></Squadron>',
                    '<SpaceUnit Name="a"/>')
        edges = census.dependencies(c, c.resolve("s"), [], [])
        self.assertEqual(len(edges), 1)
        self.assertEqual(edges[0]["count"], "3")

    def test_missing_spawn_reference_fails(self):
        c = catalog('<SpaceUnit Name="unit"><Spawned_Object_Type>missing</Spawned_Object_Type></SpaceUnit>')
        with self.assertRaisesRegex(ValueError, "missing spawned"):
            census.dependencies(c, c.resolve("unit"), [], [])

    def test_charged_hardpoint_inherits_special_profile_and_follows_downstream(self):
        c = catalog('<SpaceUnit Name="ship"><HardPoints>derived_hp</HardPoints></SpaceUnit>',
                    '<HardPoint Name="base_hp"><Fire_Projectile_Type>ordinary</Fire_Projectile_Type>'
                    '<Blast_Ability_Fire_Projectile_Type>charged</Blast_Ability_Fire_Projectile_Type></HardPoint>',
                    '<HardPoint Name="derived_hp"><Variant_Of_Existing_Type>base_hp</Variant_Of_Existing_Type></HardPoint>',
                    '<Projectile Name="ordinary"><Projectile_Damage>10</Projectile_Damage></Projectile>',
                    '<Projectile Name="charged"><Projectile_Damage>600</Projectile_Damage>'
                    '<Projectile_Blast_Area_Damage>400</Projectile_Blast_Area_Damage>'
                    '<Projectile_Blast_Area_Range>250</Projectile_Blast_Area_Range>'
                    '<Spawned_Object_Type>effect</Spawned_Object_Type></Projectile>',
                    '<Projectile Name="effect"><Projectile_Damage>20</Projectile_Damage></Projectile>')
        weapons = census.weapon(c, c.resolve("ship"))
        profiles = {p["id"]: p for p in weapons["projectiles"]}
        self.assertEqual(set(profiles), {"ordinary", "charged", "effect"})
        self.assertEqual(weapons["hardpoints"][0]["projectile"], "ordinary")
        fields = {r["tag"]: r["value"] for r in profiles["charged"]["fields"]}
        self.assertEqual(fields["Projectile_Damage"], "600")
        self.assertEqual(fields["Projectile_Blast_Area_Damage"], "400")
        self.assertEqual(fields["Projectile_Blast_Area_Range"], "250")

    def test_missing_charged_projectile_fails(self):
        c = catalog('<SpaceUnit Name="ship"><HardPoints>hp</HardPoints></SpaceUnit>',
                    '<HardPoint Name="hp"><Blast_Ability_Fire_Projectile_Type>missing</Blast_Ability_Fire_Projectile_Type></HardPoint>')
        with self.assertRaisesRegex(ValueError, "unresolved object: missing"):
            census.weapon(c, c.resolve("ship"))

    def test_unsupported_ability_never_claims_implemented(self):
        row = {"id": "unit", "class": "SpaceUnit", "category": [], "acquisition": [], "behaviours": [],
               "locomotors": [], "unit_abilities": [{"type": "MISSILE_SHIELD", "fields": {}}], "special_abilities": []}
        statuses = []
        def mechanic(key, title, status, *args):
            statuses.append(status)
            return key
        census.handler_needs(row, mechanic, lambda *args: None)
        self.assertEqual(statuses, ["missing"])

    def test_hero_damage_modes_require_a_fully_supported_profile(self):
        for kind, modifier in (("INVULNERABILITY", "TAKE_DAMAGE_MULTIPLIER, 0"),
                               ("POWER_TO_WEAPONS", "CAUSE_DAMAGE_MULTIPLIER, 2.5")):
            for unknown in (False, True):
                fields = {"Mod_Multiplier": [modifier] + (["UNKNOWN_MULTIPLIER, 2"] if unknown else [])}
                row = {"id": "unit", "class": "SpaceUnit", "category": [], "acquisition": [], "behaviours": [],
                       "locomotors": [], "unit_abilities": [{"type": kind, "fields": fields}], "special_abilities": []}
                statuses = {}
                def mechanic(key, title, status, *args):
                    statuses[key] = status
                    return key
                census.handler_needs(row, mechanic, lambda *args: None)
                self.assertEqual(statuses["unit-ability:" + kind], "implemented")
                if unknown:
                    self.assertNotIn("ability-damage-modifiers", statuses)
                    self.assertEqual(statuses["ability-other-modifiers"], "partial")
                else:
                    self.assertEqual(statuses["ability-damage-modifiers"], "implemented")
                    self.assertNotIn("ability-other-modifiers", statuses)

    def test_upgrade_admission_preserves_deferred_effects(self):
        def statuses(handler, fields, cls="UpgradeObject", collision=True):
            row = {"id": "upgrade", "class": cls, "category": [], "acquisition": [{"route": "station_menu", "level": 3}],
                   "behaviours": [], "locomotors": [], "unit_abilities": [], "catalog_namespace_collision": collision,
                   "special_abilities": [{"handler": handler, "fields": fields}]}
            result = {}
            def mechanic(key, title, status, *args):
                result[key] = status
                return key
            admitted = {}
            census.handler_needs(row, mechanic, lambda row, key, status: admitted.update({key: status}))
            self.assertEqual(admitted, result)
            self.assertEqual(result["station-unlocks"], "implemented")
            if collision:
                self.assertEqual(result["catalog-ability-name-collision"], "implemented")
            return result["special-handler:" + handler]
        self.assertEqual(statuses("Combat_Bonus_Ability", {"Activation_Style": ["Space_Automatic"],
            "Movement_Speed_Bonus_Percentage": ["0.25"]}), "implemented")
        self.assertEqual(statuses("Combat_Bonus_Ability", {"Activation_Style": ["Space_Automatic"],
            "Fire_Range_Bonus_Percentage": [".15"]}), "partial")
        self.assertEqual(statuses("Starbase_Upgrade_Ability", {"Activation_Style": ["Skirmish_Automatic"]}), "implemented")
        self.assertEqual(statuses("Income_Stream_Mod_Ability", {"Income_Multiplier": ["1.25"]}), "missing")
        self.assertEqual(statuses("Income_Stream_Mod_Ability", {"Income_Multiplier": ["1.2"],
            "Target_Stream_Source": ["Empire_Mineral_Extractor"]}), "implemented")
        self.assertEqual(statuses("Income_Stream_Mod_Ability", {"Income_Multiplier": ["1.4"],
            "Target_Stream_Source": ["Rebel_Mineral_Extractor"], "Unknown_Modifier_Field": ["1"]}), "partial")
        self.assertEqual(statuses("Income_Stream_Mod_Ability", {"Income_Multiplier": ["1.2"],
            "Target_Stream_Source": ["unloaded_source"]}), "missing")
        self.assertEqual(statuses("Income_Stream_Mod_Ability", {"Income_Multiplier": ["1.2"],
            "Target_Stream_Source": ["Empire_Mineral_Extractor"]}, cls="StarBase", collision=False), "missing")
        self.assertEqual(statuses("Battlefield_Modifier_Ability", {}), "missing")
        self.assertEqual(statuses("Combat_Bonus_Ability", {"Activation_Style": ["Space_Automatic"]},
            cls="UniqueUnit", collision=False), "missing")

    def test_underworld_ground_modifier_is_outside_the_space_census(self):
        ability = {"handler": "Income_Stream_Mod_Ability",
                   "fields": {"Target_Stream_Source": ["Skirmish_Ground_Mining_Facility_U"]}}
        row = {"id": "UL_Extort_Cash_L1_Upgrade", "class": "UpgradeObject", "category": [],
               "acquisition": [], "behaviours": [], "locomotors": [], "unit_abilities": [],
               "special_abilities": [ability]}
        outcomes = {}
        def mechanic(key, title, status, *args):
            outcomes[key] = status
            return key
        census.handler_needs(row, mechanic, lambda *args: None)
        self.assertEqual(ability["scope"], "land-or-galactic")
        self.assertNotIn("special-handler:Income_Stream_Mod_Ability", outcomes)

    def test_region_fingerprint_ignores_unrelated_edits_but_detects_handler_edits(self):
        region = {"start": "void reviewed() {", "end": "void unrelated() {"}
        source = 'void reviewed() { const char* brace = "}"; }\nvoid unrelated() {}\n'
        digest = census.region_digest(source, region)
        self.assertEqual(census.region_digest("// new header\n" + source + "// new footer\n", region), digest)
        self.assertEqual(census.region_digest(source.replace("\n", "\r\n"), region), digest)
        self.assertNotEqual(census.region_digest(source.replace('"}"', '"{"'), region), digest)
        for invalid in (source.replace("reviewed", "renamed"), source + source,
                        'void unrelated() {}\nvoid reviewed() {}\n'):
            with self.assertRaises(ValueError):
                census.region_digest(invalid, region)

    def test_stale_review_names_regions_and_refuses_to_overwrite_snapshot(self):
        moved = ["src/units/unit_tables.cpp#load_abilities", "src/sim/tactical/session_step_commands.cpp#apply_ability"]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "snapshot.json"
            output.write_text("reviewed snapshot", encoding="utf-8")
            with mock.patch.object(census, "changed_fingerprints", return_value=moved), \
                 mock.patch.object(census, "Catalog") as assets, \
                 mock.patch.object(sys, "argv", ["unit_census.py", "--game-root", "unused", "--output", str(output)]), \
                 mock.patch.object(sys, "stderr") as error:
                self.assertEqual(census.main(), 1)
                message = error.write.call_args_list[0].args[0]
                for name in moved:
                    self.assertIn(name, message)
                self.assertIn(census.REGENERATE, message)
                assets.assert_not_called()
            self.assertEqual(output.read_text(encoding="utf-8"), "reviewed snapshot")

    def test_record_encoding_is_valid_and_deterministic(self):
        data = {"units": [{"id": "a", "nested": {"b": [1, 2]}}],
                "weapon_definitions": {"hardpoints": {}, "projectiles": {}}}
        self.assertEqual(json.loads(census.encode(data)), data)
        self.assertEqual(census.encode(data), census.encode(data))


if __name__ == "__main__":
    unittest.main()
