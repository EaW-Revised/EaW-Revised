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
        host, host_out = start("host", host_log)
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
        join, join_out = start("join", join_log)
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
