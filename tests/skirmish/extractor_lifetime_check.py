"""Installed stock duel: compare the leading side's completed extractor losses."""
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def summarize(folder: Path, seed: int) -> dict:
    setup = dict(line.split("=", 1) for line in (folder / "setup.txt").read_text().splitlines() if "=" in line)
    assert int(setup["seed"]) == seed and setup["ticks"] == "36000"
    assert setup["ai_handoff_tick"] == "4500", "match the retail local-player AI handoff"
    assert setup["ai_economy_handoff_tick"] == "4500" and setup["journal_enabled"] == "1"
    assert 4500 <= int(setup["first_handoff_service"]) <= 4503
    with (folder / "minutes.csv").open(newline="", encoding="utf-8") as source:
        samples = list(csv.DictReader(source))
    ten_minutes = {int(row["player"]): row for row in samples if int(row["tick"]) == 18000}
    assert set(ten_minutes) == {1, 2}, "both AI players need a ten-minute sample"
    leader = max(ten_minutes, key=lambda player: (int(ten_minutes[player]["mines"]),
                                                float(ten_minutes[player]["credits"]), -player))
    assert int(ten_minutes[leader]["mines"]) > 0, "the leading side must have completed extractors"
    with (folder / "events.csv").open(newline="", encoding="utf-8") as source:
        events = list(csv.DictReader(source))
    completed = {row["unit"] for row in events if row["kind"] == "pad_construction_completed"}
    losses = {player: [] for player in ten_minutes}
    for row in events:
        if row["kind"] != "unit_destroyed" or row["unit"] not in completed:
            continue
        name = row["type"]
        if "MINERAL_EXTRACTOR" not in name or "PAD" in name or name.startswith("UC_"):
            continue
        player = int(row["owner"])
        if player in losses:
            losses[player].append(int(row["tick"]))
    first = {player: min(ticks) if ticks else None for player, ticks in losses.items()}
    return {"seed": seed, "ai_handoff_tick": 4500, "leader_at_10_minutes": leader,
            "mines_at_10_minutes": {player: int(row["mines"]) for player, row in ten_minutes.items()},
            "first_completed_extractor_loss_tick": first,
            "completed_extractor_losses": losses,
            "leader_retained_until_12_minutes": first[leader] is None or first[leader] >= 21600}


def run_probe(probe: str, duel: Path, seed: int, ticks: int = 36000, workers: int = 1,
              journal: bool = True, game_root: str = "env") -> None:
    duel.mkdir(parents=True, exist_ok=True)
    command = [probe, game_root, "data/art/maps/_mp_space_coruscant.ted", "Normal_Default",
               str(ticks), str(workers), str(duel), "duel:Empire:Rebel", f"--seed={seed}", "--handoff=4500"]
    if not journal:
        command.append("--no-journal")
    print(f"seed={seed} ticks={ticks} workers={workers} journal={journal}", flush=True)
    result = subprocess.run(command, capture_output=True, text=True, check=False,
                            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
    (duel / "stdout.txt").write_text(result.stdout, encoding="utf-8")
    (duel / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    if result.returncode or "SKIPPED:" in result.stdout:
        raise RuntimeError(f"installed manual acceptance probe failed ({result.returncode}): {result.stdout}\n{result.stderr}")


def main() -> int:
    probe, output = sys.argv[1:3]
    game_root = sys.argv[3] if len(sys.argv) > 3 else "env"
    folder = Path(output)
    folder.mkdir(parents=True, exist_ok=True)
    rows = []
    for seed in (67, 68, 69):
        duel = folder / str(seed)
        run_probe(probe, duel, seed, game_root=game_root)
        rows.append(summarize(duel, seed))
        print(json.dumps(rows[-1]), flush=True)
    reference = folder / "67"
    without_journal = folder / "journal-off-67"
    run_probe(probe, without_journal, 67, journal=False, game_root=game_root)
    journal_identity = all((reference / name).read_bytes() == (without_journal / name).read_bytes()
                           for name in ("hashes.csv", "battle.eawr-replay"))
    reference_hashes = (reference / "hashes.csv").read_text().splitlines()
    worker_identity = {}
    for workers in (2, 4, 8):
        short = folder / "worker-identity-67" / str(workers)
        run_probe(probe, short, 67, ticks=4800, workers=workers, game_root=game_root)
        setup = dict(line.split("=", 1) for line in (short / "setup.txt").read_text().splitlines() if "=" in line)
        assert 4500 <= int(setup["first_handoff_service"]) <= 4503
        observed = (short / "hashes.csv").read_text().splitlines()
        worker_identity[workers] = len(observed) == 4801 and observed == reference_hashes[:4801]
    passed = sum(row["leader_retained_until_12_minutes"] for row in rows)
    receipt = {"seeds": rows, "passed_seeds": passed, "required_seeds": 2, "passed": passed >= 2,
               "journal_on_off_identity_36000": journal_identity, "handoff_worker_identity_4800": worker_identity,
               "journal_replay_sha256": {name: hashlib.sha256((path / "battle.eawr-replay").read_bytes()).hexdigest()
                                           for name, path in (("on", reference), ("off", without_journal))}}
    (folder / "acceptance.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    assert journal_identity, "journal on/off changed a 36000-tick world hash or replay command stream"
    assert all(worker_identity.values()), "real-battle handoff differs on 1/2/4/8 workers"
    assert receipt["passed"], f"only {passed}/3 leaders retained completed extractors until 12 minutes"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
