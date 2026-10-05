"""Cases for test_map_mode; collected by its legacy facade."""

from map_mode_test_support import (
    FAMILIES, FIXTURE, FORBIDDEN, Image,
    LAND_RUNTIME_PATH, LAND_RUNTIME_ROLE, MapModeRunner, REFERENCE_MAPS,
    ROOT, UNRESOLVED, evidence_frame, expected_default_sh_coefficients,
    fixture_bytes, install_fixture, json, math,
    mode_source, os, pathlib, pinned_reference,
    re, scene_bloom_reference, scene_fixture, source_text,
    struct, subprocess, sys, tempfile,
    unittest,
)


class MapModeGraphicalCases:
    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
        and Image, "set the graphical Godot variables and install Pillow")
    def test_land_bloom_matches_the_retail_pass(self):
        """#201: the capture blooms like a CPU run of SceneBloom.fx over the unbloomed frame."""
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-bloom-") as temporary:
            output = pathlib.Path(temporary)
            results = {state: self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), land["logical_path"],
                                        output / f"{state}.json",
                                        ("--eawr-lighting", "sh", "--eawr-bloom", state,
                                         "--eawr-capture", str(output / f"{state}.png")))
                       for state in ("off", "on")}
            self.assertEqual(results["off"]["scene_bloom"]["status"], "off")
            bloom = results["on"]["scene_bloom"]
            self.assertEqual((bloom["status"], bloom["source"]), ("on", "environment 0"))
            # Environment 0 of _mp_land_naboo (Sunrise_Clear), minis 0x23, 0x24 and 0x28.
            self.assertEqual((bloom["strength"], bloom["cutoff"], bloom["size"]), (1.0, 0.9, 1.0))

            off = Image.open(output / "off.png").convert("RGB")
            frame, bloomed = off.tobytes(), Image.open(output / "on.png").convert("RGB").tobytes()
            width, height = off.size
            target = scene_bloom_reference.bloom_target(frame, width, height, bloom["cutoff"], bloom["size"])
            samples = close = changed = 0
            worst = 0.0
            for y in range(1, height, 4):
                for x in range(1, width, 4):
                    expected = scene_bloom_reference.combine(frame, width, height, target, bloom["strength"], x, y)
                    index = (y * width + x) * 3
                    error = max(abs(bloomed[index + channel] - expected[channel] * 255.0) for channel in range(3))
                    worst = max(worst, error)
                    samples += 1
                    close += error <= 2.5
                    changed += max(abs(bloomed[index + channel] - frame[index + channel]) for channel in range(3)) > 2
            self.assertGreater(changed * 20, samples, "bloom changes at least 5% of the frame")
            self.assertGreaterEqual(close, samples * 0.995, f"worst error {worst:.1f} levels")


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
        and Image, "set the graphical Godot variables and install Pillow")
    def test_naboo_ribbons_obey_fully_hidden_map_fog(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-water-fog-") as temporary:
            output = pathlib.Path(temporary)
            grid = output / "hidden.eawr-fog"
            scale = 1 << 24
            payload = struct.pack("<8sIIIIqqqqIQQ", b"EAWRFOG\0", 1, 0, 64, 64,
                                  0, 0, 80 * scale, 80 * scale, 1, 1, 4096) + bytes(4096)
            grid.write_bytes(payload)
            import hashlib
            capture = output / "hidden.png"
            report = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], output / "hidden.json",
                               ("--eawr-map-zoom", "0.32", "--eawr-map-target", "2750,3300",
                                "--eawr-fog-grid", str(grid), "--eawr-fog-sha256",
                                hashlib.sha256(payload).hexdigest(), "--eawr-fog-team", "0",
                                "--eawr-fog-revision", "1", "--eawr-fog-tick", "1",
                                "--eawr-capture", str(capture)),
                               allowed_failure="tactical capture is flat: nothing distinguishable was drawn")
            self.assertEqual(report["water"]["rivers_drawn"], 4)
            self.assertEqual(report["fog"]["renderer"]["attached_consumers"],
                             report["fog"]["renderer"]["declared_consumers"])
            self.assertGreater(report["fog"]["renderer"]["attached_consumers"], 547)
            self.assertEqual(Image.open(capture).convert("RGB").getextrema(),
                             ((0, 0), (0, 0), (0, 0)))


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set the graphical Godot variables and installed FoC root")
    def test_family_zero_maps_keep_authored_group_counts(self):
        expected = {
            "_land_planet_bespin_01": (4, 0),
            "_land_planet_utapau_01": (18, 0),
            "_mp_land_bespin": (4, 0),
            "_mp_land_naboo": (4, 0),
            "_mp_land_utapau": (18, 0),
            "um01_a_crimelord_unleashed": (8, 0),
            "um04_visions_of_the_past": (65, 0),
            "um11_raiders_of_the_lost_holocron": (231, 42),
        }
        with tempfile.TemporaryDirectory(prefix="eawr-family-zero-") as temporary:
            output = pathlib.Path(temporary)
            for name, (count, water_count) in expected.items():
                with self.subTest(map=name):
                    report = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                       f"data/art/maps/{name}.ted", output / f"{name}.json",
                                       ("--eawr-map-view", "overview"),
                                       allowed_failure="a resolved skydome drew nothing outside the terrain footprint")
                    self.assertEqual(report["water"]["plane_family"], 0)
                    self.assertFalse(report["water"]["plane_drawn"])
                    self.assertEqual(report["water"]["rivers_drawn"], count)
                    self.assertEqual(report["water"]["water_decoration_tracks_drawn"], water_count)
                    self.assertEqual(report["water"]["terrain_tracks_drawn"], count - water_count)


    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST for graphical MapMode checks")
    def test_water_capture_time_rejects_nonfinite_input(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-water-time-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "invalid.json"
            completed = subprocess.run(
                [os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-map", scene_fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
                 "--eawr-report", str(report), "--eawr-map-water-time", "nan"],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("--eawr-map-water-time expects a finite number",
                          json.loads(report.read_text(encoding="utf-8"))["failure"])


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the FoC corpus run")
    def test_foc_reference_map_is_default_on_foc_install(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-foc-land-") as temporary:
            report = pathlib.Path(temporary) / "land.json"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], report)
            self.assertEqual(result["profile"], "foc")
            self.assertEqual(result["layers"], ["expansion", "base"])
            self.assertEqual(result["map"]["sha256"], land["sha256"])


    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical map mode")
    def test_synthetic_map_renders_terrain_where_terrain_is(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-mode-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "map.json"
            result = self._run(root, "data/art/maps/synthetic.ted", report)

            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["kind"], "land")
            self.assertTrue(result["map"]["semantic_complete"])
            self.assertEqual(result["terrain"]["chunks"], 1)
            self.assertEqual(result["terrain"]["uploaded_surfaces"], 2)
            # 32x24 cells, four corners each, two triangles each.
            self.assertEqual(result["terrain"]["vertices"], 32 * 24 * 4)
            self.assertEqual(result["terrain"]["triangles"], 32 * 24 * 2)

            slots = result["material_slots"]
            self.assertEqual(len(slots), 2)
            self.assertEqual(slots[0]["effect_program"], "TerrainRenderBump.fx")
            self.assertEqual(slots[0]["effect_technique"], "nobump")
            self.assertEqual(slots[1]["effect_program"], "TerrainRenderBumpDual.fx")
            self.assertEqual(slots[1]["effect_technique"], "bump")
            self.assertEqual(sum(slot["cells"] for slot in slots), 32 * 24)
            for slot in slots:
                self.assertFalse(slot["legacy_route_supported"])

            # The fixture names a skydome object that resolves against no
            # catalog, so the mode must say so rather than skip it silently.
            self.assertEqual(result["skydome"]["object_id"], "EAWR_SYNTHETIC_SKYDOME")
            self.assertIn(result["skydome"]["status"],
                          {"object_not_in_catalog", "catalog_unavailable"})
            self.assertFalse(result["skydome"]["drawn"])

            evidence = result["evidence"]
            self.assertTrue(evidence["verified"])
            self.assertTrue(evidence["capture_sha256"])
            # Terrain covers its whole projected footprint, and with no skydome
            # nothing is drawn outside it.
            self.assertGreaterEqual(evidence["inside_footprint_coverage"], 0.95)
            self.assertLessEqual(evidence["outside_footprint_coverage"], 0.05)
            self.assertGreater(result["frame_time"]["milliseconds_per_frame"], 0.0)
            self.assertEqual(result["frame_time"]["timed_frames"], 120)


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_pinned_land_reference_map_renders(self):
        land = pinned_reference(LAND_RUNTIME_ROLE, "land", LAND_RUNTIME_PATH)
        with tempfile.TemporaryDirectory(prefix="eawr-map-mode-land-") as temporary:
            report = pathlib.Path(temporary) / "land.json"
            # The footprint evidence needs the top-down overview; the default
            # is the tactical view since P1-06 #27.
            result = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), land["logical_path"], report,
                ("--eawr-profile", "eaw", "--eawr-map-view", "overview"))

            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["sha256"], land["sha256"])
            self.assertEqual(result["map"]["kind"], "land")
            width = land["checkpoint"]["terrain_width"]
            height = land["checkpoint"]["terrain_height"]
            # Every cell of the pinned grid is in exactly one surface.
            self.assertEqual(sum(slot["cells"] for slot in result["material_slots"]),
                             (width - 1) * (height - 1))
            self.assertEqual(len(result["material_slots"]),
                             land["checkpoint"]["terrain_materials"])
            self.assertEqual(result["terrain"]["triangles"], (width - 1) * (height - 1) * 2)

            # Every slot that carries cells and declares a diffuse texture
            # resolves it; an undeclared slot is reported, not hidden.
            used = [slot for slot in result["material_slots"] if slot["cells"] > 0]
            self.assertTrue(used)
            for slot in used:
                if slot["declared_primary"]:
                    self.assertTrue(slot["texture_resolved"], slot["declared_primary"])

            skydome = result["skydome"]
            self.assertEqual(skydome["status"], "drawn")
            self.assertTrue(skydome["object_id"])
            self.assertTrue(skydome["model_logical_path"].startswith("data/art/models/"))
            self.assertTrue(skydome["model_sha256"])

            evidence = result["evidence"]
            self.assertTrue(evidence["verified"])
            self.assertGreaterEqual(evidence["inside_footprint_coverage"], 0.95)
            # A skydome is authored for a ground-level view, so the claim is
            # that it drew outside the terrain footprint, not that it filled it.
            self.assertGreaterEqual(evidence["outside_footprint_coverage"], 0.10)
            self.assertEqual(result["water"]["status"], "approximate")
            self.assertTrue(result["water"]["cause"])


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_default_tactical_naboo_accounts_effects_outside_capture(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-naboo-tactical-") as temporary:
            report = pathlib.Path(temporary) / "naboo.json"
            capture = pathlib.Path(temporary) / "naboo.png"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], report,
                               ("--eawr-populate", "--eawr-lighting", "sh",
                                "--eawr-environment", "map", "--eawr-shadows", "on",
                                "--eawr-capture", str(capture)))
            self.assertEqual(result["status"], "map_render_passed")
            self.assertTrue(capture.is_file())
            self.assertEqual(result["water"]["rivers_drawn"], 4)
            self.assertFalse(result["water"]["plane_drawn"])
            self.assertEqual(result["capture_identity"]["view"], "tactical")
            self.assertEqual(result["populate"]["surfaces_failed"], 0)
            self.assertTrue(result["populate"]["evidence"]["verified"])
            particles = result["map_particles"]
            self.assertTrue(particles["evidence_verified"])
            self.assertGreater(particles["outside_view_placements_at_capture"], 0)
            self.assertTrue(any("outside" in cause and "viewport" in cause
                                for placement in particles["placements"]
                                for cause in placement["causes"]))
            self.assertEqual(result["lighting"]["policy_luminance"]["status"], "verified")
            # #225: record 0 by default, and its 0x17 colour is the floor.
            environment = result["lighting"]["environment"]
            self.assertEqual((environment["record"], environment["record_name"]), (0, "Sunrise_Clear"))
            floor = result["lighting"]["shadows"]["shadow_floor"]
            self.assertEqual(floor, environment["shadow_color"])
            for value, expected in zip(floor, (0.4, 0.4157, 0.6), strict=True):
                self.assertAlmostEqual(value, expected, places=3)
            # #291: blur 1 lets the soft filter and the depth bias apply; the
            # bias is small because Godot scales it by cascade depth and filter.
            shadows = result["lighting"]["shadows"]
            self.assertEqual(shadows["blur"], 1)
            self.assertAlmostEqual(shadows["bias"], 0.05, places=6)
            self.assertEqual(shadows["normal_bias"], 5)


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_naboo_environment_record_picks_the_battle_environment(self):
        # #225: retail draws one of Naboo's two environments per battle
        # (R-SEL-03). The option pins the one a retail capture drew; the
        # floor is that record's 0x17 colour per channel.
        land = pinned_reference("m1_reference", "land")
        game_root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-environment-record-") as temporary:
            output = pathlib.Path(temporary)
            result = self._run(game_root, land["logical_path"], output / "noon.json",
                               ("--eawr-populate", "--eawr-map-effects", "off", "--eawr-lighting", "sh",
                                "--eawr-environment", "map", "--eawr-environment-record", "1",
                                "--eawr-shadows", "on", "--eawr-capture", str(output / "noon.png")))
            self.assertEqual(result["status"], "map_render_passed")
            environment = result["lighting"]["environment"]
            self.assertEqual((environment["record"], environment["record_name"]), (1, "Noon_Clear"))
            floor = result["lighting"]["shadows"]["shadow_floor"]
            self.assertEqual(floor, environment["shadow_color"])
            for value, expected in zip(floor, (0.651, 0.6431, 0.7255), strict=True):
                self.assertAlmostEqual(value, expected, places=3)
            beyond = self._run(game_root, land["logical_path"], output / "beyond.json",
                               ("--eawr-environment-record", "2"),
                               allowed_failure="--eawr-environment-record 2: the map declares 2 environment records")
            self.assertEqual(beyond["status"], "failed")


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the FoC water run")
    def test_naboo_water_capture_clock_is_repeatable_and_moves(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-water-flow-") as temporary:
            output = pathlib.Path(temporary)
            captures = []
            for index, time in enumerate(("0.2", "0.2", "1.2")):
                capture = output / f"flow-{index}.png"
                result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                   land["logical_path"], output / f"flow-{index}.json",
                                   ("--eawr-map-zoom", "0.32", "--eawr-map-target", "2750,3300",
                                    "--eawr-map-water-time", time, "--eawr-capture", str(capture)))
                self.assertEqual(result["water"]["rivers_drawn"], 4)
                self.assertFalse(result["water"]["plane_drawn"])
                captures.append(capture.read_bytes())
            self.assertEqual(captures[0], captures[1])
            self.assertNotEqual(captures[0], captures[2])


    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the populated map mode")
    def test_synthetic_placements_populate_deterministically(self):
        expected = scene_fixture.EXPECTED
        hashes = []
        with tempfile.TemporaryDirectory(prefix="eawr-map-populate-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            for attempt in range(2):
                report = pathlib.Path(temporary) / f"populated-{attempt}.json"
                result = self._run(root, scene_fixture.MAP_LOGICAL_PATH, report,
                                   ("--eawr-populate",))
                self.assertEqual(result["status"], "map_render_passed")
                self.assertTrue(result["evidence"]["verified"])
                populate = result["populate"]
                self.assertTrue(populate["requested"])
                for field in ("placements", "resolved", "drawable", "unresolved",
                              "scene_assets", "instances", "surfaces_uploaded"):
                    self.assertEqual(populate[field], expected[field], field)
                self.assertEqual(populate["drawn"], expected["drawable"])
                self.assertEqual(populate["surfaces_failed"], 0, populate["surface_failure"])
                for cause, count in populate["unresolved_by_cause"].items():
                    self.assertEqual(count, expected["by_cause"].get(cause, 0), cause)
                details = populate["unresolved_details"]
                self.assertEqual({cause: sum(group["count"] for group in groups)
                                  for cause, groups in details.items()},
                                 {cause: count for cause, count in expected["by_cause"].items() if count})
                self.assertEqual(details["shader_unsupported"], [
                    {"object_type": "EAWR_SCENE_BEACON",
                     "model_path": "data/art/models/eawr_scene_beacon.alo",
                     "shader": "MeshShadowVolume.fx", "count": 1},
                    {"object_type": "EAWR_SCENE_MARKER",
                     "model_path": "data/art/models/eawr_scene_marker.alo",
                     "shader": "MeshSolidColor.fx", "count": 1},
                ])
                self.assertEqual(populate["unsupported_surfaces_on_drawn_placements"],
                                 {"MeshShadowVolume.fx": 1})
                self._check_unit_evidence(populate)
                # Every drawn synthetic unit is large and unoccluded, so every
                # one must add coverage inside its own projected bounds.
                self.assertEqual(populate["evidence"]["units_with_coverage"], expected["drawable"])
                self.assertEqual(populate["evidence"]["changed_pixels_outside_unit_bounds"], 0)
                hashes.append(populate["scene_sha256"])
        self.assertRegex(hashes[0], r"^[0-9a-f]{64}$")
        self.assertEqual(hashes[0], hashes[1], "two consecutive runs must report the same scene hash")


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_pinned_land_reference_map_populates(self):
        land = pinned_reference(LAND_RUNTIME_ROLE, "land", LAND_RUNTIME_PATH)
        inventory = json.loads(UNRESOLVED.read_text(encoding="utf-8"))
        committed = next(entry for entry in inventory["maps"]
                         if entry["logical_path"] == land["logical_path"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-populate-land-") as temporary:
            report = pathlib.Path(temporary) / "land-populated.json"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], report, ("--eawr-profile", "eaw", "--eawr-populate", "--eawr-map-view", "overview"))
            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["sha256"], land["sha256"])
            populate = result["populate"]
            # R-ROT-01..03 (7eb40d84): regenerate the scanner pin with both formerly quarantined placements.
            self.assertEqual(populate["scene_sha256"], committed["scene_sha256"])
            self.assertEqual(populate["placements"], committed["placements"])
            self.assertEqual(populate["resolved"], committed["resolved"])
            self.assertEqual(populate["drawable"], committed["drawable"])
            self.assertEqual(populate["drawn"], committed["drawable"])
            self.assertEqual(populate["unresolved_by_cause"], {
                cause: committed["placements_by_cause"][cause]
                for cause in populate["unresolved_by_cause"]})
            self.assertEqual(populate["surfaces_failed"], 0, populate["surface_failure"])
            self._check_unit_evidence(populate)
            particles = result["map_particles"]
            self.assertTrue(particles["evidence_verified"])
            # The speeder's heat shimmer keys vertex alpha at 5/255, so its
            # distortion stays under the pixel-change threshold: drawn and
            # accounted, not verified.
            heat = [placement for placement in particles["placements"]
                    if placement["object_id"] == "p_sandspeederheat"]
            # R-ROT-01..03 (9e1ec35e): particles also restore the two tilted heat placements.
            self.assertEqual({placement["record_ordinal"] for placement in heat}, {47, 48, 49})
            self.assertEqual(len(heat), 3)
            for placement in heat:
                self.assertEqual(placement["status"], "drawn")
                self.assertIn("heat distortion cannot change a pixel above the threshold at its peak vertex alpha",
                              placement["causes"])
            self.assertEqual(particles["subpixel_heat_placements_at_capture"], 3)


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
        and Image, "set the graphical Godot variables and install Pillow")
    def test_naboo_comms_array_turns_through_its_idle_clip(self):
        # #157: FoC Naboo's Team_00 communications array (RB_COMCENTER, a
        # 120 s clip at 30 fps) turns its dish between idle clock samples 10 s
        # apart, at a fixed tactical camera. Nothing else in the frame moves.
        land = pinned_reference("m1_reference", "land")
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-land-idle-") as temporary:
            output = pathlib.Path(temporary)
            images = {}
            identities = []
            for offset in (0, 300):
                capture = output / f"idle-{offset}.png"
                result = self._run(root, land["logical_path"], output / f"idle-{offset}.json",
                                   ("--eawr-populate", "--eawr-map-zoom", "0.3", "--eawr-map-target", "529,3034",
                                    "--eawr-map-idle-offset", str(offset), "--eawr-capture", str(capture)))
                self.assertEqual(result["status"], "map_render_passed", result.get("failure"))
                self.assertEqual(result["map"]["sha256"], land["sha256"])
                animation = result["populate"]["unit_animation"]
                self.assertEqual((animation["clock"], animation["sample_at_capture"], animation["idle_offset"]),
                                 ("held", 59, offset))
                self.assertEqual(animation["idle_sample_failures"], 0)
                objects = {entry["object"]: entry for entry in animation["objects"]}
                array = objects["Team_00_Communications_Array"]
                self.assertEqual(array["clip"], "data/art/models/rb_comcenter_idle_00.ala")
                # A MultiplayerStructureMarker: no loop, no IDLE behaviour.
                self.assertEqual((array["loop"], array["restarts"], array["random_start"], array["rate_mod"]),
                                 (False, False, True, [1, 1]))
                # The mineral pads name IDLE in Behavior, so their clip restarts.
                self.assertTrue(objects["Skirmish_Mineral_Processor_Pad"]["restarts"])
                self.assertTrue(result["populate"]["evidence"]["verified"])
                identities.append(result["capture_identity"])
                # Unlit (no --eawr-lighting), so no bloom (#307) to spread the dish's change past its box.
                self.assertEqual(result["scene_bloom"]["status"], "lighting_off")
                images[offset] = Image.open(evidence_frame(capture, result)).convert("RGB")
            self.assertEqual(identities[0], identities[1])
            # e8eed89f (#515): the corrected 4:3 FOV enlarges the dish's projected sweep.
            dish = (320, 0, 930, 400)
            before, after = images[0], images[300]
            changed_inside = changed_outside = 0
            for y in range(before.height):
                for x in range(before.width):
                    if before.getpixel((x, y)) == after.getpixel((x, y)):
                        continue
                    if dish[0] <= x < dish[2] and dish[1] <= y < dish[3]:
                        changed_inside += 1
                    else:
                        changed_outside += 1
            self.assertGreater(changed_inside, 5000, "the dish turned between the two idle clock samples")
            self.assertEqual(changed_outside, 0, "only the idle clip moved")


    def test_land_idle_offset_requires_populate(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        root = os.environ.get("EAWR_EAW_GAME_ROOT")
        if not (os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and executable and root):
            self.skipTest("set the graphical Godot variables and EAWR_EAW_GAME_ROOT")
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-land-idle-refuse-") as temporary:
            report = pathlib.Path(temporary) / "refused.json"
            result = self._run(pathlib.Path(root), land["logical_path"], report,
                               ("--eawr-map-idle-offset", "30"),
                               allowed_failure="--eawr-map-idle-offset requires --eawr-populate")
            self.assertEqual(result["status"], "failed")


    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the lit map mode")
    def test_synthetic_scene_is_lit_and_shadowed_under_both_policies(self):
        expected_coefficients = expected_default_sh_coefficients()
        means = {}
        with tempfile.TemporaryDirectory(prefix="eawr-map-lighting-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            for policy in ("sh", "hemisphere"):
                report = pathlib.Path(temporary) / f"lit-{policy}.json"
                result = self._run(root, scene_fixture.MAP_LOGICAL_PATH, report,
                                   ("--eawr-populate", "--eawr-lighting", policy, "--eawr-shadows", "on"))
                self.assertEqual(result["status"], "map_render_passed")
                lighting = result["lighting"]
                self._check_lighting_report(lighting, policy)
                self.assertEqual(lighting["environment"]["source"], "alo_viewer_default")
                for channel in range(3):
                    for index in range(9):
                        self.assertAlmostEqual(lighting["sh_light_all_coefficients"][channel][index],
                                               expected_coefficients[channel][index], places=4)
                evidence = lighting["shadows"]["evidence"]
                # Box casters: the translated top face must darken, and the
                # rest of the flat terrain must not change.
                self.assertEqual(evidence["status"], "verified")
                self.assertEqual(evidence["criterion"], "translated_top_face_luminance")
                self.assertLessEqual(evidence["region_luminance_on"], 0.85 * evidence["region_luminance_off"])
                self.assertAlmostEqual(evidence["control_luminance_on"], evidence["control_luminance_off"],
                                       delta=0.01)
                self.assertEqual(lighting["shadows"]["shadow_receiving_legacy_materials"], 3)
                luminance = lighting["policy_luminance"]
                self.assertEqual(luminance["status"], "verified")
                for name in ("sh", "hemisphere"):
                    self.assertGreater(luminance[name]["mean"], 0.02)
                    self.assertLess(luminance[name]["mean"], 0.98)
                    self.assertLess(luminance[name]["saturated_fraction"], 0.5)
                means[policy] = (luminance["sh"]["mean"], luminance["hemisphere"]["mean"])
                # Units still draw and are attributable under lighting.
                self.assertTrue(result["populate"]["evidence"]["verified"])
        # The same two policy captures come out of either configured policy.
        self.assertAlmostEqual(means["sh"][0], means["hemisphere"][0], places=3)
        self.assertAlmostEqual(means["sh"][1], means["hemisphere"][1], places=3)
        self.assertGreater(abs(means["sh"][0] - means["sh"][1]), 0.01)


    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the lit map mode")
    def test_shadows_need_a_lighting_policy(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        with tempfile.TemporaryDirectory(prefix="eawr-map-lighting-off-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "refused.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-map", scene_fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
                 "--eawr-report", str(report), "--eawr-shadows", "on"],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertNotEqual(completed.returncode, 0)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "failed")
            self.assertIn("lighting policy", result["failure"])


    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_pinned_land_reference_map_is_lit_and_shadowed(self):
        land = pinned_reference(LAND_RUNTIME_ROLE, "land", LAND_RUNTIME_PATH)
        inventory = json.loads(UNRESOLVED.read_text(encoding="utf-8"))
        committed = next(entry for entry in inventory["maps"]
                         if entry["logical_path"] == land["logical_path"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-lighting-land-") as temporary:
            report = pathlib.Path(temporary) / "land-lit.json"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), land["logical_path"], report,
                               ("--eawr-profile", "eaw", "--eawr-populate", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                                "--eawr-map-view", "overview"))
            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["sha256"], land["sha256"])
            # R-ROT-01..03 (7eb40d84): the lit run uses the same regenerated full-transform scene pin.
            self.assertEqual(result["populate"]["scene_sha256"], committed["scene_sha256"])
            lighting = result["lighting"]
            self._check_lighting_report(lighting, "sh")
            self.assertEqual(lighting["shadows"]["skydome_casts_shadows"], False)
            self.assertIn(lighting["shadows"]["evidence"]["status"], {"verified", "inconclusive"})
            self.assertEqual(lighting["policy_luminance"]["status"], "verified")
