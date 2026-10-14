"""Compare the real installed correctness and independently verified benchmark runs."""
import json
import os
import pathlib
import sys


def compare(correctness: pathlib.Path, benchmark: pathlib.Path) -> None:
    left = json.loads((correctness / "result.json").read_text(encoding="utf-8"))
    right = json.loads((benchmark / "result.json").read_text(encoding="utf-8"))
    if not left.get("correctness_passed") or not right.get("release_gate_passed"):
        raise ValueError("both actual progression/continuation cases and the mature budget must pass")
    if json.loads((correctness / "case.json").read_text(encoding="utf-8")) != json.loads((benchmark / "case.json").read_text(encoding="utf-8")):
        raise ValueError("correctness and benchmark used different frozen inputs")
    for key in ("final_hash", "synchronized_frames", "galactic_frames", "players", "battles", "returns", "terminal_result", "blocked_invasion"):
        if left[key] != right[key]:
            raise ValueError(f"actual continuation differs between verification policies: {key}")
    for name in ("reservation", "transit", "blocked-invasion", "post-return"):
        if (correctness / f"{name}.eawr").read_bytes() != (benchmark / f"{name}.eawr").read_bytes():
            raise ValueError(f"saved native/world/child/Lua payload differs: {name}")
    print("installed correctness/benchmark identities, final hash and four disk payloads match")


def main(argv: list[str]) -> int:
    if not os.environ.get("EAWR_EAW_GAME_ROOT"):
        print("SKIPPED: installed policy comparison requires EAWR_EAW_GAME_ROOT")
        return 0
    try:
        compare(pathlib.Path(argv[0]), pathlib.Path(argv[1]))
    except (OSError, ValueError, KeyError, IndexError) as error:
        print(f"installed policy comparison failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
