#!/usr/bin/env python3
"""Exercise the sim_headless tick-zero contract of the pinned FoC skirmish (P2-04, #67).

Without the game, the committed m2-start replay is decoded here from its header and setup
tables alone (docs/replay-format.md, replay v3: v2 plus the squadron table), and its tick-zero state hash is recomputed
with the frozen EAWRTST encoding. That independent hash must equal the pinned value and the
census sim_headless writes from the replay, with 1, 2 and 4 workers. With
EAWR_EAW_GAME_ROOT set, `sim_headless --skirmish m2` must also rebuild the same replay bytes
from the installation, and its census must agree with the replay census.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

FIXTURE = "m2-start.eawr-replay"
FIXTURE_SHA256 = "c997bdd7bd7608816a83b29e8b9fefa5c88dfa1150240c7acc5a196d24e5fa77"
TICK_ZERO_STATE = "506e48ffa375d8a6fd29c9fa69474f63d4f455a999126e5f232bef3aefd64a2a"
CONTENT_IDENTITY = "984c71a99d5f6f58cef57bd3498754a5c2211bf7aec2b4f9ed7626273a0eb113"
FINAL_TICKS = 30


def crc32_upper(name: str) -> int:
    """The TED object-type CRC: CRC-32 of the ASCII-upper-cased name."""
    crc = 0xFFFFFFFF
    for byte in name.upper().encode("ascii"):
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def decode_setup(data: bytes) -> dict:
    """Replay-v3 header and setup tables (v2 plus squadrons, #271, #75), decoded without the
    production reader."""
    if len(data) < 104 or data[:8] != b"EAWRPLY\x00":
        raise ValueError("not a replay")
    (version, header_size, rules, math_version, bits, numerator, denominator, seed, final_ticks,
     players, reserved, units, commands) = struct.unpack_from("<HHIIIIIQQIIQQ", data, 8)
    identity = data[72:104]
    squadrons = reserved
    if (version, header_size, rules, math_version, bits, numerator, denominator) != (
            3, 104, 1, 1, 24, 1, 30) or squadrons == 0:
        raise ValueError("unexpected replay-v3 header")
    offset = 104
    player_rows = []
    for _ in range(players):
        player_rows.append(struct.unpack_from("<IIQII", data, offset))
        offset += 24
    unit_rows = []
    for _ in range(units):
        unit_rows.append(struct.unpack_from("<QQII3q4q", data, offset))
        offset += 80
    squadron_rows = []
    for _ in range(squadrons):
        container, count, zero = struct.unpack_from("<QII", data, offset)
        if zero != 0 or count == 0:
            raise ValueError("malformed squadron row")
        offset += 16
        squadron_rows.append((container, list(struct.unpack_from(f"<{count}Q", data, offset))))
        offset += 8 * count
    if commands != 0 or len(data) != offset:
        raise ValueError("the replay holds more than its header and setup")
    return {"seed": seed, "final_ticks": final_ticks, "identity": identity, "players": player_rows,
            "units": unit_rows, "squadrons": squadron_rows}


def tick_zero_state(setup: dict) -> str:
    """EAWRTST at completed tick 0: the setup, next ID max+1, random state = seed, no orders."""
    data = b"EAWRTST\x00" + struct.pack("<IIIIII", 1, 1, 1, 24, 1, 30)
    next_id = max((row[0] for row in setup["units"]), default=0) + 1
    data += struct.pack("<QQQ", 0, next_id, setup["seed"]) + setup["identity"]
    data += struct.pack("<Q", len(setup["players"]))
    data += b"".join(struct.pack("<IIQII", *row) for row in setup["players"])
    data += struct.pack("<Q", len(setup["units"]))
    for row in sorted(setup["units"]):
        data += struct.pack("<QQII3q4q", *row) + struct.pack("<QII3qQ", 0, 0, 0, 0, 0, 0, 0)
    # Live squadrons (#271): SQDN, the count, and per container its craft.
    if setup["squadrons"]:
        data += b"SQDN" + struct.pack("<Q", len(setup["squadrons"]))
        for container, members in sorted(setup["squadrons"]):
            data += struct.pack("<QQ", container, len(members)) + struct.pack(f"<{len(members)}Q", *members)
    return hashlib.sha256(data).hexdigest()


def run(program: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([program, *arguments], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def sim_fields(census: dict) -> dict:
    """What a census from the replay alone can list. The snapshot digest is left out: its
    visibility needs the sensor table, which is content (#68), not replay data."""
    player_keys = ("player_id", "team", "faction_id", "commandable")
    unit_keys = ("entity_id", "type_id", "owner", "position_raw", "rotation_raw")
    return {
        "tactical_rules_version": census["tactical_rules_version"],
        "seed": census["seed"],
        "content_identity": census["content_identity"],
        "tick_zero": {key: census["tick_zero"][key] for key in ("state_sha256", "next_entity_id")},
        "players": [{key: row[key] for key in player_keys} for row in census["players"]],
        "units": [{key: row[key] for key in unit_keys} for row in census["units"]],
    }


def check_fixture(fixtures: pathlib.Path) -> list[str]:
    errors = []
    data = (fixtures / FIXTURE).read_bytes()
    if hashlib.sha256(data).hexdigest() != FIXTURE_SHA256:
        errors.append(f"{FIXTURE} bytes changed")
    setup = decode_setup(data)
    if setup["final_ticks"] != FINAL_TICKS or setup["identity"].hex() != CONTENT_IDENTITY:
        errors.append("fixture ticks or content identity changed")
    if tick_zero_state(setup) != TICK_ZERO_STATE:
        errors.append(f"independent tick-zero hash {tick_zero_state(setup)} != {TICK_ZERO_STATE}")
    # The pinned fixture facts the header carries (plan/phase-2/m2-skirmish.md).
    players = {row[0]: row for row in setup["players"]}
    if players.get(1, (0, 0, 0, 0))[1:4] != (0, crc32_upper("Rebel"), 1) or \
            players.get(2, (0, 0, 0, 0))[1:4] != (1, crc32_upper("Empire"), 1):
        errors.append("SK-10: slot 1 Rebel team 0 and slot 2 Empire team 1, both commandable")
    types = [(row[0], row[1], row[2]) for row in setup["units"]]
    expected = [(1, "Skirmish_Rebel_Star_Base_1", 1), (2, "Rebel_X-Wing_Squadron", 1), (3, "Rebel_X-Wing_Squadron", 1),
                (4, "Y-Wing_Squadron", 1), (5, "Corellian_Corvette", 1), (6, "Nebulon_B_Frigate", 1),
                (7, "Calamari_Cruiser", 1), (8, "Skirmish_Empire_Star_Base_1", 2), (9, "TIE_Interceptor_Squadron", 2),
                (10, "TIE_Interceptor_Squadron", 2), (11, "Tartan_Patrol_Cruiser", 2), (12, "Acclamator_Assault_Ship", 2)]
    if types[:12] != [(entity, crc32_upper(name), owner) for entity, name, owner in expected]:
        errors.append("SK-24: tick-zero lobby units, their order and owners")
    # #272: after the slots, Pirates, Neutral, Hostile, Sarlacc and Hutts players (IDs 3 to 7);
    # the 15 Neutral structures go to player 4 and the 8 Hutts containers to player 7.
    if [row[0] for row in setup["players"]] != [1, 2, 3, 4, 5, 6, 7] or \
            [row[2] for row in setup["players"][2:]] != [crc32_upper(name) for name in
                                                         ("Pirates", "Neutral", "Hostile", "Sarlacc", "Hutts")]:
        errors.append("#272: the retail skirmish players of the non-playable factions")
    owners = [row[2] for row in setup["units"][12:35]]
    if len(owners) != 23 or owners.count(4) != 15 or owners.count(7) != 8:
        errors.append("SK-04, #272: 23 map objects, 15 Neutral and 8 Hutts")
    # #75: the five squadron companies (2, 3, 4, 9, 10) are their squadrons' containers; their craft
    # follow the map objects in company order.
    craft = [row[0] for row in setup["units"][35:]]
    if [container for container, _ in setup["squadrons"]] != [2, 3, 4, 9, 10] or             [member for _, members in setup["squadrons"] for member in members] != craft or             craft != list(range(36, 36 + len(craft))):
        errors.append("#75: each squadron company's craft after the map objects, one squadron per company")
    return errors


def check_replay_runs(program: str, fixtures: pathlib.Path, work: pathlib.Path) -> tuple[list[str], dict | None]:
    errors = []
    outputs = {}
    for workers in ("1", "2", "4"):
        hashes, snapshots, census = (work / f"{name}-{workers}.{ext}" for name, ext in
                                     (("hashes", "csv"), ("snapshots", "csv"), ("census", "json")))
        completed = run(program, "--replay", str(fixtures / FIXTURE), "--hash-out", str(hashes),
                        "--snapshot-out", str(snapshots), "--census-out", str(census), "--workers", workers)
        if completed.returncode != 0:
            errors.append(f"workers {workers}: exit {completed.returncode}: {completed.stdout}")
            continue
        outputs[workers] = (hashes.read_bytes(), snapshots.read_bytes(), census.read_bytes())
    if len(outputs) == 3 and not (outputs["1"] == outputs["2"] == outputs["4"]):
        errors.append("outputs differ across 1, 2 and 4 workers")
    if "1" not in outputs:
        return errors, None
    hash_rows = outputs["1"][0].decode("utf-8").splitlines()
    if hash_rows[0] != "tick,sha256" or len(hash_rows) != FINAL_TICKS + 1:
        errors.append("hash output has a header and 30 rows")
    census = json.loads(outputs["1"][2])
    if census["source"] != "replay" or census["tick_zero"]["state_sha256"] != TICK_ZERO_STATE:
        errors.append("the replay census carries the tick-zero hash")
    if census["tick_zero"]["sensor_profiles"] != 0:
        errors.append("a replay run binds no sensor table")
    snapshot_rows = outputs["1"][1].decode("utf-8").splitlines()
    if snapshot_rows[1] != f"0,{census['tick_zero']['snapshot_sha256']}":
        errors.append("the census snapshot digest is row 0 of the snapshot output")
    return errors, census


def check_arguments(program: str, fixtures: pathlib.Path, work: pathlib.Path) -> list[str]:
    errors = []
    replay = str(fixtures / FIXTURE)
    v1 = str(fixtures.parent.parent / "replay" / "fixtures" / "original-v1.eawr-replay")
    cases = {
        "skirmish without a game root": ("--skirmish", "m2", "--census-out", str(work / "a.json")),
        "skirmish without an output": ("--skirmish", "m2", "--game-root", str(work)),
        "unknown skirmish": ("--skirmish", "m3", "--game-root", str(work), "--census-out", str(work / "a.json")),
        "skirmish with a hash output": ("--skirmish", "m2", "--game-root", str(work), "--census-out",
                                        str(work / "a.json"), "--hash-out", str(work / "h.csv")),
        "ticks without a replay output": ("--skirmish", "m2", "--game-root", str(work), "--census-out",
                                          str(work / "a.json"), "--ticks", "3"),
        "census that is not json": ("--replay", replay, "--hash-out", str(work / "h.csv"), "--census-out",
                                    str(work / "a.csv")),
        "replay output without skirmish": ("--replay", replay, "--hash-out", str(work / "h.csv"), "--replay-out",
                                           str(work / "r.eawr-replay")),
        "census of a v1 replay": ("--replay", v1, "--hash-out", str(work / "h.csv"), "--census-out",
                                  str(work / "a.json")),
        "census on the replay": ("--replay", replay, "--hash-out", str(work / "h.csv"), "--census-out", replay),
    }
    for label, arguments in cases.items():
        completed = run(program, *arguments)
        if completed.returncode != 2:
            errors.append(f"{label}: exit {completed.returncode}, expected 2")
    return errors


def check_game(program: str, fixtures: pathlib.Path, work: pathlib.Path, replay_census: dict | None) -> list[str]:
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("FoC skirmish start: skipped (set EAWR_EAW_GAME_ROOT)")
        return []
    errors = []
    census_path = work / "fixture-census.json"
    replay_path = work / "fixture.eawr-replay"
    completed = run(program, "--skirmish", "m2", "--game-root", root, "--census-out", str(census_path),
                    "--replay-out", str(replay_path), "--ticks", str(FINAL_TICKS))
    if completed.returncode != 0:
        return [f"--skirmish m2: exit {completed.returncode}: {completed.stdout}"]
    if replay_path.read_bytes() != (fixtures / FIXTURE).read_bytes():
        errors.append("--skirmish m2 does not rebuild the committed replay")
    census = json.loads(census_path.read_bytes())
    if replay_census is not None and sim_fields(census) != sim_fields(replay_census):
        errors.append("the fixture census and the replay census disagree")
    listing = completed.stdout
    for line in ("player 1 Rebel team 0 human start Team_00 colour MP_Color_Blue (78, 150, 237) credits 0 income none "
                 "production none power 13525",
                 "player 2 Empire team 1 ai start Team_01 colour MP_Color_Red (237, 78, 78) credits 0 income none "
                 "production none power 9590",
                 f"tick 0 state {TICK_ZERO_STATE}"):
        if line not in listing:
            errors.append(f"listing lacks: {line}")
    players = census["players"]
    if [player["ai_combat_power"]["all_launched"] for player in players[:2]] != [14575, 11045]:
        errors.append("SK-24: all-launched AI_Combat_Power")
    if any(launch["simulated"] for launch in census["launches"]) or len(census["launches"]) != 6:
        errors.append("SK-23: six launch rows, none simulated")
    if census["tick_zero"]["sensor_profiles"] != 13:
        errors.append("#68/#271: the fixture census binds the 13 sensor profiles of the unit tables "
                      "(7 REVEAL stations and ships, the Y-Wing craft and 5 squadron containers)")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    parser.add_argument("--fixtures", required=True, type=pathlib.Path)
    args = parser.parse_args()
    errors = check_fixture(args.fixtures)
    with tempfile.TemporaryDirectory(prefix="eawr-skirmish-cli-") as temporary:
        work = pathlib.Path(temporary)
        run_errors, replay_census = check_replay_runs(args.program, args.fixtures, work)
        errors += run_errors
        errors += check_arguments(args.program, args.fixtures, work)
        errors += check_game(args.program, args.fixtures, work, replay_census)
    for error in errors:
        print(f"FAIL: {error}")
    if errors:
        return 1
    print("sim_headless skirmish start contract passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
