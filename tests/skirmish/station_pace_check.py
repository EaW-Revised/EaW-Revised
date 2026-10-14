"""Installed stock duel: verify the release gate and retain the station timeline."""
import csv
import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    probe, output = sys.argv[1:]
    folder = Path(output)
    folder.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [probe, "env", "data/art/maps/_mp_space_coruscant.ted", "Normal_Default",
         "36000", "1", str(folder), "duel:Empire:Rebel"],
        capture_output=True, text=True, check=False,
    )
    (folder / "stdout.txt").write_text(result.stdout, encoding="utf-8")
    (folder / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        return result.returncode
    if "SKIPPED:" in result.stdout:
        print(result.stdout)
        return 0
    setup = dict(line.split("=", 1) for line in (folder / "setup.txt").read_text().splitlines() if "=" in line)
    assert setup["seed"] == "67" and setup["mode"] == "duel:Empire:Rebel"
    with (folder / "minutes.csv").open(newline="", encoding="utf-8") as source:
        samples = list(csv.DictReader(source))
    start = {int(row["player"]): row for row in samples if int(row["tick"]) == 0}
    gate = {int(row["player"]): row for row in samples if int(row["tick"]) == 27000}
    assert set(start) == set(gate) == {1, 2}, "both stock AI players must be sampled"
    for player in start:
        assert int(start[player]["tech_level"]) == int(start[player]["station_level"]) == 1
        assert float(start[player]["credits"]) == 6000, "actual stock wallet must match the fixture"
        assert int(gate[player]["tech_level"]) >= 3, "both AIs must reach tech 3 by 15 minutes"
        assert int(gate[player]["ship_shots"]) > 0, "both AIs must have active ship combat"
    with (folder / "decisions.csv").open(newline="", encoding="utf-8") as source:
        upgrades = [row for row in csv.DictReader(source)
                    if row["decision"] == "completed" and row["kind"] == "station_upgrade"]
    timeline = {
        "seed": 67, "workers": 1, "ticks": 36000,
        "players": {str(player): {
            "tech_at_15_minutes": int(gate[player]["tech_level"]),
            "ship_shots_at_15_minutes": int(gate[player]["ship_shots"]),
            "station_completions": [{"tick": int(row["tick"]), "type": row["type"]}
                                    for row in upgrades if int(row["player"]) == player],
            "tech_samples": [{"tick": int(row["tick"]), "tech": int(row["tech_level"])}
                             for row in samples if int(row["player"]) == player],
        } for player in start},
    }
    (folder / "timeline.json").write_text(json.dumps(timeline, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(timeline, indent=2))
    # The owner progression gate covers every stock faction. Keep the 20-minute
    # Empire/Rebel observation above; these additional live duels only need 15 minutes.
    all_factions = {}
    for enemy in ("Empire", "Rebel"):
        duel = folder / ("Underworld_" + enemy)
        duel.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [probe, "env", "data/art/maps/_mp_space_coruscant.ted", "Normal_Default",
             "27000", "1", str(duel), "duel:Underworld:" + enemy, "--gate"],
            capture_output=True, text=True, check=False,
        )
        (duel / "stdout.txt").write_text(result.stdout, encoding="utf-8")
        (duel / "stderr.txt").write_text(result.stderr, encoding="utf-8")
        if result.returncode:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            return result.returncode
        assert "SKIPPED:" not in result.stdout, "the installed mirror cannot disappear between duels"
        with (duel / "minutes.csv").open(newline="", encoding="utf-8") as source:
            gate = {int(row["player"]): row for row in csv.DictReader(source) if int(row["tick"]) == 27000}
        assert set(gate) == {1, 2}, "both live AI players must be sampled"
        for player, row in gate.items():
            assert int(row["tech_level"]) >= 3, "all stock factions must reach tech 3 by 15 minutes"
            assert int(row["ship_shots"]) > 0, "every progression witness must have active ship combat"
            all_factions[f"Underworld_{enemy}:{player}"] = {
                "tech_at_15_minutes": int(row["tech_level"]),
                "ship_shots_at_15_minutes": int(row["ship_shots"]),
            }
    (folder / "all-faction-gate.json").write_text(json.dumps(all_factions, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(all_factions, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
