"""Compare fixed and adaptive campaign pacing over two real GNS processes.

The seeded UDP proxy applies 120 ms RTT, up to 12 ms RTT jitter and 1% loss.
It changes no host networking settings. Delivery qualification helpers remain
read-only; this fixture owns its timing arms, jitter and cross-arm hash proof.
"""
import argparse
import heapq
import json
import queue
import random
import re
import select
import socket
import statistics
import subprocess
import threading
import time
from pathlib import Path


def run(args, policy, interval, phase_lag, deferred="default"):
    arm = f"{policy}-i{interval}-phase{phase_lag}-deferred-{deferred}"
    processes, outputs, readers = [], {}, []
    first = queue.Queue()
    done = threading.Event()
    wire = [{"packets": 0, "drops": 0}, {"packets": 0, "drops": 0}]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as proxy:
        proxy.bind(("127.0.0.1", 0))

        def start(name, port):
            process = subprocess.Popen([args.executable, "--wan", name, str(port), str(args.ticks), policy, args.polling, str(interval), str(phase_lag), deferred],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            processes.append(process)
            outputs[name] = []

            def read():
                for line in process.stdout:
                    outputs[name].append(line)
                    if name == "host":
                        first.put(line)
            thread = threading.Thread(target=read, daemon=True)
            thread.start()
            readers.append(thread)

        shaper = None
        try:
            start("host", 0)
            line = first.get(timeout=30)
            if "GNS disabled" in line:
                return None
            if not line.startswith("PORT "):
                raise RuntimeError("host did not publish port: " + line)
            target = ("127.0.0.1", int(line.split()[1]))
            start("client", proxy.getsockname()[1])

            def shape():
                client = None
                pending, sequence = [], 0
                rng = [random.Random(args.seed), random.Random(args.seed + 1)]
                while not done.is_set():
                    now = time.monotonic()
                    if select.select([proxy], [], [], .001)[0]:
                        packet, source = proxy.recvfrom(65536)
                        direction = int(source == target)
                        if direction == 0:
                            client = source
                        peer = client if direction else target
                        wire[direction]["packets"] += 1
                        if rng[direction].random() < args.loss:
                            wire[direction]["drops"] += 1
                        elif peer:
                            delay = (args.rtt_ms / 2 + rng[direction].uniform(-args.jitter_ms / 2, args.jitter_ms / 2)) / 1000
                            sequence += 1
                            heapq.heappush(pending, (now + max(0, delay), sequence, packet, peer))
                    while pending and pending[0][0] <= now:
                        _, _, packet, peer = heapq.heappop(pending)
                        proxy.sendto(packet, peer)
            shaper = threading.Thread(target=shape, daemon=True)
            shaper.start()
            deadline = time.monotonic() + 135
            for process in processes:
                process.wait(timeout=max(1, deadline - time.monotonic()))
            for reader in readers:
                reader.join(timeout=5)
            results, tick_times = {}, {}
            for name, lines in outputs.items():
                output = "".join(lines)
                match = re.search(r"^RESULT (.+)$", output, re.MULTILINE)
                if not match:
                    raise RuntimeError(f"{policy}/{name} missing result: {output}")
                row = dict(part.split("=", 1) for part in match[1].split())
                classes = re.search(r"^CLASSES (.+)$", output, re.MULTILINE)
                if classes:
                    row.update(part.split("=", 1) for part in classes[1].split())
                seconds = int(row["elapsed_ms"]) / 1000
                row["stalls_per_s"] = int(row["stalls"]) / seconds
                row["ticks_per_s"] = int(row["ticks"]) / seconds
                if "cpu_ms" in row:
                    row["cpu_pct_one_core"] = float(row["cpu_ms"]) / (seconds * 10)
                results[name] = row
                tick_times[name] = {int(tick): int(epoch) for tick, epoch in
                                    re.findall(r"^TICK tick=(\d+) epoch_ns=(\d+)$", output, re.MULTILINE)}
            if any(process.returncode for process in processes):
                raise RuntimeError(f"{policy}: child process failed")
            if any(int(row["ticks"]) != args.ticks for row in results.values()):
                raise RuntimeError(f"{policy}: incomplete simulation")
            if results["host"]["hash"] != results["client"]["hash"]:
                raise RuntimeError(f"{policy}: canonical hashes differ")
            for row in results.values():
                every = int(row["interval"])
                expected = 1 + args.ticks // every + int(args.ticks % every != 0)
                if int(row["exchange_points"]) != expected:
                    raise RuntimeError(f"{arm}: setup/periodic/end exchange count differs")
            phase_ms = [(tick_times["client"][tick] - stamp) / 1e6 for tick, stamp in tick_times["host"].items()]
            if len(phase_ms) != args.ticks:
                raise RuntimeError(f"{arm}: missing paired tick timestamps")
            observed_phase = statistics.median(phase_ms)
            if phase_lag and abs(observed_phase - phase_lag) > 15:
                raise RuntimeError(f"{arm}: phase collapsed to {observed_phase:.1f} ms")
            return {"results": results, "wire": wire, "observed_client_phase_ms": {
                "median": observed_phase, "min": min(phase_ms), "max": max(phase_ms)}}
        finally:
            done.set()
            if shaper:
                shaper.join(timeout=5)
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)
            for reader in readers:
                reader.join(timeout=5)
            args.out.mkdir(parents=True, exist_ok=True)
            for name, lines in outputs.items():
                (args.out / f"{arm}-{name}.log").write_text("".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable")
    parser.add_argument("--ticks", type=int, default=600)
    parser.add_argument("--seed", type=int, default=1116)
    parser.add_argument("--loss", type=float, default=.01)
    parser.add_argument("--rtt-ms", type=float, default=120)
    parser.add_argument("--jitter-ms", type=float, default=12)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--policies", nargs="+", choices=("fixed", "adaptive", "skirmish"), default=("fixed", "adaptive"))
    parser.add_argument("--polling", choices=("tick", "continuous"), default="tick")
    parser.add_argument("--intervals", nargs="+", type=int, default=[0], help="0 uses the production default")
    parser.add_argument("--phase-lags", nargs="+", type=int, default=[0], help="client pacing offset in milliseconds")
    parser.add_argument("--deferred", nargs="+", choices=("default", "off", "on"), default=["default"],
                        help="code-only monitor comparison arms; default follows net::default_deferred_compare")
    args = parser.parse_args()
    report = {"profile": {k: v for k, v in vars(args).items() if k not in ("executable", "out")}}
    arms = []
    for policy in args.policies:
        for interval in args.intervals:
            for phase in args.phase_lags:
                for deferred in args.deferred:
                    result = run(args, policy, interval, phase, deferred)
                    if result is None:
                        print("GNS disabled: campaign WAN qualification skipped")
                        return 77
                    arm = policy if args.intervals == [0] and args.phase_lags == [0] else f"{policy}-i{interval}-phase{phase}"
                    if args.deferred != ["default"]:
                        arm += f"-deferred-{deferred}"
                    report[arm] = result
                    arms.append(result)
                    if policy == "adaptive" and not phase and any(int(row["timing_events"]) == 0 or int(row["delay"]) <= 3 for row in result["results"].values()):
                        raise RuntimeError("adaptive arm did not activate the agreed timing control")
    if len({arm["results"]["host"]["hash"] for arm in arms}) != 1:
        raise RuntimeError("WAN arms have different canonical hashes")
    (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
