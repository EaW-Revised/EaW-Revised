"""#3912: two real GNS processes relayed through a TURN server that lets permissions and allocations expire.

The session that failed on the real route went quiet about 300 s after the pair connected: GNS only created TURN
permissions when a new peer address appeared and never refreshed them, so coturn stopped forwarding in both directions
while the connection still said Connected. This runs the same shape with a server lifetime of a few seconds, long
enough for several expiries, and requires the frames to keep flowing in both directions.

The `nonce` case rotates the server nonce every two seconds, so each Refresh and CreatePermission is answered with
438 Stale Nonce first; the client has to repeat it with the new nonce or the relay lapses after one lifetime.

Usage: gns_turn_processes.py <net_gns_introduced_tests> [permission|silent|nonce]
"""
import os
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gns_turn_server import TurnServer, lan_address  # noqa: E402

SECONDS = 20
LIFETIME = 6


def run_case(executable, mode):
    address = lan_address()
    if address is None:
        print("No non-loopback IPv4 address: relayed TURN case skipped")
        return 77
    silent = mode == "silent"
    # `silent`: nothing expires, but the path dies for good a few seconds after the first relayed packet.
    server = TurnServer(address, allocation_lifetime=3600 if silent else LIFETIME,
                        permission_lifetime=3600 if silent else LIFETIME, blackhole_after=5 if silent else None,
                        nonce_lifetime=2 if mode == "nonce" else None)
    server.start()
    turn = f"{address}:{server.port}"
    processes = []
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

    def start(*args):
        child = subprocess.Popen([executable, *args], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, creationflags=flags)
        processes.append(child)
        return child

    try:
        broker = start("broker")
        first_line = queue.Queue()
        threading.Thread(target=lambda: first_line.put(broker.stdout.readline()), daemon=True).start()
        line = first_line.get(timeout=15)
        if line.startswith("GNS disabled"):
            broker.wait(timeout=5)
            print(line.strip())
            return 77
        port = str(int(line.strip()))
        peers = [start("turn-peer", str(role), port, turn, str(SECONDS), *(["silent"] if silent else ["nonce"] if mode == "nonce" else []))
                 for role in range(2)]
        failed = False
        for child in peers + [broker]:
            output, _ = child.communicate(timeout=110)
            print(output, end="")
            failed = failed or child.returncode != 0
        for entry in server.statistics():
            print(f"TURN allocation {entry}")
        if failed:
            return 1
        if silent:
            return 0
        if mode == "nonce":
            print(f"TURN stale nonce replies: {server.stale_nonce_replies}")
            refreshes = [entry for entry in server.statistics() if entry["refreshes"] >= 3]
            if server.stale_nonce_replies < 6 or len(refreshes) != 2:
                print(f"Expected stale nonces to be answered and both allocations refreshed repeatedly, got "
                      f"{server.stale_nonce_replies} stale replies and {server.statistics()}", file=sys.stderr)
                return 1
        # Both pairs must have repeated CreatePermission: one per half lifetime, at least three over the run.
        refreshed = [entry for entry in server.statistics() if entry["permission_requests"] >= 3]
        if len(refreshed) != 2:
            print(f"Expected both allocations to refresh their permission repeatedly, got {server.statistics()}",
                  file=sys.stderr)
            return 1
        return 0
    except (ValueError, queue.Empty, subprocess.TimeoutExpired) as error:
        print(f"Relayed TURN process test failed: {error}", file=sys.stderr)
        return 1
    finally:
        for child in processes:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=10)
        server.stop()


if __name__ == "__main__":
    started = time.monotonic()
    result = run_case(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "permission")
    print(f"TURN relayed case finished in {time.monotonic() - started:.1f} s with {result}")
    raise SystemExit(result)
