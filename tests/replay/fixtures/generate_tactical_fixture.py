#!/usr/bin/env python3
"""Generate the replay-v2 tactical fixtures and their independent oracle.

Standalone by design: it uses only Python's standard library, never imports or runs
project code, and implements the replay-v2 encoding, the tactical rules-v1 command
semantics, the canonical state and snapshot bytes, the Q24 quaternion-to-matrix rule,
the P2-05 sensor visibility, the derived FogGridV1 cells and the P2-09 hull and hardpoint
rules from docs/replay-format.md, docs/fixed-point.md, docs/behaviour/space-visibility.md
and docs/behaviour/space-hardpoints.md. Visibility is computed by brute force over every
observer, not through a spatial index; the durability rules use exact Python integers. Production output is
never blessed as expected data; the C++ reader, the session and sim_headless are
compared against the files written here.
"""


from __future__ import annotations
from dataclasses import dataclass, field, replace
from decimal import ROUND_HALF_EVEN, Decimal, getcontext
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from tactical_fixture_types import (
    OUT,
    NAME,
    MAGIC,
    STATE_MAGIC,
    SNAPSHOT_MAGIC,
    FORMAT_VERSION,
    HEADER_SIZE,
    RULES_VERSION,
    STATE_ENCODING_VERSION,
    SNAPSHOT_ENCODING_VERSION,
    MATH_VERSION,
    FRACTIONAL_BITS,
    ONE,
    TICK_NUMERATOR,
    TICK_DENOMINATOR,
    FINAL_TICK_COUNT,
    SEED,
    CONTENT_IDENTITY,
    COMMANDABLE,
    OPCODES,
    ORDER_KINDS,
    EVENT_KINDS,
    REASONS,
    HULL_TARGET,
    Player,
    Order,
    Unit,
    Command,
    q24,
    vec,
    rne_shift,
    decimal_pi,
    decimal_sin_cos,
    yaw_quat,
    to_matrix,
    VISIBILITY,
    VISIBILITY_SEED,
    VISIBILITY_CONTENT,
    VISIBILITY_SENSORS,
    FOG_LAYOUT,
    TIE,
    FRIGATE,
    DURABILITY,
    DURABILITY_SEED,
    DURABILITY_CONTENT,
    DURABILITY_TICKS,
    ROLES,
    STATES,
    rne_div,
    HardpointSpec,
    ProfileSpec,
    scaled,
)
from tactical_fixture_encode import (
    player_bytes,
    unit_bytes,
    order_bytes,
    command_body,
    command_bytes,
    header_bytes,
    replay_bytes,
    event_bytes,
    state_bytes,
    visibility_masks,
    snapshot_bytes,
    fog_grid_bytes,
    fog_cells,
    command_offsets,
    mutations,
)
from tactical_fixture_simulate import (
    execute,
    simulate,
    Durability,
    simulate_durability,
)
from tactical_fixture_scenarios import (
    fixture_players,
    fixture_units,
    fixture_commands,
    visibility_frames,
    visibility_players,
    visibility_units,
    build_visibility,
    durability_profiles,
    durability_players,
    durability_units,
    durability_commands,
    build_durability,
    build,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="regenerate into memory and fail if any checked-in fixture differs")
    args = parser.parse_args()
    outputs = build()
    if args.check:
        stale = []
        for name, data in sorted(outputs.items()):
            path = OUT / name
            if not path.is_file():
                stale.append(name)
                continue
            actual = path.read_bytes()
            if name.endswith((".csv", ".json")):
                # Text goldens may be checked out with CRLF under core.autocrlf.
                actual = actual.replace(b"\r\n", b"\n")
            if actual != data:
                stale.append(name)
        present = {path.name for path in OUT.glob(f"{NAME}-mutated-*.eawr-replay")}
        present |= {path.name for path in OUT.glob(f"{VISIBILITY}-frame-*.eawr-replay")}
        stale.extend(sorted(present - set(outputs)))
        if stale:
            print("stale tactical fixtures: " + ", ".join(stale), file=sys.stderr)
            return 1
        print(f"tactical fixtures current ({len(outputs)} files)")
        return 0
    for name, data in outputs.items():
        (OUT / name).write_bytes(data)
    data = outputs[f"{NAME}.eawr-replay"]
    print(json.dumps({"fixture": f"{NAME}.eawr-replay", "bytes": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()}, sort_keys=True))
    return 0



if __name__ == "__main__":
    sys.exit(main())
