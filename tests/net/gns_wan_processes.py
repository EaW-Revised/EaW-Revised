"""Two native GNS/LockstepSession processes through a bounded WAN UDP shaper.

Each direction has independent seeded loss, propagation delay, a wire MTU and
a byte-paced tail-drop queue. No host network settings or elevated rights.
"""
import argparse
import collections
import json
import queue
import random
import re
import select
import socket
import subprocess
import threading
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable")
    parser.add_argument("--ticks", type=int, default=300)
    parser.add_argument("--seed", type=int, default=1116)
    parser.add_argument("--loss", type=float, default=0.20)
    parser.add_argument("--rtt-ms", type=float, default=200)
    parser.add_argument("--mtu", type=int, default=1280)
    parser.add_argument("--queue", type=int, default=4)
    parser.add_argument("--bytes-per-second", type=int, default=125000)
    parser.add_argument("--bulk-bytes", type=int, default=0)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    processes = []
    outputs = {}
    threads = []
    done = threading.Event()
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    def start(name, role, port):
        command = [args.executable, "--wan", role, str(port), str(args.ticks)]
        if args.bulk_bytes:
            command.append(str(args.bulk_bytes))
        process = subprocess.Popen(command,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   stdin=subprocess.DEVNULL, creationflags=flags)
        processes.append(process)
        lines = outputs[name] = []
        first = queue.Queue()

        def read():
            for line in process.stdout:
                lines.append(line)
                first.put(line)
        thread = threading.Thread(target=read, daemon=True)
        thread.start()
        threads.append(thread)
        return process, first

    # One proxy socket preserves the address GNS authenticates in its direct
    # handshake. Real encrypted UDP datagrams, ACKs and retries all cross it.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as proxy:
        proxy.bind(("127.0.0.1", 0))
        proxy.setblocking(False)
        try:
            host, first = start("host", "host", 0)
            line = first.get(timeout=30)
            if "GNS disabled" in line:
                host.wait(timeout=10)
                print(line.strip())
                return 77
            if not line.startswith("PORT "):
                raise RuntimeError("host did not publish its port: " + line)
            target = ("127.0.0.1", int(line.split()[1]))
            client, _ = start("client", "client", proxy.getsockname()[1])
            rng = [random.Random(args.seed), random.Random(args.seed + 1)]
            stats = [collections.Counter(), collections.Counter()]
            wire = [collections.deque(), collections.deque()]
            propagation = [collections.deque(), collections.deque()]
            client_address = None

            def shape():
                nonlocal client_address
                while not done.is_set():
                    now = time.monotonic()
                    # Release serialized packets before admitting a new one;
                    # an already elapsed service interval cannot fill the queue.
                    for direction in (0, 1):
                        while wire[direction] and wire[direction][0][0] <= now:
                            finish, packet, peer = wire[direction].popleft()
                            propagation[direction].append((finish + args.rtt_ms / 2000, packet, peer))
                    if select.select([proxy], [], [], 0.001)[0]:
                        packet, source = proxy.recvfrom(65536)
                        direction = 1 if source == target else 0
                        if direction == 0:
                            client_address = source
                        peer = target if direction == 0 else client_address
                        counts = stats[direction]
                        counts["packets"] += 1
                        counts["bytes"] += len(packet)
                        counts["max_payload"] = max(counts["max_payload"], len(packet))
                        if len(packet) + 28 > args.mtu:
                            counts["mtu_drops"] += 1
                        elif rng[direction].random() < args.loss:
                            counts["loss_drops"] += 1
                        elif len(wire[direction]) >= args.queue:
                            counts["queue_drops"] += 1
                        elif peer:
                            # Queue occupancy ends at serialization, independent
                            # of the subsequent 100ms propagation delay (BDP).
                            finish = max(now, wire[direction][-1][0] if wire[direction] else now)
                            finish += (len(packet) + 28) / args.bytes_per_second
                            wire[direction].append((finish, packet, peer))
                    for direction in (0, 1):
                        while wire[direction] and wire[direction][0][0] <= now:
                            finish, packet, peer = wire[direction].popleft()
                            propagation[direction].append((finish + args.rtt_ms / 2000, packet, peer))
                        while propagation[direction] and propagation[direction][0][0] <= now:
                            _, packet, peer = propagation[direction].popleft()
                            proxy.sendto(packet, peer)
                            stats[direction]["delivered"] += 1

            shaper = threading.Thread(target=shape, daemon=True)
            shaper.start()
            deadline = time.monotonic() + 190
            for process in processes:
                process.wait(timeout=max(1, deadline - time.monotonic()))
            done.set()
            shaper.join(timeout=5)
            for thread in threads:
                thread.join(timeout=5)
            results = {}
            for name, lines in outputs.items():
                text = "".join(lines)
                print(f"--- {name}\n{text}", flush=True)
                match = re.search(r"^RESULT (.+)$", text, re.MULTILINE)
                if match:
                    results[name] = dict(part.split("=", 1) for part in match[1].split())
            report = {"profile": {k: v for k, v in vars(args).items() if k not in ("executable", "out")},
                      "results": results, "wire": [dict(row) for row in stats],
                      "exit_codes": [process.returncode for process in processes]}
            for name, lines in outputs.items():
                match = re.search(r"^BULK bytes=(\d+) elapsed_ms=(\d+)$", "".join(lines), re.MULTILINE)
                if match:
                    report.setdefault("bulk", {})[name] = {"bytes": int(match[1]), "elapsed_ms": int(match[2])}
            print(json.dumps(report, indent=2))
            if args.out:
                args.out.mkdir(parents=True, exist_ok=True)
                (args.out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
                for name, lines in outputs.items():
                    (args.out / f"{name}.log").write_text("".join(lines))
            if any(process.returncode for process in processes) or len(results) != 2:
                return 1
            if results["host"]["hash"] != results["client"]["hash"]:
                raise RuntimeError("WAN canonical input hashes differ")
            return 0
        finally:
            done.set()
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    raise SystemExit(main())
