"""Contracts for #80 part A: the production viewer runs the M2 tactical session live.

`--eawr-live-session m2` on Coruscant with `--eawr-populate` runs the authoritative session of
the M2 skirmish start (plan/phase-2/m2-skirmish.md) on its own simulation thread and draws its
units from the published snapshots, hiding what the local player does not see.
`--eawr-live-order` injects an order through the next-tick command path.

The structural half runs everywhere from committed sources. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT to run the Rebel
corvette's move and turn with captures, the viewer-attached hashes against a headless replay of
the recorded command stream (inside the viewer), and two worker counts against each other.
Captures stay in a temporary directory; nothing is written to the repository.
"""


import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from live_session_test_support import (
    ACCLAMATOR, CAMERA, CAPTURE_TICKS, CORUSCANT,
    CORVETTE, DUEL, DUEL_ARGS, DUEL_CAMERA,
    DUEL_CAPTURE_TICKS, DUEL_DEATH_TICK, EMPIRE_STATION, HIT_REASONS,
    LIVE_QUIT_BATTLE_SECONDS, LIVE_QUIT_EXIT_BOUND_SECONDS, LiveSessionRunner, MC80,
    NEBULON, NEBULON_ENGINES, NEBULON_FL, ORDERS,
    ROOT, S28, S28_LIVE_TICKS, STATION,
    decode_png, hit_rows_per_tick, json, os,
    pathlib, re, read, shutil,
    source_text, strict_json, subprocess, sys,
    tempfile, time, unittest,
)

from live_session_source_cases import LiveSessionSourceCases
from live_session_replay_cases import LiveSessionReplayCases
from live_session_effect_cases import LiveSessionEffectCases
from live_session_audio_cases import LiveSessionAudioCases
from live_session_death_cases import LiveSessionDeathCases
from space_hazard_cases import SpaceHazardsGpu
from test_battle_flow import BattleFlowGpu
from test_area_damage_capture import AreaDamageCapture
from test_fog_ghosts import FogGhostsGraphical
from test_barrage_capture import BarrageCapture
from test_manual_delay_capture import ManualDelayCapture


from test_pad_sale import PadSaleGraphical


class LiveSessionSources(LiveSessionSourceCases, unittest.TestCase):
    pass


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class LiveSessionGraphical(LiveSessionRunner, LiveSessionReplayCases, LiveSessionEffectCases, LiveSessionAudioCases, LiveSessionDeathCases, unittest.TestCase):
    pass


def load_tests(loader, tests, pattern):
    """Compose only the legacy classes, retaining loader order and IDs."""
    return unittest.TestSuite(loader.loadTestsFromTestCase(case) for case in (
        LiveSessionGraphical,
        LiveSessionSources,
        BattleFlowGpu,
        PadSaleGraphical,
        SpaceHazardsGpu,
        AreaDamageCapture,
        FogGhostsGraphical,
        BarrageCapture,
        ManualDelayCapture,
    ))


if __name__ == "__main__":
    unittest.main()
