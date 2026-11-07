"""Decision-log reader/writer for the async-AI quality sweep (schema gcq-1).

A log is NDJSON written by the quality-run driver (tests/galactic/ai_quality_run.hpp). The same rows reach a pool job
through its test log as "GCQ <json>" lines; `extract_runs` splits such a log into runs.

Rows ("r"): meta, ev (committed galactic event), smp (per-player sample), goals (goal-reader summary line), probe,
hash, end; tech (galactic tech account change) and tgoal (per-goal line of a tech goal) for the tech-up measurement; drd (droid-team
hero actor state) and abil (ability result) for the tech-steal trace. See README.md for the fields.
"""
from __future__ import annotations

import gzip
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator

SCHEMA = "gcq-1"
_PREFIX = re.compile(r"GCQ (\{.*\})\s*$")


@dataclass
class Run:
    meta: dict
    events: list[dict] = field(default_factory=list)
    samples: list[dict] = field(default_factory=list)
    goals: list[dict] = field(default_factory=list)
    probes: list[dict] = field(default_factory=list)
    hashes: list[dict] = field(default_factory=list)
    tech: list[dict] = field(default_factory=list)
    tgoals: list[dict] = field(default_factory=list)
    trace: list[dict] = field(default_factory=list)
    end: dict | None = None

    @property
    def key(self) -> tuple[str, str, int, str]:
        """(profile, arm, seed, probe): the identity of one run inside a sweep."""
        m = self.meta
        return (m.get("profile", "stock"), m["arm"], int(m["seed"]), m.get("probe", "none"))

    @property
    def ai_players(self) -> list[int]:
        return [int(row["p"]) for row in self.meta.get("ai", [])]

    def faction(self, player: int) -> str:
        for row in self.meta.get("ai", []):
            if int(row["p"]) == player:
                return str(row.get("faction") or row.get("type") or player)
        return str(player)

    @property
    def complete(self) -> bool:
        return self.end is not None


def _add(run: Run, row: dict) -> None:
    kind = row.get("r")
    if kind == "ev":
        run.events.append(row)
    elif kind == "smp":
        run.samples.append(row)
    elif kind == "goals":
        run.goals.append(row)
    elif kind == "probe":
        run.probes.append(row)
    elif kind == "hash":
        run.hashes.append(row)
    elif kind == "tech":
        run.tech.append(row)
    elif kind == "tgoal":
        run.tgoals.append(row)
    elif kind in ("drd", "abil"):
        run.trace.append(row)
    elif kind == "end":
        run.end = row


def parse_rows(rows: Iterable[dict]) -> list[Run]:
    """Group rows into runs; every meta row starts a new run. Rows before the first meta are ignored."""
    runs: list[Run] = []
    for row in rows:
        if row.get("r") == "meta":
            if row.get("schema") != SCHEMA:
                raise ValueError(f"unsupported decision-log schema {row.get('schema')!r} (expected {SCHEMA})")
            runs.append(Run(meta=row))
        elif runs:
            _add(runs[-1], row)
    return runs


def _open_text(path: Path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8")
    return path.open("r", encoding="utf-8", errors="replace")


def read_ndjson(path: Path) -> list[Run]:
    with _open_text(path) as handle:
        rows = (json.loads(line) for line in handle if line.strip())
        return parse_rows(rows)


def extract_runs(path: Path) -> list[Run]:
    """Runs from a ctest/pool test log: every "GCQ {...}" line, other output ignored."""
    def rows() -> Iterator[dict]:
        with _open_text(path) as handle:
            for line in handle:
                match = _PREFIX.search(line)
                if match:
                    try:
                        yield json.loads(match.group(1))
                    except json.JSONDecodeError:
                        continue  # a log line cut by the transport; the run is then marked incomplete (no end row)
    return parse_rows(rows())


def read_any(path: Path) -> list[Run]:
    """A decision-log NDJSON (optionally .gz) or a raw test log containing GCQ lines."""
    with _open_text(path) as handle:
        first = handle.readline().lstrip()
    return read_ndjson(path) if first.startswith("{") else extract_runs(path)


def run_file_name(meta: dict) -> str:
    probe = meta.get("probe", "none")
    suffix = "" if probe == "none" else f"-{probe}"
    return f"{meta.get('profile', 'stock')}-{meta['arm']}-s{int(meta['seed']):03d}-t{int(meta['ticks'])}{suffix}.ndjson.gz"


def write_run(directory: Path, run: Run) -> Path:
    """Store one run compactly (gzip NDJSON) under its stable name; returns the path."""
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / run_file_name(run.meta)
    rows = [run.meta, *run.events, *run.samples, *run.goals, *run.probes, *run.hashes, *run.tech, *run.tgoals, *run.trace]
    rows.sort(key=lambda r: (r.get("t", 0), {"meta": 0, "probe": 1, "ev": 2, "smp": 3, "goals": 4, "hash": 5, "tech": 3, "tgoal": 4, "drd": 3, "abil": 3}.get(r.get("r"), 6)))
    if run.end:
        rows.append(run.end)
    with gzip.open(path, "wt", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, separators=(",", ":"), sort_keys=True))
            handle.write("\n")
    return path


def load_directory(directory: Path) -> list[Run]:
    runs: list[Run] = []
    for path in sorted(directory.glob("*.ndjson*")):
        runs.extend(read_ndjson(path))
    return runs
