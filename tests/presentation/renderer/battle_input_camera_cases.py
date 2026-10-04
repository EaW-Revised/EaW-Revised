"""Cases for test_battle_input; collected by its legacy facade."""

from battle_input_test_support import (
    ACCLAMATOR, ATTACKER_POINT, BattleInputRunner, CAMERA,
    CLEAR_OF_DOGFIGHT, CORUSCANT, CORVETTE, CORVETTE_START,
    DESTINATION, DOGFIGHT_ORDER_TICK, DOGFIGHT_TICK, END_TICK,
    HUD_OFF, LAUNCH_PROBE_TICK, MC80, MC80_ASIDE,
    MEETING, NEBULON, NEBULON_START, OPTIONS_HIT,
    OTHER_TIE_SQUADRON, ROOT, SPOTTER_POINT, SPREAD,
    STAR_BASE, STAR_BASE_POSITION, TARGET_POINT, TARTAN,
    TIE_CRAFT, TIE_SQUADRON, VIEWPORT, X_WING_3_CRAFT,
    X_WING_SQUADRON, X_WING_SQUADRON_3, Y_WINGS, Y_WING_ASIDE,
    Y_WING_SQUADRON, collections, contextlib, decode_png,
    hashlib, inside, json, math,
    os, pathlib, re, read,
    screen, shutil, source_text, strict_json,
    subprocess, sys, tempfile, unittest,
)


class BattleInputCameraCases:
    def test_minimap_empty_icon_points_and_conditional_team_scale(self):
        # MM-16/17: override only radar presentation on the stock model-free Team.
        import xml.etree.ElementTree as ET
        sys.path.insert(0, str(ROOT))
        from tools.inventory.corpus import Corpus
        with tempfile.TemporaryDirectory(prefix="eawr-minimap-style-") as temporary:
            directory = pathlib.Path(temporary)
            source = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/containers.xml")
            self.assertIsNotNone(source)
            common = ("--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on", *SPREAD,
                "--eawr-live-input", f"40:click:icon={X_WING_SQUADRON}")
            code, baseline = self._run(directory, "fixed", common, end_tick=80)
            self.assertEqual(code, 0, baseline.get("failure"))
            fixed = next(row for row in baseline["hud"]["minimap"]["drawn"] if row["id"] == X_WING_SQUADRON)
            self.assertEqual(fixed["icon"], "i_radar_default_blip.tga")
            self.assertAlmostEqual(fixed["half_size"][0], 0.05, places=6)
            for name, tags in (("point", {"Radar_Icon_Name": "", "Radar_Blip_Size": "2.9"}),
                               ("scaled", {"Radar_Draw_To_Scale": "Yes", "Radar_Icon_Scale_Space": "200"})):
                mod = directory / name
                xml = mod / "Data/XML/containers.xml"
                xml.parent.mkdir(parents=True)
                (mod / "Data/MegaFiles.xml").write_text(
                    '<Mega_Files><File>Missing.meg</File></Mega_Files>', encoding="utf-8")
                tree = ET.fromstring(source.data)
                team = next(node for node in tree if node.attrib.get("Name", "").lower() == "team")
                for key, value in tags.items():
                    for child in list(team):
                        if child.tag.lower() == key.lower(): team.remove(child)
                    ET.SubElement(team, key).text = value
                xml.write_bytes(ET.tostring(tree, encoding="utf-8"))
                code, result = self._run(directory, name, (*common, "--eawr-mod-root", str(mod)), end_tick=80)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                self.assertEqual(result["live_session"]["final_state_sha256"], baseline["live_session"]["final_state_sha256"])
                radar = result["hud"]["minimap"]
                draw = next(row for row in radar["drawn"] if row["id"] == X_WING_SQUADRON)
                self.assertEqual(draw["at"], fixed["at"])
                self.assertEqual(draw["colour"], [209, 255, 209, 255])
                self.assertEqual(radar["icons_missing"], 0)
                if name == "point":
                    self.assertEqual((draw["icon"], draw["point_pixels"]), ("", 2))
                    width, height, pixels = decode_png((directory / f"{name}.png").read_bytes())
                    x, y, rw, rh = radar["rect"]
                    column = math.trunc((draw["at"][0] - x) / rw * math.ceil(rw))
                    row = math.trunc((draw["at"][1] - y) / rh * math.ceil(rh))
                    px = math.floor(x + (column + 0.5) * rw / math.ceil(rw))
                    py = math.floor(y + (row + 0.5) * rh / math.ceil(rh))
                    self.assertEqual(pixels[py][px], (209, 255, 209))
                else:
                    self.assertEqual(draw["point_pixels"], 0)
                    self.assertAlmostEqual(draw["half_size"][0], 400 / 12200, places=6)
                    self.assertAlmostEqual(draw["half_size"][1], 400 / 12200, places=6)

    def test_minimap_has_one_identity_per_live_squadron(self):
        # MM-15: idle and dispersed combat members share their container identity.
        with tempfile.TemporaryDirectory(prefix="eawr-minimap-squadrons-") as temporary:
            directory = pathlib.Path(temporary)
            common = ("--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-workers", "4")
            code, idle = self._run(directory, "idle", (*common, *SPREAD,
                "--eawr-live-input", f"40:click:icon={X_WING_SQUADRON}"), end_tick=80)
            self.assertEqual(code, 0, idle.get("failure"))
            self.assertEqual(idle["battle_input"]["selected"], [X_WING_SQUADRON])
            meeting = f"{MEETING[0]},{MEETING[1]},0"
            clear = f"{CLEAR_OF_DOGFIGHT[0]},{CLEAR_OF_DOGFIGHT[1]},0"
            code, combat = self._run(directory, "combat", (*common,
                "--eawr-live-order", f"1:move:{X_WING_SQUADRON}@{meeting}",
                "--eawr-live-order", f"1:move:{TIE_SQUADRON}@{meeting}",
                "--eawr-live-order", f"1:move:{OTHER_TIE_SQUADRON}@{clear}",
                "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{X_WING_SQUADRON}@{TIE_SQUADRON}",
                "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{TIE_SQUADRON}@{X_WING_SQUADRON}"),
                end_tick=DOGFIGHT_TICK)
            self.assertEqual(code, 0, combat.get("failure"))
            for result in (idle, combat):
                radar = result["hud"]["minimap"]
                drawn = {row["id"]: row for row in radar["drawn"]}
                self.assertEqual(len(drawn), len(radar["drawn"]), radar)
                members = {member for group in result["live_session"]["squadrons"] for member in group["members"]}
                self.assertFalse(members.intersection(drawn), radar)
                self.assertIn(X_WING_SQUADRON, drawn)
                group = next(row for row in result["live_session"]["squadrons"] if row["container"] == X_WING_SQUADRON)
                own = {row["entity"]: row for row in result["live_session"]["own_units"]}
                live = [own[member] for member in group["members"] if member in own]
                self.assertTrue(live)
                mean = [sum(row["position"][axis] for row in live) / len(live) for axis in (0, 1)]
                x, y, width, height = radar["rect"]
                expected = (x + (mean[0] + 6100) / 12200 * width, y + (6100 - mean[1]) / 12200 * height)
                self.assertLess(math.dist(drawn[X_WING_SQUADRON]["at"], expected), 0.3)
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
            self.assertEqual(next(row for row in idle["hud"]["minimap"]["drawn"]
                                  if row["id"] == X_WING_SQUADRON)["colour"], [209, 255, 209, 255])

    def test_authored_space_overview_clicks_reach_both_levels(self):
        # Start at Distance_Max so every scripted wheel event is an outward overview click.
        # The map and live XML configs both enter EnvironmentView::ready; removing its
        # overview-click override leaves FoC's 10 and fails the first transition.
        stages = ((10, 4, "off"), (20, 1, "overview"),
                  (30, 4, "overview"), (40, 1, "map"))
        wheels = tuple(part for tick, count, _ in stages
                       for _ in range(count) for part in ("--eawr-live-input", f"{tick}:wheel:out"))
        with tempfile.TemporaryDirectory(prefix="eawr-overview-clicks-") as temporary:
            directory = pathlib.Path(temporary)
            for name, camera in (("map", ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"),
                                 ("live", CAMERA)):
                with self.subTest(config=name):
                    code, result = self._run(directory, name,
                                             (*HUD_OFF, "--eawr-camera-zoom", "1", *wheels),
                                             # Tick-scripted clicks must share a fixed camera timer,
                                             # not expire between stages when the GPU draws slowly.
                                             camera=camera, end_tick=60, engine_args=("--fixed-fps", "60"))
                    self.assertEqual(code, 0, result.get("failure"))
                    self.assertEqual(result["map_camera"]["config_sha256"],
                                     hashlib.sha256(camera.read_bytes()).hexdigest())
                    self.assertEqual(result["battle_input"]["scripted_fired"], 10)
                    self.assertEqual([{"tick": sample["tick"], "level": sample["level"]}
                                      for sample in result["battle_input"]["overview_samples"]],
                                     [{"tick": tick, "level": level} for tick, _, level in stages])
                    self.assertEqual(result["battle_input"]["overview"], "map")
                    overview = result["map_camera"]["overview_clicks"]
                    self.assertEqual((overview["value"], overview["replaced_value"]), (5, 10))
                    self.assertEqual(overview["override"]["source_id"], "space-camera-owner-deviations")
                    self.assertEqual(overview["replaced"]["definition"], "Space_Mode")


    def test_overview_levels_hide_the_battle_ui(self):
        # #848 (foc-battle-selection.md V-5a to V-5g), HUD on: a box selects the spawn stack, then the
        # overview key enters x1, x2 and returns, once running and once paused. Each sample is the
        # first frame drawn at the new level. x1 and x2 hide the tactical shell, the pause banner and
        # the unit brackets and stop the minimap; the selection circles and squadron icons stay; every
        # level change lays the last old frame over the next nine drawn frames from 0.9 down.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        gestures = [f"f10:box:{screen(box[0])}/{screen(box[1])}",
                    "f20:key:Insert", "f40:key:Insert", "f60:key:Insert",
                    "f70:click:hud=pause",
                    "f80:key:Insert", "f100:key:Insert", "f120:key:Insert",
                    "f130:click:hud=resume"]
        with tempfile.TemporaryDirectory(prefix="eawr-battle-overview-ui-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "overview_ui",
                                     tuple(part for gesture in gestures for part in ("--eawr-live-input", gesture)))
            self.assertEqual(code, 0, result.get("failure"))
            battle = result["battle_input"]
            self.assertEqual(battle["scripted_fired"], len(gestures), battle["log"])
            samples = battle["overview_samples"]
            self.assertEqual([sample["level"] for sample in samples],
                             ["overview", "map", "off", "overview", "map", "off"], samples)
            for index, sample in enumerate(samples):
                on = sample["level"] != "off"
                paused = index >= 3
                with self.subTest(sample=index, level=sample["level"], paused=paused):
                    hud = sample["hud"]
                    self.assertEqual(hud["shell"], not on)                      # V-5b
                    self.assertEqual(hud["pause_banner"], paused and not on)    # V-5b
                    self.assertEqual(sample["health_bars"] == 0, on)            # V-5d
                    self.assertEqual(sample["shield_bars"] if on else 0, 0)     # V-5d
                    self.assertGreater(sample["circles"], 0)                    # V-5e
                    self.assertGreater(sample["icons"], 0)                      # V-5e
                    self.assertAlmostEqual(hud["fade_opacity"], 0.9, places=5)  # V-5g
            # V-5c: no minimap update while x1 or x2 is on; they resume back at the tactical camera.
            syncs = [sample["hud"]["minimap_syncs"] for sample in samples]
            self.assertEqual(syncs[0], syncs[1])
            self.assertEqual(syncs[1], syncs[2])
            self.assertGreater(syncs[3], syncs[2])
            self.assertEqual(syncs[3], syncs[4])
            overview = result["overview_ui"]
            self.assertEqual(overview["level"], "off")
            self.assertEqual(overview["distance_fog"], "not drawn")  # V-5f: nothing to turn off
            fade = overview["fade"]
            self.assertEqual(fade["frames_per_fade"], 9)
            self.assertEqual(fade["requests"], 6)
            self.assertEqual(fade["drawn_frames"], 6 * 9)
            self.assertEqual(fade["held_images"], 6, fade)
            self.assertEqual(fade["capture"], "gpu copy")
            self.assertTrue(result["hud"]["shell_shown"])
            self.assertFalse(result["hud"]["overview"])


    def test_middle_button_law_on_the_battle_camera(self):
        # The FoC middle-button law (#328) through the battle's whole input path, as a hand makes
        # it: Ctrl's own key press and auto-repeat, the button held, motion in steps, the releases,
        # all through Godot's input queue, past the battle layer, into the camera with its tactical
        # overview (the map views' camera, which the camera self-tests leave out).
        def pose(sample):
            eye, target = sample["eye"], sample["target"]
            dx, dy, dz = (eye[i] - target[i] for i in range(3))
            flat = math.hypot(dx, dz)
            return {"yaw": math.degrees(math.atan2(dx, dz)), "pitch": math.degrees(math.atan2(dy, flat)),
                    "target": target, "distance": math.hypot(flat, dy)}

        def turned(before, after):
            return abs((after["yaw"] - before["yaw"] + 180.0) % 360.0 - 180.0)

        with tempfile.TemporaryDirectory(prefix="eawr-battle-camera-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "camera", (
                "--eawr-live-input", "20:mdrag:200,0",
                "--eawr-live-input", "40:mdrag:300,0+ctrl",
                "--eawr-live-input", "55:mclick:centre+ctrl",
                "--eawr-live-input", "70:mdrag:0,-150+ctrl",
                "--eawr-live-input", "90:mclick:centre"))
            self.assertEqual(code, 0, result.get("failure"))
            battle = result["battle_input"]
            self.assertEqual(battle["scripted_fired"], 5, battle["log"])
            self.assertEqual(battle["orders"], 0, battle["log"])
            samples = {sample["label"].split(" at tick ")[0].replace("scripted ", "") + "@"
                       + sample["label"].split(" at tick ")[1]: pose(sample) for sample in battle["camera_samples"]}
            self.assertEqual(len(samples), 10, battle["camera_samples"])
            start = samples["mdrag@20 before"]
            pan = samples["mdrag@20 after"]
            # A plain middle drag translates the target and never turns.
            self.assertGreater(math.dist(pan["target"], start["target"]), 50.0, (start, pan))
            self.assertLess(turned(start, pan), 0.01, (start, pan))
            self.assertAlmostEqual(pan["pitch"], start["pitch"], delta=0.01)
            # Ctrl + a horizontal middle drag turns about the target: 300 of 1280 pixels is 23.4
            # mouse units, 35 degrees at Space_Mode's Yaw_Per_Mouse_Unit 1.5.
            before, rotated = samples["mdrag@40 before"], samples["mdrag@40 after"]
            self.assertGreater(turned(before, rotated), 20.0, (before, rotated))
            self.assertLess(math.dist(rotated["target"], before["target"]), 0.5, (before, rotated))
            self.assertAlmostEqual(rotated["distance"], before["distance"], delta=0.5)
            # A Ctrl click keeps the view.
            kept = samples["mclick@55 after"]
            self.assertLess(turned(rotated, kept), 0.01, (rotated, kept))
            self.assertAlmostEqual(kept["pitch"], rotated["pitch"], delta=0.01)
            # Ctrl + a vertical middle drag tilts and does not turn.
            before, tilted = samples["mdrag@70 before"], samples["mdrag@70 after"]
            self.assertGreater(abs(tilted["pitch"] - before["pitch"]), 5.0, (before, tilted))
            self.assertLess(turned(before, tilted), 0.01, (before, tilted))
            # A plain middle click resets turn and tilt and keeps the target.
            before, reset = samples["mclick@90 before"], samples["mclick@90 after"]
            self.assertLess(turned(start, reset), 0.01, (start, reset))
            self.assertAlmostEqual(reset["pitch"], start["pitch"], delta=0.05)
            self.assertLess(math.dist(reset["target"], before["target"]), 0.5, (before, reset))


    def test_ctrl_tilt_passes_foc_floor_and_stops_at_minus_60(self):
        # #390 (owner): the live battle camera tilts down to -60 instead of Space_Mode's Pitch_Min -10,
        # through the same input path as above. Each Ctrl drag of 300 px up is 41.7 mouse units, 62.5
        # degrees at -1.5 per unit: from 50 the first ends at -12.5 (under FoC's floor), the second
        # stops at -60. A plain drag then pans in the plane at that depth; a click resets to 50.
        def pitch(sample):
            eye, target = sample["eye"], sample["target"]
            dx, dy, dz = (eye[i] - target[i] for i in range(3))
            return math.degrees(math.atan2(dy, math.hypot(dx, dz)))

        with tempfile.TemporaryDirectory(prefix="eawr-battle-floor-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "floor", (
                "--eawr-live-input", "20:mdrag:0,-300+ctrl",
                "--eawr-live-input", "40:mdrag:0,-300+ctrl",
                "--eawr-live-input", "60:mdrag:0,200",
                "--eawr-live-input", "80:mclick:centre"))
            self.assertEqual(code, 0, result.get("failure"))
            battle = result["battle_input"]
            self.assertEqual(battle["scripted_fired"], 4, battle["log"])
            samples = {sample["label"].split(" at tick ")[0].replace("scripted ", "") + "@"
                       + sample["label"].split(" at tick ")[1]: sample for sample in battle["camera_samples"]}
            self.assertAlmostEqual(pitch(samples["mdrag@20 before"]), 50.0, delta=0.05)
            self.assertAlmostEqual(pitch(samples["mdrag@20 after"]), -12.5, delta=0.2)
            self.assertAlmostEqual(pitch(samples["mdrag@40 after"]), -60.0, delta=0.05)
            before, panned = samples["mdrag@60 before"], samples["mdrag@60 after"]
            self.assertAlmostEqual(pitch(panned), -60.0, delta=0.05)
            self.assertAlmostEqual(panned["eye"][1], before["eye"][1], delta=0.01)
            self.assertGreater(math.dist(panned["target"], before["target"]), 50.0, (before, panned))
            self.assertAlmostEqual(pitch(samples["mclick@80 after"]), 50.0, delta=0.05)


    def test_hud_takes_the_bar_and_leaves_the_battle(self):
        # The HUD on (the live default): a click on the options button stays with the HUD, while a box
        # dragged corner to corner through open space and a right click on open space reach the
        # battle. Every gesture is a viewport pixel, so none depends on where the camera starts.
        width, height = VIEWPORT
        options = (OPTIONS_HIT[0] + OPTIONS_HIT[2] / 2, OPTIONS_HIT[1] + OPTIONS_HIT[3] / 2)
        # Inset from the viewport's corners past the edge-scroll band; the box covers the stack
        # wherever the start camera frames it.
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        target = (0.6 * width, 0.35 * height)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-hud-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "hud", (
                "--eawr-live-input", f"10:click:{screen(options)}",
                "--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}",
                "--eawr-live-input", f"30:rclick:{screen(target)}",
                "--eawr-live-capture-ticks", "12,25,80"))
            self.assertEqual(code, 0, result.get("failure"))
            hud = result["hud"]
            self.assertEqual(hud["mode"], "tactical")
            self.assertEqual(hud["viewport"], list(VIEWPORT))
            # The click is on the options button by construction, and the open-space gestures miss
            # every component the HUD reports.
            self.assertTrue(inside(options, hud["options_hit_rect"]), hud["options_hit_rect"])
            components = [hud["options_rect"], hud["minimap_rect"], hud["planet_rect"],
                          *(entry["rect"] for entry in hud["panel_buttons"])]
            for point in (*box, target):
                self.assertFalse([rect for rect in components if inside(point, rect)], point)
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 3, battle["log"])
            points = battle["scripted_points"]
            self.assertEqual([point["kind"] for point in points], ["click", "box", "rclick"], points)
            for value, expected in zip(points[0]["at"], options):
                self.assertAlmostEqual(value, expected, places=2)
            # The HUD's button took the click; only the box's press and release and the right
            # click's reached the world layer.
            self.assertEqual(hud["options_presses"], 1)
            self.assertEqual(battle["events"], 4, battle["log"])
            self.assertFalse([line for line in battle["log"] if line.startswith("click")], battle["log"])
            self.assertEqual(battle["boxes"], 1, battle["log"])
            self.assertIn(CORVETTE, battle["selected"], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            order = next(line for line in battle["log"] if line.startswith("move @"))
            match = re.fullmatch(r"move @(\S+) units (\S+) tick (\d+)", order)
            self.assertTrue(match, order)
            self.assertEqual(sorted(int(unit) for unit in match.group(2).split(",")), sorted(battle["selected"]))
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            # The corvette left the stack toward the battle-plane point under the right click.
            destination = tuple(float(value) for value in match.group(1).split(",")[:2])
            position = self._position(result, CORVETTE)
            self.assertLess(math.dist(destination, position[:2]), math.dist(destination, CORVETTE_START), position)


    def test_selected_ship_bars_and_hovered_hardpoints(self):
        # #424: a selected Nebulon-B lies on one circle and shows its shield bar over its health bar;
        # hovering it shows a reticle per targetable standing hardpoint (WU-30), one fewer once a
        # hardpoint is destroyed, and a damaged ship's bars show the lost shield and hull. The hull
        # damage is the hardpoints' (hardpoint 1 destroyed, 0 and 2 damaged but standing).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-world-ui-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "nebulon", (
                *HUD_OFF, *SPREAD, *MC80_ASIDE, "--eawr-live-order", "1:move:4@-5500,4400,0",
                "--eawr-live-order", f"200:damage:{NEBULON}@1100",
                "--eawr-live-order", f"201:damage:{NEBULON}@200,0",
                "--eawr-live-order", f"202:damage:{NEBULON}@1000,1",
                "--eawr-live-order", f"203:damage:{NEBULON}@330,2",
                "--eawr-live-input", f"250:click:unit={NEBULON}",
                "--eawr-live-input", f"260:hover:unit={NEBULON}"), end_tick=270)
            self.assertEqual(code, 0, result.get("failure"))
            battle, world = result["battle_input"], result["world_ui"]
            self.assertEqual(battle["selected"], [NEBULON], battle["log"])
            self.assertEqual(battle["hovered"], NEBULON, battle)
            self.assertEqual(world["circles"], 1, world)
            self.assertEqual(len(world["bar_rows"]), 1, world)
            row = re.fullmatch(rf"{NEBULON}:s(\d+)h(\d+)", world["bar_rows"][0])
            self.assertTrue(row, world["bar_rows"])
            self.assertLess(int(row.group(1)), 10, world["bar_rows"])
            self.assertLess(int(row.group(2)), 10, world["bar_rows"])
            # The Nebulon-B's five hardpoints are targetable; the destroyed one loses its reticle.
            self.assertEqual(world["reticles"], 4, world)


    def test_reticles_keep_their_screen_size_at_every_camera_distance(self):
        # #515: a hovered Nebulon-B's reticles are 0.03 of the screen wide and 0.04 of it high
        # (38.4 x 28.8 pixels at 1280 x 720) with the camera fully in (distance 100) and fully out
        # (1900): the size is a share of the screen, not of the ship (foc-battle-world-ui WU-31).
        camera_text = read("apps/viewer/project/config/coruscant-live-session-camera.xml")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-reticle-size-") as temporary:
            directory = pathlib.Path(temporary)
            (directory / "space-live-camera-bindings.json").write_text(
                read("apps/viewer/project/config/space-live-camera-bindings.json"), encoding="utf-8")
            for name, zoom in (("near", "0"), ("far", "1")):
                camera = directory / f"{name}.xml"
                camera.write_text(re.sub(r'zoom="[0-9.]+"', f'zoom="{zoom}"', camera_text, count=1),
                                  encoding="utf-8")
                code, result = self._run(directory, f"reticle-{name}", (
                    *HUD_OFF, *SPREAD, *MC80_ASIDE, *Y_WING_ASIDE, "--eawr-live-follow-group", f"1:{NEBULON}",
                    "--eawr-live-input", f"250:hover:unit={NEBULON}"), camera=camera, end_tick=270)
                self.assertEqual(code, 0, result.get("failure"))
                battle, world = result["battle_input"], result["world_ui"]
                self.assertEqual(battle["hovered"], NEBULON, battle)
                self.assertGreater(world["reticles"], 0, world)
                self.assertAlmostEqual(world["reticle_size"][0], 38.4, delta=0.01, msg=world)
                self.assertAlmostEqual(world["reticle_size"][1], 28.8, delta=0.01, msg=world)


    def test_minimap_upper_outline_stops_at_the_far_face(self):
        # MM-09, #505: shallow views either miss the radar plane or meet it beyond
        # the rendered far plane. The outline must use its face, not a radial endpoint
        # or an unlimited corner ray. The bottom ray still meets the plane.
        with tempfile.TemporaryDirectory(prefix="eawr-minimap-far-guide-") as temporary:
            directory = pathlib.Path(temporary)
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            for pitch in (12, 21.5):
                # Keep valid orbit bounds; the initial zoom clamps to Pitch_Max.
                config = re.sub(r'<override tag="Pitch_Min" value="[^"]*"/>',
                                f'<override tag="Pitch_Min" value="{pitch - 1}"/>', CAMERA.read_text())
                config = config.replace('</constant_overrides>',
                    f'<override tag="Pitch_Max" value="{pitch}"/>'
                    '</constant_overrides>')
                camera_path = directory / f"pitch-{pitch}.xml"
                camera_path.write_text(config, encoding="utf-8")
                code, result = self._run(directory, f"pitch-{pitch}",
                                         ("--eawr-live-ai", "off"), camera=camera_path, end_tick=1)
                self.assertEqual(code, 0, result.get("failure"))
                camera = result["capture_identity"]["camera"]
                eye = camera["position"]
                forward = [camera["target"][i] - eye[i] for i in range(3)]
                length = math.sqrt(sum(value * value for value in forward))
                forward = [value / length for value in forward]
                tan_half = math.tan(math.radians(camera["fov_degrees"]) / 2)
                minimap = result["hud"]["minimap"]
                x, y, width, height = minimap["rect"]
                # The authored camera bounds are the Coruscant square, +/-6100.
                points = [(12200 * (point[0] - x) / width - 6100,
                           6100 - 12200 * (point[1] - y) / height)
                          for point in minimap["guide"]]
                self.assertEqual(len(points), 4, minimap)
                # At yaw zero the far face's two vertical edges have fixed X,
                # independent of the radar plane's reference height.
                half_width = camera["far"] * tan_half * VIEWPORT[0] / VIEWPORT[1]
                self.assertAlmostEqual(points[0][0], eye[0] - half_width, delta=0.3)
                self.assertAlmostEqual(points[1][0], eye[0] + half_width, delta=0.3)
                # Recover the plane height from the lower-left ray, then verify
                # both upper points have far-plane depth, rather than ray length.
                up = [0, -forward[2], forward[1]]
                bottom_direction = [forward[i] - up[i] * tan_half for i in range(3)]
                bottom_depth = (points[3][1] + eye[2]) / -bottom_direction[2]
                plane_height = eye[1] + bottom_depth * bottom_direction[1]
                self.assertGreater(bottom_depth, camera["near"])
                self.assertLess(bottom_depth, camera["far"])
                upper_direction = [forward[i] + up[i] * tan_half for i in range(3)]
                upper_depth = (plane_height - eye[1]) / upper_direction[1]
                if pitch == 12:
                    self.assertLess(upper_depth, 0)
                else:
                    self.assertGreater(upper_depth, camera["far"])
                for world_x, world_y in points[:2]:
                    offset = (world_x - eye[0], plane_height - eye[1], -world_y - eye[2])
                    depth = sum(offset[i] * forward[i] for i in range(3))
                    self.assertAlmostEqual(depth, camera["far"], delta=0.3)

    def test_minimap_shows_the_battle_and_moves_the_camera(self):
        # #455 (docs/behaviour/foc-minimap.md): the minimap draws the units the local player sees in
        # their colours, the camera's outline and the fog layer, nine rows a frame; a left press
        # points the camera there, a left drag past 12 pixels follows the pointer, a right click
        # orders the selection there. The camera moves are presentation only.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-minimap-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "minimap", end_tick=40)
            self.assertEqual(code, 0, shown.get("failure"))
            hud = shown["hud"]
            minimap = hud["minimap"]
            self.assertTrue(minimap["backdrop_drawn"], minimap)
            self.assertGreater(minimap["blips"], 0, minimap)
            self.assertEqual(minimap["icons_missing"], 0, minimap)
            x, y, width, height = minimap["rect"]
            # MM-10: one fog texel per minimap pixel, rebuilt in passes of nine rows a frame.
            self.assertEqual((minimap["fog"]["width"], minimap["fog"]["height"]),
                             (math.ceil(width), math.ceil(height)), minimap)
            self.assertGreaterEqual(minimap["fog"]["passes"], 1, minimap)
            self.assertEqual(hud["minimap_fog"]["rows_per_frame"], 9)
            self.assertGreater(hud["minimap_fog"]["fogged"], 0, hud["minimap_fog"])
            self.assertLess(hud["minimap_fog"]["fogged"], math.ceil(width) * math.ceil(height), hud["minimap_fog"])
            # MM-07: the Rebel start's blips carry the Rebel lobby colour; every blip sits in the minimap.
            for blip in minimap["drawn"]:
                self.assertTrue(inside(blip["at"], minimap["rect"]), blip)
            self.assertGreaterEqual(len({tuple(blip["colour"]) for blip in minimap["drawn"]}), 1)
            # MM-09: the camera's outline has four corners.
            self.assertEqual(len(minimap["guide"]), 4, minimap)

            def guide_centre(result):
                corners = result["hud"]["minimap"]["guide"]
                return (sum(point[0] for point in corners) / 4.0, sum(point[1] for point in corners) / 4.0)

            def minimap_pixel(point):
                return (x + (point[0] + 1.0) / 2.0 * width, y + (1.0 - point[1]) / 2.0 * height)

            target = (0.5, -0.5)
            code, looked = self._run(directory, "minimap-look", (
                "--eawr-live-input", f"20:click:minimap={target[0]},{target[1]}"), end_tick=40)
            self.assertEqual(code, 0, looked.get("failure"))
            self.assertEqual(looked["hud"]["minimap"]["looks"], 1, looked["hud"]["minimap"])
            # The camera now looks at the point: the outline's centre moved next to it.
            centre = guide_centre(looked)
            wanted = minimap_pixel(target)
            self.assertLess(math.dist(centre, wanted), 0.2 * width, (centre, wanted))
            self.assertGreater(math.dist(guide_centre(shown), wanted), 0.2 * width)
            # A camera move is presentation only.
            self.assertEqual(looked["live_session"]["final_state_sha256"], shown["live_session"]["final_state_sha256"])
            self.assertEqual(looked["battle_input"]["selected"], shown["battle_input"]["selected"])

            code, dragged = self._run(directory, "minimap-drag", (
                "--eawr-live-input", "20:box:minimap=-0.5,0.5/minimap=0.5,0.5"), end_tick=40)
            self.assertEqual(code, 0, dragged.get("failure"))
            drag = dragged["hud"]["minimap"]
            self.assertEqual((drag["looks"], drag["drags"]), (2, 1), drag)
            self.assertLess(math.dist(guide_centre(dragged), minimap_pixel((0.5, 0.5))), 0.2 * width)
            # The world layer never saw the minimap drag: nothing was box-selected.
            self.assertEqual(dragged["battle_input"]["selected"], shown["battle_input"]["selected"])

            width_px, height_px = VIEWPORT
            box = ((0.05 * width_px, 0.05 * height_px), (0.95 * width_px, 0.95 * height_px))
            code, moved = self._run(directory, "minimap-move", (
                "--eawr-live-input", f"10:box:{screen(box[0])}/{screen(box[1])}",
                "--eawr-live-input", "20:rclick:minimap=0.0,0.0"), end_tick=40)
            self.assertEqual(code, 0, moved.get("failure"))
            self.assertEqual(moved["hud"]["minimap"]["moves"], 1, moved["hud"]["minimap"])
            self.assertTrue(any("minimap move @" in line for line in moved["battle_input"]["log"]), moved["battle_input"]["log"])
