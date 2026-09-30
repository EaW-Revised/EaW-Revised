#!/usr/bin/env python3
"""Drift guard for the pinned P1-05 land/space TED reference checkpoints.

The pinned values live in ``plan/inventories/map-reference-maps.json``.  This
test carries an independent copy of them, so an edit to the inventory that is
not also made here fails, and an accidental loader regression that changes a
count fails the moment the inventory is regenerated.

Each entry has a role.  ``m1_reference`` marks the owner's M1 reference maps
(one land, one space; plan/phase-1/README.md).  ``regression_fixture`` marks the
earlier Alderaan campaign pins, which stay so tests measured on them keep
measuring the same map.

The structural half runs everywhere, including CI, because it reads only the
committed inventory.  The corpus half is opt-in: pass ``--validator`` and
``--game-root`` to re-measure the two maps against a read-only installation and
confirm the pin still describes reality.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


INVENTORY = Path(__file__).resolve().parents[2] / "plan" / "inventories" / "map-reference-maps.json"

# The independent pin.  These numbers were measured from a read-only install
# and are deliberately duplicated here rather than derived from the inventory.
EXPECTED = {('foc', 'data/art/maps/_mp_land_naboo.ted'): {'role': 'm1_reference',
                                               'profile': 'foc',
                                               'sha256': 'c2d8eadb44e17eb259fa169ef58733e42ca49119398fe4569c37fdb8d9662d70',
                                               'checkpoint': {'kind': 'land',
                                                              'semantic_complete': True,
                                                              'stored_size': 4384075,
                                                              'terrain_width': 249,
                                                              'terrain_height': 249,
                                                              'terrain_samples': 62001,
                                                              'terrain_materials': 64,
                                                              'environments': 2,
                                                              'water_records': 17,
                                                              'placements': 1697,
                                                              'placements_crc_absent': 0,
                                                              'placements_crc_missing': 0,
                                                              'placements_crc_collision': 0,
                                                              'placements_crc_unique': 1697,
                                                              'placements_model_renderable': 1697,
                                                              'placements_model_unresolved': 0,
                                                              'placements_model_undeclared': 0,
                                                              'placements_model_unknown': 0,
                                                              'texture_references': 112,
                                                              'textures_resolved': 112,
                                                              'sky_references': 2,
                                                              'skies_resolved': 2,
                                                              'skies_model_renderable': 2,
                                                              'notices': 1},
                                               'semantics': {'volumes': 1,
                                                             'water_textures': 2,
                                                             'declared_extents': True,
                                                             'issues': {'water_stream_invalid': 1},
                                                             'terrain_height_range': [-4065, 5428],
                                                             'terrain_checkpoints': [{'index': 0,
                                                                                      'height': 137,
                                                                                      'material': 0,
                                                                                      'intensity': 255},
                                                                                     {'index': 1,
                                                                                      'height': -294,
                                                                                      'material': 16,
                                                                                      'intensity': 255},
                                                                                     {'index': 248,
                                                                                      'height': 716,
                                                                                      'material': 17,
                                                                                      'intensity': 255},
                                                                                     {'index': 31000,
                                                                                      'height': 2706,
                                                                                      'material': 59,
                                                                                      'intensity': 255},
                                                                                     {'index': 61752,
                                                                                      'height': 716,
                                                                                      'material': 0,
                                                                                      'intensity': 255},
                                                                                     {'index': 62000,
                                                                                      'height': 723,
                                                                                      'material': 0,
                                                                                      'intensity': 255}],
                                                             'three_axis_records': []}},
 ('foc', 'data/art/maps/_mp_space_coruscant.ted'): {'role': 'm1_reference',
                                                    'profile': 'foc',
                                                    'sha256': '91a1fd50ae8ac521e43773f60a3743d64eceb3a00736d80f0ae2234d95108286',
                                                    'checkpoint': {'kind': 'space',
                                                                   'semantic_complete': True,
                                                                   'stored_size': 94765,
                                                                   'terrain_width': 0,
                                                                   'terrain_height': 0,
                                                                   'terrain_samples': 0,
                                                                   'terrain_materials': 0,
                                                                   'environments': 1,
                                                                   'water_records': 0,
                                                                   'placements': 58,
                                                                   'placements_crc_absent': 0,
                                                                   'placements_crc_missing': 0,
                                                                   'placements_crc_collision': 0,
                                                                   'placements_crc_unique': 58,
                                                                   'placements_model_renderable': 58,
                                                                   'placements_model_unresolved': 0,
                                                                   'placements_model_undeclared': 0,
                                                                   'placements_model_unknown': 0,
                                                                   'texture_references': 1,
                                                                   'textures_resolved': 1,
                                                                   'sky_references': 2,
                                                                   'skies_resolved': 2,
                                                                   'skies_model_renderable': 2,
                                                                   'notices': 2},
                                                    'semantics': {'volumes': 2,
                                                                  'water_textures': 0,
                                                                  'declared_extents': True,
                                                                  'issues': {'orientation_three_axis': 2},
                                                                  'three_axis_records': [{'ordinal': 1,
                                                                                          'object_id': 67169,
                                                                                          'type_crc': '0xAEF6E282',
                                                                                          'type': 'N_Gravity_Well_Station',
                                                                                          'position': [-228.943024,
                                                                                                       52.1383057,
                                                                                                       0],
                                                                                          'euler_degrees': [338.98999,
                                                                                                            138,
                                                                                                            181]},
                                                                                         {'ordinal': 16,
                                                                                          'object_id': 4415,
                                                                                          'type_crc': '0x95CBD2EC',
                                                                                          'type': 'Urban_Backdrop_Large',
                                                                                          'position': [2212.72339,
                                                                                                       1270.88721,
                                                                                                       -4300],
                                                                                          'euler_degrees': [0,
                                                                                                            329,
                                                                                                            254]}]}},
 ('eaw', 'data/art/maps/_mp_land_naboo.ted'): {'role': 'regression_fixture',
                                               'profile': 'eaw',
                                               'sha256': 'd9ec66494bcbe5d48d9998eed5e95d6438bed1d2ccf3ec2441276cb7002c442c',
                                               'checkpoint': {'kind': 'land',
                                                              'semantic_complete': True,
                                                              'stored_size': 3211461,
                                                              'terrain_width': 249,
                                                              'terrain_height': 249,
                                                              'terrain_samples': 62001,
                                                              'terrain_materials': 64,
                                                              'environments': 2,
                                                              'water_records': 17,
                                                              'placements': 1709,
                                                              'placements_crc_absent': 0,
                                                              'placements_crc_missing': 0,
                                                              'placements_crc_collision': 0,
                                                              'placements_crc_unique': 1709,
                                                              'placements_model_renderable': 1709,
                                                              'placements_model_unresolved': 0,
                                                              'placements_model_undeclared': 0,
                                                              'placements_model_unknown': 0,
                                                              'texture_references': 106,
                                                              'textures_resolved': 106,
                                                              'sky_references': 2,
                                                              'skies_resolved': 2,
                                                              'skies_model_renderable': 2,
                                                              'notices': 1},
                                               'semantics': {'volumes': 1,
                                                             'water_textures': 2,
                                                             'declared_extents': False,
                                                             'issues': {'water_stream_invalid': 1},
                                                             'terrain_height_range': [-4065, 5428],
                                                             'terrain_checkpoints': [{'index': 0,
                                                                                      'height': 137,
                                                                                      'material': 0,
                                                                                      'intensity': 255},
                                                                                     {'index': 1,
                                                                                      'height': -294,
                                                                                      'material': 16,
                                                                                      'intensity': 255},
                                                                                     {'index': 248,
                                                                                      'height': 716,
                                                                                      'material': 17,
                                                                                      'intensity': 255},
                                                                                     {'index': 31000,
                                                                                      'height': 2706,
                                                                                      'material': 7,
                                                                                      'intensity': 255},
                                                                                     {'index': 61752,
                                                                                      'height': 716,
                                                                                      'material': 0,
                                                                                      'intensity': 255},
                                                                                     {'index': 62000,
                                                                                      'height': 723,
                                                                                      'material': 0,
                                                                                      'intensity': 255}],
                                                             'three_axis_records': []}},
 ('eaw', 'data/art/maps/_mp_space_coruscant.ted'): {'role': 'regression_fixture',
                                                    'profile': 'eaw',
                                                    'sha256': '745b0f89d10867a4875eaeddae109154fef2e3f77adfe6b02888576b2cdd139c',
                                                    'checkpoint': {'kind': 'space',
                                                                   'semantic_complete': True,
                                                                   'stored_size': 65219,
                                                                   'terrain_width': 0,
                                                                   'terrain_height': 0,
                                                                   'terrain_samples': 0,
                                                                   'terrain_materials': 0,
                                                                   'environments': 1,
                                                                   'water_records': 0,
                                                                   'placements': 60,
                                                                   'placements_crc_absent': 0,
                                                                   'placements_crc_missing': 0,
                                                                   'placements_crc_collision': 0,
                                                                   'placements_crc_unique': 60,
                                                                   'placements_model_renderable': 60,
                                                                   'placements_model_unresolved': 0,
                                                                   'placements_model_undeclared': 0,
                                                                   'placements_model_unknown': 0,
                                                                   'texture_references': 1,
                                                                   'textures_resolved': 1,
                                                                   'sky_references': 2,
                                                                   'skies_resolved': 2,
                                                                   'skies_model_renderable': 2,
                                                                   'notices': 1},
                                                    'semantics': {'volumes': 2,
                                                                  'water_textures': 0,
                                                                  'declared_extents': False,
                                                                  'issues': {'orientation_three_axis': 1},
                                                                  'three_axis_records': [{'ordinal': 24,
                                                                                          'object_id': 4415,
                                                                                          'type_crc': '0x95CBD2EC',
                                                                                          'type': 'Urban_Backdrop_Large',
                                                                                          'position': [2212.72339,
                                                                                                       1270.88721,
                                                                                                       -4300],
                                                                                          'euler_degrees': [0,
                                                                                                            329,
                                                                                                            254]}]}},
 ('eaw', 'data/art/maps/_land_planet_alderaan_02.ted'): {'role': 'regression_fixture',
                                                         'profile': 'eaw',
                                                         'sha256': 'e4b5980c05f99d685a5faa37ea6fbeb5923f0ef62c8890c4ce786f7dde08dc12',
                                                         'checkpoint': {'kind': 'land',
                                                                        'semantic_complete': True,
                                                                        'stored_size': 1375866,
                                                                        'terrain_width': 187,
                                                                        'terrain_height': 187,
                                                                        'terrain_samples': 34969,
                                                                        'terrain_materials': 64,
                                                                        'environments': 4,
                                                                        'water_records': 21,
                                                                        'placements': 606,
                                                                        'placements_crc_absent': 0,
                                                                        'placements_crc_missing': 0,
                                                                        'placements_crc_collision': 0,
                                                                        'placements_crc_unique': 606,
                                                                        'placements_model_renderable': 606,
                                                                        'placements_model_unresolved': 0,
                                                                        'placements_model_undeclared': 0,
                                                                        'placements_model_unknown': 0,
                                                                        'texture_references': 96,
                                                                        'textures_resolved': 96,
                                                                        'sky_references': 4,
                                                                        'skies_resolved': 4,
                                                                        'skies_model_renderable': 4,
                                                                        'notices': 3},
                                                         'semantics': {'volumes': 1,
                                                                       'water_textures': 2,
                                                                       'declared_extents': False,
                                                                       'issues': {'orientation_three_axis': 2,
                                                                                  'water_stream_invalid': 1},
                                                                       'terrain_height_range': [-570,
                                                                                                7410],
                                                                       'terrain_checkpoints': [{'index': 0,
                                                                                                'height': 148,
                                                                                                'material': 19,
                                                                                                'intensity': 255},
                                                                                               {'index': 1,
                                                                                                'height': 237,
                                                                                                'material': 19,
                                                                                                'intensity': 255},
                                                                                               {'index': 186,
                                                                                                'height': -61,
                                                                                                'material': 0,
                                                                                                'intensity': 255},
                                                                                               {'index': 17484,
                                                                                                'height': 1025,
                                                                                                'material': 35,
                                                                                                'intensity': 254},
                                                                                               {'index': 34782,
                                                                                                'height': 3034,
                                                                                                'material': 1,
                                                                                                'intensity': 255},
                                                                                               {'index': 34968,
                                                                                                'height': 5090,
                                                                                                'material': 1,
                                                                                                'intensity': 255}],
                                                                       'three_axis_records': [{'ordinal': 47,
                                                                                               'object_id': 32831,
                                                                                               'type_crc': '0x8BE3542C',
                                                                                               'type': 'Land_Speeder',
                                                                                               'position': [1861.21484,
                                                                                                            2330.83643,
                                                                                                            149.937393],
                                                                                               'euler_degrees': [-0.318490565,
                                                                                                                 -0.226374283,
                                                                                                                 218.881042]},
                                                                                              {'ordinal': 48,
                                                                                               'object_id': 32830,
                                                                                               'type_crc': '0x8BE3542C',
                                                                                               'type': 'Land_Speeder',
                                                                                               'position': [1900.28833,
                                                                                                            2374.08276,
                                                                                                            149.912109],
                                                                                               'euler_degrees': [-0.132363409,
                                                                                                                 0.378899276,
                                                                                                                 221.327698]}]}},
 ('eaw', 'data/art/maps/_space_planet_alderaan_01.ted'): {'role': 'regression_fixture',
                                                          'profile': 'eaw',
                                                          'sha256': '148c4dbb1a9f2a51b1e8f510f2f2596db0bfd161dbcacebf64b9ce7adb4710de',
                                                          'checkpoint': {'kind': 'space',
                                                                         'semantic_complete': True,
                                                                         'stored_size': 44696,
                                                                         'terrain_width': 0,
                                                                         'terrain_height': 0,
                                                                         'terrain_samples': 0,
                                                                         'terrain_materials': 0,
                                                                         'environments': 1,
                                                                         'water_records': 0,
                                                                         'placements': 41,
                                                                         'placements_crc_absent': 0,
                                                                         'placements_crc_missing': 0,
                                                                         'placements_crc_collision': 0,
                                                                         'placements_crc_unique': 41,
                                                                         'placements_model_renderable': 41,
                                                                         'placements_model_unresolved': 0,
                                                                         'placements_model_undeclared': 0,
                                                                         'placements_model_unknown': 0,
                                                                         'texture_references': 1,
                                                                         'textures_resolved': 1,
                                                                         'sky_references': 2,
                                                                         'skies_resolved': 2,
                                                                         'skies_model_renderable': 2,
                                                                         'notices': 2},
                                                          'semantics': {'volumes': 2,
                                                                        'water_textures': 0,
                                                                        'declared_extents': False,
                                                                        'issues': {'orientation_three_axis': 2},
                                                                        'three_axis_records': [{'ordinal': 15,
                                                                                                'object_id': 19636,
                                                                                                'type_crc': '0x31A5FA23',
                                                                                                'type': 'Asteroid '
                                                                                                        'Huge',
                                                                                                'position': [-2615.72119,
                                                                                                             1329.76367,
                                                                                                             -140],
                                                                                                'euler_degrees': [71,
                                                                                                                  145,
                                                                                                                  26]},
                                                                                               {'ordinal': 19,
                                                                                                'object_id': 19489,
                                                                                                'type_crc': '0x31A5FA23',
                                                                                                'type': 'Asteroid '
                                                                                                        'Huge',
                                                                                                'position': [2936.90796,
                                                                                                             630.880676,
                                                                                                             -140],
                                                                                                'euler_degrees': [42,
                                                                                                                  0,
                                                                                                                  82.67556]}]}}}

# An install root or drive letter in a committed inventory is a clean-room
# failure, not a cosmetic one.
FORBIDDEN = re.compile(r"[A-Za-z]:[\\/]|SteamLibrary|steamapps|workshop", re.IGNORECASE)

ROLES = {"m1_reference", "regression_fixture"}

CRC_BUCKETS = (
    "placements_crc_absent", "placements_crc_missing",
    "placements_crc_collision", "placements_crc_unique",
)
MODEL_BUCKETS = (
    "placements_model_renderable", "placements_model_unresolved",
    "placements_model_undeclared", "placements_model_unknown",
)


def check_inventory() -> dict[str, dict]:
    text = INVENTORY.read_text(encoding="utf-8")
    found = FORBIDDEN.search(text)
    if found is not None:
        raise AssertionError(f"inventory leaks an installation path: {found.group(0)!r}")
    document = json.loads(text)
    if document["schema_version"] != 5:
        raise AssertionError("reference-map inventory schema changed without updating this pin")
    for field in ("status", "generator", "provenance", "resolution_policy", "role_policy"):
        if not document.get(field):
            raise AssertionError(f"inventory lacks {field}")

    raw_references = document["references"]
    if not isinstance(raw_references, list):
        raise AssertionError("reference-map references must be a list")
    identities = [(entry["profile"], entry["logical_path"]) for entry in raw_references]
    if len(set(identities)) != len(identities):
        raise AssertionError("duplicate reference-map profile and logical_path")
    if len(raw_references) != len(EXPECTED):
        raise AssertionError("reference-map count drifted")
    references = {(entry["profile"], entry["logical_path"]): entry for entry in raw_references}
    if set(references) != set(EXPECTED):
        raise AssertionError(
            f"pinned reference set drifted: {sorted(references)} != {sorted(EXPECTED)}"
        )
    m1_kinds = sorted(entry["checkpoint"]["kind"] for entry in raw_references
                      if entry.get("role") == "m1_reference")
    if m1_kinds != ["land", "space"]:
        raise AssertionError(f"expected exactly one land and one space m1_reference, got {m1_kinds}")
    for identity, expected in EXPECTED.items():
        path = identity[1]
        entry = references[identity]
        if entry.get("role") not in ROLES or entry["role"] != expected["role"]:
            raise AssertionError(f"{path}: pinned role drifted: {entry.get('role')!r}")
        if entry["profile"] != expected["profile"] or entry["sha256"] != expected["sha256"]:
            raise AssertionError(f"{path}: pinned identity drifted")
        checkpoint = entry["checkpoint"]
        if checkpoint != expected["checkpoint"]:
            differing = {
                key: (checkpoint.get(key), value)
                for key, value in expected["checkpoint"].items()
                if checkpoint.get(key) != value
            }
            raise AssertionError(f"{path}: pinned checkpoint drifted: {differing}")
        if entry.get("semantics") != expected["semantics"]:
            raise AssertionError(f"{path}: pinned semantics drifted")
        if sum(checkpoint[key] for key in CRC_BUCKETS) != checkpoint["placements"]:
            raise AssertionError(f"{path}: CRC buckets do not sum to placements")
        if sum(checkpoint[key] for key in MODEL_BUCKETS) != checkpoint["placements"]:
            raise AssertionError(f"{path}: model buckets do not sum to placements")
        if checkpoint["textures_resolved"] > checkpoint["texture_references"]:
            raise AssertionError(f"{path}: more textures resolved than referenced")
        if checkpoint["skies_resolved"] > checkpoint["sky_references"]:
            raise AssertionError(f"{path}: more skies resolved than referenced")
        land = checkpoint["kind"] == "land"
        if land != (checkpoint["terrain_samples"] > 0):
            raise AssertionError(f"{path}: terrain presence disagrees with map kind")
        if land and checkpoint["terrain_width"] * checkpoint["terrain_height"] != checkpoint["terrain_samples"]:
            raise AssertionError(f"{path}: terrain dimensions disagree with the sample count")
    return references


def check_corpus(validator: Path, game_root: str) -> None:
    for identity, expected in EXPECTED.items():
        path = identity[1]
        completed = subprocess.run(
            [str(validator), "--profile", expected["profile"], "--game-root", game_root,
             "--inspect-map", path],
            check=False, capture_output=True, text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"{path}: inspection failed: {completed.stderr.strip()}")
        observed = json.loads(completed.stdout)
        if observed["sha256"] != expected["sha256"]:
            raise AssertionError(f"{path}: installed content hash differs from the pin")
        differing = {
            key: (observed.get(key), value)
            for key, value in expected["checkpoint"].items()
            if observed.get(key) != value
        }
        if differing:
            raise AssertionError(f"{path}: measured corpus differs from the pin: {differing}")
        if observed.get("semantics") != expected["semantics"]:
            raise AssertionError(f"{path}: measured semantics differ from the pin: {observed.get('semantics')}")
        unresolved = observed["unresolved_references"]
        if set(unresolved) != {"placement_type_crcs", "model_names", "texture_names", "sky_object_ids"}:
            raise AssertionError(f"{path}: unresolved-reference shape changed")
        if any(unresolved.values()):
            raise AssertionError(
                f"{path}: pinned reference map now has unresolved references: {unresolved}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validator", type=Path)
    parser.add_argument("--game-root")
    args = parser.parse_args()

    check_inventory()
    if args.validator is not None and args.game_root:
        check_corpus(args.validator, args.game_root)
        print("map reference-map pins match the read-only installed corpus")
    else:
        print("map reference-map pins are self-consistent (corpus check not requested)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
