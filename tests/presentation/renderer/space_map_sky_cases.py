"""Cases for test_space_map_mode; collected by its legacy facade."""

from unittest import mock

from space_map_test_support import (
    ALDERAAN_BLOCKER, ALDERAAN_SPACE, NONE, REFERENCE_MAPS,
    SPACE_STATUSES, decode_png, fixture, json,
    os, pathlib, read_pgm, scene_fixture,
    tempfile, unittest,
)


def _secondary_scale_changed_pixels(self, scales, fov=60):
    # The two fixtures differ only in the secondary object's authored scale.
    # Returns how many sampled pixels differ between the two captures.
    with tempfile.TemporaryDirectory(prefix="eawr-sky-scale-") as temporary:
        directory = pathlib.Path(temporary)
        images = []
        for scale in scales:
            root = fixture.write_fixture_root(directory / f"root-{scale}", "meshgloss")
            xml = root / "GameData" / "Data" / "XML" / "eawr_space_objects.xml"
            # The secondary layer uses its own additive model over the same
            # walls, so its contribution is visible against the primary sky.
            additive = [[("Color", (0.25, 0.5, 0.75, 1.0)),
                         ("UVScrollRate", (0.0, 0.0, 0.0, 0.0))]] * 2
            (root / "GameData/Data/Art/Models" / "eawr_space_sky2.alo").write_bytes(
                fixture.alo_bytes(["MeshAdditive.fx", "MeshAdditive.fx"], materials=additive))
            xml.write_text(
                '<SpaceProps><SpaceProp Name="' + fixture.SKY_OBJECT + '">'
                '<Space_Model_Name>' + fixture.SKY_MODEL + '</Space_Model_Name></SpaceProp>'
                '<SpaceProp Name="' + fixture.SECONDARY_SKY_OBJECT + '">'
                '<Space_Model_Name>eawr_space_sky2.alo</Space_Model_Name>'
                '<Scale_Factor>' + str(scale) + '</Scale_Factor>'
                '</SpaceProp></SpaceProps>', encoding="utf-8")
            code, report, capture = self._run(
                root, fixture.MAP_LOGICAL_PATH, directory, f"sky-{scale}",
                ("--eawr-space-camera", f"0,0,0,-1,0,0,0,1,0,{fov},1,5000", "--eawr-populate"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(any(item["role"] == "secondary_sky" and item["status"] == "drawn"
                                for item in report["space"]["surfaces"]))
            images.append(decode_png(capture.read_bytes()))
        width, height, visible = images[0]
        self.assertEqual((width, height), images[1][:2])
        clipped = images[1][2]
        changed = sum(max(abs(a - b) for a, b in zip(visible[y][x], clipped[y][x])) > 16
                      for y in range(0, height, 4) for x in range(0, width, 4))
        return changed


def test_secondary_sky_authored_scale_changes_its_visible_contribution(self):
    # Its geometry is visible at scale 1 and entirely beyond the 100,000-unit
    # far plane at scale 1000. Independent radius normalization makes the two
    # captures identical and is the negative control for this regression.
    changed = _secondary_scale_changed_pixels(self, (1, 1000))
    self.assertGreater(changed, 100, "authored secondary scale must change its visible contribution")


def test_sky_between_old_and_retail_far_plane_is_drawn(self):
    # #6447: at scale 160 the fixture walls lie about 65,000 to 110,000 units
    # from the eye. The far plane clips by view depth, so a 20 degree view
    # (depth at least 0.94 x distance) keeps every wall fragment beyond the
    # former 60,000 far plane and the nearer ones inside the retail background
    # range of 100,000 (BP-75). Scale 1000 is clipped either way.
    changed = _secondary_scale_changed_pixels(self, (160, 1000), fov=20)
    self.assertGreater(changed, 100, "sky geometry between 60,000 and 100,000 units must not be clipped")


def test_primary_sky_texture_named_with_source_art_suffix_uses_the_shipped_dds(self):
    # #6760: a mod's sky model names its texture with a source-art suffix (.jpg, .png) and ships
    # only the .dds. The material rule retries the authored name's first dot-token with .dds
    # (RGC-122); the environment loader used to probe only .tga/.dds suffixes and drew no sky.
    with tempfile.TemporaryDirectory(prefix="eawr-sky-suffix-") as temporary:
        directory = pathlib.Path(temporary)
        root = fixture.write_fixture_root(directory / "root", "meshgloss")
        declared = {"eawr_space_a.tga": "eawr_space_a.jpg", "eawr_space_b.dds": "eawr_space_b.png"}
        surfaces = [(mesh, declared[texture], *rest) for mesh, texture, *rest in fixture.SURFACES]
        with mock.patch.object(fixture, "SURFACES", surfaces):
            model = fixture.alo_bytes(["MeshGloss.fx"] * 2, materials=fixture.MESHGLOSS)
        data = root / "GameData" / "Data" / "Art"
        (data / "Models" / fixture.SKY_MODEL).write_bytes(model)
        (data / "Textures" / "eawr_space_a.tga").unlink()
        (data / "Textures" / "eawr_space_a.dds").write_bytes(
            fixture.dds_bgra_bytes(fixture.QUADRANTS["eawr_space_a.tga"]))
        code, report, _ = self._run(root, fixture.MAP_LOGICAL_PATH, directory, "suffix",
                                    ("--eawr-space-camera", "0,0,0,-1,0,0,0,1,0,60,1,5000", "--eawr-populate"))
        self.assertEqual(code, 0, report.get("failure"))
        sky = [item for item in report["space"]["surfaces"] if item["role"] == "primary_sky"]
        self.assertEqual(len(sky), 2)
        self.assertTrue(all(item["status"] == "drawn" for item in sky), sky)
        self.assertEqual(sorted(item["texture"].rsplit("/", 1)[-1] for item in sky),
                         ["eawr_space_a.dds", "eawr_space_b.dds"])


def test_primary_sky_texture_dotted_name_follows_the_first_dot_token_rule(self):
    # The retail retry (RGC-122) cuts an authored name that is not a .tga/.dds name at its first dot:
    # "eawr_space_a.01" loads the shipped eawr_space_a.dds, and "eawr_space_b.02" does not load a file
    # that is only shipped as eawr_space_b.02.dds. No stock or tested-mod asset relies on a dotted stem
    # resolving by plain suffix (scan of stock FoC, Remake 4.0/5.0, RaW, BFC and FotR).
    with tempfile.TemporaryDirectory(prefix="eawr-sky-dotted-") as temporary:
        directory = pathlib.Path(temporary)
        root = fixture.write_fixture_root(directory / "root", "meshgloss")
        declared = {"eawr_space_a.tga": "eawr_space_a.01", "eawr_space_b.dds": "eawr_space_b.02"}
        surfaces = [(mesh, declared[texture], *rest) for mesh, texture, *rest in fixture.SURFACES]
        with mock.patch.object(fixture, "SURFACES", surfaces):
            model = fixture.alo_bytes(["MeshGloss.fx"] * 2, materials=fixture.MESHGLOSS)
        data = root / "GameData" / "Data" / "Art"
        (data / "Models" / fixture.SKY_MODEL).write_bytes(model)
        (data / "Textures" / "eawr_space_a.tga").unlink()
        (data / "Textures" / "eawr_space_a.dds").write_bytes(
            fixture.dds_bgra_bytes(fixture.QUADRANTS["eawr_space_a.tga"]))
        (data / "Textures" / "eawr_space_b.dds").rename(data / "Textures" / "eawr_space_b.02.dds")
        code, report, _ = self._run(root, fixture.MAP_LOGICAL_PATH, directory, "dotted",
                                    ("--eawr-space-camera", "0,0,0,-1,0,0,0,1,0,60,1,5000", "--eawr-populate"))
        self.assertEqual(code, 0, report.get("failure"))
        sky = {item["mesh"]: item for item in report["space"]["surfaces"] if item["role"] == "primary_sky"}
        self.assertEqual(sky["SkyA"]["status"], "drawn", sky["SkyA"])
        self.assertTrue(sky["SkyA"]["texture"].endswith("eawr_space_a.dds"), sky["SkyA"])
        self.assertEqual(sky["SkyB"]["status"], "not_drawn", sky["SkyB"])
        self.assertIn("eawr_space_b.02", sky["SkyB"]["cause"])


def test_synthetic_primary_sky_draws_each_surface_with_its_own_texture(self):
    with tempfile.TemporaryDirectory(prefix="eawr-space-") as temporary:
        directory = pathlib.Path(temporary)
        code, result, _ = self._synthetic(directory, "baseline")
        self.assertEqual(code, 0, result["failure"])
        self.assertEqual(result["status"], "space_primary_sky_passed")
        self._check_identity(result)
        space = result["space"]
        self.assertEqual(space["load_status"], "loaded")
        self.assertEqual(space["material_status"], "compiled")
        self.assertEqual(space["submission_status"], "matched")
        self.assertEqual(space["pixel_evidence_status"], "verified")
        self.assertEqual(space["sky_object"]["declared_tag"], "Space_Model_Name")
        self.assertEqual(result["populate"]["declared_placements"], fixture.EXPECTED["declared_placements"])
        surfaces = space["surfaces"]
        self.assertEqual(len(surfaces), fixture.EXPECTED["surfaces"])
        self.assertEqual(len({surface["texture"]["logical_path"] for surface in surfaces}), 2)
        self.assertEqual(len({surface["renderer_asset"] for surface in surfaces}), 2)
        self.assertEqual(surfaces[0]["texture"]["source_origin"], "bottom_left")
        self.assertEqual(surfaces[1]["texture"]["format"], "bgra8")
        for surface, texture in zip(surfaces, ("eawr_space_a.tga", "eawr_space_b.dds")):
            self.assertEqual(surface["status"], "accepted")
            self.assertEqual(surface["adapter_id"], "eawr-space-primary-sky-v1")
            self.assertEqual(surface["original_shader"], fixture.QUALIFIED_SHADER)
            self.assertEqual((surface["technique"], surface["pass_name"]), ("", ""))
            self.assertEqual(surface["compiler"], "compiled")
            self.assertIs(surface["casts_shadows"], False)
            self.assertRegex(surface["texture"]["sha256"], r"^[0-9a-f]{64}$")
            pixels = surface["pixels"]
            self.assertEqual(pixels["status"], "verified")
            self.assertEqual(pixels["isolated_changed_outside"], 0)
            # Orientation and swizzle: each UV quadrant shows its own
            # texture quadrant's colour, not a mirrored or BGR one.
            for quadrant, expected in zip(pixels["uv_quadrants"], fixture.QUADRANTS[texture]):
                self.assertGreater(quadrant["pixels"], 1000)
                for measured, wanted in zip(quadrant["mean_rgb"], expected):
                    self.assertAlmostEqual(measured, wanted, delta=6, msg=texture)
        expected = [(surface["entity"], surface["renderer_asset"]) for surface in surfaces]
        observed = [(item["entity"], item["asset"]) for item in space["submissions"]["observed"]]
        self.assertEqual(observed, expected)
        self.assertEqual(space["lifecycle"]["resources_after_teardown"], 0)
        self.assertEqual(space["lifecycle"]["instances_after_teardown"], 0)
        self.assertEqual(space["evidence"]["changed_outside"], 0)

        # Repeatable: the same inputs capture the same pixels.
        code, again, _ = self._synthetic(directory, "again")
        self.assertEqual(code, 0)
        self.assertEqual(again["captures"]["configured"], result["captures"]["configured"])


def test_rigid_parent_sky_moves_in_predeclared_regions_repeatably(self):
    with tempfile.TemporaryDirectory(prefix="eawr-space-rigid-") as temporary:
        directory = pathlib.Path(temporary)
        code, baseline, baseline_png = self._synthetic(directory, "baseline")
        self.assertEqual(code, 0, baseline["failure"])
        code, rigid, rigid_png = self._synthetic(directory, "rigid", "rigid")
        self.assertEqual(code, 0, rigid["failure"])
        self.assertEqual(rigid["status"], "space_primary_sky_passed")
        self._check_identity(rigid)
        self.assertEqual(rigid["space"]["pixel_evidence_status"], "verified")
        self.assertEqual(rigid["space"]["evidence"]["changed_outside"], 0)
        for surface, texture in zip(rigid["space"]["surfaces"],
                                    ("eawr_space_a.tga", "eawr_space_b.dds")):
            self.assertEqual(surface["status"], "accepted")
            self.assertEqual(surface["pixels"]["status"], "verified")
            for quadrant, expected in zip(surface["pixels"]["uv_quadrants"], fixture.QUADRANTS[texture]):
                self.assertGreater(quadrant["pixels"], 1000)
                for measured, wanted in zip(quadrant["mean_rgb"], expected):
                    self.assertAlmostEqual(measured, wanted, delta=6)
        width, height, before = decode_png(baseline_png.read_bytes())
        _, _, after = decode_png(rigid_png.read_bytes())
        masks = []
        for path in (baseline_png, rigid_png):
            for surface in range(2):
                mw, mh, bits = read_pgm(path.with_suffix(f".mask-surface-{surface}.pgm"))
                self.assertEqual((mw, mh), (width, height))
                masks.append(bits)
        self.assertNotEqual(masks[0], masks[2], "SkyA must move")
        self.assertNotEqual(masks[1], masks[3], "SkyB must move")
        moved = outside = 0
        for y in range(height):
            for x in range(width):
                if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                    continue
                moved += 1
                if not any(mask[y][x] for mask in masks):
                    outside += 1
        self.assertGreater(moved, 1000)
        self.assertEqual(outside, 0)
        self.assertEqual(rigid["space"]["lifecycle"]["resources_after_teardown"], 0)
        self.assertEqual(rigid["space"]["lifecycle"]["instances_after_teardown"], 0)
        code, repeat, _ = self._synthetic(directory, "rigid-repeat", "rigid")
        self.assertEqual(code, 0, repeat["failure"])
        self.assertEqual(repeat["captures"]["configured"], rigid["captures"]["configured"])
        self.assertEqual(repeat["space"]["lifecycle"]["resources_after_teardown"], 0)
        self.assertEqual(repeat["space"]["lifecycle"]["instances_after_teardown"], 0)


def test_texture_swap_changes_only_that_surface(self):
    with tempfile.TemporaryDirectory(prefix="eawr-space-swap-") as temporary:
        directory = pathlib.Path(temporary)
        code, baseline, baseline_png = self._synthetic(directory, "baseline")
        self.assertEqual(code, 0, baseline["failure"])
        code, swapped, swapped_png = self._synthetic(directory, "swap", "swap")
        self.assertEqual(code, 0, swapped["failure"])
        first = baseline["space"]["surfaces"]
        second = swapped["space"]["surfaces"]
        self.assertEqual(first[0]["texture"]["sha256"], second[0]["texture"]["sha256"])
        self.assertNotEqual(first[1]["texture"]["sha256"], second[1]["texture"]["sha256"])
        self.assertEqual(first[0]["pixels"]["uv_quadrants"], second[0]["pixels"]["uv_quadrants"])
        for quadrant, expected in zip(second[1]["pixels"]["uv_quadrants"], fixture.SWAPPED_B):
            for measured, wanted in zip(quadrant["mean_rgb"], expected):
                self.assertAlmostEqual(measured, wanted, delta=6)
        # Pixel attribution outside this process: every changed pixel lies
        # in surface 1's predeclared (dilated) mask.
        width, height, before = decode_png(baseline_png.read_bytes())
        _, _, after = decode_png(swapped_png.read_bytes())
        mask_width, mask_height, mask = read_pgm(baseline_png.with_suffix(".mask-surface-1.pgm"))
        _, _, other = read_pgm(baseline_png.with_suffix(".mask-surface-0.pgm"))
        self.assertEqual((width, height), (mask_width, mask_height))
        changed_inside = changed_outside = changed_other = 0
        for y in range(height):
            for x in range(width):
                if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                    continue
                if mask[y][x]:
                    changed_inside += 1
                else:
                    changed_outside += 1
                if other[y][x]:
                    changed_other += 1
        self.assertGreater(changed_inside, 10000)
        self.assertEqual(changed_outside, 0)
        self.assertEqual(changed_other, 0)


def test_negative_controls_invalidate_their_evidence(self):
    camera = ("--eawr-space-camera", fixture.CAMERA)
    with tempfile.TemporaryDirectory(prefix="eawr-space-negative-") as temporary:
        directory = pathlib.Path(temporary)

        code, result, _ = self._synthetic(directory, "drop", extra=camera + ("--eawr-space-control", "drop-submission"))
        self._expect_failure(code, result, "failed", "removed submission")
        self.assertEqual(result["space"]["submission_status"], "mismatch")
        self.assertEqual(result["space"]["surfaces"][1]["pixels"]["status"], "not_submitted")
        self.assertEqual(result["space"]["surfaces"][0]["pixels"]["status"], "verified")

        code, result, _ = self._synthetic(directory, "broken", extra=camera + ("--eawr-space-control", "broken-shader"))
        self._expect_failure(code, result, "failed", "broken shader")
        self.assertEqual(result["space"]["material_status"], "compile_failed")
        self.assertEqual(result["space"]["surfaces"][1]["compiler"], "rejected")
        self.assertIn("EAWR-RENDER-0007", result["space"]["surfaces"][1]["upload_failure"])
        lifecycle = result["space"]["lifecycle"]
        self.assertTrue(lifecycle["partial_failure"])
        self.assertEqual(lifecycle["resources_after_partial_failure"], 0)

        code, result, _ = self._synthetic(directory, "offcam", extra=("--eawr-space-camera", fixture.OFF_CAMERA, *NONE))
        self._expect_failure(code, result, "failed", "frustum-out camera")
        self.assertEqual({s["pixels"]["status"] for s in result["space"]["surfaces"]}, {"not_projected"})
        self.assertEqual(result["space"]["submission_status"], "matched",
                         "submission is counted separately from visible pixels")

        code, result, _ = self._synthetic(directory, "occfull", extra=camera + ("--eawr-space-control", "occluder-full"))
        self._expect_failure(code, result, "failed", "fully occluded surface")
        self.assertEqual(result["space"]["surfaces"][0]["pixels"]["status"], "occluded")
        self.assertEqual(result["space"]["evidence"]["occlusion"]["status"], "verified")

        code, result, _ = self._synthetic(directory, "missing", "missing_texture")
        self._expect_failure(code, result, "space_blocked", "missing texture must not grey-pass")
        self.assertEqual(result["space"]["surfaces"][1]["status"], "texture_not_in_vfs")
        self.assertEqual(result["space"]["material_status"], "not_attempted")

        code, result, _ = self._synthetic(directory, "unqualified", "unqualified_shader")
        self._expect_failure(code, result, "space_blocked", "unreviewed shader identity")
        self.assertEqual(result["space"]["surfaces"][1]["status"], "shader_not_qualified")

        code, result, _ = self._synthetic(directory, "nocamera", extra=NONE)
        self._expect_failure(code, result, "failed", "no fixed camera")
        self.assertEqual(result["space"]["camera"]["status"], "malformed")

        code, result, _ = self._synthetic(directory, "collinear", extra=(
            "--eawr-space-camera", "0,0,0,0,-1,0,0,1,0,60,1,5000", *NONE))
        self._expect_failure(code, result, "failed", "collinear up vector")
        self.assertEqual(result["space"]["camera"]["status"], "up_collinear")

        code, result, _ = self._synthetic(directory, "lit", extra=camera + ("--eawr-lighting", "sh"))
        self._expect_failure(code, result, "failed", "lighting is not composed for space")
        self.assertIn("composes no lighting", result["failure"])


def test_unsupported_primary_sky_continues(self):
    with tempfile.TemporaryDirectory(prefix="eawr-unsupported-sky-") as temporary:
        directory = pathlib.Path(temporary)
        code, result, _ = self._synthetic(directory, "populate", extra=(
            "--eawr-space-camera", fixture.CAMERA, "--eawr-populate"))
        self.assertEqual(code, 0, result["failure"])
        self.assertEqual(result["space"]["slice"], "E-space-environment-v1")
        self.assertTrue(all(item["status"] == "not_drawn" for item in result["space"]["surfaces"]))
        self.assertIn("primary sky drew no surface; continuing without a primary sky",
                      result["space"]["not_rendered"])


def test_missing_tactical_cameras_fails_before_sky(self):
    with tempfile.TemporaryDirectory(prefix="eawr-space-missing-tactical-camera-") as temporary:
        directory = pathlib.Path(temporary)
        root = fixture.write_fixture_root(directory / "fixture")
        (root / "GameData/Data/XML/TacticalCameras.xml").unlink()
        code, result, _ = self._run(
            root, fixture.MAP_LOGICAL_PATH, directory, "missing-tactical-camera",
            ("--eawr-space-camera", fixture.CAMERA, "--eawr-populate"))
        self._expect_failure(code, result, "failed", "missing tactical camera XML")
        self.assertEqual(result["space"]["slice"], "E-space-environment-v1")
        self.assertIn("Space_Mode camera: EAWR-VFS-0002", result["failure"])
        self.assertIn("data/xml/tacticalcameras.xml", result["failure"])
        self.assertIn("asset is not present in the mounted view", result["failure"])
        self.assertNotIn("primary sky drew no surface", result["failure"])


def test_nonfinite_camera_fails_with_strict_json(self):
    # M1: NaN, infinity and a float overflow are refused before any upload,
    # and the failure report stays RFC 8259 JSON (strict_json in _run).
    cameras = {
        "nan": "nan,0,0,0,0,-1,0,1,0,60,1,5000",
        "inf": "0,0,0,0,0,-1,0,1,0,inf,1,5000",
        "neginf": "0,0,0,0,0,-1,0,1,0,60,1,-inf",
        "overflow": "0,0,0,0,0,-1,0,1e999,0,60,1,5000",
    }
    with tempfile.TemporaryDirectory(prefix="eawr-space-camera-") as temporary:
        directory = pathlib.Path(temporary)
        for name, text in cameras.items():
            code, result, _ = self._synthetic(directory, name, extra=("--eawr-space-camera", text, *NONE))
            self._expect_failure(code, result, "failed", name)
            space = result["space"]
            self.assertEqual(space["camera"]["status"], "nonfinite", name)
            self.assertEqual(space["camera"]["input"], text, name)
            self.assertEqual(space["material_status"], "not_attempted", name)
            self.assertIsNone(result["capture_identity"]["camera"], name)


def test_unwritable_requested_artifacts_fail_and_release(self):
    # M2: every requested capture and mask write is checked; a failure
    # names the artifact, exits non-zero and still releases the uploads.
    with tempfile.TemporaryDirectory(prefix="eawr-space-artifacts-") as temporary:
        directory = pathlib.Path(temporary)
        blocker = directory / "blocker"
        blocker.write_bytes(b"a regular file, not a directory")
        cases = {
            # Parent of the configured capture is a regular file.
            "parent": (blocker / "capture.png", "capture.png"),
            # A directory already occupies a phase capture's path.
            "phase": (directory / "phase.png", "phase.sky_disabled.png"),
            # A directory already occupies surface 0's exported mask path.
            "mask": (directory / "mask.png", "mask.mask-surface-0.pgm"),
        }
        (directory / "phase.sky_disabled.png").mkdir()
        (directory / "mask.mask-surface-0.pgm").mkdir()
        for name, (capture, artifact) in cases.items():
            code, result, _ = self._synthetic(directory, name, capture=capture)
            self._expect_failure(code, result, "failed", name)
            self.assertIn(artifact, result["failure"], name)
            failed = result["space"]["artifacts"]["failed"]
            self.assertTrue(any(artifact in item for item in failed), (name, failed))
            self.assertNotEqual(result["space"]["artifacts"]["status"], "written", name)
            lifecycle = result["space"]["lifecycle"]
            self.assertEqual(lifecycle["resources_after_teardown"], 0, name)
            self.assertEqual(lifecycle["instances_after_teardown"], 0, name)


def test_hidden_bone_visibility_fails_closed(self):
    # M3: an identity chain carrying a hidden bone is not silently dropped
    # into an unconditionally visible boneless upload.
    with tempfile.TemporaryDirectory(prefix="eawr-space-bones-") as temporary:
        directory = pathlib.Path(temporary)
        for variant, bone in (("hidden_bone", "Root"), ("hidden_ancestor", "Root")):
            code, result, _ = self._synthetic(directory, variant, variant)
            self._expect_failure(code, result, "space_blocked", variant)
            space = result["space"]
            self.assertEqual(space["plan_status"], "surface_rejected", variant)
            self.assertEqual(space["material_status"], "not_attempted", variant)
            for surface in space["surfaces"]:
                self.assertEqual(surface["status"], "bone_visibility_unsupported", variant)
                self.assertIn(f"'{bone}'", surface["detail"], variant)
                self.assertIn("hidden", surface["detail"], variant)
                self.assertEqual(surface["renderer_asset"], 0, variant)


def test_occluder_and_lifecycle_controls_pass(self):
    camera = ("--eawr-space-camera", fixture.CAMERA)
    with tempfile.TemporaryDirectory(prefix="eawr-space-controls-") as temporary:
        directory = pathlib.Path(temporary)
        code, result, _ = self._synthetic(directory, "occluder", extra=camera + ("--eawr-space-control", "occluder"))
        self.assertEqual(code, 0, result["failure"])
        occlusion = result["space"]["evidence"]["occlusion"]
        self.assertEqual(occlusion["status"], "verified")
        self.assertGreater(occlusion["occluder_pixels"], 1000)
        self.assertEqual(occlusion["occluder_changed_by_sky"], 0)
        self.assertEqual(result["space"]["lifecycle"]["resources_after_teardown"], 0)

        # Live lifecycle: each cycle submits and draws the composed sky, so
        # instances exist when its assets are released and re-uploaded.
        code, baseline, _ = self._synthetic(directory, "reload-baseline")
        self.assertEqual(code, 0, baseline["failure"])
        code, result, _ = self._synthetic(directory, "reload", extra=camera + ("--eawr-space-control", "reload-cycle"))
        self.assertEqual(code, 0, result["failure"])
        lifecycle = result["space"]["lifecycle"]
        self.assertEqual(lifecycle["reload_cycles"], 3)
        self.assertEqual(lifecycle["mode"], "live")
        self.assertEqual(lifecycle["instances_before_release"], [2, 2, 2])
        self.assertEqual(lifecycle["resources_before_release"], [2, 2, 2])
        self.assertEqual(lifecycle["instances_after_release"], [0, 0, 0])
        self.assertEqual(lifecycle["resources_after_release"], [0, 0, 0])
        self.assertEqual(lifecycle["resources_after_reupload"], [2, 2, 2])
        self.assertEqual(lifecycle["resources_after_teardown"], 0)
        self.assertEqual(lifecycle["instances_after_teardown"], 0)
        self.assertEqual(result["captures"]["configured"], baseline["captures"]["configured"],
                         "the composition after three live cycles draws the same pixels")


def test_lit_foreground_excludes_the_sky_from_shadows(self):
    # Design gate: a controlled lit receiver lies where surface 0 would
    # shadow it under one fixed directional light. The positive control
    # (a depth-writing, two-winding copy of the sky surfaces with casting
    # enabled) must darken the receiver; the same caster with the cast
    # flag off, and the shipped sky, must leave it unchanged.
    camera = ("--eawr-space-camera", fixture.CAMERA)
    with tempfile.TemporaryDirectory(prefix="eawr-space-shadow-") as temporary:
        directory = pathlib.Path(temporary)
        runs = {}
        for control in ("sky-shadow-caster-cast", "sky-shadow-caster", "sky-shadow-cast", "sky-shadow"):
            runs[control] = self._synthetic(directory, control, extra=camera + ("--eawr-space-control", control))

        code, positive, _ = runs["sky-shadow-caster-cast"]
        self._expect_failure(code, positive, "failed", "a casting surface must shadow the lit receiver")
        shadows = positive["space"]["shadows"]
        self.assertIs(shadows["sky_cast_flag"], True)
        self.assertEqual(shadows["sky_material"], "depth_writing_caster_control")
        self.assertEqual(shadows["lit_foreground_control"], "shadowed_by_sky")
        receiver_pixels = shadows["receiver_pixels"]
        self.assertGreater(receiver_pixels, 1000)
        self.assertGreater(shadows["receiver_changed_by_sky"] * 10, receiver_pixels * 9)
        self.assertIn("shadowed the lit foreground receiver", positive["failure"])

        expected = {
            # control: (cast flag, material)
            "sky-shadow-caster": (False, "depth_writing_caster_control"),
            "sky-shadow-cast": (True, "shipped_adapter"),
            "sky-shadow": (False, "shipped_adapter"),
        }
        for control, (flag, material) in expected.items():
            code, result, _ = runs[control]
            self.assertEqual(code, 0, (control, result["failure"]))
            self.assertEqual(result["status"], "space_primary_sky_passed", control)
            shadows = result["space"]["shadows"]
            self.assertIs(shadows["sky_cast_flag"], flag, control)
            self.assertEqual(shadows["sky_material"], material, control)
            self.assertEqual(shadows["lit_foreground_control"], "unchanged", control)
            self.assertEqual(shadows["receiver_pixels"], receiver_pixels, control)
            self.assertEqual(shadows["receiver_changed_by_sky"], 0, control)
            self.assertTrue(result["lighting"]["composed"], control)
            self.assertEqual(result["space"]["submissions"]["expected"][-1]["role"], "lit_receiver_control")
            for surface in result["space"]["surfaces"]:
                self.assertEqual(surface["pixels"]["status"], "verified", control)
                self.assertIs(surface["casts_shadows"], flag, control)


def test_land_path_refuses_space_options(self):
    with tempfile.TemporaryDirectory(prefix="eawr-space-land-") as temporary:
        directory = pathlib.Path(temporary)
        root = scene_fixture.write_fixture_root(directory / "land")
        code, result, _ = self._run(root, scene_fixture.MAP_LOGICAL_PATH, directory, "land",
                                    ("--eawr-space-camera", fixture.CAMERA))
        self.assertNotEqual(code, 0)
        self.assertEqual(result["map"]["kind"], "land")
        self.assertIn("kind-2", result["failure"])


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the read-only pinned space map run")
def test_pinned_space_reference_map_passes_or_names_its_blocker(self):
    pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
    entry = next(item for item in pinned["references"] if item["logical_path"] == ALDERAAN_SPACE)
    self.assertEqual(entry["role"], "regression_fixture")
    self.assertEqual(entry["checkpoint"]["kind"], "space")
    with tempfile.TemporaryDirectory(prefix="eawr-space-corpus-") as temporary:
        directory = pathlib.Path(temporary)
        # A camera at the source origin is only an input for this probe; no
        # corpus camera is frozen, so a pass here is not an accepted view.
        code, result, _ = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), entry["logical_path"],
                                    directory, "alderaan", ("--eawr-profile", "eaw", "--eawr-space-camera", fixture.CAMERA, *NONE))
        self.assertEqual(result["map"]["sha256"], entry["sha256"])
        self.assertEqual(result["map"]["kind"], "space")
        self.assertIn(result["status"], SPACE_STATUSES, result["failure"])
        space = result["space"]
        self.assertFalse(space["environment_complete"])
        self.assertEqual(space["environment"]["count"], entry["checkpoint"]["environments"])
        # Pinned current classification (P1-06-space-primary-sky.md, with
        # the MeshGloss route of P1-06-meshgloss-sky-route.md): the
        # declared primary sky resolves to a two-surface model whose star
        # surface is accepted and whose sun billboard is rejected with
        # these named causes, so nothing is drawn. An earlier
        # catalog/model blocker, a different surface set, or a pass all
        # fail here; a pass must be accepted deliberately, not by default.
        self.assertEqual(result["status"], "space_blocked", result["failure"])
        self.assertNotEqual(code, 0)
        self.assertEqual(space["plan_status"], "surface_rejected")
        self.assertEqual(space["load_status"], "blocked")
        self.assertEqual(space["material_status"], "not_attempted")
        self.assertEqual(space["environment"]["primary_sky"], ALDERAAN_BLOCKER["primary_sky"])
        self.assertEqual(space["sky_object"]["status"], "ready")
        self.assertEqual(space["sky_object"]["declared_tag"], "Space_Model_Name")
        self.assertEqual(space["model"]["logical_path"], ALDERAAN_BLOCKER["model"])
        self.assertEqual(space["model"]["sha256"], ALDERAAN_BLOCKER["model_sha256"])
        surfaces = space["surfaces"]
        self.assertEqual(len(surfaces), len(ALDERAAN_BLOCKER["surfaces"]))
        for surface, pinned in zip(surfaces, ALDERAAN_BLOCKER["surfaces"]):
            name, shader, texture, first, required = pinned
            self.assertEqual(surface["mesh_name"], name)
            self.assertEqual(surface["original_shader"], shader)
            self.assertEqual(surface["texture"]["logical_path"], texture)
            self.assertEqual(surface["status"], first, surface)
            self.assertTrue(set(required) <= set(surface["causes"]), surface)
            if first == "accepted":
                self.assertEqual(surface["causes"], [], surface)
                self.assertEqual(surface["material"]["route"], "meshgloss", surface)
                self.assertEqual(surface["adapter_id"], "eawr-space-sky-meshgloss-v1", surface)
                self.assertEqual(surface["unconsumed_parameters"], ["Shininess"], surface)
            self.assertEqual(surface["renderer_asset"], 0)
