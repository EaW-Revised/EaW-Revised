"""Shared fixtures and runners for test_battle_input."""

import collections
import contextlib
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
from test_space_map_mode import decode_png, strict_json  # noqa: E402
from viewer_mode_sources import source_text  # noqa: E402

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
    return source_text(path)


class BattleInputRunner:
    def _run(self, directory: pathlib.Path, name: str, extra=(), camera=CAMERA, end_tick=END_TICK,
             map_path=CORUSCANT, session="m2", env=None, engine_args=()):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        report = directory / f"{name}.json"
        completed = subprocess.run(
            [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), *engine_args, "--",
             "--eawr-map", map_path, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
             "--eawr-report", str(report), "--eawr-capture", str(directory / f"{name}.png"),
             "--eawr-populate", *([] if camera is None else ["--eawr-map-camera-config", str(camera)]),
             "--eawr-live-session", session,
             "--eawr-live-ticks", str(end_tick), *extra],
            cwd=ROOT, env=None if env is None else {**os.environ, **env},
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
        self.assertTrue(report.is_file(), completed.stdout[-4000:])
        return completed.returncode, strict_json(report.read_text(encoding="utf-8"))


    def _position(self, result, entity):
        units = {unit["entity"]: unit["position"] for unit in result["live_session"]["own_units"]}
        self.assertIn(entity, units)
        return units[entity]



    # #497: the local player's right click on an enemy attacks it in the M2 battle with fog on, as
    # the owner gives the order. The Nebulon-B spots: it stops 1100 units from the target, inside
    # its 1200-unit sight, so the target shows through the fog. The corvette waits 3000 units from
    # the target, beyond its own sight and its 800-unit weapons, so any hit it lands comes from the
    # order: it closes (OR-02 to OR-06) and fires. The AI is off so the Empire stays where the debug
    # moves put it; the camera follows the corvette, then the spotter.
    def _attack_through_fog(self, directory, name, target, target_point, order_tick, end_tick, reticle=False):
        camera = directory / f"{name}-camera.xml"
        shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
        camera.write_text(re.sub(r"<initial [^>]*/>", f'<initial target_x="{ATTACKER_POINT[0]}" '
                                 f'target_y="{ATTACKER_POINT[1]}" target_height="0" zoom="0.5" yaw_degrees="0"/>',
                                 CAMERA.read_text(encoding="utf-8")), encoding="utf-8")
        code, result = self._run(directory, name, (
            *HUD_OFF, "--eawr-live-ai", "off", "--eawr-live-step", "10",
            # #531: the run draws every unit (the Acclamator may not be spotted yet at the hover);
            # the sim's fog and the order are unchanged.
            *(("--eawr-live-reveal", "on") if reticle else ()),
            "--eawr-live-order", f"1:move:{CORVETTE}@{ATTACKER_POINT[0]},{ATTACKER_POINT[1]},0",
            "--eawr-live-order", f"1:move:{NEBULON}@{SPOTTER_POINT[0]},{SPOTTER_POINT[1]},0",
            "--eawr-live-order", f"1:move:{target}@{target_point[0]},{target_point[1]},0",
            "--eawr-live-follow-group", f"1:{CORVETTE}",
            # #531: a reticle is only there to click while its target is on screen, so that run
            # follows the target for the last stretch instead of the spotter.
            "--eawr-live-follow-group", f"{order_tick - (100 if reticle else 200)}:{target if reticle else NEBULON}",
            "--eawr-live-input", f"900:click:unit={CORVETTE}",
            # #531: the hover draws the target's reticles; the right click lands on the first one.
            *(("--eawr-live-input", f"{order_tick - 20}:hover:unit={target}") if reticle else ()),
            "--eawr-live-input",
            f"{order_tick}:rclick:reticle={target}:first" if reticle else f"{order_tick}:rclick:unit={target}"),
            camera=camera, end_tick=end_tick)
        self.assertEqual(code, 0, result.get("failure"))
        self.result = result
        battle, live = result["battle_input"], result["live_session"]
        self.assertEqual((battle["scripted_fired"], battle["orders"]), (3 if reticle else 2, 1), battle["log"])
        self.assertEqual(battle["selected"], [CORVETTE], battle["log"])
        self.assertEqual(live["rejected"], [])
        self.assertIs(live["headless_hashes_equal"], True)
        # Fog is on: the local player does not see every unit.
        self.assertIs(live["reveal"], bool(reticle))
        if not reticle:
            self.assertGreater(live["hidden_units"], 0)
        order = next(line for line in battle["log"] if line.startswith("attack "))
        named = r" hardpoint (\d+)" if reticle else ""
        match = re.fullmatch(rf"attack {target}{named} units {CORVETTE} tick (\d+)", order)
        self.assertTrue(match, battle["log"])
        self.hardpoint = int(match.group(1)) if reticle else None
        return int(match.group(match.lastindex)), live


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
