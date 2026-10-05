"""Contracts for the P1-06 viewer map mode.

The structural half runs everywhere: it reads committed sources and the
committed synthetic fixture, and needs neither Godot nor an installed corpus.

The graphical half is opt-in. Set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE to run the synthetic map through the pinned Godot binary
and check its decoded-PNG evidence. Set EAWR_EAW_GAME_ROOT as well to add a
read-only run over the pinned land reference map; that run's capture is never
written to the repository, only its numbers and hashes are asserted.

P1-11 adds `--eawr-populate`: the synthetic placement fixture
(tests/assets/fixtures/scene_fixture.py) is populated in the graphical half,
twice, to show the scene hash is repeatable, and the pinned land map's
populated scene hash is checked against plan/inventories/unresolved-placements.json.

P1-04 adds `--eawr-lighting <sh|hemisphere|off>`, `--eawr-shadows <on|off>` and
`--eawr-environment <default|map>`. The synthetic placement fixture is lit
under both policies with shadows on; the pinned land map is lit with SH and
the alo-viewer default environment, numbers only.
"""


import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from map_mode_test_support import (
    FAMILIES, FIXTURE, FORBIDDEN, Image,
    LAND_RUNTIME_PATH, LAND_RUNTIME_ROLE, MapModeRunner, REFERENCE_MAPS,
    ROOT, UNRESOLVED, evidence_frame, expected_default_sh_coefficients,
    fixture_bytes, install_fixture, json, math,
    mode_source, os, pathlib, pinned_reference,
    re, scene_bloom_reference, scene_fixture, source_text,
    struct, subprocess, sys, tempfile,
    unittest,
)

from map_mode_structure_cases import MapModeStructureCases
from map_mode_structure_cases import PopulatedStructureCases
from map_mode_structure_cases import LightingStructureCases
from map_mode_graphical_cases import MapModeGraphicalCases


class MapModeStructure(MapModeStructureCases, unittest.TestCase):
    pass


class PopulatedStructure(PopulatedStructureCases, unittest.TestCase):
    pass


class LightingStructure(LightingStructureCases, unittest.TestCase):
    pass


class MapModeGraphical(MapModeRunner, MapModeGraphicalCases, unittest.TestCase):
    pass


def load_tests(loader, tests, pattern):
    """Compose only the legacy classes, retaining loader order and IDs."""
    return unittest.TestSuite(loader.loadTestsFromTestCase(case) for case in (
        LightingStructure,
        MapModeGraphical,
        MapModeStructure,
        PopulatedStructure,
    ))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
