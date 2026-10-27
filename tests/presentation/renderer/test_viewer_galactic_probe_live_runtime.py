"""Stock probe fact-finding: real mouse input, read-only UI evidence, no fixtures."""
import json
import base64
import hashlib
import struct
import pathlib
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_hero_visible_runtime as hero
from tests.presentation.renderer import test_viewer_galactic_fleet_drag_runtime as drag_support

DRIVER = r'''extends SceneTree
var viewport: SubViewport
var directory = %(directory)s
var card = %(card)s
var hit_points = %(hit_points)s
var origin = %(origin)s
var destination = %(destination)s
var origin_point = Vector2(%(origin_point)s)
var destination_point = Vector2(%(point)s)
var home_destination_point = Vector2(%(home_point)s)
var mode = %(mode)s
var probes = %(probes)s
var notes = []
var elapsed = 0.0
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280,720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load(ProjectSettings.get_setting("application/run/main_scene")).instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
    call_deferred("run")
func _process(delta):
    elapsed += delta
    if elapsed > 300.0:
        push_error("probe input driver exceeded its 300-second watchdog")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit(1)
    return false
func wait(seconds): await create_timer(seconds).timeout
func motion(point, held=false):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24,0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event,true)
func mouse(point, down, button=MOUSE_BUTTON_LEFT, twice=false):
    var event = InputEventMouseButton.new()
    event.position = point
    event.global_position = point
    event.button_index = button
    event.pressed = down
    event.double_click = twice
    viewport.push_input(event,true)
func centre(name):
    var node = viewport.find_child(name,true,false)
    if node == null or not node.is_visible_in_tree(): return null
    var point = node.get_global_rect().get_center()
    if hit_points.has(name):
        point = Vector2(hit_points[name][0],hit_points[name][1])
    if not Rect2(Vector2.ZERO,Vector2(1280,720)).has_point(point): return null
    return point
func click(point, button=MOUSE_BUTTON_LEFT, twice=false):
    if point == null:
        notes.append({"missing_click":true,"elapsed":elapsed})
        return
    motion(point)
    mouse(point,true,button,twice)
    await wait(0.08)
    mouse(point,false,button)
func click_name(name):
    notes.append({"click":name,"found":centre(name)!=null,"elapsed":elapsed})
    await click(centre(name))
func story_ok(node):
    if node is Button and node.is_visible_in_tree() and node.text == "OK":
        return node.get_global_rect().get_center()
    for child in node.get_children():
        var found = story_ok(child)
        if found != null: return found
    return null
func dismiss_story():
    var point = story_ok(viewport)
    if point != null:
        await click(point)
        await wait(0.5)
func collect(node, rows):
    if node is Control and node.is_visible_in_tree():
        var row = {"name":str(node.name),"rect":str(node.get_global_rect()),"tooltip":node.tooltip_text}
        if node is Label or node is Button:
            row["text"] = node.text
        rows.append(row)
    for child in node.get_children(): collect(child,rows)
func shot(label):
    await process_frame
    await RenderingServer.frame_post_draw
    viewport.get_texture().get_image().save_png(directory+"/"+label+".png")
    var rows = []
    collect(viewport,rows)
    notes.append({"shot":label,"elapsed":elapsed,"ui":rows})
    flush_notes()
func flush_notes():
    var file = FileAccess.open(directory+"/driver.json",FileAccess.WRITE)
    file.store_string(JSON.stringify(notes,"  "))
    file.close()
# Observe the player's real F6 save; never call a game method or alter saved bytes.
# Header offsets: docs/checkpoint-format.md. IntelligenceSource wire fields:
# checkpoint_fields_internal.hpp (u64 source/ability/string length, text, u32 mask/u64 end).
func keypress(code):
    for down in [true,false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.physical_keycode = code
        event.pressed = down
        viewport.push_input(event,true)
        await wait(0.08)
func saved_clock(label):
    var path = directory+"/saves/Quick Save.eawrsave"
    var previous = -1
    if FileAccess.file_exists(path):
        var old = FileAccess.open(path,FileAccess.READ)
        old.seek(192)
        previous = old.get_64()
        old.close()
    await keypress(KEY_F6)
    var stop = elapsed+30.0
    var acknowledged = false
    while elapsed < stop:
        await wait(0.2)
        var menu = viewport.find_child("GalacticSaveMenu",true,false)
        var panel = menu.find_child("IDD_DIALOG_BOX1",true,false) if menu != null else null
        if panel != null and panel.is_visible_in_tree():
            acknowledged = true
            break
    # Opening the destination during publication can deny Windows' atomic rename.
    # Read it once, only after the ordinary save-completion UI acknowledges it.
    var bytes = FileAccess.get_file_as_bytes(path) if acknowledged else PackedByteArray()
    if bytes.size() < 304 or bytes.decode_u64(192) <= previous:
        notes.append({"clock_failure":label,"acknowledged":acknowledged,"previous_tick":previous})
        await shot("clock-failure")
        push_error("real quick-save did not publish a new clock sample")
        return {}
    # JSON is retained by the rig artifact collector; preserve the exact save bytes.
    var copy = FileAccess.open(directory+"/"+label+".checkpoint.json",FileAccess.WRITE)
    copy.store_string(JSON.stringify({"eawrsave_base64":Marshalls.raw_to_base64(bytes)}))
    copy.close()
    assert(bytes.slice(0,8) == PackedByteArray([69,65,87,82,67,75,80,0]))
    assert(bytes.decode_u16(8) == 1)
    var frame = bytes.decode_u64(200)
    # Scan only the direct galaxy owner's archive, excluding replay/setup/history copies.
    var owner = PackedByteArray()
    var offset = 304
    for section in range(bytes.decode_u32(220)):
        var size = bytes.decode_u64(offset+8)
        if bytes.decode_u16(offset) == 17:
            var game_owner = bytes.slice(offset+16,offset+16+size)
            assert(game_owner.decode_u32(0) == 5)
            owner = game_owner.slice(12,12+game_owner.decode_u64(4))
        offset += 16+size
    assert(not owner.is_empty())
    var sources = []
    var needle = "Generic_Probe_Droid".to_utf8_buffer()
    for index in range(24,owner.size()-needle.size()-12):
        if owner[index] != 71 and owner[index] != 103: continue
        if owner.slice(index,index+needle.size()).get_string_from_ascii().nocasecmp_to("Generic_Probe_Droid") != 0: continue
        if owner.decode_u64(index-8) != needle.size() or owner.decode_u64(index-16) != 1: continue
        var end = owner.decode_u64(index+needle.size()+4)
        var mask = owner.decode_u32(index+needle.size())
        var actor = owner.decode_u64(index-24)
        # Keep expired stored entries too: expiry must remove them in runtime,
        # rather than merely making this read-only observer filter them out.
        if actor > 0 and mask > 0 and mask < (1<<19) and end <= frame+1800:
            var source = {"actor":actor,"mask":mask,"end_frame":end}
            if not sources.has(source): sources.append(source)
    var observation = {"clock_sample":label,"session_tick":bytes.decode_u64(192),
        "galactic_frame":frame,"flags":bytes.decode_u16(10),"sources":sources,"sha256":FileAccess.get_sha256(path)}
    notes.append(observation)
    flush_notes()
    await keypress(KEY_ESCAPE) # dismiss the normal quick-save acknowledgement and resume
    await wait(0.3)
    return observation
func spy_lifetime():
    await click(planet_point())
    var observation = {}
    for attempt in range(16):
        observation = await saved_clock("spy-await-"+str(attempt))
        if observation.is_empty(): return false
        if not observation.get("sources",[]).is_empty(): break
        await wait(1.0)
    if observation.get("sources",[]).size() != 1:
        push_error("no unique live finite probe effect after real travel")
        flush_notes()
        return false
    var end = observation["sources"][0]["end_frame"]
    await shot("spy-game-000")
    for checkpoint in [{"label":"030","frame":end-900},{"label":"061","frame":end+30}]:
        var sample = 0
        while observation["galactic_frame"] < checkpoint["frame"]:
            await wait(min(20.0,max(1.0,(checkpoint["frame"]-observation["galactic_frame"])/30.0*0.85)))
            observation = await saved_clock("spy-game-"+checkpoint["label"]+"-sample-"+str(sample))
            if observation.is_empty(): return false
            sample += 1
        notes.append({"game_checkpoint":checkpoint["label"],"galactic_frame":observation["galactic_frame"],
            "effect_end_frame":end,"effect_start_frame":end-1800,"sources":observation["sources"]})
        await shot("spy-game-"+checkpoint["label"])
    return true
func planet_point():
    return destination_point
func probe_source():
    # Current stock story spawns occupy Coruscant slot 1 before the probe completes.
    # The baseline report confirms the purchased Probe_Droid_Team in slot 2.
    var point = centre("fleet_presence_"+str(origin)+"_2")
    if point == null: point = centre("fleet_presence_"+str(destination)+"_2")
    if point == null: point = centre("q3_0001")
    return point
func drag_probe(special):
    var source = probe_source()
    notes.append({"drag_source":str(source),"special":special,"elapsed":elapsed})
    if source == null: return false
    motion(source)
    mouse(source,true)
    await wait(0.15)
    motion(source+Vector2(24,0),true)
    await wait(0.15)
    motion(source+Vector2(48,0),true)
    await wait(0.2)
    if mode == "visibility":
        for probe in probes:
            var position = probe["point"]
            var point = Vector2(position[0],position[1])
            motion(point,true)
            await wait(1.0)
            await shot("hold-"+probe["kind"])
            notes.append({"target_kind":probe["kind"],"target":probe["id"],
                "slot":str(centre("galaxy_ability_slot_"+str(probe["id"]))),
                "slot_visible":centre("galaxy_ability_slot_"+str(probe["id"]))!=null,"native_drag":viewport.gui_is_dragging()})
        mouse(source,false)
        return true
    motion(planet_point(),true)
    await wait(0.7)
    await shot("04-drag-preview")
    if mode == "held-enemy": return true
    var slot = centre("galaxy_ability_slot_"+str(destination))
    notes.append({"special_slot":str(slot),"native_drag":viewport.gui_is_dragging(),"elapsed":elapsed})
    var target = slot if special and slot != null else planet_point()
    motion(target,true)
    await wait(0.3)
    await shot("05-drop-target")
    mouse(target,false)
    return not special or slot != null
func ground_attempt(target_name):
    await click(origin_point+Vector2(25,5))
    await click(origin_point+Vector2(25,5),MOUSE_BUTTON_LEFT,true)
    await wait(3.0)
    await click_name("b_planet_tab")
    await wait(0.5)
    await shot("ground-before")
    var source = centre("q3_0001")
    var target = centre(target_name)
    notes.append({"ground_source":str(source),"ground_target":str(target),"target_name":target_name,
        "source_found":source!=null,"target_found":target!=null})
    if source != null and target != null:
        motion(source)
        mouse(source,true)
        await wait(0.2)
        motion(source+Vector2(24,0),true)
        await wait(0.2)
        motion(target,true)
        await wait(0.5)
        await shot("ground-held")
        mouse(target,false)
        await wait(3.0)
        await shot("ground-after")
        # The base grid drop names the requested company cell, so attempt its return to orbit.
        if target_name == "Pad_09":
            source = centre("Pad_09")
            target = centre("q3_0001")
            if source != null and target != null:
                motion(source)
                mouse(source,true)
                await wait(0.2)
                motion(source+Vector2(24,0),true)
                await wait(0.2)
                motion(target,true)
                await wait(0.5)
                mouse(target,false)
                await wait(2.0)
                await shot("ground-return-attempt")
func run():
    await wait(3.0)
    await dismiss_story()
    await shot("00-start")
    await click_name("b_space_tab")
    await wait(0.5)
    await click_name(card)
    await wait(0.5)
    await shot("01-queued")
    await click_name("b_play_pause")
    var deadline = elapsed+100.0
    while elapsed < deadline:
        await wait(0.5)
        var head = viewport.find_child("queue00",true,false)
        if head == null or not head.is_visible_in_tree(): break
    # The production queue may not be visible until the next UI publication.
    # Wait for the actual fleet badge instead of treating that transient as completion.
    while elapsed < deadline and probe_source() == null:
        await dismiss_story()
        await wait(0.5)
    await wait(2.0)
    await shot("02-produced")
    await click(probe_source())
    await wait(0.5)
    await shot("03-selected-probe")
    await click(origin_point)
    await wait(0.3)
    if mode == "visibility":
        await click(planet_point())
        await wait(0.5)
        await shot("selected-enemy-no-drag")
    await wait(0.3)
    if mode == "ground-grid":
        await ground_attempt("Pad_09")
        await shot("ground-finished")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
        return
    var admitted = false
    if mode == "right-click":
        # A galaxy badge selects its planet. Select the actual focused fleet card
        # before returning to the unchanged overview pose for a right-click move.
        await click(origin_point+Vector2(25,5))
        await click(origin_point+Vector2(25,5),MOUSE_BUTTON_LEFT,true)
        await wait(3.0)
        await click_name("q3_0001")
        await shot("04-selected-for-right-click")
        var key = InputEventKey.new()
        key.keycode = KEY_HOME
        key.physical_keycode = KEY_HOME
        key.pressed = true
        viewport.push_input(key,true)
        await wait(0.08)
        key = InputEventKey.new()
        key.keycode = KEY_HOME
        key.physical_keycode = KEY_HOME
        key.pressed = false
        viewport.push_input(key,true)
        await wait(0.5)
        await shot("04-overview-for-right-click")
        await click(home_destination_point,MOUSE_BUTTON_RIGHT)
        await shot("05-right-click-move")
        admitted = true
    else:
        admitted = await drag_probe(mode == "slot")
    if mode == "held-enemy":
        await shot("held-enemy-final")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
        return
    if mode == "visibility":
        await ground_attempt("Slot_00")
        await shot("visibility-finished")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
        return
    notes.append({"special_slot_found":admitted,"mode":mode})
    await wait(1.0)
    await shot("06-released")
    if mode == "slot":
        var observed = await spy_lifetime()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit(0 if observed else 1)
        return
    for index in range(10):
        await wait(2.0)
        await shot("arrival-"+str(index*2+3))
    await shot("07-arrival")
    if mode == "world":
        await click(planet_point(),MOUSE_BUTTON_RIGHT)
        await wait(1.0)
        await shot("08-right-click-planet")
        await click(planet_point())
        await click(planet_point(),MOUSE_BUTTON_LEFT,true)
        await wait(3.0)
        await shot("09-double-click-planet")
        var remaining = probe_source()
        notes.append({"remaining_probe_click_target":remaining!=null})
        if remaining != null: await click(remaining,MOUSE_BUTTON_RIGHT)
        await wait(1.0)
        await shot("10-right-click-probe")
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''


@unittest.skipUnless(hero.RUNTIME and hero.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticProbeLiveRuntime(unittest.TestCase):
    campaign = hero.GalacticHeroVisibleRuntime.campaign
    launch = hero.GalacticHeroVisibleRuntime.launch

    def run_stock_cases(self, modes):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-probe-live-"))
        initial = self.launch(directory, "initial", ["--eawr-galactic-paused", "yes",
            "--eawr-capture", str(directory / "initial.png")])
        origin = initial["map_input"]["selected"]
        planets = {p["id"]: p for p in initial["planets"]}
        neighbours = {r["to"] if r["from"] == origin else r["from"]
                      for r in initial["routes"] if origin in (r["from"], r["to"])}
        # Displayed routes are trade lanes, not the proximity graph. The stock capital
        # has no enemy within its authored movement radius; stage at its own Anaxes,
        # then use adjacent Rebel Carida (a real-input move, not a fixture teleport).
        staging = next(p for p in planets.values() if p["name"] == "Anaxes")
        target = next(p for p in planets.values() if p["name"] == "Carida")
        self.assertEqual(staging["relation"], "own")
        self.assertEqual(target["relation"], "other")
        (directory / "selection.json").write_text(json.dumps({"origin": planets[origin], "staging": staging, "target": target,
            "neighbours": [planets[p] for p in sorted(neighbours)]}, indent=2), encoding="utf-8")
        points = {c["name"]: [c["hit_rect"][0]+c["hit_rect"][2]/2,
                               c["hit_rect"][1]+c["hit_rect"][3]/2]
                  for c in initial["command_bar"]["controls"]}
        menu_driver = directory / "menu.gd"
        menu_driver.write_text(hero.DRIVER % {"card": '""', "tab": '"b_space_tab"',
            "hero_icon": '"unused"', "hit_points": json.dumps(points),
            "directory": json.dumps(directory.as_posix()), "limit": "90.0"}, encoding="utf-8")
        menu = self.launch(directory, "menu", ["--eawr-galactic-paused", "yes"], menu_driver)
        choice = next(c for c in menu["command_bar"]["production_choices"] if c["type"] == "Probe_Droid_Team")
        self.assertTrue(choice["enabled"])
        points.update({c["name"]: [c["hit_rect"][0]+c["hit_rect"][2]/2,
                                  c["hit_rect"][1]+c["hit_rect"][3]/2]
                       for c in menu["command_bar"]["controls"]})
        neutral = next((p for p in planets.values() if p["relation"] == "neutral" and p["layer"] == "world"
                        and 20 < drag_support.GalacticFleetDragRuntime.project(initial["camera"], p["position"])[0] < 1260
                        and 30 < drag_support.GalacticFleetDragRuntime.project(initial["camera"], p["position"])[1] < 540), None)
        probes = [dict(target, kind="enemy"), dict(planets[origin], kind="own")]
        if neutral: probes.append(dict(neutral, kind="neutral"))
        for probe in probes:
            probe["point"] = drag_support.GalacticFleetDragRuntime.project(initial["camera"], probe["position"])
        target_point = drag_support.GalacticFleetDragRuntime.project(initial["camera"], target["position"])
        # Home resets the overview's XY scroll; startup initially positions it at Coruscant.
        home_camera = {**initial["camera"], "eye": list(initial["camera"]["eye"]),
                       "target": list(initial["camera"]["target"])}
        for key in ("eye", "target"):
            home_camera[key][0] -= planets[origin]["position"][0]
            home_camera[key][2] += planets[origin]["position"][1]
        home_point = drag_support.GalacticFleetDragRuntime.project(home_camera, target["position"])
        for mode in modes:
            case = directory / mode
            case.mkdir()
            driver = case / "input.gd"
            driver.write_text(DRIVER % {"directory": json.dumps(case.as_posix()),
                "card": json.dumps(choice["button"]), "hit_points": json.dumps(points), "probes": json.dumps([{k: p[k] for k in ("id", "kind", "point")} for p in probes]), "origin": origin, "destination": target["id"],
                "origin_point": "%s,%s" % tuple(drag_support.GalacticFleetDragRuntime.project(initial["camera"], planets[origin]["position"])),
                "point": "%s,%s" % tuple(target_point), "home_point": "%s,%s" % tuple(home_point),
                "mode": json.dumps(mode)}, encoding="utf-8")
            final = self.launch(case, "final", ["--eawr-galactic-paused", "yes", "--eawr-galactic-save-dir", str(case / "saves")], driver)
            self.assertEqual(final["status"], "closed")
            expected = {"held-enemy": "held-enemy-final.png", "visibility": "visibility-finished.png", "ground-grid": "ground-finished.png", "slot": "spy-game-061.png", "right-click": "07-arrival.png",
                        "world": "10-right-click-probe.png"}[mode]
            self.assertTrue((case / expected).is_file())
            notes = json.loads((case / "driver.json").read_text(encoding="utf-8"))
            if mode == "held-enemy":
                self.assertTrue(final["destruction_hud"]["visible"], "held enemy probe offers its Spy slot")
                self.assertTrue(final["destruction_hud"]["drop_actor"])
                self.assertEqual(final["destruction_hud"]["raid_slots"], 0)
            if mode == "visibility":
                targets = {row["target_kind"]: row for row in notes if "target_kind" in row}
                self.assertTrue(targets["enemy"]["native_drag"])
                self.assertTrue(targets["enemy"]["slot_visible"])
                enemy_frame = next(row for row in notes if row.get("shot") == "hold-enemy")
                self.assertGreaterEqual(sum(row["name"].startswith("galaxy_ability_slot_")
                                            for row in enemy_frame["ui"]), 2)
                self.assertFalse(targets["own"]["slot_visible"])
            if mode in ("visibility", "ground-grid"):
                attempts = [row for row in notes if "ground_source" in row]
                self.assertEqual(len(attempts), 1)
                self.assertTrue(attempts[0]["source_found"], "focused probe card must be reached")
                self.assertTrue(attempts[0]["target_found"], "ground drop target must be reached")
                self.assertGreater(final["fleet_hud"]["drag"]["begins"], 0, "visible fleet portrait starts a real drag")
                self.assertEqual(final["ground_hud"]["land_orders"], 0, "orbit-only probe is refused by the ground target")
                self.assertTrue(any(card["planet"] == origin and any(group["type"] == choice["type_id"] for group in card["groups"])
                                    for card in final["fleet_hud"]["cards"]), "refused probe remains in its orbital fleet")
            if mode in ("world", "right-click"):
                self.assertFalse(final["paused"], "lone consumed probe must leave the campaign running")
                self.assertEqual(final["battle_dialog"]["phase"], "hidden", "empty carrier must not create a battle")
                self.assertFalse(any(group["type"] == choice["type_id"] for card in final["fleet_hud"]["cards"]
                                     for group in card["groups"]), "consumed probe card disappears")
                intelligence = next(row for row in final["hud"]["intelligence"] if row["planet"] == target["id"])
                for fact in ("ground", "space", "structures", "control"):
                    self.assertIsNotNone(intelligence[fact], "ordinary probe arrival reveals " + fact)
            if mode == "slot":
                self.assertTrue(next(row["special_slot_found"] for row in notes if "special_slot_found" in row))
                self.assertEqual(final["destruction_hud"]["hero_drops"], 1)
                checkpoints = {row["game_checkpoint"]: row for row in notes if "game_checkpoint" in row}
                self.assertEqual(set(checkpoints), {"030", "061"})
                for sample in (row for row in notes if "clock_sample" in row):
                    saved = case / (sample["clock_sample"] + ".checkpoint.json")
                    data = base64.b64decode(json.loads(saved.read_text(encoding="utf-8"))["eawrsave_base64"], validate=True)
                    self.assertEqual(hashlib.sha256(data).hexdigest(), sample["sha256"])
                    self.assertEqual(hashlib.sha256(data[304:]).digest(), data[272:304])
                    self.assertEqual(struct.unpack_from("<Q", data, 200)[0], sample["galactic_frame"])
                    self.assertEqual(sample["flags"] & 1, 0, "probe must not create a child battle")
                self.assertLess(checkpoints["030"]["galactic_frame"], checkpoints["030"]["effect_end_frame"])
                self.assertTrue(checkpoints["030"]["sources"], "probe effect is live at 30 game seconds")
                self.assertFalse(checkpoints["061"]["sources"], "probe effect is gone after 60 game seconds")
                for label, minimum in (("030", 900), ("061", 1830)):
                    row = checkpoints[label]
                    self.assertGreaterEqual(row["galactic_frame"]-row["effect_start_frame"], minimum)
                intelligence = next(row for row in final["hud"]["intelligence"] if row["planet"] == target["id"])
                for fact in ("ground", "space", "structures", "control"):
                    self.assertIsNone(intelligence[fact], "expired probe no longer reveals " + fact)
                self.assertEqual(final["live_session"]["pause_depth"], 0)
                self.assertEqual(final["battle_dialog"]["phase"], "hidden")
                self.assertFalse(final["paused"])
        print("PROBE-LIVE-DIRECTORY", directory)

    def test_stock_probe_world_and_special_slot(self):
        self.run_stock_cases(("world", "slot", "ground-grid", "visibility", "right-click"))

    def test_spy_slots_all_planets(self):
        self.run_stock_cases(("visibility",))

    def test_stock_probe_held_enemy(self):
        self.run_stock_cases(("held-enemy",))


if __name__ == "__main__":
    unittest.main()
