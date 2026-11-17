"""#6909 viewer half: a stock hero dragged from its planet onto an enemy planet shows its ability tile, and the release
opens the action menu there (WGCH-01/02, WGCH-151..155).

One stock unit per action kind runs the owner's steps on shipped data: the sandbox campaign, the ability data, the drag
from the hero's fleet icon, the tile, the release, the travel, the arrival and the menu are the shipped ones. The staging is
a data overlay (`--eawr-mod-root`): the hero's company or its recruiting structure on the active faction's first starting
planet, and for the neutralize case an enemy hero company on every enemy starting planet. A recruited hero is bought with
the viewer's purchase hook (`--eawr-galactic-queue`), the ordinary purchase command, so it is placed by the game's own
creation path (orbit slot, stack icon) like the owner's.
"""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support

CAMPAIGN_FILE = "campaigns_underworld_gc.xml"

DRIVER = '''extends SceneTree
var frame = 0
var elapsed = 0.0
var viewport: SubViewport
var mode = "wait"
var wait_key = "eawr_ability_stage"
var round = 1
var target = ""
var badge = ""
var from = Vector2.ZERO
var drag_frame = 0
var tile_name = ""
var release_frame = 0
var modal_name = ""
var modal_frames = 0
var clicked = 0
var click_row = ""
var notice = ""
var directory = %(directory)s
var action_rows = %(rows)s
var close_rows = ["h_close", "corrupt_close"]
var again = %(again)s
var after = %(after)s
var phase = ""
var step = 0
var mark = 0
var press_at = Vector2.ZERO
var planet_b = ""
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280,720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(instance)
    viewport.notify_mouse_entered()
    current_scene = viewport
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    event.relative = Vector2(24,0)
    viewport.push_input(event,true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event,true)
func rmouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_RIGHT
    event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event,true)
func hero():
    if not Engine.has_meta("eawr_ability_hero"): return PackedStringArray()
    return str(Engine.get_meta("eawr_ability_hero")).split(" ")
func hero_point(fields, at):
    if fields.size() <= at + 1 or fields[at] == "-": return null
    return Vector2(float(fields[at]), float(fields[at + 1]))
func centre(name):
    var node = viewport.find_child(name,true,false)
    if node == null or not node.is_visible_in_tree():
        return null
    var rect = node.get_global_rect()
    return rect.position + rect.size / 2.0
func shot(label):
    viewport.get_texture().get_image().save_png(directory + "/" + label + ".png")
func finish(why):
    print("DRAG_END=", why, " round=", round, " target=", target, " tile=", tile_name, " modal=", modal_name)
    shot("end")
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
func visible_tile():
    var named = "galaxy_ability_slot_" + target
    if centre(named) != null: return named
    if centre("galaxy_destruction_first_slot") != null: return "galaxy_destruction_first_slot"
    return ""
func start_drag():
    var stage = str(Engine.get_meta(wait_key)).split(" ")
    target = stage[0]
    badge = stage[2]
    print("DRAG_STAGE=round ", round, " target ", target, " home ", stage[1], " icon ", badge, " elapsed=", elapsed)
    shot("home-" + str(round))
    drag_frame = 0
    tile_name = ""
    release_frame = 0
    modal_name = ""
    modal_frames = 0
    clicked = 0
    click_row = ""
    mode = "drag"
func drag_step():
    drag_frame += 1
    if drag_frame == 10:
        from = centre(badge)
        if from == null:
            finish("no-fleet-icon")
            return
        print("DRAG_FROM=", from)
        motion(from,false)
        mouse(from,true)
    if drag_frame == 12: motion(from + Vector2(24,0),true)
    if drag_frame == 14: motion(from + Vector2(48,0),true)
    if drag_frame >= 20 and tile_name == "":
        var name = visible_tile()
        if name != "":
            tile_name = name
            release_frame = drag_frame + 6
            var slot = viewport.find_child(name,true,false)
            for child in slot.get_children():
                if child is Label: print("DRAG_CAPTION=", child.text.to_upper())
            print("DRAG_TILE=", name)
            motion(centre(name) + Vector2(slot.size.x,0),true)
        elif drag_frame == 300:
            print("DRAG_TILE=none")
            shot("no-tile")
            finish("no-tile")
            return
    if tile_name != "" and drag_frame == release_frame - 2: shot("tile-" + str(round))
    if tile_name != "" and drag_frame == release_frame:
        motion(centre(tile_name),true)
        mouse(centre(tile_name),false)
        print("DRAG_RELEASED round=", round)
        notice = ""
        mode = "menu"
func menu_step():
    drag_frame += 1
    if modal_name == "":
        for candidate in ["hero_action_modal", "corruption_modal"]:
            var modal = viewport.find_child(candidate,true,false)
            if modal != null and modal.is_visible_in_tree():
                modal_name = candidate
        if modal_name == "":
            # A host refusal shows its notice instead of a menu: stop there, the notice is in the log.
            if notice != "" and drag_frame > 600: finish("refused")
            if elapsed > 900.0: finish("no-menu")
            return
    modal_frames += 1
    if modal_frames == 20:
        print("DRAG_MODAL=", modal_name, " round=", round)
        shot("menu-" + str(round))
        var rows = close_rows if (again and round == 1) else action_rows
        for row in rows:
            var node = viewport.find_child(row,true,false)
            if node != null and node.is_visible_in_tree() and not node.disabled:
                click_row = row
                break
        if click_row == "":
            print("DRAG_ACTION_CLICK=none")
            finish("menu-open-no-row")
            return
        motion(centre(click_row),false)
        clicked = modal_frames
    if clicked != 0 and modal_frames == clicked + 4:
        mouse(centre(click_row),true)
        print("DRAG_ACTION_CLICK=", click_row, " round=", round)
    if clicked != 0 and modal_frames == clicked + 6:
        mouse(centre(click_row) if centre(click_row) != null else Vector2(0,0),false)
    if clicked != 0 and modal_frames == clicked + 60:
        if again and round == 1:
            round = 2
            wait_key = "eawr_ability_stage2"
            mode = "after" if after != "" else "wait"
            phase = "settle"
            step = 0
        else:
            finish("acted")
# #7357: after Close the hero must still take orders (the owner's acceptance). "icon": its stack icon is dragged onto an
# empty orbit slot of another planet B (not the one it stands at, not its start; a hostile one first), an ordinary move. "panel": its planet is clicked and double-clicked (the fleet panel shows in the
# focused view) and its cell in that panel is clicked: the hero is selected. The fixture reports the hero every frame.
func on_screen(point):
    return point != null and point.x >= 0 and point.y >= 0 and point.x < 1280 and point.y < 720
func double_click(point):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = true
    event.double_click = true
    event.position = point
    event.global_position = point
    viewport.push_input(event,true)
    mouse(point,false)
func after_step():
    step += 1
    var fields = hero()
    if step %% 60 == 0: print("DRAG_HERO=", " ".join(fields), " phase=", phase)
    if fields.size() < 14:
        if step > 1200: finish("no-hero")
        return
    if phase == "settle" and step >= 60:
        print("DRAG_AFTER_CLOSE=", " ".join(fields))
        shot("after-close")
        phase = "icon" if after == "icon" else "planet"
        mark = step
    elif phase == "icon":
        if not Engine.has_meta("eawr_ability_stage2"):
            if step > mark + 900: finish("no-icon")
            return
        press_at = centre(str(Engine.get_meta("eawr_ability_stage2")).split(" ")[2])
        # The owner's acceptance: on to another planet B, not back to A and not necessarily home.
        var home = hero_point(fields, 12)
        if press_at == null or not on_screen(home):
            if step > mark + 900: finish("no-icon-or-home")
            return
        var t = step - mark
        if t == 1:
            motion(press_at,false)
            mouse(press_at,true)
        elif t == 3: motion(press_at + Vector2(24,0),true)
        elif t == 5: motion(press_at + Vector2(48,0),true)
        elif t >= 7 and t <= 16: motion(press_at + (home - press_at) * (t - 6) / 10.0,true)
        elif t > 16:
            # As the fleet drag case: hovering a planet shows its empty orbit slot icons; the move is the drop on one.
            var slot = null
            for icon in range(3):
                if slot == null: slot = centre("fleet_empty_slot_%%d" %% icon)
            var receiver = viewport.find_child("fleet_route_preview",true,false)
            if t %% 10 == 0 or slot != null:
                print("DRAG_ROUTE=", receiver.get_meta("route_preview") if receiver != null and receiver.has_meta("route_preview") else "none",
                    " slot=", slot, " dragging=", viewport.gui_is_dragging())
            if slot == null:
                if t > 120:
                    shot("no-slot")
                    mouse(home,false)
                    finish("no-empty-slot")
                return
            motion(slot,true)
            phase = "drop"
            press_at = slot
            mark = step
    elif phase == "drop":
        if step == mark + 5:
            var receiver = viewport.find_child("fleet_route_preview",true,false)
            print("DRAG_ROUTE_AT_DROP=", receiver.get_meta("route_preview") if receiver != null and receiver.has_meta("route_preview") else "none")
            mouse(press_at,false)
            planet_b = fields[11]
            print("DRAG_MOVE_DROP=", press_at, " other=", fields[11])
            phase = "move"
            mark = step
    elif phase == "planet":
        press_at = hero_point(fields, 9)
        if not on_screen(press_at):
            if step > mark + 900: finish("no-planet-point")
            return
        var t = step - mark
        if t == 1:
            motion(press_at,false)
            mouse(press_at,true)
            mouse(press_at,false)
        elif t == 20:
            double_click(press_at)
            print("DRAG_PLANET_FOCUS=", press_at)
            phase = "cell"
            mark = step
    elif phase == "cell":
        if fields[8] == "-":
            if step > mark + 600:
                shot("no-cell")
                finish("no-cell")
            return
        press_at = centre(fields[8])
        if press_at == null: return
        motion(press_at,false)
        mouse(press_at,true)
        mouse(press_at,false)
        print("DRAG_CELL_CLICK=", fields[8])
        phase = "selected"
        mark = step
    elif phase == "selected" and step >= mark + 30:
        print("DRAG_SELECTED=", "yes" if fields[2] == fields[3] else "no", " selected=", fields[2], " hero_fleet=", fields[3])
        shot("selected")
        # The move itself is the "icon" round's: the focused view hides the start planet.
        finish("selected")
    elif phase == "move":
        if fields[1] == "1" or fields[0] == planet_b:
            print("DRAG_MOVED=", " ".join(fields))
            finish("moved")
        elif step > mark + 600:
            print("DRAG_STUCK=", " ".join(fields))
            shot("stuck")
            finish("stuck")
func _process(delta):
    elapsed += delta
    if elapsed > 1000.0:
        print("DRAG_WHY=", Engine.get_meta("eawr_ability_why") if Engine.has_meta("eawr_ability_why") else "unset")
        finish("timeout")
        return false
    var label = viewport.find_child("GalacticAbilityNotice",true,false)
    if label != null and label.visible and label.text != notice:
        notice = label.text
        print("DRAG_NOTICE=", notice)
    if mode == "wait":
        if Engine.has_meta(wait_key): start_drag()
    elif mode == "drag": drag_step()
    elif mode == "menu": menu_step()
    elif mode == "after": after_step()
    return false
'''


def sandbox(tree, name):
    """The sandbox campaign node, its active faction, that faction's first starting planet and the other factions' planets."""
    campaign = next(node for node in tree if node.get("Name") == name)
    active = campaign.findtext("Starting_Active_Player").strip()
    rows = [[value.strip() for value in node.text.split(",")] for node in campaign.iter("Starting_Forces")]
    mine = next(row[1] for row in rows if row[0] == active)
    enemies = {}
    for row in rows:
        if row[0] != active and row[1] not in enemies.setdefault(row[0], []):
            enemies[row[0]].append(row[1])
    return campaign, active, mine, enemies


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticStockAbilityDragRuntime(unittest.TestCase):
    """Per action kind: tile while dragging, menu after the drop, the action runs."""

    def run_case(self, campaign, member, rows, enemy_faction="", enemy_company="", company="", recruit="", structure="",
                 again=False, enemy_planet="", panel=False, after="", home=""):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-ability-drag-%s-" % member))
        from tools.inventory.corpus import Corpus
        source = Corpus(pathlib.Path(support.GAME_ROOT)).read_effective("foc", "data/xml/" + CAMPAIGN_FILE)
        self.assertIsNotNone(source)
        tree = ET.fromstring(source.data)
        node, active, mine, enemies = sandbox(tree, campaign)
        mine = home or mine  # `home`: one of the active faction's own starting planets to place the hero at
        if recruit:
            # The sandboxes start over the population cap (23 of 20), and a purchase is admitted only into free room
            # (WGEP-69): drop the active faction's starting ships and ground units, keep its structures.
            keep = ("Star_Base", "Ground_", "Barracks", "Factory", "Droid_Works", "Palace", "Academy", "Cantina")
            for row in list(node.iter("Starting_Forces")):
                fields = [value.strip() for value in row.text.split(",")]
                if fields[0] == active and not any(word in fields[-1] for word in keep):
                    node.remove(row)
        if company:
            ET.SubElement(node, "Starting_Forces").text = "%s, %s, %s" % (active, mine, company)
        if structure:
            # A recruiting structure is placed at the start; its planet is listed as a special-case production planet so
            # the shipped admission rules accept it there.
            ET.SubElement(node, "Starting_Forces").text = "%s, %s, %s" % (active, mine, structure)
            ET.SubElement(node, "Special_Case_Production").text = "%s, %s, %s" % (active, mine, structure)
        prey = enemy_planet
        if enemy_company:
            # The enemy hero stands on one enemy planet, whose name the fixture is given as the drop planet (a plain
            # name, which is also how the viewer shows it). The planet is in view of the opening galaxy camera, so its
            # drop slot can be shown: a drop slot is only drawn where the planet is on screen.
            ET.SubElement(node, "Starting_Forces").text = "%s, %s, %s" % (enemy_faction, prey, enemy_company)
        xml = directory / "mod" / "Data" / "XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        (xml / CAMPAIGN_FILE).write_bytes(ET.tostring(tree, encoding="utf-8"))
        report, driver = directory / "report.json", directory / "driver.gd"
        driver.write_text(DRIVER % {"directory": json.dumps(directory.as_posix()), "rows": json.dumps(rows),
                                    "again": "true" if again else "false", "after": json.dumps(after)}, encoding="utf-8")
        purchase = ("--eawr-galactic-queue", recruit) if recruit else ()
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", campaign, "--eawr-galactic-hud-fixture", "ability-stock:%s:%s%s%s" % (member, enemy_faction, ":" + prey if prey else "", ":panel" if panel else ""),
            "--eawr-galactic-paused", "no", "--eawr-galactic-dismiss-story", "yes", *purchase,
            "--eawr-mod-root", str(directory / "mod"), "--eawr-game-root", support.GAME_ROOT,
            "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=1300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        print("ABILITY-DRAG-DIRECTORY", member, directory)
        lines = [line for line in result.stdout.splitlines() if line.startswith("DRAG_")]
        print("\n".join(lines))
        data = json.loads(report.read_text(encoding="utf-8")) if report.is_file() else {}
        return result, data, lines

    def check(self, result, data, lines, caption, modal, accepted, drops=1):
        text = "\n".join(lines)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DRAG_STAGE=", text, "the hero must be on the map with an enemy planet to drop on")
        self.assertIn("DRAG_CAPTION=" + caption, text, "the tile the owner drops on, shown while dragging")
        self.assertIn("DRAG_RELEASED", text)
        self.assertIn("DRAG_MODAL=" + modal, text, "the release must open the action menu on the target planet")
        self.assertIn("DRAG_END=acted", text)
        self.assertEqual(data["destruction_hud"]["hero_drops"], drops)
        self.assertEqual(data["fleet_hud"]["drag"]["orders"], 0, "the release is not an ordinary fleet order")
        self.assertGreaterEqual(data[accepted]["choices_accepted"], 1, "the chosen action ran")

    def test_stock_droids_steal_by_dragging_to_an_enemy_planet(self):
        # R2-D2 and C-3PO come with the story, not from a recruiting structure: the staged company stands in orbit.
        result, data, lines = self.run_case("Sandbox_Origin_Rebel", "Droid_R2D2", ["h_0000"], enemy_faction="Underworld",
                                            company="Droids_Team")
        self.check(result, data, lines, "STEAL", "hero_action_modal", "hero_action_hud")

    def test_stock_bounty_hunter_drop_on_a_planet_without_an_enemy_hero_says_so(self):
        # Neutralize needs a visible enemy hero at the planet (WGCH-20): the tile shows while dragging, the hunter travels,
        # and on arrival the host answers with the specific refusal (reason 22) and the notice, with no menu and no charge.
        # The menu with a hero to neutralize has its own live HUD case (test_viewer_galactic_hero_actions_live_runtime).
        # The staged company stands in a fleet of its own; a bought hunter stands in orbit slot 0 with the planet's other
        # fleets, whose stack icon carries them all and shows no tile (WGCH-153): which drag offers its tile there is an
        # open retail question.
        result, data, lines = self.run_case("Sandbox_Origin_Rebel", "Generic_Bounty_Hunter_R", ["h_0000"],
                                            enemy_faction="Empire", enemy_planet="Dathomir", company="Bounty_Hunter_Team_R")
        text = chr(10).join(lines)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DRAG_STAGE=", text, "the hero must be on the map with an enemy planet to drop on")
        self.assertIn("DRAG_CAPTION=NEUTRALIZE", text, "the tile the owner drops on, shown while dragging")
        self.assertIn("DRAG_RELEASED", text)
        self.assertIn("DRAG_NOTICE=There is no enemy hero on that planet to neutralize.", text)
        self.assertIn("DRAG_END=refused", text)
        self.assertNotIn("DRAG_MODAL=", text)
        self.assertEqual(data["destruction_hud"]["hero_drops"], 1)
        self.assertEqual(data["destruction_hud"]["refusal_reasons"], [22], "neutralize_no_hero")
        self.assertEqual(data["hero_action_hud"]["choices_accepted"], 0)

    def test_recruited_saboteur_corrupts_by_dragging_then_again_on_the_planet_it_reached(self):
        # Round 1 arrives by travel and closes the menu; round 2 drops the parked hero on the planet it is at.
        result, data, lines = self.run_case("Sandbox_Origin_Underworld", "Underworld_Saboteur",
                                            ["corrupt_button_icon_a", "corrupt_button_icon_b", "corrupt_button_icon_c"],
                                            enemy_faction="Empire", enemy_planet="Bothawui", recruit="Underworld_Saboteur_Team",
                                            structure="U_Ground_Palace", again=True)
        # Stock slot 1 at Bothawui is authored as a land mission battle. A solo land choice runs on the timed route
        # (WGCH-198, deviation from retail), so its row is offered available: the second round clicks it, the choice is
        # charged and no mission battle starts.
        self.check(result, data, lines, "SPREAD CORRUPTION", "corruption_modal", "corruption_hud", drops=2)
        self.assertEqual(data["corruption_hud"]["choices_accepted"], 1, "the land choice ran once")
        self.assertEqual(data["corruption_hud"]["accepted_slot"], 1, "the stock slot 1 land mission choice was the one that ran")
        self.assertIn("DRAG_ACTION_CLICK=corrupt_button_icon_a", chr(10).join(lines), "its row was enabled, so the second round could click it")

    def free_after_close(self, after):
        # #7357: the owner's Cancel on the Corrupt Planet dialog. Retail leaves the hero free (WGCH-121): round 1 travels to the
        # enemy planet and closes the menu, then the parked hero is given orders by real input. The drop planet is the owner's:
        # the Empire's Corellia, with its starbase and ships in orbit. A player reaches it from a planet next to it, where it
        # is revealed (a route needs a revealed planet, WGM-18): the Defiler starts at Zann's Bothawui, Corellia's neighbour
        # in this conquest.
        result, data, lines = self.run_case("Sandbox_Equal_Footing_Underworld", "Underworld_Saboteur", [],
                                            enemy_faction="Empire", enemy_planet="Corellia", company="Underworld_Saboteur_Team",
                                            home="Bothawui", again=True, after=after)
        text = chr(10).join(lines)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DRAG_MODAL=corruption_modal", text, "the release must open the corruption dialog")
        self.assertIn("DRAG_ACTION_CLICK=corrupt_close", text, "round 1 closes the dialog")
        self.assertEqual(data["corruption_hud"]["closes"], 1)
        self.assertEqual(data["corruption_hud"]["choices_accepted"], 0)
        if after == "panel":
            self.assertIn("DRAG_SELECTED=yes", text, "the hero is selected from its planet's fleet panel after Close")
            self.assertIn("DRAG_END=selected", text)
        else:
            self.assertIn("DRAG_END=moved", text, "the hero's stack icon takes an ordinary move to another planet after Close")
            self.assertEqual(data["fleet_hud"]["drag"]["orders"], 1, "the drop home is one ordinary fleet order")

    def test_saboteur_after_close_by_dragging_its_stack_icon_to_another_planet(self):
        self.free_after_close("icon")

    def test_saboteur_after_close_by_its_cell_in_the_focused_planet_panel(self):
        self.free_after_close("panel")


if __name__ == "__main__":
    unittest.main()
