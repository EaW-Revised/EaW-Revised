"""Export a built child and rerun it with the ordinary tactical CLI and mounted content."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--builder", required=True)
    parser.add_argument("--program", required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIP: installed-game child export requires EAWR_EAW_GAME_ROOT")
        return 77
    with tempfile.TemporaryDirectory(prefix="eawr-conflict-entry-") as directory:
        for map_name in (None, "data/art/maps/_space_planet_abregadorae_01.ted"):
            case = Path(directory) / ("galactic" if map_name else "skirmish")
            case.mkdir()
            command = [args.builder, root, str(case)]
            if map_name:
                command.append(map_name)
            subprocess.run(command, check=True)
            reference = None
            for workers in (1, 2, 4, 8):
                expected = (case / f"child-{workers}.csv").read_bytes()
                if reference is None:
                    reference = expected
                assert expected == reference, "child hashes differ across workers"
                target = case / f"standalone-{workers}.csv"
                subprocess.run([args.program, "--replay", str(case / f"child-{workers}.eawr-replay"),
                                "--game-root", root, "--workers", str(workers), "--hash-out", str(target)], check=True)
                assert target.read_bytes() == expected, f"standalone {workers}-worker child hashes differ"
            print(f"Conflict child on {case.name} map replays alone with identical 1/2/4/8-worker hashes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
