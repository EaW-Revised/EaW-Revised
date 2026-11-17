"""Host and join roles of eawr_online_test against a local TLS relay (the bridge harness fixtures)."""
import hashlib
import os
import pathlib
import ssl
import shutil
import queue
import re
import subprocess
import sys
import threading
import time
import uuid

SKIP = 77
# Both roles prepare for this long; the host must see the guest's Ready well before a prepare could end.
PREPARE_MS = 1500
READY_BOUND_MS = 1000


def ready_latency_ms(host_log, join_log):
    """Guest click to host-sees-Ready, from the two roles' machine-wide steady clocks (same machine)."""
    clicked = re.search(r"ready_click_steady_ms=(\d+)", join_log)
    seen = re.search(r"peer_ready_steady_ms=(\d+)", host_log)
    assert clicked and seen, "ready timing lines missing: " + join_log + host_log
    return int(seen.group(1)) - int(clicked.group(1))


def main():
    tool, relay, root = sys.argv[1:4]
    pin_mode = len(sys.argv) > 4 and sys.argv[4] == "pin"
    tool, relay = str(pathlib.Path(tool).resolve()), str(pathlib.Path(relay).resolve())
    directory = pathlib.Path(root) / uuid.uuid4().hex
    directory.mkdir(parents=True)
    directory = directory.resolve()
    clean = directory / "clean-working-directory"
    clean.mkdir()
    environment = {k: v for k, v in os.environ.items() if k not in ("EAWR_EAW_GAME_ROOT", "EAWR_REMAKE_MOD_ROOT", "EAWR_GAME_DATA_ROOT")}
    cert, key = directory / "certificate.pem", directory / "key.pem"
    try:
        subprocess.run([relay, "--generate-certificate", "--certificate-file", str(cert), "--key-file", str(key)], check=True, capture_output=True)
    except OSError as error:
        if getattr(error, "winerror", 0) == 4551:  # Application Control policy on this host blocks the relay exe
            print("skipped: host policy blocks the relay executable")
            clean.rmdir()
            directory.rmdir()
            return SKIP
        raise
    server = subprocess.Popen([relay, "--certificate-file", str(cert), "--key-file", str(key), "--port", "0", "--room-aliases", "1"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children = []
    try:
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(line) for line in server.stdout], daemon=True).start()
        deadline = time.monotonic() + 30
        while True:
            try:
                line = lines.get(timeout=0.1)
                break
            except queue.Empty:
                assert server.poll() is None, "relay exited before readiness: " + server.stderr.read()
                assert time.monotonic() < deadline, "relay readiness timeout"
        assert line.startswith("relay ready port="), line
        port = line.strip().split("=")[1]
        der = ssl.PEM_cert_to_DER_cert(cert.read_text())
        pin = hashlib.sha256(der).hexdigest()
        fingerprint = subprocess.run([relay, "--fingerprint", "--certificate-file", str(cert)], capture_output=True, text=True, check=True)
        assert fingerprint.stdout.strip() == pin, "--fingerprint reads PEM and hashes certificate DER"
        trust = ["--pin", pin] if pin_mode else ["--ca-file", str(cert), "--certificate-name", "localhost"]
        common = ["--address", "127.0.0.1", "--port", port, *trust, "--timeout", "60"]

        # OS trust must fail closed on this self-signed cert, with valid DNS options.
        untrusted = subprocess.run([tool, "host", "--address", "127.0.0.1", "--port", port,
                                    "--certificate-name", "localhost", "--system-ca", "--timeout", "15",
                                    "--log", str(directory / "untrusted.log")],
                                   capture_output=True, text=True, timeout=30, cwd=clean, env=environment)
        assert untrusted.returncode == 1 and "RESULT: FAIL" in untrusted.stdout, untrusted.stdout + untrusted.stderr
        assert "(EAWR-NET-1519)" not in untrusted.stdout, "system trust must reach TLS rather than reject valid DNS options"

        # A service without room aliases cannot give the host a code: the host fails at once with the typed diagnostic.
        bare = subprocess.Popen([relay, "--certificate-file", str(cert), "--key-file", str(key), "--port", "0", "--room-aliases", "0"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        children.append(bare)
        bare_lines = queue.Queue()
        threading.Thread(target=lambda: [bare_lines.put(row) for row in bare.stdout], daemon=True).start()
        bare_ready = bare_lines.get(timeout=30)
        assert bare_ready.startswith("relay ready port="), bare_ready
        bare_common = ["--address", "127.0.0.1", "--port", bare_ready.strip().split("=")[1], *trust, "--timeout", "30"]
        no_code = subprocess.run([tool, "host", *bare_common, "--log", str(directory / "no-code.log")],
                                 capture_output=True, text=True, timeout=60, cwd=clean, env=environment)
        assert no_code.returncode == 1 and "RESULT: FAIL" in no_code.stdout and "(EAWR-NET-1787)" in no_code.stdout, no_code.stdout + no_code.stderr
        bare.terminate()
        bare.wait(timeout=10)

        # Argument errors exit 2 and never touch the network.
        bad = subprocess.run([tool, "host", "--port", "0"], capture_output=True, text=True, timeout=30)
        assert bad.returncode == 2 and "RESULT: FAIL" in bad.stderr, bad.stdout + bad.stderr

        def start(role, log, extra=()):
            child = subprocess.Popen([tool, role, *common, "--log", str(log), *extra], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=clean, env=environment)
            children.append(child)
            out = queue.Queue()
            threading.Thread(target=lambda: [out.put(line) for line in child.stdout], daemon=True).start()
            return child, out

        def collect(child, out):
            deadline = time.monotonic() + 75
            text = []
            while True:
                try:
                    text.append(out.get(timeout=0.1))
                except queue.Empty:
                    assert time.monotonic() < deadline, "tool completion timeout"
                    if child.poll() is not None and out.empty():
                        return "".join(text)

        # Wrong code: the join role fails with a message and exit 1.
        host_log, join_log = directory / "host.log", directory / "join.log"
        # Both roles prepare their match setup slowly, as a game install does; Ready must not wait for it.
        slow = ("--prepare-ms", str(PREPARE_MS))
        host, host_out = start("host", host_log, slow)
        head, code = [], None
        deadline = time.monotonic() + 40
        while code is None:
            try:
                line = host_out.get(timeout=0.1)
            except queue.Empty:
                assert host.poll() is None, "host exited early: " + "".join(head) + host.stderr.read()
                assert time.monotonic() < deadline, "no invitation code"
                continue
            head.append(line)
            found = re.search(r"Invitation code: (\w{4})", line)
            if found:
                code = found.group(1)
        wrong = "ZZZZ" if code != "ZZZZ" else "YYYY"
        refused = subprocess.run([tool, "join", *common, "--log", str(directory / "wrong.log"), "--code", wrong],
                                 capture_output=True, text=True, timeout=60, cwd=clean, env=environment)
        if refused.returncode == 3:
            return SKIP
        assert refused.returncode == 1 and "RESULT: FAIL" in refused.stdout, refused.stdout + refused.stderr

        # The join role prompts for the code on stdin.
        join, join_out = start("join", join_log, slow)
        join.stdin.write(code + "\n")
        join.stdin.flush()
        host_text = "".join(head) + collect(host, host_out)
        join_text = collect(join, join_out)
        host.wait(timeout=60)
        join.wait(timeout=60)
        assert host.returncode == 0, host_text + host.stderr.read()
        assert join.returncode == 0, join_text + join.stderr.read()
        for text in (host_text, join_text):
            assert "RESULT: PASS" in text, text
            assert "Route: direct" in text and "(confirmed by the peer)" in text, text
        hashes = [re.search(r"state hash: (\w{16})", t).group(1) for t in (host_text, join_text)]
        assert hashes[0] == hashes[1], hashes
        ready_ms = ready_latency_ms(host_log.read_text(), join_log.read_text())
        assert 0 <= ready_ms < READY_BOUND_MS, f"the host saw the guest's Ready {ready_ms} ms after the click (bound {READY_BOUND_MS} ms, prepare {PREPARE_MS} ms)"

        # Sanitized logs: no code, no peer address, no service address.
        for log in (host_log, join_log):
            content = log.read_text()
            assert "RESULT: PASS" in content and "Route:" in content, content
            assert not re.search(r"(?<![A-Za-z0-9_])" + re.escape(code) + r"(?![A-Za-z0-9_])", content), "invitation code leaked into " + log.name
            assert pin not in content, "certificate pin leaked"
            assert "127.0.0.1" not in content and "localhost" not in content, "address leaked into " + log.name
        assert not list(clean.iterdir()), "tool read/write smoke must keep the empty working directory clean"
        print("online test kit: host and join PASS over TLS+GNS, peer-confirmed tick-10 hashes, direct route, wrong code FAIL, sanitized logs")
        print("trust:", "pin" if pin_mode else "explicit CA file", "; system CA self-signed rejection passed")
        print(f"ready latency: the host saw the guest's Ready {ready_ms} ms after the click (prepare {PREPARE_MS} ms, bound {READY_BOUND_MS} ms)")
        print("no-game-data runtime: empty cwd, game-root environment unset:", clean)
        return 0
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait()
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(clean, ignore_errors=True)
        for path in directory.iterdir():
            path.unlink()
        directory.rmdir()


if __name__ == "__main__":
    sys.exit(main())
