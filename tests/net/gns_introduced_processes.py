"""Two real GNS processes, with an in-process introduction fixture in a broker."""
import os
import queue
import subprocess
import sys
import threading


def run_case(mode):
    executable = sys.argv[1]
    processes = []
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

    def start(*args):
        child = subprocess.Popen([executable, *args], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, creationflags=flags)
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
        names = (("peer", "peer") if mode == 0 else ("peer", "delayed-ack") if mode == 1
                 else ("retry-host", "retry-expiry") if mode == 2 else ("retry-native-host", "retry-native") if mode == 3
                 else ("closure-refusal-host", "closure-refusal-peer"))
        peers = [start(names[role], str(role), port) for role in range(2)]
        failed = False
        for child in peers + [broker]:
            output, _ = child.communicate(timeout=55)
            print(output, end="")
            failed = failed or child.returncode != 0
        return 1 if failed else 0
    except (ValueError, queue.Empty, subprocess.TimeoutExpired) as error:
        print(f"Introduced process test failed: {error}", file=sys.stderr)
        return 1
    finally:
        # Own Popen handles only; no PID discovery or shared-host process cleanup.
        for child in processes:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=10)


if __name__ == "__main__":
    for mode in (0, 1, *([2] * 10), *([3] * 5), 4):
        result = run_case(mode)
        if result != 0:
            raise SystemExit(result)
