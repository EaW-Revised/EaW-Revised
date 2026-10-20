"""Cases for test_space_map_mode; collected by its legacy facade."""

import xml.etree.ElementTree as ET
from space_map_test_support import (
    NONE, REFERENCE_MAPS, ROOT, decode_png,
    fixture, json, math, os,
    pathlib, tempfile, unittest,
)


def test_compiled_environment_material_routes(self):
    with tempfile.TemporaryDirectory(prefix="eawr-compiled-environment-") as temporary:
        directory = pathlib.Path(temporary)
        root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
        model = root / "GameData/Data/Art/Models" / fixture.SKY_MODEL
        # The environment applies the object's quarter turn to the sky;
        # look toward source -X to see these source +Y fixture walls.
        options = ("--eawr-space-camera", "0,0,0,-1,0,0,0,1,0,60,1,5000", "--eawr-populate")
        images = {}
        additive = [[("Color", (0.25, 0.5, 0.75, 1.0)),
                     ("UVScrollRate", (0.0, 0.0, 0.0, 0.0))]] * 2
        for shader in ("MeshGloss.fx", "MeshGloss.fxo", "MeshGlossForStarfields.fxo",
                       "MeshAdditive.fx", "MeshAdditive.fxo", "MeshAdditive2x.fxo"):
            fields = additive if "Additive" in shader else fixture.MESHGLOSS
            model.write_bytes(fixture.alo_bytes([shader, shader], materials=fields))
            code, report, capture = self._run(root, fixture.MAP_LOGICAL_PATH, directory,
                                              shader.replace(".", "-"), options)
            self.assertEqual(code, 0, report["failure"])
            surfaces = [item for item in report["space"]["surfaces"] if item["role"] == "primary_sky"]
            self.assertEqual(len(surfaces), 2)
            self.assertTrue(all(item["status"] == "drawn" for item in surfaces), surfaces)
            images[shader] = decode_png(capture.read_bytes())
        gloss = images["MeshGloss.fx"][2]
        self.assertGreater(sum(gloss[y][x] != gloss[0][0]
                               for y in range(50, 670, 8) for x in range(50, 1230, 8)), 500,
                           "alias identity requires visible fixture pixels")
        self.assertEqual(images["MeshGloss.fx"], images["MeshGloss.fxo"])
        self.assertEqual(images["MeshGloss.fx"], images["MeshGlossForStarfields.fxo"])
        self.assertEqual(images["MeshAdditive.fx"], images["MeshAdditive.fxo"])
        baseline = images["MeshAdditive.fx"][2]
        doubled = images["MeshAdditive2x.fxo"][2]
        background = baseline[0][0]
        tested = 0
        for y in range(50, 670, 8):
            for x in range(50, 1230, 8):
                before, after = baseline[y][x], doubled[y][x]
                if before == background:
                    continue
                for value, actual, clear in zip(before, after, background):
                    self.assertAlmostEqual(actual, min(255, clear + 2 * (value - clear)), delta=2)
                tested += 1
        self.assertGreater(tested, 500)


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the sunlit MC80 shadow regression")
def test_sunlit_mc80_shadow_acne(self):
    # #667: compare the same lit surface with shadows on/off. Dividing
    # stored RGB cancels the hull texture. The preview's default sun
    # exposes the owner-visible band; the map sun lights it differently.
    # The declared patch lies inside that band, away from contact edges.
    with tempfile.TemporaryDirectory(prefix="eawr-mc80-shadow-acne-") as temporary:
        directory = pathlib.Path(temporary)
        config = ROOT / "apps/viewer/project/config"
        tree = ET.parse(config / "coruscant-live-session-camera.xml")
        tree.getroot().find("initial").attrib.update(
            target_x="-4795.853027", target_y="4020.553467", target_height="-290",
            zoom="0", yaw_degrees="0")
        bindings = directory / "space-live-camera-bindings.json"
        bindings.write_text((config / bindings.name).read_text(encoding="utf-8"), encoding="utf-8")
        camera = directory / "camera.xml"
        tree.write(camera, encoding="utf-8", xml_declaration=True)
        options = ("--eawr-populate", "--eawr-live-session", "m2", "--eawr-live-ticks", "1",
                   "--eawr-live-reveal", "on", "--eawr-live-ai", "off", "--eawr-hud", "off",
                   "--eawr-environment", "default", "--eawr-lighting", "sh",
                   "--eawr-map-camera-config", str(camera))
        images = {}
        reports = {}
        for shadow in ("on", "off"):
            code, report, capture = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                "data/art/maps/_mp_space_coruscant.ted", directory, f"mc80-{shadow}",
                options + ("--eawr-shadows", shadow))
            self.assertEqual(code, 0, report["failure"])
            reports[shadow] = report
            width, height, images[shadow] = decode_png(capture.read_bytes())
            self.assertEqual((width, height), (1280, 720))
        self.assertEqual(reports["on"]["capture_identity"]["camera"],
                         reports["off"]["capture_identity"]["camera"])
        self.assertEqual(reports["on"]["lighting"]["shadow_max_distance"], 8192)
        self.assertTrue(reports["on"]["lighting"]["shadows"])
        self.assertFalse(reports["off"]["lighting"]["shadows"])
        ratios = {}
        for y in range(400, 550):
            for x in range(360, 850):
                denominator = sum(images["off"][y][x])
                if denominator >= 96:
                    ratios[x, y] = sum(images["on"][y][x]) / denominator
        changes = [abs(ratio - ratios[neighbour])
                   for (x, y), ratio in ratios.items()
                   for neighbour in ((x + 1, y), (x, y + 1)) if neighbour in ratios]
        self.assertGreater(len(ratios), 10000, "the declared hull patch must be visible")
        fraction = sum(change > 0.04 for change in changes) / len(changes)
        (directory / "acne-metric.json").write_text(json.dumps({
            "rectangle": [360, 400, 850, 550], "pixels": len(ratios),
            "pairs": len(changes), "high_frequency_fraction": fraction}), encoding="utf-8")
        self.assertLess(fraction, 0.02, "sunlit MC80 hull has shadow-induced hatching")
        # A positive control prevents disabling every shadow from passing.
        darkened = sum(sum(images["off"][y][x]) >= 96
                       and sum(images["on"][y][x]) < 0.9 * sum(images["off"][y][x])
                       for y in range(height) for x in range(width))
        self.assertGreater(darkened, 200, "the hull still receives visible shadows")


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant shadow report")
def test_shadow_report_default_and_debug_ship(self):
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
    options = ("--eawr-populate", "--eawr-map-effects", "on", "--eawr-lighting", "sh",
               "--eawr-environment", "map", "--eawr-shadows", "on",
               "--eawr-map-camera-config", str(camera))
    # #667: the ordinary and the debug view share the space shadow
    # settings; the debug view only fits the range to the selected ship.
    settings = {"shadow_mode": "parallel_4_splits", "shadow_bias": 0.05,
                "shadow_normal_bias": 5.0, "shadow_blur": 1.0}
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-shadow-report-") as temporary:
        directory = pathlib.Path(temporary)
        code, ordinary, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                      directory, "ordinary", options)
        self.assertEqual(code, 0, ordinary["failure"])
        lighting = ordinary["lighting"]
        self.assertEqual((lighting["policy"], lighting["composed"], lighting["shadows"], lighting["environment"]),
                         ("sh", True, True, "map"))
        self.assertEqual(lighting["shadow_max_distance"], 8192.0)
        self.assertEqual(lighting["shadow_filter"], "soft_medium")
        self.assertEqual(lighting["shadow_atlas_size"], 4096)
        # 7eb40d84 (#1043) admits the secondary sky's additive vertex-colour material as well.
        self.assertEqual((lighting["shadow_receiving_materials"], lighting["shadow_variant_failures"]), (138, 0))
        for key, value in settings.items():
            self.assertEqual(lighting[key], value, key)
        self.assertNotIn("shadow_scope", lighting)
        code, debug, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                   directory, "debug", options +
                                   ("--eawr-space-place-object", "Star_Destroyer@55"))
        self.assertEqual(code, 0, debug["failure"])
        for key, value in settings.items():
            self.assertEqual(debug["lighting"][key], value, key)
        self.assertLess(debug["lighting"]["shadow_max_distance"], lighting["shadow_max_distance"])
        self.assertEqual(debug["lighting"]["shadow_floor"], lighting["shadow_floor"])
        self.assertEqual(debug["lighting"]["shadow_scope"],
                         "scene directional light (debug evidence only)")
        self.assertEqual(debug["populate"]["debug_ship"]["status"], "composed")


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant hardpoint state run")
def test_debug_ship_hardpoint_states(self):
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
    # Explicit budgets keep the hardpoint test independent of ambient
    # prewarmed allocation; 256/proxy reproduces its original admission.
    options = ("--eawr-populate", "--eawr-map-camera-config", str(camera),
               "--eawr-space-place-object", "Star_Destroyer@55", "--eawr-map-particle-capacity", "16384",
               "--eawr-map-attached-capacity", "256")
    hurt = ("HP_Star_Destroyer_Weapon_FL", "HP_Star_Destroyer_Weapon_BL", "HP_Star_Destroyer_Weapon_ML",
            "HP_Star_Destroyer_Shield_Generator")

    def states(state):
        return tuple(item for hardpoint in hurt for item in ("--eawr-space-hardpoint-state", f"{hardpoint}={state}"))

    def ship_emitters(result):
        return {item["identity"].rsplit("/", 1)[-1]: item["status"]
                for item in result["map_particles"]["placements"] if "#55/proxy#" in item["identity"]}

    # #136: a spawned ship has every hardpoint intact; a destroyed one
    # drops its attached model and shows its damage decal.
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-hardpoints-") as temporary:
        directory = pathlib.Path(temporary)
        code, intact, intact_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                             directory, "intact", options)
        self.assertEqual(code, 0, intact["failure"])
        ship = next(item for item in intact["populate"]["placements"] if item["object"] == "Star_Destroyer")
        self.assertEqual((ship["hardpoints"], ship["hidden_damage_decal_surfaces"]), (10, 8))
        # Tractor beam and fighter bay attach no model; the engines' model
        # holds only a hidden collision mesh.
        self.assertEqual(intact["populate"]["hardpoints_attached"], 7)
        self.assertEqual(intact["populate"]["debug_ship"]["hardpoint_states"], {})
        # The ship is in the attached-effect plan: its intact hardpoints
        # hide their 20 damage emitters; one other emitter runs.
        attached = intact["populate"]["attached_effects"]
        self.assertEqual((attached["hidden_by_hardpoint_state"], attached["capacity_exhausted"]), (20, 0))
        self.assertEqual(ship_emitters(intact), {"proxy#20": "drawn"})
        # Damaged hardpoints keep their models, hidden decals, and hidden
        # emitters: their capture must match intact exactly.
        code, damaged, damaged_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                               directory, "damaged", options + states("damaged"))
        self.assertEqual(code, 0, damaged["failure"])
        self.assertEqual((damaged["populate"]["instances"], damaged["populate"]["hardpoints_attached"]),
                         (intact["populate"]["instances"], 7))
        self.assertEqual(damaged["populate"]["attached_effects"]["hidden_by_hardpoint_state"], 20)
        self.assertEqual(ship_emitters(damaged), ship_emitters(intact))
        self.assertEqual(damaged["map_particles"]["live_rids_after_release"], 0)
        self.assertEqual(intact_png.read_bytes(), damaged_png.read_bytes())
        # Back to intact, the hook retains the same art before the first frame.
        code, healed, healed_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "healed",
                                             options + states("damaged") + states("intact"))
        self.assertEqual(code, 0, healed["failure"])
        self.assertEqual(healed["populate"]["attached_effects"]["hidden_by_hardpoint_state"], 20)
        self.assertEqual(ship_emitters(healed), {"proxy#20": "drawn"})
        self.assertEqual(healed_png.read_bytes(), intact_png.read_bytes())
        code, destroyed, destroyed_png = self._run(
            root, "data/art/maps/_mp_space_coruscant.ted", directory, "destroyed",
            options + ("--eawr-space-hardpoint-state", "HP_Star_Destroyer_Shield_Generator=destroyed",
                       "--eawr-space-hardpoint-state", "hp_star_destroyer_weapon_fl=Damaged"))
        self.assertEqual(code, 0, destroyed["failure"])
        ship = next(item for item in destroyed["populate"]["placements"] if item["object"] == "Star_Destroyer")
        self.assertEqual(ship["hidden_damage_decal_surfaces"], 7)
        self.assertEqual(destroyed["populate"]["hardpoints_attached"], 6)
        self.assertEqual(destroyed["populate"]["debug_ship"]["hardpoint_states"],
                         {"HP_Star_Destroyer_Shield_Generator": "destroyed",
                          "hp_star_destroyer_weapon_fl": "damaged"})
        self.assertEqual(destroyed["populate"]["instances"], intact["populate"]["instances"] - 1)
        # The damaged front-left weapon remains visually intact.
        self.assertEqual(set(ship_emitters(destroyed)), {f"proxy#{proxy}" for proxy in (14, 15, 20)})
        self.assertNotEqual(intact_png.read_bytes(), destroyed_png.read_bytes())
        code, unknown, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "unknown",
                                     options + ("--eawr-space-hardpoint-state", "HP_Nowhere=destroyed"))
        self.assertNotEqual(code, 0)
        self.assertIn("HP_Nowhere is not a hardpoint of Star_Destroyer", unknown["failure"])


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant station marker run")
def test_station_markers_attach_no_effects(self):
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
    options = ("--eawr-populate", "--eawr-map-camera-config", str(camera))

    def emitters(result, record):
        return [item["status"] for item in result["map_particles"]["placements"]
                if f"#{record}/proxy#" in item["identity"]]

    # #284: Team_00/01_Space_Station (records 48 and 54) are markers and
    # used to attach eb_station_01's 8 p_rb_station_damage and 2
    # p_blink_white_big proxies each: 81 records, 52 admitted.
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-markers-") as temporary:
        directory = pathlib.Path(temporary)
        code, markers, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "markers", options)
        self.assertEqual(code, 0, markers["failure"])
        roles = {item["ordinal"]: item["role"] for item in markers["populate"]["placements"]}
        self.assertEqual((roles[48], roles[54]), ("marker", "marker"))
        attached = markers["populate"]["attached_effects"]
        self.assertEqual((attached["records"], attached["admitted"], attached["hidden_by_hardpoint_state"],
                          attached["capacity_exhausted"]), (59, 32, 0, 0))
        self.assertEqual((emitters(markers, 48), emitters(markers, 54)), ([], []))
        # A real station on the marker hides the 10 damage emitters under
        # its intact hardpoints' Damage_Particles bones, as retail does
        # when it builds the hardpoints; only its blink light runs.
        code, station, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "station",
                                     options + ("--eawr-space-place-object", "Rebel_Star_Base_1@48"))
        self.assertEqual(code, 0, station["failure"])
        attached = station["populate"]["attached_effects"]
        self.assertEqual((attached["records"], attached["admitted"], attached["hidden_by_hardpoint_state"]),
                         (71, 33, 10))
        self.assertEqual(emitters(station, 48), ["drawn"])


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant population fault probe")
def test_supported_population_upload_failure_fails_run_with_placement(self):
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-upload-fault-") as temporary:
        code, result, _ = self._run(
            pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
            "data/art/maps/_mp_space_coruscant.ted", pathlib.Path(temporary), "upload-fault",
            ("--eawr-populate", "--eawr-space-populate-reject-upload-test"))
        self.assertNotEqual(code, 0)
        self.assertEqual(result["status"], "failed")
        self.assertIn("supported surface placement uploads failed", result["failure"])
        self.assertIn("placement 0 (Skirmish_Merchant_Dock) surface 0", result["failure"])
        self.assertIn("injected supported surface upload failure", result["failure"])


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant attached-effects run")
def test_coruscant_attached_effects_tick_in_environment_view(self):
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-effects-") as temporary:
        directory = pathlib.Path(temporary)
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        code, result, capture = self._run(
            pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
            "data/art/maps/_mp_space_coruscant.ted", directory, "effects",
            ("--eawr-populate", "--eawr-map-camera-config", str(camera),
             "--eawr-lighting", "sh", "--eawr-shadows", "on"))
        self.assertEqual(code, 0, result["failure"])
        self.assertEqual(result["status"], "space_environment_rendered")
        self.assertGreater(result["map_camera"]["steps"], 0)
        population = result["populate"]
        self.assertGreater(population["team_colour"]["colour_variants"], 0)
        self.assertGreater(population["team_colour"]["drawn_by_status"]["faction_colour"], 0)
        self.assertGreater(population["unit_animation"]["placements_animated"], 0)
        self.assertGreater(population["unit_animation"]["instances_animated"], 0)
        self.assertEqual(population["unit_animation"]["sample_at_capture"], 59)
        effects = result["map_particles"]
        self.assertGreater(effects["attached_records"], 0)
        self.assertGreater(effects["advanced_frames"], 0)
        # A fixed capture holds the effects on the frame-count clock (#186).
        self.assertEqual(effects["clock"], "held")
        self.assertTrue(any(item["status"] == "drawn" for item in effects["placements"]))
        self.assertEqual(effects["live_rids_after_release"], 0)
        self.assertEqual(effects["live_resources_after_release"], 0)
        self.assertTrue(capture.is_file())


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the authored asteroid density")
def test_coruscant_prewarmed_asteroid_density(self):
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    options = ("--eawr-populate", "--eawr-space-camera",
               "-356.6,689.44,574.609,-356.6,0,-3.9,0,1,0,55,10,60000")

    def field(report):
        return [p for p in report["map_particles"]["placements"] if "#12/proxy#" in p["identity"]]

    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-pebbles-") as temporary:
        directory = pathlib.Path(temporary)
        code, full, full_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                        directory, "full", options)
        self.assertEqual(code, 0, full["failure"])
        effects = field(full)
        self.assertEqual(len(effects), 2)
        self.assertGreaterEqual(effects[0]["particles"], 825)
        self.assertGreaterEqual(effects[1]["particles"], 395)
        self.assertEqual(full["populate"]["attached_effects"]["capacity_exhausted"], 0)
        for effect in effects:
            self.assertLess(effect["particles"], effect["capacity"])
        code, repeat, repeat_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                            directory, "repeat", options)
        self.assertEqual(code, 0, repeat["failure"])
        self.assertEqual(field(repeat), effects)
        self.assertEqual(full_png.read_bytes(), repeat_png.read_bytes())
        code, limited, limited_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
            directory, "limited", options + ("--eawr-map-attached-capacity", "256",
                                               "--eawr-map-particle-capacity", "8192"))
        self.assertEqual(code, 0, limited["failure"])
        self.assertEqual([p["capacity"] for p in field(limited)], [256, 256])
        self.assertTrue(all(p["particles"] <= 256 for p in field(limited)))
        self.assertEqual(limited["map_particles"]["aggregate_capacity"], 8192)
        self.assertNotEqual(full_png.read_bytes(), limited_png.read_bytes())


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant idle clip run")
def test_coruscant_props_tumble_through_their_idle_clips(self):
    # #145: the asteroid field and space junk idle clips play from
    # per-placement start frames at their XML playback; five seconds later
    # on the idle clock (--eawr-map-idle-offset 150) they have turned.
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    # The south-east asteroid field: mining pad, Asteroid Field Small, junk.
    camera = "1419,271,5380,1920,-150,5380,0,1,0,55,10,60000"
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-idle-") as temporary:
        directory = pathlib.Path(temporary)
        frames = {}
        for offset in (0, 150):
            code, result, capture = self._run(
                root, "data/art/maps/_mp_space_coruscant.ted", directory, f"idle-{offset}",
                ("--eawr-populate", "--eawr-space-camera", camera, "--eawr-map-idle-offset", str(offset)))
            self.assertEqual(code, 0, result["failure"])
            animation = result["populate"]["unit_animation"]
            self.assertEqual((animation["clock"], animation["sample_at_capture"], animation["idle_offset"]),
                             ("held", 59, offset))
            self.assertEqual(animation["idle_sample_failures"], 0)
            hulls = {entry["object"]: entry for entry in animation["bound_idle_hulls"]}
            self.assertEqual(hulls["Asteroid Field Small"]["clip"],
                             "data/art/models/w_asteroid_mass_small_idle_00.ala")
            self.assertTrue(hulls["Asteroid Field Small"]["loop"])
            self.assertEqual(hulls["Asteroid Field Small"]["rate_mod"], [1, 1])
            # No Loop_Idle_Anim_00: the IDLE behaviour restarts it instead.
            self.assertFalse(hulls["Space_Junk_Small"]["loop"])
            self.assertTrue(hulls["Space_Junk_Small"]["restarts"])
            self.assertTrue(all(entry["random_start"] for entry in hulls.values()))
            frames[offset] = decode_png(capture.read_bytes())
    width, height, before = frames[0]
    self.assertEqual((width, height), (1280, 720))
    _, _, after = frames[150]

    def changed(x_range, y_range):
        return sum(1 for y in y_range for x in x_range
                   if max(abs(a - b) for a, b in zip(before[y][x], after[y][x])) > 40)

    self.assertGreater(changed(range(0, width, 4), range(0, height, 4)), 500,
                       "the asteroids and junk turned between the two idle clock samples")
    # Control: empty sky above the field is the same in both captures.
    self.assertEqual(changed(range(700, 1000, 2), range(0, 60, 2)), 0)


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant nebula clock run")
def test_coruscant_nebula_moves_on_the_effect_clock(self):
    # #185: Nebula.fx moves only through TIME, a 20 s vertex wave and a UV
    # scroll. The environment view runs it on the idle clips' clock at the
    # retail 0.03 s per tick, so five seconds later on that clock
    # (--eawr-map-idle-offset 150) the nebula has moved, and one offset
    # captured twice is the same image.
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    self._nebula_moves(root, "forward_plus")


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the lit live nebula regression")
def test_coruscant_live_nebula_has_visible_coverage(self):
    # FW-19: the opaque sky must precede additive backdrop layers, even
    # though Godot puts depth_draw_never sky shaders in its alpha pass.
    # Use the owner's live camera policy with an explicit pose aimed at
    # the nebula cluster; the opening fleet pose does not frame the clouds.
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    camera = ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml"
    options = ("--eawr-populate", "--eawr-map-effects", "on",
               "--eawr-live-session", "m2", "--eawr-live-ticks", "1",
               "--eawr-live-ai", "off", "--eawr-hud", "off",
               "--eawr-environment", "map", "--eawr-lighting", "sh",
               "--eawr-shadows", "on", "--eawr-map-camera-config", str(camera),
               "--eawr-space-camera", "4000,2298,-1872,4000,0,-3800,0,1,0,55,10,60000")
    with tempfile.TemporaryDirectory(prefix="eawr-live-nebula-") as temporary:
        directory = pathlib.Path(temporary)
        for reveal in ("off", "on"):
            with self.subTest(reveal=reveal):
                code, result, capture = self._run(
                    root, "data/art/maps/_mp_space_coruscant.ted", directory, f"nebula-{reveal}",
                    options + ("--eawr-live-reveal", reveal))
                self.assertEqual(code, 0, result["failure"])
                nebulae = [item for item in result["space"]["surfaces"] if item["route"] == "nebula"]
                self.assertEqual(len(nebulae), 7)
                self.assertTrue(all(item["status"] == "drawn" for item in nebulae))
                width, height, pixels = decode_png(capture.read_bytes())
                self.assertEqual((width, height), (1280, 720))
                # The warm cloud filaments occupy this rectangle. The
                # blue starfield and foreground stations cannot satisfy
                # the coverage floor when the sky overwrites the nebula.
                coverage = sum(pixels[y][x][0] > 48 and pixels[y][x][0] > pixels[y][x][2]
                               for y in range(10, 680, 2) for x in range(250, 600, 2))
                self.assertGreater(coverage, 1000, "lit nebula filaments must contribute visible pixels")


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for other lit nebula backdrops")
def test_other_nebula_backdrops_move_in_lit_views(self):
    # Frame the authored Endor ion cluster and Bothawui's southeast cloud.
    # A successful upload is insufficient: the animated layer must change
    # pixels after the sky draw, on maps with different sky/nebula assets.
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    cases = (("endor", 3, "0,2298,1928,0,0,0,0,1,0,55,10,60000"),
             ("bothawui", 2, "4500,2298,7428,4500,0,5500,0,1,0,55,10,60000"))
    with tempfile.TemporaryDirectory(prefix="eawr-other-nebula-") as temporary:
        directory = pathlib.Path(temporary)
        for name, count, camera in cases:
            with self.subTest(map=name):
                images = []
                for offset in (0, 150):
                    code, result, capture = self._run(
                        root, f"data/art/maps/_mp_space_{name}.ted", directory, f"{name}-{offset}",
                        ("--eawr-populate", "--eawr-map-effects", "off", "--eawr-space-camera", camera,
                         "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                         "--eawr-map-idle-offset", str(offset)))
                    self.assertEqual(code, 0, result["failure"])
                    nebulae = [item for item in result["space"]["surfaces"] if item["route"] == "nebula"]
                    self.assertEqual(len(nebulae), count)
                    self.assertTrue(all(item["status"] == "drawn" for item in nebulae))
                    width, height, pixels = decode_png(capture.read_bytes())
                    self.assertEqual((width, height), (1280, 720))
                    images.append(pixels)
                changed = sum(max(abs(a - b) for a, b in zip(images[0][y][x], images[1][y][x])) > 24
                              for y in range(100, 620, 4) for x in range(200, 1080, 4))
                self.assertGreater(changed, 1000, "lit nebula motion must survive backdrop ordering")


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the galactic planet map markers")
def test_planet_map_marker_models_are_not_environment(self):
    # #3077: every galactic planet space map places a Death_Star_Position
    # marker whose editor model uses the planet shader. Markers are never
    # drawn in game; the map's own backdrop planet still is.
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    with tempfile.TemporaryDirectory(prefix="eawr-planet-marker-") as temporary:
        code, result, _ = self._run(root, "data/art/maps/_space_planet_corellia_01.ted",
                                    pathlib.Path(temporary), "corellia",
                                    ("--eawr-populate", "--eawr-map-effects", "off"))
        self.assertEqual(code, 0, result["failure"])
    surfaces = result["space"]["surfaces"]
    self.assertFalse([item for item in surfaces if item["object"].lower() == "death_star_position"])
    planets = [item for item in surfaces if item["role"].startswith("planet")]
    self.assertTrue(planets and all(item["status"] == "drawn" for item in planets), planets)


def _place_at(self, ship):
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    with tempfile.TemporaryDirectory(prefix="eawr-death-star-") as temporary:
        code, result, _ = self._run(root, "data/art/maps/_space_planet_nalhutta_01.ted",
                                    pathlib.Path(temporary), "nalhutta",
                                    ("--eawr-populate", "--eawr-map-effects", "off",
                                     "--eawr-space-place-at", ship))
        self.assertEqual(code, 0, result["failure"])
    return result


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Nal Hutta Death Star placement")
def test_background_authored_space_unit_is_drawn_at_its_pose(self):
    # #3935 (BP-75): the Death Star authors In_Background for its tactical unit too. A placed ship is
    # a live simulated object, so it is registered and drawn (the placement run fails with "no
    # supported visible hull" when it is skipped as map environment). It is counted as a backdrop
    # ship and drawn (composed surfaces) at a pose like the one the debug build gave a spawned one
    # (z -3100, below the ship plane), never the #3077 marker. The pose itself is asserted on the
    # live session (live_session_effect_cases.py); this run asserts the placed/backdrop counts only.
    result = self._place_at("Death_Star_II@-1328,-762,-3100,0")
    self.assertEqual(result["populate"]["placed_ships"], 1)
    self.assertEqual(result["populate"]["backdrop_ships"], 1)
    self.assertGreater(result["populate"]["composed"], 0, result["populate"])
    surfaces = result["space"]["surfaces"]
    self.assertTrue(surfaces)
    self.assertFalse([item for item in surfaces if item["object"].lower() == "death_star_position"])


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Nal Hutta ship placement")
def test_ordinary_space_unit_is_not_a_background_scene_object(self):
    # Negative: a ship without In_Background is a world-scene ship.
    result = self._place_at("Star_Destroyer@0,0,0,0")
    self.assertEqual(result["populate"]["placed_ships"], 1)
    self.assertEqual(result["populate"]["backdrop_ships"], 0)


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the Coruscant opening pose")
def test_coruscant_default_pose_is_the_tactical_opening_pose(self):
    # One opening rule: without a map camera config the default view
    # frames local player 1's spawn marker at the Space_Mode defaults; the
    # project config's tactical camera opens on the same target, pitch and
    # yaw, but at distance 1200 instead of FoC's Distance_Default 1000
    # (owner deviation, #337/#348). The config-less view keeps FoC's.
    root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
    camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
    with tempfile.TemporaryDirectory(prefix="eawr-coruscant-opening-") as temporary:
        directory = pathlib.Path(temporary)
        code, plain, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "default")
        self.assertEqual(code, 0, plain["failure"])
        code, tactical, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "tactical",
                                      ("--eawr-map-camera-config", str(camera)))
        self.assertEqual(code, 0, tactical["failure"])
    self.assertEqual(plain["space"]["camera"]["source"], "default")
    default = plain["space"]["camera"]["default"]
    self.assertEqual((default["target_kind"], default["target_record"], default["target_type"]),
                     ("start_marker", 55, "Team_01_Spawn_Point_Marker"))
    self.assertEqual(tactical["space"]["camera"]["source"], "space map camera (project config, Space_Mode XML)")
    plain_camera = dict(plain["capture_identity"]["camera"])
    tactical_camera = dict(tactical["capture_identity"]["camera"])
    plain_eye = plain_camera.pop("position")
    tactical_eye = tactical_camera.pop("position")
    self.assertEqual(plain_camera, tactical_camera)
    target = plain_camera["target"]
    plain_offset = [eye - aim for eye, aim in zip(plain_eye, target)]
    self.assertAlmostEqual(math.hypot(*plain_offset), 1000.0, delta=0.05)
    for axis, offset in enumerate(plain_offset):
        self.assertAlmostEqual(tactical_eye[axis] - target[axis], offset * 1.2, delta=0.05)


@unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_EAW_GAME_ROOT for the FoC corpus run")
def test_foc_reference_map_is_default_on_foc_install(self):
    pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
    entry = next(item for item in pinned["references"]
                 if item["role"] == "m1_reference" and item["checkpoint"]["kind"] == "space")
    with tempfile.TemporaryDirectory(prefix="eawr-space-foc-") as temporary:
        code, result, _ = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                    entry["logical_path"], pathlib.Path(temporary), "coruscant",
                                    ("--eawr-space-camera", fixture.CAMERA, *NONE))
        self.assertEqual(result["profile"], "foc")
        self.assertEqual(result["layers"], ["expansion", "base"])
        self.assertEqual(result["map"]["sha256"], entry["sha256"])
        self.assertEqual(result["status"], "space_blocked")
        self.assertNotEqual(code, 0)
