"""Contracts for #82 P2-19: selection and orders on the live battle.

`--eawr-live-session m2` on Coruscant runs the M2 skirmish; the battle input (apps/viewer/src/
battle_input.cpp) takes the local player's pointer and keys ahead of the camera, selects what the
player sees and gives orders only through the session's OrderInput. `--eawr-live-input` replays
player gestures through Godot's own input queue at presentation ticks, so a run drives the same
path as a mouse.

The structural half runs everywhere. The graphical half is opt-in (EAWR_GODOT_VIEWER_RUNTIME_TEST,
EAWR_GODOT_EXECUTABLE, EAWR_EAW_GAME_ROOT): a click on the Rebel spawn stack selects the unit on
top and a right click moves it, as the same order through --eawr-live-order would; a run that only selects, boxes and recalls groups ends on the hashes of a run with no
input at all; the overview key steps the camera out; middle drags and clicks, Ctrl key events
included, pan, turn, tilt and reset the battle camera (#328 law), read from the report's camera
samples; with the HUD on, the command bar keeps the
clicks on its drawn pixels and the open battle keeps the rest.

#424: FoC's battle UI in the world (docs/behaviour/foc-battle-world-ui.md). A squadron is one unit,
its team container: a click on its icon or on one of its craft selects it, a right click moves it
through the session (space-fighters FO-01) and the craft follow; the report's world_ui member says
what the frame drew (circles per selected craft, the hovered craft's own health bar and no shield
bar, the icons and their states, the hovered ship's hardpoint reticles).
"""

import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from test_space_map_mode import strict_json  # noqa: E402

CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"
CAMERA = ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml"
# The Rebel Corellian_Corvette, unit 5 of the M2 start, stands at (-5057, 4700).
CORVETTE = 5
CORVETTE_START = (-5057.0, 4700.0)
DESTINATION = (-4600.0, 5150.0)
END_TICK = 150
# The live session draws the tactical HUD by default (#338). Whether a unit shows under the command
# bar depends on where the start camera looks, so the tests of the world layer itself run without
# the HUD and test_hud_takes_the_bar_and_leaves_the_battle aims its gestures at viewport pixels.
HUD_OFF = ("--eawr-hud", "off")
VIEWPORT = (1280, 720)
# The Rebel star base, unit 1 of the M2 start, stands apart from the spawn stack (#452 guard target).
STAR_BASE = 1
STAR_BASE_POSITION = (-3811.0, 4460.0)
# #424: the M2 start (sim_headless --skirmish m2): Rebel X-wing squadrons 2 (craft 36-40) and 3
# (41-45), the Y-wing squadron 4 (craft 46-48), the MC80 7; Empire TIE Interceptor squadrons 9 and
# 10, the Acclamator 12. Every Rebel company starts on the spawn stack; debug moves spread them first.
Y_WING_SQUADRON = 4
Y_WINGS = (46, 47, 48)
NEBULON = 6
# #665: where the Nebulon-B stands at the M2 start (it takes no order unless a test gives one).
NEBULON_START = (-4875.0, 4509.0)
# A craft of X-wing squadron 3 (its members are 41 to 45).
X_WING_3_CRAFT = 43
ACCLAMATOR = 12
X_WING_SQUADRON = 2
X_WING_SQUADRON_3 = 3
TIE_SQUADRON = 9
# #497: the Empire Tartan 11 and the craft of TIE Interceptor squadron 9; the attack-through-fog
# layout: the Nebulon-B spots 1100 units from the target on its line to the Empire start, the
# corvette waits 3000 units from it.
TARTAN = 11
TIE_CRAFT = range(49, 56)
TARGET_POINT = (-1192.0, 1054.0)
SPOTTER_POINT = (-2000.0, 1800.0)
ATTACKER_POINT = (-3300.0, 3300.0)
# #435: the two squadrons fly to one point mid-map (a move needs no sight of the enemy), attack
# each other once there (FT-01 keeps only a target the squadron sees) and dogfight.
MEETING = (-950.0, 900.0)
DOGFIGHT_ORDER_TICK = 1450
DOGFIGHT_TICK = 1600
# #518: by this tick the Rebel station has launched its first squadron (SK-23, 10 s delay).
LAUNCH_PROBE_TICK = 700
# The M2 start's other TIE Interceptor squadron, idle by the Empire start (TARGET_POINT), a few
# hundred units from MEETING. #511's fog rules reveal a shooter's cell to the target's owner's
# whole team (V-19), so once the dogfight's fire starts, squadron 10 sees X_WING_SQUADRON within its
# own idle chase range plus attack distance (FT-02: 200 + 500 on the TIE Interceptor) and joins the
# same combat cell too, unless it is out of that reach when the shooting starts. Sent well clear of
# the whole cluster of M2 start positions so it cannot re-acquire either dogfighter.
OTHER_TIE_SQUADRON = 10
CLEAR_OF_DOGFIGHT = (3800.0, -4700.0)
SPREAD = ("--eawr-live-order", "1:move:5@-4450,5050,0", "--eawr-live-order", "1:move:2@-5450,5250,0",
          "--eawr-live-order", "1:move:3@-4650,4250,0")
# #518 added the MC80 (unit 7) to the Rebel start, stacked on the spawn with the Nebulon-B; a hover
# or click at the Nebulon-B's screen point picks the topmost contact there (P-1), and with the
# camera close in that is the MC80. Tests that aim at the Nebulon-B send the MC80 aside first.
MC80 = 7
MC80_ASIDE = ("--eawr-live-order", f"1:move:{MC80}@-5000,3900,0")
# #666: the Nebulon-B flies 90 units below the Y-wing squadron (unit 4) left on the spawn, so the
# squadron's world-UI icon, which a hover meets before any unit, can cover the Nebulon-B's
# screen point. Tests that hover the Nebulon-B close in send the squadron aside as well.
Y_WING_ASIDE = ("--eawr-live-order", f"1:move:{Y_WING_SQUADRON}@-5450,4250,0")
# The options button's hit rect at 1280x720 (test_tactical_hud pins it; the run checks the report).
OPTIONS_HIT = (192.1875, 690.938, 22.5, 22.5)


def screen(point) -> str:
    return f"screen={round(point[0], 3)},{round(point[1], 3)}"


def inside(point, rect) -> bool:
    x, y, width, height = rect
    return x <= point[0] <= x + width and y <= point[1] <= y + height


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class BattleInputSources(unittest.TestCase):
    def test_orders_leave_only_through_order_input(self):
        source = read("apps/viewer/src/battle_input.cpp")
        # UI-07: no direct session access; every order is an OrderInput call.
        for forbidden in ("submit(", "platform::", "tactical/session.hpp", "->take("):
            self.assertNotIn(forbidden, source)
        self.assertIn("input->world_command(pick, ui::CommandOrigin::world_click, ", source)
        self.assertIn("input->stop(ui::CommandOrigin::hotkey)", source)

    def test_world_layer_goes_before_the_camera(self):
        mode = read("apps/viewer/src/map_mode.cpp")
        body = mode[mode.index("void MapMode::input("):]
        self.assertLess(body.index("state.battle->input("), body.index("state.space->camera_event("))

    def test_live_camera_leaves_foc_order_keys_free(self):
        config = read("apps/viewer/project/config/coruscant-live-session-camera.xml")
        self.assertIn('bindings path="space-live-camera-bindings.json"', config)
        bindings = json.loads(read("apps/viewer/project/config/space-live-camera-bindings.json"))
        keys = {binding["control"] for binding in bindings["bindings"] if binding["device"] == "keyboard"}
        # FoC's order keys: A attack, S stop, M move, T attack-move, G guard (#452).
        self.assertFalse(keys & {"A", "S", "M", "T", "G", "W", "D", "Insert"}, keys)
        self.assertTrue({"Left", "Right", "Up", "Down"} <= keys)

    def test_live_camera_follows_the_space_map_camera(self):
        # The live battle camera is FoC's same tactical camera: the space map table without WASD,
        # middle-button law included (#328: plain middle drag pans, Ctrl rotates, a click resets).
        live = json.loads(read("apps/viewer/project/config/space-live-camera-bindings.json"))
        space = json.loads(read("apps/viewer/project/config/space-map-camera-bindings.json"))
        for key in ("edge_scroll", "click_reset", "screen_mouse_units"):
            self.assertEqual(live.get(key), space.get(key), key)
        self.assertNotIn("pan_speed_scale", live)
        wasd = {"W", "A", "S", "D"}
        expected = [row for row in space["bindings"] if not (row["device"] == "keyboard" and row["control"] in wasd)]
        self.assertEqual(live["bindings"], expected)

    def test_live_camera_starts_at_the_space_map_default_distance(self):
        # Project deviations (owner): both space cameras open at distance 1200 (#337/#348), a bit
        # further out than FoC's Distance_Default 1000, of the close-zoom range 100..1900 (#390,
        # FoC's Space_Mode is 200..1900), so zoom 0.611111.
        def zoom(path):
            return float(re.search(r'<initial [^>]*zoom="([0-9.]+)"', read(path)).group(1))

        def overrides(path):
            return re.findall(r'<override tag="([A-Za-z_]+)" value="(-?[0-9.]+)"/>', read(path))
        live = zoom("apps/viewer/project/config/coruscant-live-session-camera.xml")
        self.assertEqual(live, zoom("apps/viewer/project/config/coruscant-space-map-camera.xml"))
        self.assertAlmostEqual(100.0 + live * 1800.0, 1200.0, delta=0.01)
        live_overrides = overrides("apps/viewer/project/config/coruscant-live-session-camera.xml")
        self.assertEqual(live_overrides, overrides("apps/viewer/project/config/coruscant-space-map-camera.xml"))
        self.assertEqual(dict(live_overrides),
                         {"Distance_Min": "100", "Tactical_Min_Scroll_Speed": "823.529412",
                          "Pitch_Min": "-60", "Tactical_Overview_Clicks": "5"})

    def test_world_ui_follows_the_behaviour_note(self):
        note = read("docs/behaviour/foc-battle-world-ui.md")
        self.assertNotRegex(note, r"0x[0-9a-fA-F]{6,}")
        for heading in ("## Selection circle", "## Shield and health bars", "## Squadron icon",
                        "## Dogfight grid", "## Hardpoint reticles", "## Unverified"):
            self.assertIn(heading, note)
        source = read("apps/viewer/src/world_ui_view.cpp")
        # The rules live in the engine-free model; the view resolves art and draws.
        for rule in ("ui::bar_visibility(", "ui::bar_scale(", "ui::bar_level(", "ui::health_bar_colour(",
                     "ui::hardpoint_reticle_tint(", "ui::selection_circle_side(", "ui::bar_anchor_lift(",
                     "ui::bar_candidate(", "ui::combat_grid_slot(", "ui::combat_cell_point(",
                     "ui::hardpoint_reticle_rect(", "ui::hardpoint_reticle_anchor("):
            self.assertIn(rule, source)
        # UI-07: the world UI never reaches the session.
        for forbidden in ("submit(", "platform::", "tactical/session.hpp", "order_input("):
            self.assertNotIn(forbidden, source)

    def test_behaviour_note_marks_what_is_unverified(self):
        note = read("docs/behaviour/foc-battle-selection.md")
        self.assertNotRegex(note, r"0x[0-9a-fA-F]{6,}")
        for heading in ("## Picking", "## Control groups", "## Tactical overview", "## Unverified"):
            self.assertIn(heading, note)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class BattleInputGraphical(unittest.TestCase):
    def _run(self, directory: pathlib.Path, name: str, extra=(), camera=CAMERA, end_tick=END_TICK):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        report = directory / f"{name}.json"
        completed = subprocess.run(
            [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", CORUSCANT, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
             "--eawr-report", str(report), "--eawr-capture", str(directory / f"{name}.png"),
             "--eawr-populate", "--eawr-map-camera-config", str(camera), "--eawr-live-session", "m2",
             "--eawr-live-ticks", str(end_tick), *extra],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
        self.assertTrue(report.is_file(), completed.stdout[-4000:])
        return completed.returncode, strict_json(report.read_text(encoding="utf-8"))

    def _position(self, result, entity):
        units = {unit["entity"]: unit["position"] for unit in result["live_session"]["own_units"]}
        self.assertIn(entity, units)
        return units[entity]

    def test_click_selects_and_right_click_moves(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-move-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "move", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"15:rclick:@{DESTINATION[0]},{DESTINATION[1]},0",
                "--eawr-live-capture-ticks", "10,20,80,150"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            # The M2 start stacks the Rebel companies on their spawn: the click at the corvette's
            # centre picks the highest contact there (P-1), one unit.
            self.assertEqual(len(battle["selected"]), 1, battle["log"])
            picked = battle["selected"][0]
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            order = next(line for line in battle["log"] if line.startswith("move @"))
            match = re.fullmatch(r"move @(\S+) units (\d+) tick (\d+)", order)
            self.assertTrue(match, order)
            point, units, tick = match.group(1), int(match.group(2)), int(match.group(3))
            self.assertEqual(units, picked)
            # The right click lands where the pointer was: the battle-plane point of the projected
            # destination.
            x, y, _ = (float(value) for value in point.split(","))
            self.assertLess(math.dist((x, y), DESTINATION), 1.0, point)
            # The picked unit left the stack toward it.
            position = self._position(result, picked)
            self.assertGreater(math.dist(CORVETTE_START, position[:2]), 100.0, position)
            self.assertLess(math.dist(DESTINATION, position[:2]), math.dist(DESTINATION, CORVETTE_START), position)
            # The same order through the debug hook at the tick the click was stamped for gives the
            # same unit the same path: the click is an ordinary recorded command.
            code, hook = self._run(directory, "hook", (*HUD_OFF, "--eawr-live-order", f"{tick}:move:{picked}@{point}"))
            self.assertEqual(code, 0, hook.get("failure"))
            self.assertEqual(self._position(hook, picked), position)
            self.assertEqual(hook["live_session"]["final_state_sha256"], live["final_state_sha256"])

    # #452 (docs/behaviour/space-orders.md OR-01): Ctrl+right click on empty space attack-moves the
    # selection, exactly as the same order through the debug hook.
    def test_ctrl_right_click_attack_moves(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-move-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "attack_move", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"15:rclick:@{DESTINATION[0]},{DESTINATION[1]},0+ctrl"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual((battle["scripted_fired"], battle["orders"]), (2, 1), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            picked = battle["selected"][0]
            order = next(line for line in battle["log"] if line.startswith("attack-move @"))
            match = re.fullmatch(r"attack-move @(\S+) units (\d+) tick (\d+)", order)
            self.assertTrue(match, order)
            point, units, tick = match.group(1), int(match.group(2)), int(match.group(3))
            self.assertEqual(units, picked)
            x, y, _ = (float(value) for value in point.split(","))
            self.assertLess(math.dist((x, y), DESTINATION), 1.0, point)
            position = self._position(result, picked)
            self.assertLess(math.dist(DESTINATION, position[:2]), math.dist(DESTINATION, CORVETTE_START), position)
            code, hook = self._run(directory, "attack_move_hook",
                                   (*HUD_OFF, "--eawr-live-order", f"{tick}:attack_move:{picked}@{point}"))
            self.assertEqual(code, 0, hook.get("failure"))
            self.assertEqual(hook["live_session"]["final_state_sha256"], live["final_state_sha256"])

    # OR-01, OR-14: Ctrl+Alt+right click on an own unit outside the selection (the Rebel star base)
    # guards it: the ship closes to within the guard range, as the same order through the hook.
    def test_ctrl_alt_right_click_guards_an_own_unit(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-guard-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "guard", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"15:rclick:unit={STAR_BASE}+ctrl+alt"), end_tick=450)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual((battle["scripted_fired"], battle["orders"]), (2, 1), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            picked = battle["selected"][0]
            order = next(line for line in battle["log"] if line.startswith("guard "))
            match = re.fullmatch(rf"guard {STAR_BASE} units (\d+) tick (\d+)", order)
            self.assertTrue(match, order)
            self.assertEqual(int(match.group(1)), picked)
            tick = int(match.group(2))
            position = self._position(result, picked)
            self.assertLess(math.dist(STAR_BASE_POSITION, position[:2]), math.dist(STAR_BASE_POSITION, CORVETTE_START),
                            position)
            code, hook = self._run(directory, "guard_hook",
                                   (*HUD_OFF, "--eawr-live-order", f"{tick}:guard:{picked}@{STAR_BASE}"), end_tick=450)
            self.assertEqual(code, 0, hook.get("failure"))
            self.assertEqual(hook["live_session"]["final_state_sha256"], live["final_state_sha256"])

    # OR-01: the T and G keys arm the attack-move and guard modes; the next right click uses them.
    def test_t_and_g_arm_attack_move_and_guard(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-modes-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "modes", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", "12:key:T",
                "--eawr-live-input", f"15:rclick:@{DESTINATION[0]},{DESTINATION[1]},0",
                "--eawr-live-input", "40:key:G",
                "--eawr-live-input", f"45:rclick:unit={STAR_BASE}"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            log = battle["log"]
            self.assertEqual((battle["scripted_fired"], battle["orders"]), (5, 2), log)
            self.assertEqual(live["rejected"], [])
            self.assertIn("attack-move mode", log)
            self.assertIn("guard mode", log)
            self.assertTrue(any(line.startswith("attack-move @") for line in log), log)
            self.assertTrue(any(line.startswith(f"guard {STAR_BASE} ") for line in log), log)

    # #497: the local player's right click on an enemy attacks it in the M2 battle with fog on, as
    # the owner gives the order. The Nebulon-B spots: it stops 1100 units from the target, inside
    # its 1200-unit sight, so the target shows through the fog. The corvette waits 3000 units from
    # the target, beyond its own sight and its 800-unit weapons, so any hit it lands comes from the
    # order: it closes (OR-02 to OR-06) and fires. The AI is off so the Empire stays where the debug
    # moves put it; the camera follows the corvette, then the spotter.
    def _attack_through_fog(self, directory, name, target, target_point, order_tick, end_tick):
        camera = directory / f"{name}-camera.xml"
        shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
        camera.write_text(re.sub(r"<initial [^>]*/>", f'<initial target_x="{ATTACKER_POINT[0]}" '
                                 f'target_y="{ATTACKER_POINT[1]}" target_height="0" zoom="0.5" yaw_degrees="0"/>',
                                 CAMERA.read_text(encoding="utf-8")), encoding="utf-8")
        code, result = self._run(directory, name, (
            *HUD_OFF, "--eawr-live-ai", "off", "--eawr-live-step", "10",
            "--eawr-live-order", f"1:move:{CORVETTE}@{ATTACKER_POINT[0]},{ATTACKER_POINT[1]},0",
            "--eawr-live-order", f"1:move:{NEBULON}@{SPOTTER_POINT[0]},{SPOTTER_POINT[1]},0",
            "--eawr-live-order", f"1:move:{target}@{target_point[0]},{target_point[1]},0",
            "--eawr-live-follow-group", f"1:{CORVETTE}",
            "--eawr-live-follow-group", f"{order_tick - 200}:{NEBULON}",
            "--eawr-live-input", f"900:click:unit={CORVETTE}",
            "--eawr-live-input", f"{order_tick}:rclick:unit={target}"), camera=camera, end_tick=end_tick)
        self.assertEqual(code, 0, result.get("failure"))
        battle, live = result["battle_input"], result["live_session"]
        self.assertEqual((battle["scripted_fired"], battle["orders"]), (2, 1), battle["log"])
        self.assertEqual(battle["selected"], [CORVETTE], battle["log"])
        self.assertEqual(live["rejected"], [])
        self.assertIs(live["headless_hashes_equal"], True)
        # Fog is on: the local player does not see every unit.
        self.assertIs(live["reveal"], False)
        self.assertGreater(live["hidden_units"], 0)
        order = next(line for line in battle["log"] if line.startswith("attack "))
        match = re.fullmatch(rf"attack {target} units {CORVETTE} tick (\d+)", order)
        self.assertTrue(match, battle["log"])
        return int(match.group(1)), live

    def test_right_click_attacks_a_capital_ship_seen_through_fog(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-ship-") as temporary:
            tick, live = self._attack_through_fog(pathlib.Path(temporary), "attack_ship", TARTAN, TARGET_POINT, 2500, 3300)
            hits = [hit for hit in live["first_hits"] if hit["shooter"] == CORVETTE and hit["target"] == TARTAN]
            self.assertEqual(len(hits), 1, live["first_hits"])
            self.assertGreater(hits[0]["tick"], tick, hits)
            # It closed to its slot (0.9 x 800 short of the Tartan) instead of holding 3000 away.
            position = next(unit["position"] for unit in live["own_units"] if unit["entity"] == CORVETTE)
            self.assertLess(math.dist(position[:2], TARGET_POINT), 1000.0, position)

    def test_right_click_attacks_a_squadron_seen_through_fog(self):
        # The right click on a TIE Interceptor's craft orders the attack on its squadron, the team
        # container (S-7); the corvette's hits go to the squadron's craft (FO-04).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-squadron-") as temporary:
            tick, live = self._attack_through_fog(pathlib.Path(temporary), "attack_squadron", TIE_SQUADRON, TARGET_POINT,
                                                  2100, 2900)
            hits = [hit for hit in live["first_hits"] if hit["shooter"] == CORVETTE and hit["target"] in TIE_CRAFT]
            self.assertTrue(hits, live["first_hits"])
            self.assertGreater(min(hit["tick"] for hit in hits), tick, hits)

    def test_selection_alone_keeps_the_replay(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-select-") as temporary:
            directory = pathlib.Path(temporary)
            code, idle = self._run(directory, "idle", HUD_OFF)
            self.assertEqual(code, 0, idle.get("failure"))
            box = "@-5250,4550,0/@-4850,4850,0"
            code, result = self._run(directory, "select", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"20:dclick:unit={CORVETTE}",
                "--eawr-live-input", f"30:box:{box}",
                "--eawr-live-input", "40:key:1+ctrl",
                "--eawr-live-input", "50:click:@-4700,5100,0",
                "--eawr-live-input", "60:key:1",
                "--eawr-live-input", "65:key:1",
                "--eawr-live-input", "90:key:Insert",
                "--eawr-live-capture-ticks", "35,70,100"))
            self.assertEqual(code, 0, result.get("failure"))
            battle = result["battle_input"]
            self.assertEqual(battle["scripted_fired"], 8, battle["log"])
            self.assertEqual(battle["orders"], 0, battle["log"])
            self.assertGreater(len(battle["selected"]), 1, battle["log"])
            self.assertEqual(battle["groups"]["1"], battle["selected"], battle["log"])
            self.assertEqual(battle["camera_focuses"], 1, battle["log"])
            self.assertEqual(battle["overview"], "overview", battle["log"])
            self.assertIn("box select", battle["log"])
            # UI-C2: selection is presentation state; the session ends where an idle one does.
            self.assertEqual(result["live_session"]["final_state_sha256"], idle["live_session"]["final_state_sha256"])
            self.assertEqual(result["live_session"]["completed_ticks"], idle["live_session"]["completed_ticks"])
            self.assertEqual(self._position(result, CORVETTE), self._position(idle, CORVETTE))

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
                                             camera=camera, end_tick=60)
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

    def test_squadron_selects_and_moves_as_one_unit(self):
        # #424: a click on the Y-wing squadron's icon selects the squadron (its container), a right
        # click moves it, and its craft fly there in formation (FO-01); hovering one craft shows that
        # craft's own health bar and no shield bar (WU-17) while the squadron keeps its three circles.
        destination = (-4550.0, 5150.0)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-squadron-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "squadron", (
                *HUD_OFF, *SPREAD,
                "--eawr-live-input", f"30:click:icon={Y_WING_SQUADRON}",
                "--eawr-live-input", f"40:rclick:@{destination[0]},{destination[1]},0",
                "--eawr-live-input", f"290:hover:unit={Y_WINGS[1]}"), end_tick=300)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live, world = result["battle_input"], result["live_session"], result["world_ui"]
            self.assertEqual(battle["scripted_fired"], 3, battle["log"])
            self.assertEqual(battle["selected"], [Y_WING_SQUADRON], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(line.startswith("move @") and f" units {Y_WING_SQUADRON} " in line
                                for line in battle["log"]), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            for craft in Y_WINGS:
                position = self._position(result, craft)
                self.assertLess(math.dist(destination, position[:2]), 250.0, (craft, position))
            self.assertTrue(world["ring_loaded"] and world["atlas_loaded"], world["unresolved"])
            self.assertEqual(world["circles"], 3, world)
            self.assertTrue(any(re.match(rf"^{Y_WING_SQUADRON}:selected:10:y=", row) for row in world["icon_rows"]),
                            world["icon_rows"])
            self.assertEqual(world["shield_bars"], 0, world)
            # The pointer rests where the hovered craft was drawn. The squadron's icon sits at the
            # squadron's centre and takes the pointer first (WU-23): its own small bar already shows
            # (icon_rows), and #502 no craft gets a bar of its own from that; on a craft the pointer
            # shows that craft's bar alone.
            if battle["hovered_icon"] is not None:
                self.assertEqual(battle["hovered_icon"], Y_WING_SQUADRON, battle)
                self.assertEqual(world["bar_rows"], [], world)
            else:
                self.assertIn(battle["hovered"], Y_WINGS, battle)
                self.assertEqual(world["bar_rows"], [f"{battle['hovered']}:h10"], world)

    def test_double_clicking_an_icon_selects_its_squadron_type(self):
        # #550 (foc-battle-world-ui WU-23a, walk WSU-38): a double click on an X-wing squadron's icon
        # selects every own squadron with a craft of its leader's type (X-Wing) on screen (both of the
        # M2 start's X-wing squadrons), and no squadron of another craft type (the Y-wing squadron and
        # the TIE squadrons stay out). Selection is presentation state: the replay is unchanged.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-icon-dclick-") as temporary:
            directory = pathlib.Path(temporary)
            # SPREAD's move takes squadron 3 below a 1280x720 frame of the start camera; here it
            # heads up screen instead, so its craft are well inside the frame at the double click.
            code, result = self._run(directory, "icon-dclick", (
                *HUD_OFF, *SPREAD[:4], "--eawr-live-order", f"1:move:{X_WING_SQUADRON_3}@-4750,5000,0",
                "--eawr-live-input", f"100:dclick:icon={X_WING_SQUADRON}"), end_tick=110)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 1, battle["log"])
            self.assertIn("double click: craft type on screen", battle["log"])
            # A wider viewport can also show a squadron the station launched; only X-wing squadrons
            # (the ones with S-foil craft) may join.
            x_wings = {row["container"] for row in live["squadrons"] if row["owner"] == 1 and row["sfoil_craft"] > 0}
            self.assertLessEqual({X_WING_SQUADRON, X_WING_SQUADRON_3}, set(battle["selected"]), battle["log"])
            self.assertLessEqual(set(battle["selected"]), x_wings, battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = {row.split(":")[0]: row for row in result["world_ui"]["icon_rows"]}
            for squadron in (X_WING_SQUADRON, X_WING_SQUADRON_3):
                self.assertIn(":selected:", rows[str(squadron)], rows)
            self.assertIn(":normal:", rows[str(Y_WING_SQUADRON)], rows)

    def test_double_clicking_a_fighter_over_a_ship_selects_no_ship(self):
        # #665, the owner's case (walk WSU-10 to WSU-12, WSU-18): X-wing squadron 3 flies onto the
        # Nebulon-B, and a double click lands just beside one of its craft (35 units above it: a
        # fighter moves between the two presses), over the ship. FoC picks a craft by its collision
        # mesh or else its Mouse_Collide_Override_Sphere_Radius sphere (50 for every M2 craft) and a
        # ship by its collision mesh, highest contact first: the craft wins and the double click adds
        # X-wing squadrons, never the ship. The old per-unit box missed the craft there and hit the
        # ship's box, which is how the ship got in.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-fighter-over-ship-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "fighter-over-ship", (
                *HUD_OFF, "--eawr-live-order", f"1:move:{X_WING_SQUADRON_3}@{NEBULON_START[0]},{NEBULON_START[1]},0",
                "--eawr-live-input", f"150:dclick:unit={X_WING_3_CRAFT}+@0,0,35"), end_tick=160)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 1, battle["log"])
            # Every drawn unit has a collision mesh from the unit tables; the craft have spheres.
            self.assertGreater(battle["pick_meshes"], 0, battle)
            self.assertGreater(battle["pick_spheres"], 0, battle)
            self.assertEqual(len(battle["double_click_candidates"]), 1, battle["log"])
            candidates = battle["double_click_candidates"][0]
            # The old box pick: the highest box contact under the pointer was the Nebulon-B's.
            boxes = [row for row in candidates if row["box_z"] is not None]
            self.assertTrue(boxes, candidates)
            self.assertEqual(max(boxes, key=lambda row: row["box_z"])["entity"], NEBULON, candidates)
            # The pick volumes: the craft's sphere is the highest contact.
            volumes = [row for row in candidates if row["contact_z"] is not None]
            winner = max(volumes, key=lambda row: row["contact_z"])
            self.assertEqual((winner["entity"], winner["volume"]), (X_WING_SQUADRON_3, "sphere"), candidates)
            self.assertIn("double click: type on screen", battle["log"], candidates)
            x_wings = {row["container"] for row in live["squadrons"] if row["owner"] == 1 and row["sfoil_craft"] > 0}
            self.assertIn(X_WING_SQUADRON_3, battle["selected"], candidates)
            self.assertLessEqual(set(battle["selected"]), x_wings, battle["log"])
            self.assertIs(live["headless_hashes_equal"], True)

    def test_launched_squadron_selects_and_moves_as_one_unit(self):
        # #518: a squadron the Rebel station launches (SK-23) is a squadron like a tick-zero one: it
        # registers with its whole roster when its container appears, its icon takes a click that
        # selects the container, and a right click moves it as one unit. The Empire's launches
        # register the same way. A probe run finds the station's first launch and its tick.
        destination = (-4300.0, 5000.0)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-launched-") as temporary:
            directory = pathlib.Path(temporary)
            code, probe = self._run(directory, "launch-probe", HUD_OFF, end_tick=LAUNCH_PROBE_TICK)
            self.assertEqual(code, 0, probe.get("failure"))
            rows = probe["live_session"]["squadrons"]
            start = [row["container"] for row in rows if not row["launched"]]
            self.assertEqual(start, [2, 3, 4, TIE_SQUADRON, TIE_SQUADRON + 1], rows)
            launched = [row for row in rows if row["launched"]]
            self.assertTrue(any(row["owner"] == 2 for row in launched), rows)
            for row in launched:
                self.assertIn(len(row["members"]), (3, 4, 5, 7), row)
                self.assertGreater(row["container"], max(row["members"]), row)
            # #632 (foc-battle-world-ui WU-37, WU-38): FoC's small white flag sits on the icon of a
            # squadron the local player's hangar launched; the start's squadrons and the enemy's
            # launched squadrons show none.
            flagged = set(probe["world_ui"]["flags_seen"])
            own_launched = {str(row["container"]) for row in launched if row["owner"] == 1}
            self.assertTrue(own_launched & flagged, (own_launched, flagged))
            self.assertLessEqual(flagged, own_launched, probe["world_ui"])
            self.assertFalse(flagged & {str(container) for container in start}, probe["world_ui"])
            self.assertTrue(probe["world_ui"]["flag_rows"], probe["world_ui"])
            rebel = [row for row in launched if row["owner"] == 1]
            self.assertTrue(rebel, rows)
            squadron = min(rebel, key=lambda row: row["container"])
            tick = squadron["seen_tick"]
            self.assertGreater(tick, 0, squadron)
            code, result = self._run(directory, "launched", (
                *HUD_OFF,
                "--eawr-live-input", f"{tick + 30}:click:icon={squadron['container']}",
                "--eawr-live-input", f"{tick + 40}:rclick:@{destination[0]},{destination[1]},0"), end_tick=tick + 400)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live, world = result["battle_input"], result["live_session"], result["world_ui"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            self.assertEqual(battle["selected"], [squadron["container"]], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(line.startswith("move @") and f" units {squadron['container']} " in line
                                for line in battle["log"]), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            for craft in squadron["members"]:
                position = self._position(result, craft)
                self.assertLess(math.dist(destination, position[:2]), 250.0, (craft, position))
            self.assertTrue(any(row.startswith(f"{squadron['container']}:selected:") for row in world["icon_rows"]), world)
            self.assertEqual(world["circles"], len(squadron["members"]), world)

    def test_launched_squadron_locks_its_sfoils(self):
        # #614 (space-abilities AB-31): the S-foil button of a squadron the Rebel station launches
        # locks its craft's S-foils like a tick-zero squadron's: the order reaches every flying craft
        # and each craft plays its DEPLOY clip. The launched craft draw in launch slots, which used
        # to get no S-foil clips (the button was clickable and nothing moved). X-wing squadron 2 is
        # the reference; the station's first launch is an X-wing squadron (SK-23).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-launched-sfoils-") as temporary:
            directory = pathlib.Path(temporary)

            def squadron_row(result, container):
                rows = [row for row in result["live_session"]["squadrons"] if row["container"] == container]
                self.assertEqual(len(rows), 1, result["live_session"]["squadrons"])
                return rows[0]

            def click_sfoil(name, container, tick):
                # SPREAD moves the stacked start squadrons apart, so an icon click picks its own squadron.
                code, result = self._run(directory, name, (
                    *SPREAD, "--eawr-live-input", f"{tick + 30}:click:icon={container}",
                    "--eawr-live-input", f"{tick + 40}:click:ability=0"), end_tick=tick + 160)
                self.assertEqual(code, 0, result.get("failure"))
                battle, live = result["battle_input"], result["live_session"]
                self.assertEqual(battle["scripted_fired"], 2, battle["log"])
                self.assertEqual(battle["selected"], [container], battle["log"])
                self.assertEqual(live["ability_requests"]["issued"], 1, live["ability_requests"])
                self.assertEqual(live["rejected"], [])
                self.assertIs(live["headless_hashes_equal"], True)
                return result, squadron_row(result, container)

            start, start_row = click_sfoil("start-sfoils", X_WING_SQUADRON, 0)
            crafts = len(start_row["members"])
            self.assertGreater(crafts, 0, start_row)
            self.assertEqual((start_row["sfoil_craft"], start_row["sfoils_locked"]), (crafts, crafts), start_row)
            self.assertEqual(start["live_session"]["sfoil_switches"], crafts)

            code, probe = self._run(directory, "launch-probe", HUD_OFF, end_tick=LAUNCH_PROBE_TICK)
            self.assertEqual(code, 0, probe.get("failure"))
            rebel = [row for row in probe["live_session"]["squadrons"] if row["launched"] and row["owner"] == 1]
            self.assertTrue(rebel, probe["live_session"]["squadrons"])
            squadron = min(rebel, key=lambda row: row["container"])
            launched, launched_row = click_sfoil("launched-sfoils", squadron["container"], squadron["seen_tick"])
            crafts = len(launched_row["members"])
            self.assertEqual((launched_row["sfoil_craft"], launched_row["sfoils_locked"]), (crafts, crafts), launched_row)
            self.assertEqual(launched["live_session"]["sfoil_switches"], crafts)

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

    def test_dogfighting_squadrons_share_a_grid_cell(self):
        # #435: X-wing squadron 2 and TIE Interceptor squadron 9 meet mid-map and attack each other.
        # The orders target the team containers (space-fighters FO-04); the craft fire at the other
        # squadron's craft, and once they dogfight both squadrons hold one combat cell, whose icons
        # sit in its grid (foc-battle-world-ui WU-25, WU-26). TIE Interceptor squadron 10, the M2
        # start's other idle Empire squadron, is sent well clear first (OTHER_TIE_SQUADRON) so #511's
        # fire-reveals-the-shooter fog (V-19) cannot let its own idle scan (FT-02) pull it into the
        # same cell once the dogfight starts shooting.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-dogfight-") as temporary:
            directory = pathlib.Path(temporary)
            meeting = f"{MEETING[0]},{MEETING[1]},0"
            clear = f"{CLEAR_OF_DOGFIGHT[0]},{CLEAR_OF_DOGFIGHT[1]},0"
            # The live camera over the meeting point: the view draws (and so points at) what it sees.
            camera = directory / "dogfight-camera.xml"
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            camera.write_text(re.sub(r"<initial [^>]*/>", f'<initial target_x="{MEETING[0]}" target_y="{MEETING[1]}" '
                                     'target_height="0" zoom="0.5" yaw_degrees="0"/>', CAMERA.read_text(encoding="utf-8")),
                              encoding="utf-8")
            code, result = self._run(directory, "dogfight", (
                *HUD_OFF, "--eawr-live-ai", "off",
                "--eawr-live-order", f"1:move:{X_WING_SQUADRON}@{meeting}",
                "--eawr-live-order", f"1:move:{TIE_SQUADRON}@{meeting}",
                "--eawr-live-order", f"1:move:{OTHER_TIE_SQUADRON}@{clear}",
                "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{X_WING_SQUADRON}@{TIE_SQUADRON}",
                "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{TIE_SQUADRON}@{X_WING_SQUADRON}"),
                camera=camera, end_tick=DOGFIGHT_TICK)
            self.assertEqual(code, 0, result.get("failure"))
            live, world = result["live_session"], result["world_ui"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = {row.split(":")[0]: row for row in world["icon_rows"]}
            cells = {}
            for squadron in (X_WING_SQUADRON, TIE_SQUADRON):
                found = re.search(r":grid(-?\d+,-?\d+)$", rows.get(str(squadron), ""))
                self.assertTrue(found, world["icon_rows"])
                cells[squadron] = found.group(1)
            self.assertEqual(cells[X_WING_SQUADRON], cells[TIE_SQUADRON], world["icon_rows"])
            self.assertGreaterEqual(world["max_grid_icons"], 2, world)
            # Squadrons not in a dogfight keep their icons over themselves.
            self.assertTrue(all(":grid" not in row for key, row in rows.items()
                                if key not in (str(X_WING_SQUADRON), str(TIE_SQUADRON))), world["icon_rows"])
            # WU-24 x WU-26 (debug build, foc-battle-world-ui.md): the screen offset that hovers an
            # icon below its squadron reaches a gridded icon the same as any other's, on top of the
            # grid's own within-cell layout. Two squadrons sharing one cell sit one row apart from
            # each other (WU-25, WU-26): for this cell's occupancy of two, combat_grid_slot's row
            # component is 0 for both, so if the offset reached only one of them (or reached them by
            # different amounts) their reported icon Y would differ; it does not.
            ys = {}
            for squadron in (X_WING_SQUADRON, TIE_SQUADRON):
                found = re.search(r":y=(-?\d+)", rows[str(squadron)])
                self.assertTrue(found, world["icon_rows"])
                ys[squadron] = int(found.group(1))
            self.assertEqual(ys[X_WING_SQUADRON], ys[TIE_SQUADRON], world["icon_rows"])

    def _dogfight_run(self, directory, name, inputs, end_tick=DOGFIGHT_TICK):
        # The #435 dogfight (X-wing squadron 2 and TIE squadron 9 in one combat cell at DOGFIGHT_TICK,
        # the live camera over the meeting point) with `inputs` (--eawr-live-input values) added.
        meeting = f"{MEETING[0]},{MEETING[1]},0"
        clear = f"{CLEAR_OF_DOGFIGHT[0]},{CLEAR_OF_DOGFIGHT[1]},0"
        camera = directory / "dogfight-camera.xml"
        shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
        camera.write_text(re.sub(r"<initial [^>]*/>", f'<initial target_x="{MEETING[0]}" target_y="{MEETING[1]}" '
                                 'target_height="0" zoom="0.5" yaw_degrees="0"/>', CAMERA.read_text(encoding="utf-8")),
                          encoding="utf-8")
        return self._run(directory, name, (
            *HUD_OFF, "--eawr-live-ai", "off",
            "--eawr-live-order", f"1:move:{X_WING_SQUADRON}@{meeting}",
            "--eawr-live-order", f"1:move:{TIE_SQUADRON}@{meeting}",
            "--eawr-live-order", f"1:move:{OTHER_TIE_SQUADRON}@{clear}",
            "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{X_WING_SQUADRON}@{TIE_SQUADRON}",
            "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{TIE_SQUADRON}@{X_WING_SQUADRON}",
            *(arg for value in inputs for arg in ("--eawr-live-input", value))),
            camera=camera, end_tick=end_tick)

    def test_the_dogfight_grid_holds_still_while_the_dogfight_holds(self):
        # #564 (WU-26, debug build): the grid's cell point sits at the craft type's Layer_Z_Adjust, a
        # constant, so the icons in a held cell do not bob as the fighters pitch. The same dogfight
        # read at three later ticks: one cell, and every gridded icon on the same screen Y.
        seen = []
        for end_tick in (DOGFIGHT_TICK, DOGFIGHT_TICK + 25, DOGFIGHT_TICK + 50):
            with tempfile.TemporaryDirectory(prefix="eawr-battle-dogfight-still-") as temporary:
                code, result = self._dogfight_run(pathlib.Path(temporary), "dogfight-still", (), end_tick=end_tick)
                self.assertEqual(code, 0, result.get("failure"))
                rows = {row.split(":")[0]: row for row in result["world_ui"]["icon_rows"]}
                seen.append({squadron: re.search(r":y=(-?\d+):grid(-?\d+,-?\d+)$", rows[str(squadron)])
                             for squadron in (X_WING_SQUADRON, TIE_SQUADRON)})
        for tick, found in zip((0, 25, 50), seen):
            self.assertTrue(all(found.values()), (tick, found))
        cells = {match.group(2) for found in seen for match in found.values()}
        self.assertEqual(len(cells), 1, cells)
        for squadron in (X_WING_SQUADRON, TIE_SQUADRON):
            ys = [found[squadron].group(1) for found in seen]
            self.assertEqual(len(set(ys)), 1, (squadron, ys))

    def test_right_clicking_an_enemy_squadron_icon_attacks_the_squadron(self):
        # #553 (WU-23b, FO-04): with X-wing squadron 2 selected by its icon, a right click on the TIE
        # squadron's icon in the dogfight's grid cell orders the attack on that squadron's container.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-icon-rclick-") as temporary:
            code, result = self._dogfight_run(pathlib.Path(temporary), "icon-rclick", (
                f"{DOGFIGHT_TICK - 30}:click:icon={X_WING_SQUADRON}",
                f"{DOGFIGHT_TICK - 20}:rclick:icon={TIE_SQUADRON}"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            self.assertEqual(battle["selected"], [X_WING_SQUADRON], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(line.startswith(f"attack {TIE_SQUADRON} units {X_WING_SQUADRON} ")
                                for line in battle["log"]), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)

    def test_double_clicking_a_dogfighting_squadrons_grid_icon(self):
        # #550 (WU-23a, WU-25, WU-26): the dogfight above, then a double click on X-wing squadron 2's
        # icon in the combat cell's grid. The icon takes the click where it is drawn: squadron 2 is
        # selected and the TIE squadron sharing its cell is not (another craft type); X-wing squadron
        # 3 is still at the spawn stack, far off this camera, so none of its craft is on screen and it
        # is not added (WSU-38).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-dogfight-dclick-") as temporary:
            code, result = self._dogfight_run(pathlib.Path(temporary), "dogfight-dclick",
                                              (f"{DOGFIGHT_TICK - 10}:dclick:icon={X_WING_SQUADRON}",))
            self.assertEqual(code, 0, result.get("failure"))
            battle, world = result["battle_input"], result["world_ui"]
            self.assertEqual(battle["scripted_fired"], 1, battle["log"])
            self.assertEqual(battle["selected"], [X_WING_SQUADRON], battle["log"])
            rows = {row.split(":")[0]: row for row in world["icon_rows"]}
            self.assertRegex(rows[str(X_WING_SQUADRON)], r":selected:.*:grid-?\d+,-?\d+$")
            self.assertIn(":normal:", rows[str(TIE_SQUADRON)], rows)
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)

    def test_unit_cards_show_and_change_the_selection(self):
        # #425 (docs/behaviour/foc-unit-cards.md): the box's selection fills the command bar's cards,
        # one per unit or squadron, grouped by ability; a card click goes through the HUD (never the
        # world layer) and selects like FoC's Tactical_Select_Object, Shift+click deselects, a double
        # click runs the click again, and a hovered card names its type after Encyclopedia_Delay.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-cards-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "cards", (*boxed, "--eawr-live-input", "25:hover:card=0"), end_tick=60)
            self.assertEqual(code, 0, shown.get("failure"))
            battle, hud = shown["battle_input"], shown["hud"]
            cards = battle["unit_cards"]
            self.assertEqual(cards["slots"], 24)
            self.assertGreaterEqual(cards["groups"], 2, cards)
            self.assertIn(CORVETTE, battle["selected"], battle["log"])
            by_slot = {card["slot"]: card for card in cards["cards"]}
            # Every selected unit is on exactly one card; squadron craft only through their squadron.
            members = sorted(member for card in cards["cards"] for member in card["members"])
            self.assertEqual(members, sorted(battle["selected"]))
            self.assertTrue(any(card["squadron"] for card in cards["cards"]), cards)
            # #435: the selection holds a squadron as its container (#424); its one card stands for
            # the container, and selecting the card selects the squadron as one unit.
            for card in cards["cards"]:
                if card["squadron"]:
                    self.assertEqual(card["members"], [card["unit"]], card)
                    self.assertEqual(sum(1 for other in cards["cards"] if card["unit"] in other["members"]), 1, cards)
            # Groups follow UnitAbilityType order, each from a new column.
            abilities = [by_slot[slot]["ability"] for slot in sorted(by_slot)]
            self.assertEqual(abilities, sorted(abilities))
            for previous, card in zip(sorted(by_slot), sorted(by_slot)[1:]):
                if by_slot[card]["ability"] != by_slot[previous]["ability"]:
                    self.assertEqual(card % 2, 0, cards)
            drawn = {card["slot"]: card for card in hud["unit_cards"]["drawn"]}
            self.assertEqual(sorted(drawn), sorted(by_slot))
            for card in drawn.values():
                self.assertTrue(card["icon_drawn"], card)
            tooltip = hud["unit_cards"]["tooltip"]
            self.assertIsNotNone(tooltip, hud["unit_cards"])
            self.assertEqual(tooltip["slot"], 0)
            self.assertTrue(tooltip["text"], tooltip)
            # This suite runs without a font cache (engine-font warnings); every card texture resolves.
            self.assertFalse([line for line in hud["diagnostics"] if "EAWR-UI-0322" in line], hud["diagnostics"])

            first = by_slot[0]
            group = sorted(member for card in cards["cards"] if card["ability"] == first["ability"]
                           for member in card["members"])
            code, picked = self._run(directory, "card-click", (*boxed, "--eawr-live-input", "30:click:card=0"),
                                     end_tick=60)
            self.assertEqual(code, 0, picked.get("failure"))
            self.assertEqual(sorted(picked["battle_input"]["selected"]), group, picked["battle_input"]["log"])
            self.assertEqual(picked["hud"]["unit_cards"]["clicks"], 1)
            # The HUD took the card click: only the box's press and release reached the world layer.
            self.assertEqual(picked["battle_input"]["events"], 2, picked["battle_input"]["log"])
            # UI-C2: a card click is presentation only.
            self.assertEqual(picked["live_session"]["final_state_sha256"], shown["live_session"]["final_state_sha256"])

            squadron = next(card for card in cards["cards"] if card["squadron"])
            # #435: a squadron card's click selects containers, never craft (the M2 Rebel craft are
            # 36-48), and the order that follows goes to those containers and is accepted.
            code, ordered = self._run(directory, "card-order", (
                *boxed, "--eawr-live-input", f"30:click:card={squadron['slot']}",
                "--eawr-live-input", f"35:rclick:@{DESTINATION[0]},{DESTINATION[1]},0"), end_tick=60)
            self.assertEqual(code, 0, ordered.get("failure"))
            chosen = ordered["battle_input"]["selected"]
            self.assertIn(squadron["unit"], chosen, ordered["battle_input"]["log"])
            self.assertTrue(set(chosen).isdisjoint(range(36, 49)), chosen)
            units = ",".join(str(entity) for entity in chosen)
            self.assertTrue(any(line.startswith("move @") and f" units {units} " in line
                                for line in ordered["battle_input"]["log"]), ordered["battle_input"]["log"])
            self.assertEqual(ordered["live_session"]["rejected"], [])
            code, dropped = self._run(directory, "card-shift", (
                *boxed, "--eawr-live-input", f"30:click:card={squadron['slot']}+shift"), end_tick=60)
            self.assertEqual(code, 0, dropped.get("failure"))
            self.assertEqual(sorted(dropped["battle_input"]["selected"]),
                             sorted(set(battle["selected"]) - set(squadron["members"])), dropped["battle_input"]["log"])

            code, twice = self._run(directory, "card-dclick", (*boxed, "--eawr-live-input", "30:dclick:card=0"),
                                    end_tick=60)
            self.assertEqual(code, 0, twice.get("failure"))
            self.assertEqual(twice["hud"]["unit_cards"]["double_clicks"], 1)
            self.assertEqual(twice["hud"]["unit_cards"]["clicks"], 2)
            # C-1, C-2: the first action selects the card's ability group (the Nebulon-B and the MC80
            # share DEFEND); the second runs on that one-group selection and selects the card's own
            # unit (or, on a collapsed stack, its type's card units).
            self.assertEqual(sorted(twice["battle_input"]["selected"]), sorted(first["members"]))

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

    def test_ability_buttons_draw_and_request(self):
        # #454 (docs/behaviour/foc-ability-buttons.md): the box's selection gets one button per
        # ability group under its border, with the engine's icons; a left release requests the ability
        # and a right release autofire, FoC's default keys press the buttons. Since #76 the buttons read
        # the simulation's ability state (a cut ability, the Y-wing's ION_CANNON_SHOT, shows disabled:
        # space-abilities AB-03) and the requests become ability commands in the replay.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-abilities-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "abilities", boxed, end_tick=60)
            self.assertEqual(code, 0, shown.get("failure"))
            battle, drawn = shown["battle_input"], shown["hud"]["ability_buttons"]
            cards = battle["unit_cards"]
            groups = sorted({card["ability"] for card in cards["cards"] if card["ability"] != 0})
            self.assertEqual(drawn["components"], 24)
            self.assertEqual(len(drawn["buttons"]), len(groups), drawn)
            self.assertEqual(drawn["textures_missing"], 0, drawn)
            for button in drawn["buttons"]:
                self.assertTrue(button["icon"].startswith("i_sa_"), button)
                # AB-03: only the cut HUNT is always disabled; ION_CANNON_SHOT reads the squadron
                # container's own ready slot since #561 (space-abilities AB-60).
                self.assertEqual(button["disabled"], button["name"] == "HUNT", button)
                self.assertFalse(button["autofire"], button)
                self.assertEqual(button["recharge"], 1)
                # AB-01: the button of a group spanning columns a..b is special_button_(a + b).
                columns = [card["slot"] // 2 for card in cards["cards"] if card["ability"] == button["ability"]]
                self.assertEqual(button["component"], min(columns) + max(columns) + (1 if button["second"] else 0))
            self.assertEqual(drawn["marks"], [])

            code, clicked = self._run(directory, "ability-click", (
                *boxed, "--eawr-live-input", "30:click:ability=0", "--eawr-live-input", "35:rclick:ability=0",
                "--eawr-live-input", "40:key:O+shift"), end_tick=60)
            self.assertEqual(code, 0, clicked.get("failure"))
            log = clicked["battle_input"]["log"]
            self.assertEqual(clicked["hud"]["ability_buttons"]["clicks"], 1)
            self.assertEqual(clicked["hud"]["ability_buttons"]["right_clicks"], 1)
            # #76: every request reaches the simulation's ability commands (a cut one is refused there).
            requests = clicked["live_session"]["ability_requests"]
            # Shift+O is DEFEND's key; the Nebulon-B's group has DEFEND in the M2 start, and switching
            # it on changes the battle.
            if any(button["name"] == "DEFEND" for button in drawn["buttons"]):
                self.assertEqual(clicked["battle_input"]["ability_bar"]["hotkeys"], 1, log)
                self.assertEqual(requests["issued"] + requests["refused"], 3, requests)
                self.assertNotEqual(clicked["live_session"]["final_state_sha256"],
                                    shown["live_session"]["final_state_sha256"])
            else:
                self.assertEqual(requests["issued"] + requests["refused"], 2, requests)
            self.assertIs(clicked["live_session"]["headless_hashes_equal"], True)
            self.assertEqual(clicked["battle_input"]["selected"], battle["selected"])

            code, demo = self._run(directory, "ability-demo", (*boxed, "--eawr-live-ability-demo", "on"), end_tick=60)
            self.assertEqual(code, 0, demo.get("failure"))
            staged = demo["hud"]["ability_buttons"]
            self.assertTrue(demo["battle_input"]["ability_bar"]["demo"])
            by_rank = staged["buttons"]
            self.assertTrue(any(button["disabled"] for button in by_rank), by_rank)
            self.assertTrue(any(button["autofire"] for button in by_rank), by_rank)
            self.assertTrue(any(button["recharge"] < 1 for button in by_rank), by_rank)
            self.assertGreater(staged["dials_drawn"], 0, staged)
            # #521 (AB-08): a mark that is drawn at all (autofire box, dial or icon) always carries
            # the ability's icon too, including the ready-on-autofire rank; none draws a bare box.
            self.assertTrue(staged["marks"], staged["marks"])
            self.assertTrue(all(mark["icon"] for mark in staged["marks"]), staged["marks"])
            self.assertEqual(staged["textures_missing"], 0, staged)
            self.assertEqual(demo["live_session"]["final_state_sha256"], shown["live_session"]["final_state_sha256"])

    def test_ability_buttons_clear_with_the_selection(self):
        # #534: the ability area mirrors the current selection (docs/behaviour/foc-ability-buttons.md
        # "Interface"); its buttons must not linger once the selection they belonged to is gone,
        # whichever way it went: a deselect click, a squadron clicked off, its last unit dying, or a
        # group thinned down to nothing. Checked on both the model (battle_input.ability_bar) and what
        # the HUD actually drew (hud.ability_buttons), so a stale button can't hide behind a stale model.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-abilities-clear-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "abilities-shown", boxed, end_tick=60)
            self.assertEqual(code, 0, shown.get("failure"))
            cards = shown["battle_input"]["unit_cards"]["cards"]
            self.assertTrue(shown["battle_input"]["ability_bar"]["buttons"], shown["battle_input"]["ability_bar"])
            self.assertTrue(shown["hud"]["ability_buttons"]["buttons"], shown["hud"]["ability_buttons"])

            # A click on empty space deselects everything: no button is left over the empty panel.
            code, cleared = self._run(directory, "abilities-cleared", (
                *boxed, "--eawr-live-input", f"30:click:@{DESTINATION[0]},{DESTINATION[1]},0"), end_tick=60)
            self.assertEqual(code, 0, cleared.get("failure"))
            self.assertEqual(cleared["battle_input"]["selected"], [], cleared["battle_input"]["log"])
            self.assertEqual(cleared["battle_input"]["ability_bar"]["buttons"], [])
            self.assertEqual(cleared["hud"]["ability_buttons"]["buttons"], [])

            # #424: a squadron selected by its icon, then deselected the same way.
            code, squadron_cleared = self._run(directory, "abilities-squadron-cleared", (
                "--eawr-live-input", f"20:click:icon={Y_WING_SQUADRON}",
                "--eawr-live-input", f"30:click:@{DESTINATION[0]},{DESTINATION[1]},0"), end_tick=60)
            self.assertEqual(code, 0, squadron_cleared.get("failure"))
            self.assertEqual(squadron_cleared["battle_input"]["selected"], [], squadron_cleared["battle_input"]["log"])
            self.assertEqual(squadron_cleared["battle_input"]["ability_bar"]["buttons"], [])
            self.assertEqual(squadron_cleared["hud"]["ability_buttons"]["buttons"], [])

            # A group thinned to nothing: Shift+click off a squadron card drops exactly its members
            # (proven by test_unit_cards_show_and_change_the_selection); when it is the sole holder of
            # its ability, that ability's button must go with it, never lingering for the units left.
            squadron_card = next(card for card in cards if card["squadron"])
            self.assertNotEqual(squadron_card["ability"], 0, squadron_card)
            sole_holder = sum(1 for card in cards if card["ability"] == squadron_card["ability"]) == 1
            code, thinned = self._run(directory, "abilities-group-thinned", (
                *boxed, "--eawr-live-input", f"30:click:card={squadron_card['slot']}+shift"), end_tick=60)
            self.assertEqual(code, 0, thinned.get("failure"))
            self.assertTrue(thinned["battle_input"]["selected"], thinned["battle_input"]["log"])
            before, after = shown["battle_input"]["ability_bar"]["buttons"], thinned["battle_input"]["ability_bar"]["buttons"]
            if sole_holder:
                self.assertLess(len(after), len(before), (before, after))
            self.assertEqual(len(thinned["hud"]["ability_buttons"]["buttons"]), len(after))

            # The selection's only unit dying (HD-30 scripted damage, #81) clears the bar too. The
            # Rebel companies start stacked on the spawn point (P-1 picks the topmost contact there),
            # so clearing the stack around the Nebulon-B first (as test_selected_ship_bars_and_-
            # hovered_hardpoints does) is what makes the click land on it alone.
            clear_stack = (*SPREAD, *MC80_ASIDE, "--eawr-live-order", f"1:move:{Y_WING_SQUADRON}@-5500,4400,0")
            code, isolated = self._run(directory, "abilities-lone-nebulon", (
                *clear_stack, "--eawr-live-input", f"250:click:unit={NEBULON}"), end_tick=270)
            self.assertEqual(code, 0, isolated.get("failure"))
            self.assertEqual(isolated["battle_input"]["selected"], [NEBULON], isolated["battle_input"]["log"])
            nebulon_card = next(card for card in isolated["battle_input"]["unit_cards"]["cards"]
                                if NEBULON in card["members"])
            self.assertNotEqual(nebulon_card["ability"], 0, nebulon_card)
            self.assertTrue(isolated["battle_input"]["ability_bar"]["buttons"], isolated["battle_input"]["ability_bar"])
            self.assertTrue(isolated["hud"]["ability_buttons"]["buttons"], isolated["hud"]["ability_buttons"])

            code, died = self._run(directory, "abilities-death", (
                *clear_stack, "--eawr-live-input", f"250:click:unit={NEBULON}",
                "--eawr-live-order", f"255:damage:{NEBULON}@500000"), end_tick=270)
            self.assertEqual(code, 0, died.get("failure"))
            self.assertEqual(died["battle_input"]["selected"], [], died["battle_input"]["log"])
            self.assertEqual(died["battle_input"]["ability_bar"]["buttons"], [])
            self.assertEqual(died["hud"]["ability_buttons"]["buttons"], [])

    def test_card_click_uses_selection_changed_in_the_same_frame(self):
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-cards-same-frame-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "boxed", boxed, end_tick=40)
            self.assertEqual(code, 0, shown.get("failure"))
            cards = shown["battle_input"]["unit_cards"]["cards"]
            self.assertGreater(len(cards), 1, cards)
            ability = next(card["ability"] for card in cards if card["slot"] == 0)
            expected = sorted(member for card in cards if card["ability"] == ability
                              for member in card["members"])
            self.assertNotEqual(expected, sorted(shown["battle_input"]["selected"]))

            # Both events are replayed after the frame's initial card refresh. The click must
            # resolve slot 0 against the box's new selection, then publish those cards to the HUD.
            code, picked = self._run(directory, "same-frame", (
                *boxed, "--eawr-live-input", "20:click:card=0"), end_tick=40)
            self.assertEqual(code, 0, picked.get("failure"))
            battle = picked["battle_input"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            self.assertEqual(battle["unit_cards"]["clicks"], 1, battle["log"])
            self.assertEqual(sorted(battle["selected"]), expected, battle["log"])
            self.assertEqual(picked["hud"]["unit_cards"]["clicks"], 1)
            self.assertEqual(battle["events"], 2, battle["log"])
            self.assertEqual(sorted(member for card in battle["unit_cards"]["cards"]
                                    for member in card["members"]), expected)

    def test_bad_inputs_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-bad-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "unknown", ("--eawr-live-input", "5:click:unit=999"))
            self.assertNotEqual(code, 0)
            self.assertIn("is not a unit of the start", result["failure"])
            code, result = self._run(directory, "malformed", ("--eawr-live-input", "5:jump:unit=5"))
            self.assertNotEqual(code, 0)
            self.assertIn("--eawr-live-input expects", result["failure"])


if __name__ == "__main__":
    unittest.main()
