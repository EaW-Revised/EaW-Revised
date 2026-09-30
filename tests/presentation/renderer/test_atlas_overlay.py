"""P1-10 viewer atlas overlay contract.

The structural assertions always run: they pin the overlay's option surface,
its presentation-only canvas ownership, and the report fields the completion
evidence depends on. The graphical assertion is opt-in through the same
environment switches the accepted P1-01 runtime exercise uses, because it needs
the pinned Godot 4.7.2 console binary and a real GPU.
"""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "assets" / "fixtures"))

import atlas_overlay_fixture as fixture  # noqa: E402
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import mode_source  # noqa: E402


class AtlasOverlayContract(unittest.TestCase):
    def test_viewer_accepts_the_atlas_and_icon_options(self):
        source = mode_source("viewer_host")
        self.assertIn('argument == "--eawr-atlas"', source)
        self.assertIn('argument == "--eawr-icon"', source)
        self.assertIn("load_mega_texture_atlas(*filesystem, options_->atlas_path)", source)
        self.assertIn("directory.find(options_->icon_name)", source)

    def test_overlay_is_presentation_only_screen_space_work(self):
        source = mode_source("viewer_host")
        self.assertIn("canvas_item_add_texture_rect_region", source)
        self.assertIn("canvas_item_set_default_texture_filter", source)
        self.assertIn("CANVAS_ITEM_TEXTURE_FILTER_NEAREST", source)
        # The overlay owns its own canvas RIDs and frees them before the report.
        self.assertIn("void free_rids()", source)
        self.assertIn("release_atlas_overlay();", source)
        # It must never enter the 3D asset registry or the material routes.
        start = source.index("bool ViewerHost::start_atlas_overlay()")
        end = source.index("bool ViewerHost::verify_atlas_capture")
        overlay_body = source[start:end]
        for forbidden in ("renderer_->upload", "renderer_->submit", "MaterialDescription",
                          "instance_create", "scenario"):
            self.assertNotIn(forbidden, overlay_body)

    def test_overlay_report_records_identity_rectangle_and_evidence(self):
        source = mode_source("viewer_host")
        for token in ('\\"atlas\\"', '\\"has_alpha\\"', '\\"rectangle\\"',
                      '\\"opaque_non_background\\"', '\\"transparent_background\\"',
                      '\\"atlas_overlay_verified\\"', '\\"orientation_consistent\\"',
                      "atlas_overlay_exercise_passed"):
            self.assertIn(token, source)

    def test_overlay_flip_values_stay_derived_and_false(self):
        header = (ROOT / "include/eawr/assets/assets.hpp").read_text(encoding="utf-8")
        self.assertIn("bool flip_x{};", header)
        self.assertIn("bool flip_y{};", header)
        self.assertIn("not stored with per-entry flips", header)
        directory = fixture.mtd_bytes()
        # The documented record is 81 bytes and its final byte is has_alpha.
        self.assertEqual(len(directory), 4 + len(fixture.ENTRIES) * fixture.RECORD_BYTES)
        self.assertEqual(directory[4 + fixture.RECORD_BYTES - 1], 1)
        self.assertEqual(directory[4 + 2 * fixture.RECORD_BYTES - 1], 0)

    def test_fixture_is_self_contained_and_deterministic(self):
        first = (fixture.mtd_bytes(), fixture.tga_bytes())
        second = (fixture.mtd_bytes(), fixture.tga_bytes())
        self.assertEqual(first, second)
        self.assertEqual(len(first[1]), 18 + fixture.PAGE_WIDTH * fixture.PAGE_HEIGHT * 4)
        opaque, transparent = fixture.alpha_on_texel_counts()
        self.assertGreaterEqual(opaque, 16)
        self.assertGreater(transparent, 0)
        # Both entries must be asymmetric on both axes for the orientation
        # evidence to be able to fail.
        for index in (0, 1):
            self.assertGreaterEqual(fixture.distinct_opaque_colours(index), 2)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_graphical_overlay_run_proves_rectangle_and_alpha_sampling(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable,
                        "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        with tempfile.TemporaryDirectory(prefix="eawr-atlas-overlay-") as temporary:
            workspace = pathlib.Path(temporary)
            mod_root = fixture.write_fixture_root(workspace / "fixture")
            # G4: the leaf has no atlas; all page assets must come from its parent.
            leaf = workspace / "leaf" / "Data"
            leaf.mkdir(parents=True)
            (leaf / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            chain = str(leaf.parent) + ";" + str(mod_root)
            report = workspace / "atlas.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-mod-root", chain,
                 "--eawr-atlas", fixture.MTD_LOGICAL_PATH,
                 "--eawr-icon", fixture.ALPHA_ON_NAME,
                 "--eawr-report", str(report),
                 "--eawr-capture", str(workspace / "mod-chain.png")],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            result = json.loads(report.read_text(encoding="utf-8"))

            self.assertEqual(result["status"], "atlas_overlay_exercise_passed")
            self.assertTrue(result["atlas_overlay_verified"])
            atlas = result["atlas"]
            self.assertEqual(atlas["mtd"]["logical_path"], fixture.MTD_LOGICAL_PATH)
            self.assertEqual(atlas["mtd"]["sha256"],
                             hashlib.sha256(fixture.mtd_bytes()).hexdigest())
            self.assertEqual(atlas["page"]["logical_path"], fixture.PAGE_LOGICAL_PATH)
            self.assertEqual(atlas["page"]["sha256"],
                             hashlib.sha256(fixture.tga_bytes()).hexdigest())
            self.assertEqual(atlas["page"]["width"], fixture.PAGE_WIDTH)
            self.assertEqual(atlas["page"]["height"], fixture.PAGE_HEIGHT)
            self.assertEqual(atlas["page"]["source_origin"], "top-left")

            name, x, y, width, height, has_alpha = fixture.ENTRIES[0]
            self.assertEqual(atlas["icon"]["name"], name)
            self.assertEqual(atlas["icon"]["rectangle"],
                             {"x": x, "y": y, "width": width, "height": height})
            self.assertEqual(atlas["icon"]["has_alpha"], bool(has_alpha))
            # No stored flip exists in the documented record; the derived
            # sampling values stay false.
            self.assertFalse(atlas["icon"]["flip_x"])
            self.assertFalse(atlas["icon"]["flip_y"])

            opaque, transparent = fixture.alpha_on_texel_counts()
            evidence = atlas["evidence"]
            self.assertEqual(evidence["opaque_sampled"], opaque)
            self.assertEqual(evidence["transparent_sampled"], transparent)
            self.assertGreaterEqual(evidence["opaque_non_background"], 16)
            self.assertEqual(evidence["transparent_background"], transparent)
            # The rectangle is asymmetric on both axes, so a flipped or
            # transposed sampling cannot keep one drawn colour per source
            # colour. The overlay therefore proves the displayed orientation.
            self.assertEqual(evidence["distinct_opaque_colours"],
                             fixture.distinct_opaque_colours(0))
            self.assertGreaterEqual(evidence["distinct_opaque_colours"], 2)
            self.assertTrue(evidence["orientation_consistent"])
            self.assertTrue(evidence["verified"])
            self.assertTrue(result["capture_sha256"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_graphical_overlay_run_samples_a_dds_backing_page(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable,
                        "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        with tempfile.TemporaryDirectory(prefix="eawr-atlas-overlay-dds-") as temporary:
            workspace = pathlib.Path(temporary)
            mod_root = fixture.write_fixture_root(workspace / "fixture")
            report = workspace / "atlas-dds.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-mod-root", str(mod_root),
                 "--eawr-atlas", fixture.DDS_MTD_LOGICAL_PATH,
                 "--eawr-icon", fixture.ALPHA_ON_NAME,
                 "--eawr-report", str(report)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            result = json.loads(report.read_text(encoding="utf-8"))
            atlas = result["atlas"]
            self.assertEqual(result["status"], "atlas_overlay_exercise_passed")
            self.assertEqual(atlas["page"]["logical_path"], fixture.DDS_PAGE_LOGICAL_PATH)
            self.assertEqual(atlas["page"]["sha256"],
                             hashlib.sha256(fixture.dds_bytes()).hexdigest())
            # The DDS masks are A8R8G8B8, which the decoder reports as bgra8
            # and the overlay restores to the documented channel order.
            self.assertEqual(atlas["page"]["format"], "bgra8")
            self.assertEqual(atlas["page"]["source_origin"], "top-left")
            opaque, transparent = fixture.alpha_on_texel_counts()
            evidence = atlas["evidence"]
            self.assertEqual(evidence["opaque_sampled"], opaque)
            self.assertEqual(evidence["transparent_sampled"], transparent)
            self.assertEqual(evidence["transparent_background"], transparent)
            self.assertEqual(evidence["distinct_opaque_colours"],
                             fixture.distinct_opaque_colours(0))
            self.assertTrue(evidence["orientation_consistent"])
            self.assertTrue(result["atlas_overlay_verified"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_graphical_overlay_run_draws_the_alpha_off_entry(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable,
                        "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        with tempfile.TemporaryDirectory(prefix="eawr-atlas-overlay-off-") as temporary:
            workspace = pathlib.Path(temporary)
            mod_root = fixture.write_fixture_root(workspace / "fixture")
            report = workspace / "atlas-off.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-mod-root", str(mod_root),
                 "--eawr-atlas", fixture.MTD_LOGICAL_PATH,
                 "--eawr-icon", fixture.ALPHA_OFF_NAME,
                 "--eawr-report", str(report)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            result = json.loads(report.read_text(encoding="utf-8"))
            atlas = result["atlas"]
            name, x, y, width, height, _has_alpha = fixture.ENTRIES[1]
            self.assertEqual(result["status"], "atlas_overlay_exercise_passed")
            self.assertEqual(atlas["icon"]["name"], name)
            self.assertFalse(atlas["icon"]["has_alpha"])
            self.assertEqual(atlas["icon"]["rectangle"],
                             {"x": x, "y": y, "width": width, "height": height})
            self.assertEqual(atlas["evidence"]["transparent_sampled"], 0)
            self.assertEqual(atlas["evidence"]["opaque_sampled"], width * height)
            self.assertGreaterEqual(atlas["evidence"]["opaque_non_background"], 16)
            self.assertEqual(atlas["evidence"]["distinct_opaque_colours"],
                             fixture.distinct_opaque_colours(1))
            self.assertTrue(atlas["evidence"]["orientation_consistent"])
            self.assertTrue(result["atlas_overlay_verified"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_graphical_overlay_converts_a_bottom_left_source_once(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable,
                        "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        with tempfile.TemporaryDirectory(prefix="eawr-atlas-overlay-bottom-left-") as temporary:
            workspace = pathlib.Path(temporary)
            mod_root = fixture.write_fixture_root(workspace / "fixture")
            report = workspace / "atlas-bottom-left.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-mod-root", str(mod_root),
                 "--eawr-atlas", fixture.BOTTOM_LEFT_MTD_LOGICAL_PATH,
                 "--eawr-icon", fixture.ALPHA_ON_NAME,
                 "--eawr-report", str(report)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "atlas_overlay_exercise_passed")
            self.assertTrue(result["atlas_overlay_verified"])
            atlas = result["atlas"]
            self.assertEqual(atlas["page"]["logical_path"],
                             fixture.BOTTOM_LEFT_PAGE_LOGICAL_PATH)
            self.assertEqual(atlas["page"]["sha256"],
                             hashlib.sha256(fixture.bottom_left_tga_bytes()).hexdigest())
            self.assertEqual(atlas["page"]["width"], fixture.PAGE_WIDTH)
            self.assertEqual(atlas["page"]["height"], fixture.PAGE_HEIGHT)
            self.assertEqual(atlas["page"]["source_origin"], "bottom-left")
            name, x, y, width, height, has_alpha = fixture.ENTRIES[0]
            self.assertEqual(atlas["icon"]["name"], name)
            self.assertEqual(atlas["icon"]["rectangle"],
                             {"x": x, "y": y, "width": width, "height": height})
            self.assertEqual(atlas["icon"]["has_alpha"], bool(has_alpha))
            self.assertFalse(atlas["icon"]["flip_x"])
            self.assertFalse(atlas["icon"]["flip_y"])
            opaque, transparent = fixture.alpha_on_texel_counts()
            evidence = atlas["evidence"]
            self.assertEqual(evidence["opaque_sampled"], opaque)
            self.assertEqual(evidence["transparent_sampled"], transparent)
            self.assertEqual(evidence["transparent_background"], transparent)
            self.assertEqual(evidence["distinct_opaque_colours"],
                             fixture.distinct_opaque_colours(0))
            self.assertTrue(evidence["orientation_consistent"])
            self.assertTrue(evidence["verified"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
