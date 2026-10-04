"""Shared fixtures and runners for test_live_session."""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from test_space_map_mode import decode_png, strict_json  # noqa: E402
from viewer_mode_sources import source_text  # noqa: E402

CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"
CAMERA = ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml"
# The Rebel Corellian_Corvette, unit 5 of the M2 start (sim_headless --skirmish m2), stands on
# the Rebel spawn at (-5057, 4700) facing 336 degrees. It is ordered north-east at tick 15 and
# to face west at tick 150.
CORVETTE = 5
# Unit 1 is the Rebel station, Skirmish_Rebel_Star_Base_1 (death clone Rebel_Star_Base_1_Death_Clone).
STATION = 1
# Unit 6 is the Rebel Nebulon_B_Frigate. Its HardPoints: FL, FR, BL, BR weapons (each with a
# Death_Breakoff_Prop), then the engines (none).
NEBULON = 6
# Unit 7 is the Rebel Calamari_Cruiser (the MC80, SK-22); unit 8 is the Empire station,
# Skirmish_Empire_Star_Base_1.
MC80 = 7
EMPIRE_STATION = 8
# 1884ff2f (#518): the updated roster puts the Acclamator at 12, after the Tartan at 11.
ACCLAMATOR = 12
# ea57f4da (#1102): drawn TIE craft 50 crosses both fog edges in the current arrival trace.
FOG_CROSSING_FIGHTER = 50
# Unit 6 is the Rebel Nebulon_B_Frigate; HardPoints index 0 is HP_Nebulon_Weapon_FL, 4 the engines.
NEBULON = 6
NEBULON_FL = 0
NEBULON_ENGINES = 4
ORDERS = ("--eawr-live-order", f"15:move:{CORVETTE}@-4600,5150,0",
          "--eawr-live-order", f"150:face:{CORVETTE}@-5600,4700,0")
CAPTURE_TICKS = (0, 60, 120, 180, 240)
# #80: the #74 duel (tests/fidelity/S-15-duel-damage.json) as sim_headless --scenario writes its
# replay; the Nebulon-B (unit 1, Rebel player 2) kills the Tartan (unit 2) near tick 718. #475's
# collection-tree candidate order (CO-03) moved this from 713 once it landed in combat.cpp's
# hardpoint and priority-set scans (space-targeting CO rules); re-pinned after the #435 base-merge.
DUEL = ROOT / "tests/fidelity/fixtures/S-15-duel-damage.eawr-replay"
DUEL_CAMERA = ROOT / "apps/viewer/project/config/coruscant-duel-camera.xml"
# 720: the first --eawr-live-step-3 frame past the fatal hit at tick 718 (its impact and the
# death explosion); 719 is not a capture tick since it falls between frames at that step.
DUEL_CAPTURE_TICKS = (90, 300, 600, 720)
DUEL_DEATH_TICK = 718
DUEL_ARGS = ("--eawr-live-replay", str(DUEL), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
             "--eawr-live-step", "3", "--eawr-environment", "map", "--eawr-lighting", "sh")
HIT_REASONS = ("shield", "armor_reduced", "detonation")
# #456: S-28 (tests/fidelity/S-28-squadron-vs-capital.json) as sim_headless --scenario writes its
# replay; the Acclamator (unit 1) fires concussion missiles at the corvette (unit 2), the first hit
# at tick 249.
S28 = ROOT / "tests/fidelity/fixtures/S-28-squadron-vs-capital.eawr-replay"
# The session's own simulation thread keeps ticking in the background past a held presented tick
# (BEP-02), so a run driven past --eawr-live-ticks can see a later missile the presented window
# never actually reached; bound comparisons to this same window on every --eawr-live-step tested.
S28_LIVE_TICKS = 300
# The live window-close test: how long the battle runs before the close, and the bound on the time
# from the close request to the process exit (FoC's own close is instant; the rig manages about a
# second, the bound leaves room for a busy desktop).
LIVE_QUIT_BATTLE_SECONDS = 60
LIVE_QUIT_EXIT_BOUND_SECONDS = 3.0


def hit_rows_per_tick(effects):
    # The battle effects' spawn log, as {tick: hit particles spawned}.
    counts = {}
    for tick, key, _ in effects["spawn_log"]:
        if key.split(":")[0] in HIT_REASONS:
            counts[tick] = counts.get(tick, 0) + 1
    return counts


def read(path: str) -> str:
    return source_text(path)


class LiveSessionRunner:
    def _run(self, directory: pathlib.Path, name: str, extra=(), session=("--eawr-live-session", "m2"), camera=CAMERA, env=None, map_name=CORUSCANT, engine_args=()):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        report = directory / f"{name}.json"
        camera_args = ("--eawr-map-camera-config", str(camera)) if camera is not None else ()
        completed = subprocess.run(
            [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), *engine_args, "--",
             "--eawr-map", map_name, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
             "--eawr-report", str(report), "--eawr-capture", str(directory / f"{name}.png"),
             "--eawr-populate", *camera_args, *session, *extra],
            cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        self.assertTrue(report.is_file(), completed.stdout[-4000:])
        return completed.returncode, strict_json(report.read_text(encoding="utf-8"))


    def _battle_end(self, directory, name, station, result_name, extra=()):
        # #453: the station falls at tick 15, so the battle ends at 225 (VT-11). Step 3: frame n
        # shows tick 3(n - 1); tick 225 is frame 76.
        return self._run(directory, name, (
            "--eawr-live-order", f"15:damage:{station}@1000000", "--eawr-live-ticks", "300",
            "--eawr-live-step", "3", "--eawr-live-workers", "2", "--eawr-live-capture-ticks", "12,18",
            # A capture shows the HUD as the frame before drew it: the end panel, opened on frame 76,
            # is taken a few frames later.
            "--eawr-live-capture-frames", "80",
            "--eawr-live-replay-out", str(directory / f"{name}.eawr-replay"), *extra))


    def assert_map_teardown_before_host_destruction(self, text):
        # #961: Godot deletes the host's child nodes before its extension destructor.
        # The audio players are those children, so releasing the map from that
        # destructor dereferenced freed nodes even when --eawr-audio was off.
        # Check the actual lifecycle trace as well as the exit code: a stale
        # pointer can survive several clean exits before the allocator reuses it.
        teardown = text.index("map mode teardown begins")
        freed = text.index("space view freed")
        host = text.index("viewer host freed (members follow)")
        self.assertLess(teardown, freed, text[-4000:])
        self.assertLess(freed, host, text[-4000:])
