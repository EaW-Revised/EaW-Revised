"""Genuine campaign admission, borrowed planet map and released child presentation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from .campaign_space_cases import qualified_campaign_arguments
    from .prepared_campaign_view_cases import PreparedCampaignViewContracts
except ImportError:
    import test_viewer_planet_hud_runtime as support
    from campaign_space_cases import qualified_campaign_arguments
    from prepared_campaign_view_cases import PreparedCampaignViewContracts
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.inventory.corpus import Corpus


def faction_colours():
    """Factions.xml `Color` and `No_Colorization_Color` RGB per lower-case faction name."""
    corpus, colours = Corpus(support.GAME_ROOT), {}
    for filename in ("factions.xml", "expansion_factions.xml"):
        source = corpus.read_effective("foc", "data/xml/" + filename)
        if source is None:
            continue
        for faction in ET.fromstring(source.data):
            row = colours.setdefault(faction.attrib.get("Name", "").lower(), {})
            for tag in ("Color", "No_Colorization_Color"):
                node = faction.find(tag)
                if node is not None and node.text:
                    row[tag] = [int(part) for part in node.text.replace(",", " ").split()[:3]]
    return colours


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class CampaignSpaceCycle(unittest.TestCase):
    def test_campaign_reserve_drag_ghost_and_wheel(self):
        # Ordinary campaign Move/Merge/Fight admission; all deployment gestures use Godot input.
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-reserve-preview-"))
        # A normal fleet pathfinder gesture below admits one ship and leaves room
        # for identity reserves, without a debug spawn or tactical purchase menu.
        import copy
        corpus = Corpus(support.GAME_ROOT)
        campaigns = ET.fromstring(corpus.read_effective("foc", "data/xml/campaigns_underworld_gc.xml").data)
        campaign = copy.deepcopy(next(row for row in campaigns if row.attrib.get("Name") == "Sandbox_Gateways_Rebel"))
        campaign.set("Name", "EAWR_Reserve_Preview_Proof")
        for row in list(campaign):
            if row.tag in {"Starting_Forces", "AI_Player_Control", "Special_Case_Production", "Campaign_Set"} or row.tag.endswith("_Story_Name"):
                campaign.remove(row)
        for faction in ("Rebel", "Empire", "Underworld"):
            ET.SubElement(campaign, "AI_Player_Control").text = faction + ", None"
        forces = [("Rebel", "Bespin", "Rebel_Star_Base_1"), ("Empire", "Byss", "Empire_Star_Base_1"),
                  ("Empire", "Byss", "Tartan_Patrol_Cruiser")] + [("Rebel", "Bespin", "Corellian_Corvette")] * 3
        for faction, planet, unit in forces:
            ET.SubElement(campaign, "Starting_Forces").text = f"{faction}, {planet}, {unit}"
        campaigns.append(campaign)
        mod = directory / "campaign"
        xml = mod / "Data/XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        ET.ElementTree(campaigns).write(xml / "Campaigns_Underworld_GC.xml", encoding="utf-8")
        arguments = qualified_campaign_arguments("win")
        arguments[1] = "EAWR_Reserve_Preview_Proof"
        arguments += ["--eawr-mod-root", str(mod), "--eawr-galactic-focus", "4", "--eawr-galactic-flight-ms", "0"]
        def run(label, reinforce=None, reserve=None):
            target = directory / label
            target.mkdir()
            driver = target / "input.gd"
            actions = []
            if reinforce is not None:
                def centre(rect):
                    x, y, w, h = rect
                    return [x + w / 2, y + h / 2]
                actions = [[0.5, "click", centre(reinforce)]]
            if reserve is not None:
                actions += [[1.0, "press", centre(reserve)],
                           [1.25, "motion", [640, 250]], [1.8, "capture", "before"],
                           [2.0, "wheel", [640, 250]], [2.5, "capture", "rotated"],
                           [2.7, "cancel", [640, 250]], [3.2, "capture", "cancelled"]]
            driver.write_text('''extends SceneTree
var age = 0.0
var index = 0
var actions = %s
var fleet_stage = 0
var source = Vector2.ZERO
var target = Vector2.ZERO
var pathfinder_seen = false
var ready_msec = 0
var viewport: SubViewport
func _initialize():
    # Leave wall-clock time for native discovery/focus and fleet input before frame-20 Move.
    Engine.max_fps = 1
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(instance)
    viewport.notify_mouse_entered()
    current_scene = viewport
    ready_msec = Time.get_ticks_msec()
func pointer(point, down, moving):
    var event
    if moving:
        event = InputEventMouseMotion.new()
        event.relative = Vector2(24, 0)
        event.button_mask = MOUSE_BUTTON_MASK_LEFT if down else 0
    else:
        event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
        event.double_click = fleet_stage == 0
    event.position = point
    event.global_position = point
    viewport.push_input(event, true)
func _process(delta):
    var slot = root.find_child("q1_0000", true, false)
    var ship = root.find_child("q1_0001", true, false)
    # Discovery can invalidate the command-line focus on the first publication.
    # Re-select the drawn planet through native world input before organizing its fleet.
    if fleet_stage == 0 and (slot == null or not slot.is_visible_in_tree()):
        var planet = root.find_child("planet_name_4", true, false)
        if planet != null and planet.is_visible_in_tree():
            var label = planet.get_global_rect()
            var point = Vector2(label.position.x - 28, label.get_center().y)
            pointer(point, true, false)
            pointer(point, false, false)
    if slot != null and slot.is_visible_in_tree() and ship != null and ship.is_visible_in_tree() and Time.get_ticks_msec() - ready_msec > 2000:
        pathfinder_seen = pathfinder_seen or slot.get_meta("pathfinder_member", 0) > 0
        fleet_stage += 1
        if fleet_stage == 1:
            source = ship.get_global_rect().get_center()
            target = slot.get_global_rect().get_center()
            pointer(source, false, true)
        if fleet_stage == 3: pointer(source, true, false)
        if fleet_stage == 5: pointer(source + Vector2(24, 0), true, true)
        if fleet_stage == 7: pointer(target, true, true)
        if fleet_stage == 9: pointer(target, false, false)
    if root.find_child("CampaignSpaceBattleReady", true, false) == null:
        return false
    Engine.max_fps = 60
    age += min(delta, 0.1)
    if index < actions.size() and age > actions[index][0]:
        var action = actions[index]
        index += 1
        if action[1] == "capture":
            viewport.get_texture().get_image().save_png(%s + "/" + action[2] + ".png")
        elif action[1] == "motion":
            var event = InputEventMouseMotion.new()
            event.position = Vector2(action[2][0], action[2][1])
            event.global_position = event.position
            event.relative = Vector2(25, 0)
            event.button_mask = MOUSE_BUTTON_MASK_LEFT
            viewport.push_input(event, true)
        else:
            var buttons = [true, false] if action[1] != "press" else [true]
            for down in buttons:
                var event = InputEventMouseButton.new()
                event.position = Vector2(action[2][0], action[2][1])
                event.global_position = event.position
                event.button_index = MOUSE_BUTTON_WHEEL_UP if action[1] == "wheel" else (MOUSE_BUTTON_RIGHT if action[1] == "cancel" else MOUSE_BUTTON_LEFT)
                event.pressed = down
                viewport.push_input(event, true)
    if age > (0.8 if actions.is_empty() else (1.2 if actions.size() == 1 else 3.5)):
        var file = FileAccess.open(%s + "/staging.json", FileAccess.WRITE)
        file.store_string(JSON.stringify({"pathfinder": pathfinder_seen, "fleet_stage": fleet_stage, "source": [source.x, source.y], "target": [target.x, target.y]}))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(actions), json.dumps(target.as_posix()), json.dumps(target.as_posix())), encoding="utf-8")
            result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                *arguments, "--eawr-game-root", support.GAME_ROOT,
                "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
                "--eawr-report", str(target / "report.json")], cwd=support.ROOT,
                capture_output=True, text=True, timeout=1200, check=False)
            (target / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((target / "tactical.json").is_file(), result.stdout + result.stderr)
            self.assertTrue(json.loads((target / "staging.json").read_text())["pathfinder"], "native fleet drag did not publish a pathfinder")
            return json.loads((target / "tactical.json").read_text(encoding="utf-8"))
        initial = run("geometry")
        panel = initial["hud"]["production"]
        opened = run("pane", panel["reinforce_rect"])["hud"]["production"]
        slots = [slot for slot in opened["pool"] if slot["enabled"]]
        self.assertTrue(slots, opened)
        self.assertTrue(opened["pane_open"], opened)
        result = run("drag", panel["reinforce_rect"], slots[0]["rect"])
        preview = result["live_session"]["placement_preview"]
        samples = [row for row in preview["samples"] if row["clones"]]
        self.assertTrue(samples, preview)
        yaws = list(dict.fromkeys(row["facing_yaw"] for row in samples))
        self.assertEqual(len(yaws), 2, samples)
        self.assertAlmostEqual((yaws[1] - yaws[0]) % 360, 15, delta=0.01)
        self.assertIsNone(result["battle_input"]["production"]["placing"])
        # Check actual drawn tint, independently of the preview pose report.
        from PIL import Image
        before = Image.open(directory / "drag/before.png").convert("RGB")
        cancelled = Image.open(directory / "drag/cancelled.png").convert("RGB")
        constants = ET.fromstring(corpus.read_effective("foc", "data/xml/gameconstants.xml").data)
        bad = [int(value) for value in constants.findtext("ReinforcementOverlayBadColor").replace(",", " ").split()[:3]]
        dominant = max(range(3), key=lambda index: bad[index])
        coloured = 0
        # Keep the moving live flagship outside the crop; only the cursor clone can satisfy it.
        for y in range(170, 310):
            for x in range(560, 720):
                a, b = before.getpixel((x, y)), cancelled.getpixel((x, y))
                if a[dominant] > 40 and all(a[dominant] > a[i] * 1.5 for i in range(3) if i != dominant) and max(abs(a[i] - b[i]) for i in range(3)) > 25:
                    coloured += 1
        self.assertGreater(coloured, 30, "no coloured preview model near the drag cursor")
        print("campaign reserve ghost/wheel evidence:", directory)

    def test_company_created_commanders_show_on_flagship(self):
        try:
            from .campaign_space_cases import commander_campaign_arguments
        except ImportError:
            from campaign_space_cases import commander_campaign_arguments
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-commander-"))
        report = directory / "report.json"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--",
            *commander_campaign_arguments(support.GAME_ROOT, directory),
            "--eawr-game-root", support.GAME_ROOT, "--eawr-lighting", "sh",
            "--eawr-shadows", "on", "--eawr-environment", "map",
            "--eawr-report", str(report), "--eawr-capture", str(directory / "returned.png")],
            cwd=support.ROOT, capture_output=True, text=True, timeout=1200, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertTrue(data["space_cycle_proof"]["fight_enabled"])
        tactical = json.loads((directory / "tactical.json").read_text(encoding="utf-8"))
        riders = [row for row in tactical["live_session"]["carrier_riders"]
                  if row["type"] == "Generic_Fleet_Commander_Rebel"]
        self.assertEqual(len(riders), 2, tactical["live_session"]["carrier_riders"])
        self.assertEqual(len({row["carrier"] for row in riders}), 1)
        self.assertTrue(all(row["carrier_type"] == "Nebulon_B_Frigate" for row in riders), riders)
        self.assertTrue(all(row["reveal_raw"] * 2 == row["base_reveal_raw"] * 3 for row in riders), riders)
        self.assertIn("Generic_Fleet_Commander_Rebel", json.dumps(tactical["world_ui"]))
        self.assertTrue((directory / "space-cycle" / "tactical.png").is_file())
        print("commander GC lit capture:", directory / "space-cycle" / "tactical.png")

    def cycle(self, mode, resize=None):
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-space-cycle-"))
        report, capture = directory / "report.json", directory / "returned.png"
        began = time.monotonic()
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--",
            *qualified_campaign_arguments(mode),
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
            *(["--eawr-resize-in-loading", resize] if resize else []),
            "--eawr-report", str(report), "--eawr-capture", str(capture)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=1200, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        proof_status = data.get("space_cycle_proof", {})
        diagnostic = {key: proof_status.get(key) for key in
                      ("moved", "fight_enabled", "summary_seen", "retreat_requested", "retreat_started", "attack_tick")}
        diagnostic["input_refusals"] = len(proof_status.get("input_refusals", []))
        print(f"campaign {mode} cycle: {time.monotonic() - began:.3f}s, "
              f"{data.get('frames')} viewer frames, load={data.get('handoff', {}).get('load_ms')}ms, "
              f"proof={diagnostic}")
        phase_files = list((directory / "space-cycle").glob("*-phase.json"))
        if phase_files:
            phases = sorted((json.loads(path.read_text(encoding="utf-8")) for path in phase_files),
                            key=lambda phase: (phase["viewer_frame"], phase["steady_milliseconds"]))
            for previous, current in zip(phases, phases[1:]):
                milliseconds = current["steady_milliseconds"] - previous["steady_milliseconds"]
                print(f"  {previous['stage']} -> {current['stage']}: {milliseconds}ms, "
                      f"viewer_frame={current['viewer_frame']}, tactical_tick={current['tactical_tick']}")
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        proof = data["space_cycle_proof"]
        self.assertTrue(proof["moved"] and proof["fight_enabled"] and proof["summary_seen"])
        self.assertEqual(proof.get("input_refusals", []), [], "ordinary cycle input must reach its owner")
        self.assertTrue(proof["map"].lower().endswith(".ted"))
        required = {"galaxy", "fight", "loading", "tactical", "summary"}
        if mode == "win":
            self.assertTrue(proof["won"], "the combat branch must win through ordinary attack commands")
        if mode == "loss":
            self.assertFalse(proof["won"], "the original one-squadron attack must remain a genuine loss")
            self.assertEqual(proof["survivors"], 0)
            self.assertEqual(proof["original_members"], 1)
            self.assertEqual(proof["merge_orders"], 0)
        else:
            self.assertGreater(proof["merge_orders"], 0)
            self.assertEqual(proof["original_members"], 3)
        if mode == "retreat":
            required |= {"countdown", "cancelled"}
            self.assertTrue(proof["retreat_started"] and proof["retreat_cancelled"] and proof["retreat_restarted"])
            # HCB-15b (#3078): the trigger is the command bar's authored b_retreat slot, not a
            # top-left project button: enabled when eligible, flashing in the local countdown.
            button = proof["retreat_button"]
            self.assertTrue(button["enabled"] and button["flashing"] and button["idle_after_cancel"], button)
            self.assertIsNotNone(button["point"])
            self.assertLess(button["point"][0], 1280 / 2, button)
            self.assertGreater(button["point"][1], 720 / 2, button)
        self.assertEqual({Path(path).stem for path in proof["captures"]}, required)
        self.assertTrue(all(Path(path).is_file() for path in proof["captures"]))
        self.assertEqual(data["battle_dialog"]["phase"], "hidden")
        self.assertTrue(proof["acknowledged"])
        self.assertGreater(proof["summary_milliseconds"], 0)
        if mode != "loss":
            self.assertGreater(proof["survivors"], 0)
        self.assertTrue(proof["survivors_preserved"])
        self.assertTrue(data["handoff"]["requested"], "ordinary handoff remains reported after child teardown")
        self.assertEqual(len(data["handoff_cycles"]), 1)
        cycle = data["handoff_cycles"][0]
        self.assertTrue(cycle["child_expired"] and cycle["feed_expired"] and cycle["scene_expired"])
        self.assertTrue(cycle["galaxy_same_instance"])
        self.assertTrue(capture.is_file())
        tactical = json.loads((directory / "tactical.json").read_text(encoding="utf-8"))
        loading = tactical["loading_screen"]
        if resize:
            # #3270/#3272: the window resized while the loading screen was up; the loading screen's layout
            # and the tactical camera both follow the new viewport (edge scroll reads the camera's extent).
            width, height = (int(part) for part in resize.split("x"))
            self.assertEqual([data["battle_viewport"]["width"], data["battle_viewport"]["height"]], [width, height])
            self.assertGreaterEqual(data["battle_viewport"]["syncs"], 1)
            self.assertEqual(loading["layout_size"], [width, height])
        self.assertEqual(loading["dialog"], "IDD_SPACE_BATTLE_LOAD_DIALOG")
        self.assertTrue(loading["campaign"] and loading["planet"] and loading["owner"])
        planet = tactical["hud"]["planet_name"]
        self.assertEqual(planet["source"], "text", planet)
        self.assertTrue(planet["text_id"], planet)
        self.assertEqual(planet["context"], loading["planet"], planet)
        self.assertTrue(planet["text"] and loading["title"].endswith(planet["text"]), planet)
        self.assertGreater(tactical["hud"]["planet_pixels"], 0)
        self.assertGreaterEqual(loading["basic_income"], 0)
        # WGT-120: a campaign battle does not colorize by player; models take the type's or the faction's
        # No_Colorization_Color, and squadron frames the faction's Color (WU-21).
        live = tactical["live_session"]
        slots = {row["slot"]: row["faction"].lower() for row in live["start_slots"]}
        factions = faction_colours()
        rendered = [row for row in live["rendered_colours"] if row["player"] in slots]
        self.assertTrue(rendered, live["rendered_colours"])
        for row in rendered:
            self.assertIn(row["source"], {"type", "faction"}, row)
            if row["source"] == "faction":
                self.assertEqual(row["rgb"], factions[slots[row["player"]]]["No_Colorization_Color"], row)
        frames = [row for row in tactical["world_ui"]["identity_geometry"] if row["owner"] in slots]
        print(f"campaign squadron frames checked: {len(frames)}")
        for row in frames:
            self.assertEqual(row["tint"], factions[slots[row["owner"]]]["Color"], row)
        # #3150 (WBP-52): a pad's capture bar and model tint take the model colour (type, else faction), never
        # the lobby colour a campaign battle lacks (which left them white).
        pads = []
        for pad in live["pads"]:
            player = next((value for value in (pad["target"], pad["owner"]) if value in slots), None)
            if player is None:
                continue
            pads.append(pad)
            self.assertIn("capture_rgb", pad, pad)
            expected = factions[slots[player]]["No_Colorization_Color"]
            self.assertEqual(pad["capture_rgb"], expected, pad)
        print(f"campaign pads checked: {len(pads)} of {len(live['pads'])}")

    @unittest.skip("Pending qualified stock winning-matchup GPU acceptance (legacy EAWR-2645)")
    def test_viewer_gc_space_win(self):
        self.cycle("win")

    def test_viewer_gc_space_retreat(self):
        self.cycle("retreat")

    def test_viewer_gc_space_loss(self):
        self.cycle("loss")

    def test_viewer_gc_space_resize_in_loading(self):
        self.cycle("loss", resize="1600x900")


if __name__ == "__main__":
    unittest.main()
