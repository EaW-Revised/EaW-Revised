#!/usr/bin/env python3
"""Load smoke for the standalone relay server: simulated browsing players against a local relay.

Starts `eawr_relay_server` on loopback with a throwaway certificate and its default limits, then opens
directory + menu connections in steps (a browsing player holds two connections). Every player connects from its
own loopback address (127.x.y.z), so the per-IP limits (connections_per_ip, accepts_per_minute, the four new
directory principals per prefix per minute) stay out of the way and only the global caps are measured.

Reports, per step: players attempted, connections accepted, connections refused, relay memory, and the first
failure. A final step opens a couple of players more than the configured connection cap and expects the surplus to be
refused cleanly. Local only: it never contacts a deployed relay.

    python tools/net/relay_load_smoke.py <eawr_relay_server> [--steps 64,128,256,512] [--json out.json]

Exit 0 when every step up to the cap is fully accepted and the surplus is refused.
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def text(value: str) -> bytes:
    raw = value.encode("ascii")
    return struct.pack("!H", len(raw)) + raw


def send(stream: ssl.SSLSocket, kind: int, payload: bytes = b"") -> None:
    body = bytes((2, kind)) + payload
    stream.sendall(struct.pack("!I", len(body)) + body)


def receive(stream: ssl.SSLSocket) -> bytes:
    def exact(size: int) -> bytes:
        data = b""
        while len(data) < size:
            chunk = stream.recv(size - len(data))
            if not chunk:
                raise EOFError("connection closed by the relay")
            data += chunk
        return data
    size = struct.unpack("!I", exact(4))[0]
    return exact(size)


def rss_mib(pid: int) -> float | None:
    """Resident set of the relay process in MiB, without third-party packages."""
    if sys.platform.startswith("linux"):
        try:
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024
        except OSError:
            return None
        return None
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
        kernel, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
        kernel.OpenProcess.restype = wintypes.HANDLE
        handle = kernel.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
        if not handle:
            return None
        try:
            counters = Counters(); counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return None
            return counters.WorkingSetSize / (1024 * 1024)
        finally:
            kernel.CloseHandle(handle)
    return None


def raise_client_fd_limit(wanted: int) -> None:
    if sys.platform == "win32":
        return
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    target = wanted if hard == resource.RLIM_INFINITY else min(wanted, hard)
    if soft < target:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))


HIDDEN = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
READY_TIMEOUT = 30


class Relay:
    def __init__(self, exe: str, directory: Path, extra: list[str]) -> None:
        self.process: subprocess.Popen | None = None
        self.cert, key = directory / "cert.pem", directory / "key.pem"
        subprocess.check_output([exe, "--generate-certificate", "--certificate-file", str(self.cert),
                                 "--key-file", str(key)], text=True, creationflags=HIDDEN, timeout=READY_TIMEOUT)
        config = directory / "relay.local.conf"
        config.write_text(f"certificate_file={self.cert}\nkey_file={key}\nport=0\n", encoding="utf-8")
        self.process = subprocess.Popen([exe, "--config", str(config)] + extra, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, creationflags=HIDDEN)
        self.errors: list[str] = []
        first_line: queue.Queue = queue.Queue()
        threading.Thread(target=lambda: first_line.put(self.process.stdout.readline()), daemon=True).start()
        try:
            ready = first_line.get(timeout=READY_TIMEOUT).strip()
        except queue.Empty:
            ready = ""
        if not ready.startswith("relay ready port="):
            self.stop()
            raise RuntimeError(f"relay did not start within {READY_TIMEOUT} s: {ready!r}")
        self.port = int(ready.split("=")[1])
        threading.Thread(target=lambda: [None for _ in self.process.stdout], daemon=True).start()
        threading.Thread(target=lambda: [self.errors.append(line) for line in self.process.stderr], daemon=True).start()

    def stop(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()


def address_of(player: int) -> str:
    return f"127.{1 + player // 65025}.{(player // 255) % 255}.{1 + player % 255}"


def open_connection(context: ssl.SSLContext, port: int, source: str, kind: int, payload: bytes,
                    expect: int) -> tuple[ssl.SSLSocket | None, str]:
    native = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    native.settimeout(20)
    try:
        native.bind((source, 0))
        native.connect(("127.0.0.1", port))
        stream = context.wrap_socket(native, server_hostname="localhost")
    except (OSError, ssl.SSLError) as error:
        native.close()
        return None, f"connect/handshake: {type(error).__name__}: {error}"
    try:
        send(stream, kind, payload)
        reply = receive(stream)
    except (OSError, ssl.SSLError, EOFError, struct.error) as error:
        stream.close()
        return None, f"open kind {kind}: {type(error).__name__}: {error}"
    if len(reply) < 2 or reply[0] != 2 or reply[1] != expect:
        stream.close()
        return None, f"open kind {kind}: unexpected reply {reply[:6].hex()}"
    return stream, ""


def open_player(context: ssl.SSLContext, port: int, player: int) -> tuple[list[ssl.SSLSocket], list[str]]:
    """One browsing player: a directory connection and a menu connection from the player's own address."""
    source = address_of(player)
    streams: list[ssl.SSLSocket] = []
    failures: list[str] = []
    for kind, payload, expect, label in ((20, text(""), 30, "directory"), (50, text(f"Smoke{player}"), 53, "menu")):
        stream, reason = open_connection(context, port, source, kind, payload, expect)
        if stream is None:
            failures.append(f"player {player} {label}: {reason}")
        else:
            streams.append(stream)
    return streams, failures


def run_step(context: ssl.SSLContext, relay: Relay, first: int, count: int, held: list[ssl.SSLSocket],
             workers: int) -> dict:
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(lambda p: open_player(context, relay.port, p), range(first, first + count)))
    accepted = refused = 0
    failures: list[str] = []
    for streams, errors in results:
        held.extend(streams)
        accepted += len(streams)
        refused += len(errors)
        failures.extend(errors)
    return {"players": count, "connections_accepted": accepted, "connections_refused": refused,
            "first_failure": failures[0] if failures else None, "seconds": round(time.monotonic() - started, 1),
            "relay_rss_mib": None if (m := rss_mib(relay.process.pid)) is None else round(m, 1)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("relay_server", help="path to eawr_relay_server")
    parser.add_argument("--steps", default="64,128,256,512", help="cumulative player counts (two connections each)")
    parser.add_argument("--max-connections", type=int, default=0,
                        help="pass --max-connections to the relay (0 keeps the built-in default)")
    parser.add_argument("--workers", type=int, default=16, help="concurrent client handshakes")
    parser.add_argument("--max-seconds", type=float, default=100,
                        help="hard runtime bound: kill the relay and exit 2 (keep below the CTest TIMEOUT)")
    parser.add_argument("--json", help="write the report here")
    args = parser.parse_args()
    steps = [int(item) for item in args.steps.split(",")]
    extra = ["--max-connections", str(args.max_connections)] if args.max_connections else []
    raise_client_fd_limit(4 * max(steps) + 64)
    report: dict = {"steps": []}
    held: list[ssl.SSLSocket] = []
    relays: list[Relay] = []

    def hard_stop(*_: object) -> None:
        for item in relays:
            try:
                item.process.kill()
            except OSError:
                pass
        os._exit(2)

    # CTest kills this process without running `finally`, so bound the run below its TIMEOUT and clean up on signals.
    for name in ("SIGTERM", "SIGINT", "SIGBREAK"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), hard_stop)
    watchdog = threading.Timer(args.max_seconds, hard_stop)
    watchdog.daemon = True
    watchdog.start()
    with tempfile.TemporaryDirectory() as scratch:
        relay = Relay(str(Path(args.relay_server).resolve()), Path(scratch), extra)
        relays.append(relay)
        try:
            context = ssl.create_default_context(cafile=str(relay.cert))
            report["relay_rss_idle_mib"] = rss_mib(relay.process.pid)
            done = 0
            ok = True
            for target in steps:
                step = run_step(context, relay, done, target - done, held, args.workers)
                step["players_total"] = target
                step["connections_open"] = len(held)
                report["steps"].append(step)
                print(json.dumps(step), flush=True)
                done = target
                if step["connections_refused"]:
                    ok = False
                    break
            if ok:
                cap = args.max_connections or 1024
                surplus = max(0, cap - len(held)) // 2 + 2
                step = run_step(context, relay, done, surplus, held, args.workers)
                step["surplus_beyond_cap"] = True
                report["steps"].append(step)
                print(json.dumps(step), flush=True)
                # The cap must be exact: refused connections exist, and no more than `cap` stay open.
                ok = step["connections_refused"] > 0 and len(held) <= cap
                report["connections_open_at_end"] = len(held)
            report["relay_alive"] = relay.process.poll() is None
            ok = ok and report["relay_alive"]
            report["ok"] = ok
        finally:
            for stream in held:
                try:
                    stream.close()
                except OSError:
                    pass
            relay.stop()
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("relay load smoke " + ("passed" if report["ok"] else "FAILED"))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
