import importlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/performance"))
drill = importlib.import_module("hull_drill")


class HullDrillTests(unittest.TestCase):
    def test_categories_ignore_model_name_hints(self):
        self.assertEqual(drill.ship_class(dict(model="capital_fighter.alo")), "unknown")
        self.assertEqual(drill.ship_class(dict(layer="Corvette")), "corvette")
        self.assertEqual(drill.ship_class(dict(layer="SuperCapital")), "capital")
        self.assertEqual(drill.ship_class(dict(categories="Fighter, AntiFighter")), "fighter")
        self.assertEqual(drill.ship_class(dict(xml_type="StarBase")), "structure")
        self.assertEqual(drill.ship_class(dict(layer="Frigate")), "frigate")

    def test_conservative_bounds_near_plane_and_off_axis(self):
        camera = dict(eye=[0, 0, 0], target=[0, 0, -1], up=[0, 1, 0], width=1000, height=1000, fov=90, near=1, far=1000)
        self.assertFalse(drill.visibility([-1, -1, -101, 2, 2, 2], camera)["offscreen"])
        self.assertTrue(drill.visibility([200, 0, -101, 2, 2, 2], camera)["offscreen"])
        self.assertTrue(drill.visibility([0, 0, 100, 2, 2, 2], camera)["offscreen"])
        self.assertIsNone(drill.visibility([-2, -2, -2, 4, 4, 4], camera)["tiny"])
        self.assertTrue(drill.visibility([0, 0, -500, .1, .1, .1], camera)["tiny"])
        self.assertIsNone(drill.visibility(None, camera)["offscreen"])

    def test_hidden_retained_hull_and_classes_reconcile(self):
        piece = dict(unit=1, asset=10, material=20, shader=30, role="hull", surfaces=1, hull_part=2,
                     requested_visible=False, categories="Fighter", mesh_names=["mesh"], model="model")
        data = drill.census(dict(hull_census=dict(scope="retained", camera=None, pieces=[piece, dict(piece, unit=2, hull_part=1)]),
                                 cohorts=[dict(rigid=False, count=2), dict(rigid=True, count=3)]))
        self.assertEqual(data["classes"]["fighter"]["units"], 2)
        self.assertEqual(data["classes"]["fighter"]["hidden_surfaces"], 2)
        self.assertEqual(data["classes"]["fighter"]["hull_parts"], dict(body=1, declared_turret=1))
        self.assertEqual(data["rigid_retained_candidate_saving"], 2)
        self.assertEqual(sum(v["hull_surfaces"] for v in data["classes"].values()), data["roles"]["hull"])

    def test_rejects_invalid_measurement(self):
        with self.assertRaises(ValueError):
            drill.pass_rows(dict(valid=False, meta={}, scenes=[]))
        with self.assertRaises(ValueError):
            drill.pass_rows(dict(valid=True, meta=dict(exit_code=0), scenes=[dict(drawtype_ablate="none", render_thread_observed=False)]))

    def test_source_indices_and_unowned_hull_are_preserved(self):
        piece = dict(unit=0, asset=10, material=20, shader=30, role="hull", surfaces=1,
                     requested_visible=True, source_model="actual_model", model="owning_model",
                     source_mesh=3, mesh_names=["duplicate_name"], model_meshes=7, model_submeshes=9)
        data = drill.census(dict(hull_census=dict(scope="retained", camera=None,
                                                pieces=[piece, dict(piece, source_mesh=4, asset=11)])))
        group = data["classes"]["unowned"]
        self.assertEqual(group["units"], 0)
        self.assertEqual(group["meshes"], 2)
        self.assertEqual(group["original_model_totals"], [("actual_model", 7, 9)])
        self.assertEqual(data["top10"][0]["model"], "actual_model")
        self.assertEqual(group["hull_surfaces"], data["roles"]["hull"])


if __name__ == "__main__":
    unittest.main()
