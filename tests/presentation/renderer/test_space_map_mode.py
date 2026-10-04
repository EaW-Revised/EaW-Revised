"""Contracts for E-space-primary-sky-v1 (P1-06, #27): the environment-only
space map mode.

The structural half runs everywhere from committed sources and the original
synthetic fixture (tests/assets/fixtures/space_environment_fixture.py).

The graphical half is opt-in: set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE to run the synthetic space map, its negative controls and
the land refusal through the pinned Godot binary. Set EAWR_EAW_GAME_ROOT as
well for a read-only run over the pinned Alderaan space map (a regression
fixture in the reference-map inventory, not the M1 reference): it must report
the current pinned `space_blocked` classification (ALDERAAN_BLOCKER) exactly.
Its capture is never written to the repository.

Nothing here claims original visual parity, environment completeness, Planet,
Nebula, secondary sky, cloud or water rendering.
"""


import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from space_map_test_support import (
    ALDERAAN_BLOCKER, ALDERAAN_SPACE, COVERAGE, NONE,
    REFERENCE_MAPS, ROOT, SPACE_STATUSES, SpaceMapRunner,
    _reject_constant, decode_png, fixture, json,
    math, mode_source, os, pathlib,
    read, read_pgm, scene_fixture, source_text,
    strict_json, struct, subprocess, sys,
    tempfile, unittest, zlib,
)

from space_map_source_cases import SpaceSourceCases
from space_map_graphical_cases import SpaceGraphicalCases


class SpaceStructure(SpaceSourceCases, unittest.TestCase):
    pass


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical space map mode")
class SpaceGraphical(SpaceMapRunner, SpaceGraphicalCases, unittest.TestCase):
    pass


def load_tests(loader, tests, pattern):
    """Compose only the legacy classes, retaining loader order and IDs."""
    return unittest.TestSuite(loader.loadTestsFromTestCase(case) for case in (
        SpaceGraphical,
        SpaceStructure,
    ))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
