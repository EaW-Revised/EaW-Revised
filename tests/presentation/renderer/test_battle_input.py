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


import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
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

from battle_input_source_cases import BattleInputSourceCases
from battle_input_camera_cases import BattleInputCameraCases
from battle_input_order_cases import BattleInputOrderCases
from battle_input_card_cases import BattleInputCardCases
from battle_input_production_cases import BattleInputProductionCases


class BattleInputSources(BattleInputSourceCases, unittest.TestCase):
    pass


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class BattleInputGraphical(BattleInputRunner, BattleInputCameraCases, BattleInputOrderCases, BattleInputCardCases, BattleInputProductionCases, unittest.TestCase):
    pass


def load_tests(loader, tests, pattern):
    """Compose only the legacy classes, retaining loader order and IDs."""
    return unittest.TestSuite(loader.loadTestsFromTestCase(case) for case in (
        BattleInputGraphical,
        BattleInputSources,
    ))


if __name__ == "__main__":
    unittest.main()
