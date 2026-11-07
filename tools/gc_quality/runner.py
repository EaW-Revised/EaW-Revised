"""Quality-sweep runner: plan, submit, collect and report N seeds x 2 arms on the offload hosts.

    python -m gc_quality.runner plan   --out out/gcq/sweep1 --profile stock --seeds 1-4 --noise-seeds 101-104 --ticks 18000
    python -m gc_quality.runner submit --out out/gcq/sweep1            # run it detached (see README.md)
    python -m gc_quality.runner status --out out/gcq/sweep1
    python -m gc_quality.runner collect --out out/gcq/sweep1          # also done by submit after each job
    python -m gc_quality.runner report --out out/gcq/sweep1
    python -m gc_quality.runner smoke  --out out/gcq/smoke             # which hosts can read which data

Run from tools/. Seeds are independent: all arms of one seed go to one host (same host and build for the pair), seeds
spread over the host pool of the profile. Traces stay compact: the quality-run driver writes no file on the host, the
decision log travels in the job's test log, and `collect` keeps only the gzip NDJSON per run and deletes the raw test
log (unless --keep-raw).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

from . import logs

ROOT = Path(__file__).resolve().parents[2]
MATRIX = Path(__file__).with_name("sweep_matrix.json")
MARKER = ROOT / "tools" / "gc_quality" / "sweep.enabled"
HOSTS_EXAMPLE = Path(__file__).with_name("hosts.example.json")
DEFAULT_HOSTS = ROOT / "config" / "gc-quality-hosts.local.json"

# Seconds of wall time per tick, from the design's estimates (tools/gc_quality/README.md has the measured values once
# a sweep has run); used only to size --timeout of a job.
POLL_SECONDS = 30
SECONDS_PER_TICK = {("stock", "reference"): 0.04, ("stock", "async"): 0.03, ("raw", "reference"): 0.08, ("raw", "async"): 0.06,
                     ("bfc", "reference"): 0.08, ("bfc", "async"): 0.06,
                     ("rawsep", "reference"): 0.08, ("rawsep", "async"): 0.06}


def case_name(profile: str, arm: str, seed: int, ticks: int, probe: str = "none") -> str:
    name = f"galactic_ai_quality_{profile}_{arm}_s{seed}_t{ticks}"
    return name if probe == "none" else f"{name}_{probe}"


def registered_cases() -> list[dict]:
    """Every case of sweep_matrix.json in registration order (what the CMake registration creates)."""
    matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
    cases = []
    for profile, spec in matrix["profiles"].items():
        for run in spec["runs"]:
            for arm in run["arms"]:
                for seed in run["seeds"]:
                    probe = run.get("probe", "none")
                    cases.append({"name": case_name(profile, arm, seed, run["ticks"], probe), "profile": profile, "arm": arm,
                                  "seed": seed, "ticks": run["ticks"], "probe": probe})
    return cases


def parse_seeds(text: str) -> list[int]:
    seeds: list[int] = []
    for part in filter(None, (p.strip() for p in text.split(","))):
        if "-" in part:
            low, high = part.split("-", 1)
            seeds.extend(range(int(low), int(high) + 1))
        else:
            seeds.append(int(part))
    return seeds


def load_hosts(path: Path | None) -> dict[str, list[str]]:
    path = path or DEFAULT_HOSTS
    if not path.exists():
        raise SystemExit(f"no host pool file {path}: copy {HOSTS_EXAMPLE.name} to {DEFAULT_HOSTS.relative_to(ROOT)} (ignored by git) and fill in the pool names")
    data = json.loads(path.read_text(encoding="utf-8"))
    return {profile: list(hosts) for profile, hosts in data["pools"].items()}


def make_plan(profile: str, seeds: list[int], noise_seeds: list[int], ticks: int, probes: list[str], probe_seeds: list[int],
              pool: list[str], arms=("reference", "async")) -> list[dict]:
    """One job per seed (all of its cases); seeds are spread over the pool, longest work first, to the least loaded host."""
    registered = {c["name"]: c for c in registered_cases()}
    work: list[tuple[int, list[str], float]] = []

    def add(seed, names):
        missing = [n for n in names if n not in registered]
        if missing:
            raise SystemExit(f"not in sweep_matrix.json (edit it, the CMake registration follows): {', '.join(missing)}")
        cost = sum(SECONDS_PER_TICK[(profile, registered[n]["arm"])] * registered[n]["ticks"] for n in names)
        work.append((seed, names, cost))

    for seed in seeds:
        add(seed, [case_name(profile, arm, seed, ticks) for arm in arms])
    for seed in noise_seeds:
        add(seed, [case_name(profile, "reference", seed, ticks)])
    for seed in probe_seeds:
        names = [case_name(profile, arm, seed, 7200, probe) for probe in probes for arm in arms]
        if names:
            add(1000 + seed, names)
    load = {host: 0.0 for host in pool}
    jobs = []
    for seed, names, cost in sorted(work, key=lambda w: -w[2]):
        host = min(load, key=lambda h: (load[h], pool.index(h)))
        load[host] += cost
        jobs.append({"id": f"j{len(jobs) + 1:02d}", "profile": profile, "seed": seed, "host": host, "tests": names,
                     "estimate_seconds": round(cost), "state": "pending"})
    return jobs


def client_command(job: dict, plan: dict, job_dir: Path) -> list[str]:
    regex = "^(" + "|".join(job["tests"]) + ")$"
    timeout = min(28800, max(3600, int(job["estimate_seconds"] * 1.6) + 1800))
    command = [sys.executable, str(ROOT / "tools" / "offload" / "windows_build.py"), "--dirty",
               "--preset", "windows-msvc-ninja", "--target", "galactic_ai_campaign_tests", "--game-data",
               "--ctest-regex", regex, "--ctest-verbose", "--validation", "batch", "--no-werror-probe", "--no-probe-build",
               "--lock-timeout", "28800", "--timeout", str(timeout), "--out", str(job_dir)]
    for host in job["host"].split(","):  # a preference list, like windows_build.py --host (repeatable)
        command += ["--host", host]
    if plan.get("strict_probe_base"):
        command += ["--strict-probe-base", plan["strict_probe_base"]]
    return command


@contextmanager
def sweep_marker():
    """The marker file makes the CMake configuration register the quality cases; it rides in the --dirty throwaway commit."""
    existed = MARKER.exists()
    if not existed:
        MARKER.write_text("registers the galactic_ai_quality_* cases (cmake/EawrGcQuality.cmake)\n", encoding="utf-8")
    try:
        yield
    finally:
        if not existed and MARKER.exists():
            MARKER.unlink()


_RESULT = re.compile(r"Test\s+#\d+:\s+(\S+)\s+\.+\s*(\*\*\*)?(Passed|Failed|Skipped|Not Run|Timeout|Exception[^ ]*)", re.I)


def test_outcomes(text: str) -> dict[str, str]:
    return {m.group(1): m.group(3).lower() for m in _RESULT.finditer(text)}


def collect_job(sweep: Path, job: dict, keep_raw: bool = False) -> dict:
    """Extract the runs of a finished job into compact logs and drop the raw test log."""
    job_dir = sweep / "jobs" / job["id"]
    index_path = sweep / "index.json"
    index = json.loads(index_path.read_text(encoding="utf-8")) if index_path.exists() else []
    raw = next(iter(sorted(job_dir.glob("*.test.log"))), None)
    summary = {"runs": 0, "incomplete": 0, "outcomes": {}}
    if raw is None:
        job["note"] = "no test log (the job did not reach its tests)"
        return summary
    text = raw.read_text(encoding="utf-8", errors="replace")
    summary["outcomes"] = test_outcomes(text)
    for run in logs.extract_runs(raw):
        path = logs.write_run(sweep / "logs", run)
        summary["runs"] += 1
        summary["incomplete"] += 0 if run.complete else 1
        index = [e for e in index if e["file"] != path.name]
        index.append({"file": path.name, "job": job["id"], "host": job["host"], "complete": run.complete,
                      "ticks": (run.end or {}).get("ticks"), "wall_s": (run.end or {}).get("wall_s"),
                      "arm": run.meta["arm"], "profile": run.meta.get("profile", "stock"), "seed": run.meta["seed"],
                      "probe": run.meta.get("probe", "none")})
    index_path.write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
    if not keep_raw:
        raw.unlink()
    return summary


def _load(sweep: Path) -> dict:
    return json.loads((sweep / "plan.json").read_text(encoding="utf-8"))


def _save(sweep: Path, plan: dict) -> None:
    (sweep / "plan.json").write_text(json.dumps(plan, indent=1) + "\n", encoding="utf-8")


def cmd_plan(args) -> int:
    sweep = Path(args.out)
    hosts = load_hosts(args.hosts_file)
    pool = [h for h in args.hosts.split(",") if h] if args.hosts else hosts.get(args.profile)
    if not pool:
        raise SystemExit(f"no hosts for profile {args.profile}")
    jobs = make_plan(args.profile, parse_seeds(args.seeds), parse_seeds(args.noise_seeds or ""), args.ticks,
                     [p for p in (args.probes or "").split(",") if p], parse_seeds(args.probe_seeds or ""), pool,
                     arms=tuple(args.arms.split(",")))
    base = args.strict_probe_base
    if base is None:
        base = subprocess.run(["git", "rev-parse", "origin/main"], cwd=ROOT, capture_output=True, text=True).stdout.strip() or None
    plan = {"profile": args.profile, "ticks": args.ticks, "strict_probe_base": base, "max_parallel": args.max_parallel, "jobs": jobs}
    sweep.mkdir(parents=True, exist_ok=True)
    _save(sweep, plan)
    for job in jobs:
        print(f"{job['id']} {job['host']:<14} ~{job['estimate_seconds'] / 3600:.1f} h  {len(job['tests'])} cases: {', '.join(job['tests'])}")
    print(f"plan written: {sweep / 'plan.json'}")
    return 0


def cmd_submit(args) -> int:
    sweep = Path(args.out)
    plan = _load(sweep)
    limit = args.max_parallel or plan.get("max_parallel", 1)
    running: dict[str, subprocess.Popen] = {}
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with sweep_marker():
        while True:
            for job in plan["jobs"]:
                if job["state"] == "pending" and len(running) < limit:
                    job_dir = sweep / "jobs" / job["id"]
                    job_dir.mkdir(parents=True, exist_ok=True)
                    with open(sweep / f"{job['id']}-client.log", "w", encoding="utf-8") as log:
                        running[job["id"]] = subprocess.Popen(client_command(job, plan, job_dir), cwd=ROOT, stdout=log,
                                                              stderr=subprocess.STDOUT, creationflags=flags)
                    job["state"], job["started"] = "running", time.strftime("%Y-%m-%dT%H:%M:%S")
                    _save(sweep, plan)
                    print(f"started {job['id']} on {job['host']}", flush=True)
            for job_id, process in list(running.items()):
                code = process.poll()
                if code is None:
                    continue
                job = next(j for j in plan["jobs"] if j["id"] == job_id)
                summary = collect_job(sweep, job, keep_raw=args.keep_raw)
                job["state"] = "done" if code == 0 else "failed"
                job["exit"], job["summary"] = code, summary
                del running[job_id]
                _save(sweep, plan)
                print(f"{job_id} {job['state']} (exit {code}): {summary['runs']} runs, {summary['incomplete']} incomplete", flush=True)
            if not running and not any(j["state"] == "pending" for j in plan["jobs"]):
                break
            time.sleep(POLL_SECONDS)
    return 0 if all(j["state"] == "done" for j in plan["jobs"]) else 1


def cmd_collect(args) -> int:
    sweep = Path(args.out)
    plan = _load(sweep)
    for job in plan["jobs"]:
        if job["state"] in ("running", "done", "failed"):
            summary = collect_job(sweep, job, keep_raw=args.keep_raw)
            print(f"{job['id']}: {summary['runs']} runs ({summary['incomplete']} incomplete), tests {summary['outcomes']}")
            job["summary"] = summary
    _save(sweep, plan)
    return 0


def cmd_status(args) -> int:
    plan = _load(Path(args.out))
    for job in plan["jobs"]:
        print(f"{job['id']} {job['host']:<14} {job['state']:<8} {len(job['tests'])} cases ~{job['estimate_seconds'] / 3600:.1f} h {job.get('summary', {}).get('outcomes', '')}")
    return 0


def cmd_report(args) -> int:
    from . import compare
    sweep = Path(args.out)
    return compare.main([str(sweep / "logs"), "--out", str(sweep / "report.md"), "--title", args.title])


def cmd_walltime(args) -> int:
    index = json.loads((Path(args.out) / "index.json").read_text(encoding="utf-8"))
    rows: dict[tuple, list[float]] = {}
    for e in index:
        if e.get("wall_s") and e.get("ticks"):
            rows.setdefault((e["profile"], e["arm"], e["host"]), []).append(e["wall_s"] / e["ticks"])
    print("| profile | arm | host | runs | wall per 54,000 ticks (min) |\n|---|---|---|---|---|")
    for (profile, arm, host), values in sorted(rows.items()):
        print(f"| {profile} | {arm} | {host} | {len(values)} | {sum(values) / len(values) * 54000 / 60:.0f} |")
    return 0


def cmd_smoke(args) -> int:
    """Which hosts can run the harness: one 300-tick case per profile and host (RaW skips where the mod is absent)."""
    sweep = Path(args.out)
    hosts = load_hosts(args.hosts_file)
    jobs = []
    for profile, pool in hosts.items():
        for host in (args.hosts.split(",") if args.hosts else pool):
            jobs.append({"id": f"s{len(jobs) + 1:02d}", "profile": profile, "seed": 1, "host": host,
                         "tests": [case_name(profile, "reference", 1, 300)], "estimate_seconds": 600, "state": "pending"})
    sweep.mkdir(parents=True, exist_ok=True)
    _save(sweep, {"profile": "smoke", "ticks": 300, "strict_probe_base": None, "max_parallel": 1, "jobs": jobs})
    for job in jobs:
        print(f"{job['id']} {job['host']} {job['tests'][0]}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--out", required=True)
    p.add_argument("--profile", default="stock", choices=("stock", "raw", "bfc", "rawsep"))
    p.add_argument("--seeds", default="1-4", help="seeds run on both arms, e.g. 1-4,7")
    p.add_argument("--noise-seeds", default="", help="extra seeds run on the reference arm only (noise floor)")
    p.add_argument("--ticks", type=int, default=54000, help="a tick count registered in sweep_matrix.json")
    p.add_argument("--arms", default="reference,async")
    p.add_argument("--probes", default="", help="comma list of near1,nearall,deep1")
    p.add_argument("--probe-seeds", default="")
    p.add_argument("--hosts", default="", help="override the host pool (comma list of --host names)")
    p.add_argument("--hosts-file", type=Path)
    p.add_argument("--max-parallel", type=int, default=1)
    p.add_argument("--strict-probe-base", help="full sha for --strict-probe-base (default: origin/main)")
    p.set_defaults(func=cmd_plan)
    for name, func in (("submit", cmd_submit), ("collect", cmd_collect), ("status", cmd_status), ("report", cmd_report),
                       ("walltime", cmd_walltime)):
        p = sub.add_parser(name)
        p.add_argument("--out", required=True)
        if name in ("submit", "collect"):
            p.add_argument("--keep-raw", action="store_true", help="keep the raw test log of each job")
        if name == "submit":
            p.add_argument("--max-parallel", type=int, default=0)
        if name == "report":
            p.add_argument("--title", default="Async galactic AI quality report")
        p.set_defaults(func=func)
    p = sub.add_parser("smoke")
    p.add_argument("--out", required=True)
    p.add_argument("--hosts", default="")
    p.add_argument("--hosts-file", type=Path)
    p.set_defaults(func=cmd_smoke)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
