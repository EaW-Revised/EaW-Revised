"""N GNS processes meshed through a loopback room broker (docs/net-online-npeer.md, test plan: GNS mesh, partial mesh).

The parent owns an InProcessGnsRoom and relays only the room's opaque signals. Each member is its own process with its
own GNS identity and a real ICE data path. Cases: 3 and 4 members (complete mesh, whole frames, 1 MiB ordered tail,
host leaves after Lock and the survivors keep delivering), a newcomer refused because it cannot reach one member (the
broker drops that pair's signals) with a second newcomer then filling the slot, and a newcomer with a different build.
The nine-member case runs only when EAWR_GNS_MESH_NINE=1 (once per slice, at the final head).
"""
import os
import queue
import re
import subprocess
import sys
import threading
import time

FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


class Case:
    def __init__(self, executable, label):
        self.executable = executable
        self.label = label
        self.processes = []
        self.outputs = {}
        self.drains = {}

    def start(self, name, *args):
        child = subprocess.Popen([self.executable, *map(str, args)], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=FLAGS)
        self.processes.append((name, child))
        if name != "broker":  # the broker's first line is read by broker_port, which then calls drain()
            self.drain(child)
        return child

    def drain(self, child):
        """Read a child's output while it runs. A pipe holds only a few KB on Windows: a member that traces a lot would
        block in its write, and stop talking to the mesh, until the parent got round to it (#4021)."""
        lines = []
        thread = threading.Thread(target=lambda: lines.extend(child.stdout), daemon=True)
        thread.start()
        self.drains[child.pid] = (lines, thread)

    def collect(self, child, timeout):
        """Wait for the child to exit and return everything it wrote; TimeoutExpired if it does not exit in time."""
        child.wait(timeout=timeout)
        lines, thread = self.drains[child.pid]
        thread.join(10)
        return "".join(lines)

    def kill_all(self):
        # Own Popen handles only; no PID discovery or shared-host process cleanup.
        for _, child in self.processes:
            if child.poll() is None:
                child.kill()
        for _, child in self.processes:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass


def broker_port(case, broker):
    first_line = queue.Queue()
    threading.Thread(target=lambda: first_line.put(broker.stdout.readline()), daemon=True).start()
    line = first_line.get(timeout=20)
    if line.startswith("GNS disabled"):
        broker.wait(timeout=5)
        print(line.strip())
        return None
    case.drain(broker)
    return str(int(line.strip()))


def member(case, port, slot, seats, identity, scenario, participants, *options):
    return case.start(identity, "member", slot, seats, identity, port, scenario, participants, *options)


def finish(case, timeout):
    failed = False
    deadline = time.monotonic() + timeout
    for name, child in case.processes:
        try:
            output = case.collect(child, max(1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            child.kill()
            output = case.collect(child, 10) + f"\n{name}: timed out\n"
            failed = True
        case.outputs[name] = output
        print(f"--- {case.label}: {name} (exit {child.returncode})")
        print(output, end="")
        failed = failed or child.returncode != 0
    return failed


def summarise(case):
    """Signals per pair, measured at the adapters: the cost of a full ICE exchange through the room."""
    sent = {}
    for output in case.outputs.values():
        for match in re.finditer(r"SIGNALS slot=(\d+) remote=(\d+) sent_blobs=(\d+) sent_bytes=(\d+) "
                                 r"received_blobs=(\d+) received_bytes=(\d+)", output):
            slot, remote, blobs, size, rblobs, rsize = map(int, match.groups())
            sent[(slot, remote)] = (blobs, size, rblobs, rsize)
    if not sent:
        return
    pairs = {}
    for (slot, remote), value in sent.items():
        pairs.setdefault(tuple(sorted((slot, remote))), []).append(value)
    blobs = [sum(v[0] for v in values) for values in pairs.values()]
    sizes = [sum(v[1] for v in values) for values in pairs.values()]
    print(f"SIGNAL-STATS {case.label}: {len(pairs)} pairs, blobs per pair min/max {min(blobs)}/{max(blobs)}, "
          f"bytes per pair min/max {min(sizes)}/{max(sizes)}")


def mesh_case(executable, seats, scenario="mesh"):
    case = Case(executable, f"{seats}-member {scenario}")
    try:
        broker = case.start("broker", "broker", seats, seats)
        port = broker_port(case, broker)
        if port is None:
            return 77
        for slot in range(seats):
            member(case, port, slot, seats, f"peer{slot}", scenario, seats)
            time.sleep(0.4)
        failed = finish(case, 300 if scenario.startswith("repair_twice") else 200 if scenario == "repair_gave_up" else 150 if seats > 4 else 100)
        summarise(case)
        if scenario.startswith("repair") or scenario == "maintenance":
            hashes = [match.group(1) for output in case.outputs.values()
                      for match in re.finditer(r"BATTLE .* hash=([0-9a-f]+)", output)]
            expected = seats - (1 if scenario in ("repair_refused", "repair_bad_key", "repair_twice_refused") else 0)
            # An abandoned pair removes one of its two seats (which one is the incident's decision).
            allowed = (seats - 2, seats - 1) if scenario == "repair_gave_up" else (expected,)
            if len(hashes) not in allowed or len(set(hashes)) != 1:
                print(f"Mixed-worker repair hashes differ or are missing: {hashes}")
                failed = True
        return 1 if failed else 0
    except (ValueError, queue.Empty, subprocess.TimeoutExpired) as error:
        print(f"Mesh case failed: {error}", file=sys.stderr)
        return 1
    finally:
        case.kill_all()


def lobby_retry_case(executable):
    case = Case(executable, "newcomer first ICE failure")
    try:
        broker = case.start("broker", "broker", 3, 3, "peer1", "peer2", "first")
        port = broker_port(case, broker)
        if port is None:
            return 77
        probe = None
        for slot in range(3):
            child = member(case, port, slot, 3, f"peer{slot}", "lobby_retry", 3, "short=1")
            if slot == 1:
                probe = child
            time.sleep(0.4)
        output = case.collect(probe, 45)
        if probe.returncode != 0:
            print(output, end="")
            return 1
        return 1 if finish(case, 100) else 0
    finally:
        case.kill_all()


def refused_case(executable, kind):
    """Newcomer `peer2` cannot join (blocked pair to peer1, or a different build); `peer2b` then fills slot 2."""
    case = Case(executable, f"refused newcomer ({kind})")
    seats = 3
    try:
        drop = ("peer2", "peer1") if kind == "unreachable" else ()
        broker = case.start("broker", "broker", seats, seats + 1, *drop)
        port = broker_port(case, broker)
        if port is None:
            return 77
        short = ("short=1",)
        member(case, port, 0, seats, "peer0", "mesh", seats, *short,
               *(("refused_seat=2", "refused_other=1") if kind == "unreachable" else ()))
        time.sleep(0.4)
        member(case, port, 1, seats, "peer1", "mesh", seats, *short, *(("vacated=2",) if kind == "unreachable" else ()))
        time.sleep(0.4)
        if kind == "unreachable":
            bad = member(case, port, 2, seats, "peer2", "refused", seats, "left=1762", "refused_other=1", *short)
        else:
            bad = member(case, port, 2, seats, "peer2", "refused", seats, "left=1503", "build=mesh-test-build-other", *short)
        try:
            bad_output = case.collect(bad, 60)
        except subprocess.TimeoutExpired:
            bad.kill()
            bad_output = case.collect(bad, 10)
            bad_output += "\npeer2: timed out\n"
            print(bad_output)
            return 1
        print(f"--- {case.label}: peer2 (exit {bad.returncode})")
        print(bad_output, end="")
        if bad.returncode != 0:
            # Show what the others saw before they are killed.
            case.processes = [(n, c) for n, c in case.processes if n != "peer2"]
            finish(case, 25)
            return 1
        case.outputs["peer2"] = bad_output
        time.sleep(0.3)
        # The refused slot is vacant again: another newcomer fills it and the room then starts.
        member(case, port, 2, seats, "peer2b", "mesh", seats, *short)
        remaining = [(n, c) for n, c in case.processes if n != "peer2"]
        case.processes = remaining
        failed = finish(case, 120)
        return 1 if failed else 0
    except (ValueError, queue.Empty, subprocess.TimeoutExpired) as error:
        print(f"Refused case failed: {error}", file=sys.stderr)
        return 1
    finally:
        case.kill_all()


if __name__ == "__main__":
    executable = sys.argv[1]
    cases = [lambda: mesh_case(executable, 2, "maintenance"),
             lambda: mesh_case(executable, 3), lambda: mesh_case(executable, 4),
             lambda: refused_case(executable, "unreachable"), lambda: refused_case(executable, "build"),
             lambda: mesh_case(executable, 4, "repair"), lambda: mesh_case(executable, 3, "repair_refused"),
             lambda: mesh_case(executable, 3, "repair_bad_key"), lambda: mesh_case(executable, 4, "repair_return"),
             lambda: mesh_case(executable, 4, "repair_late_arm"), lambda: mesh_case(executable, 3, "repair_lower")]
    if os.environ.get("EAWR_GNS_MESH_NINE") == "1":
        cases.append(lambda: mesh_case(executable, 9, "repair"))
    if len(sys.argv) > 2 and sys.argv[2] == "lobby_stale":
        cases = [lambda: lobby_retry_case(executable)]
    if len(sys.argv) > 2 and sys.argv[2] == "lobby_retry":
        cases = [lambda: lobby_retry_case(executable), lambda: refused_case(executable, "unreachable")]
    if len(sys.argv) > 2 and sys.argv[2] == "twice":
        # #4021: six members, one pair silent twice 36 s apart; then the same with seat 5 unable to re-attach. Separate
        # arguments: the same with the second acceptor arming 3 s late, and a pair given up on that a late request must
        # not revive.
        cases = [lambda: mesh_case(executable, 6, "repair_twice"), lambda: mesh_case(executable, 6, "repair_twice_refused")]
    if len(sys.argv) > 2 and sys.argv[2] == "twice_late":
        cases = [lambda: mesh_case(executable, 6, "repair_twice_late")]
    if len(sys.argv) > 2 and sys.argv[2] == "gave_up":
        cases = [lambda: mesh_case(executable, 4, "repair_gave_up")]
    if len(sys.argv) > 2 and sys.argv[2] == "maintenance":
        cases = [lambda: mesh_case(executable, 2, "maintenance")]
    if len(sys.argv) > 2 and sys.argv[2] == "nine":
        # The nine-member case on its own (run once per slice at its final head).
        cases = [lambda: mesh_case(executable, 9, "repair")] if os.environ.get("EAWR_GNS_MESH_NINE") == "1" else []
        if not cases:
            print("Nine-member mesh skipped: set EAWR_GNS_MESH_NINE=1")
            raise SystemExit(77)
    for run in cases:
        result = run()
        if result != 0:
            raise SystemExit(result)
