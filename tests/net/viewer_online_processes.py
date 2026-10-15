"""Two independent GNS processes through a real TLS introduction service."""
import pathlib
import queue
import socket
import subprocess
import sys
import threading
import time
import uuid


def main():
    bridge, relay, root = sys.argv[1:]
    directory = pathlib.Path(root) / uuid.uuid4().hex
    directory.mkdir(parents=True)
    cert, key = directory / "certificate.pem", directory / "key.pem"
    subprocess.run([relay, "--generate-certificate", "--certificate-file", str(cert), "--key-file", str(key)], check=True, capture_output=True)
    server = subprocess.Popen([relay, "--certificate-file", str(cert), "--key-file", str(key), "--port", "0", "--room-aliases", "1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
                if server.poll() is not None:
                    raise AssertionError("relay exited before readiness: " + server.stderr.read())
                assert time.monotonic() < deadline, "relay readiness timeout"
        assert line.startswith("relay ready port="), line
        port = line.strip().split("=")[1]
        health = subprocess.run([relay, "--health", "--address", "localhost", "--port", port,
                                 "--ca-file", str(cert), "--certificate-name", "localhost"],
                                capture_output=True, text=True, timeout=10)
        assert health.returncode == 0 and health.stdout.strip() == "healthy", health.stdout + health.stderr

        def run(role, invitation, selected_port=port):
            child = subprocess.run([bridge, role, selected_port, str(cert), str(invitation)], capture_output=True, text=True, timeout=35)
            if child.returncode == 77:
                return 77
            assert child.returncode == 0, child.stdout + child.stderr
            return 0

        invitation = directory / "cancelled.txt"
        if run("cancel", invitation) == 77:
            return 77
        run("refused", invitation)
        with socket.socket() as closed:
            closed.bind(("127.0.0.1", 0))
            closed_port = str(closed.getsockname()[1])
        run("retry", directory / "retry.txt", closed_port)

        def pair(name, host_role, guest_role, expected=None, code_length=4, selected_port=port):
            invitation = directory / (name + ".txt")
            host = subprocess.Popen([bridge, host_role, selected_port, str(cert), str(invitation)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            children.append(host)
            deadline = time.monotonic() + 30
            while not invitation.exists() or len(invitation.read_text()) != code_length:
                assert host.poll() is None, host.communicate()
                assert time.monotonic() < deadline, "publication timeout"
                time.sleep(0.01)
            guest = subprocess.Popen([bridge, guest_role, selected_port, str(cert), str(invitation)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            children.append(guest)
            while not all(pathlib.Path(str(invitation) + "." + role).exists() for role in (host_role, guest_role)):
                for child in (host, guest):
                    assert child.poll() is None, child.communicate()
                assert time.monotonic() < deadline, "setup/tick-zero timeout"
                time.sleep(0.01)
            proof = tuple(pathlib.Path(str(invitation) + "." + role).read_text() for role in (host_role, guest_role))
            if expected is None:
                assert proof[0] == proof[1], "online battle hashes agree"
            else:
                assert proof == expected, proof
            pathlib.Path(str(invitation) + ".done").touch()
            audit = pathlib.Path(str(invitation) + ".private-audit")
            private_values = audit.read_text().splitlines() if audit.exists() else []
            for child in (host, guest):
                out, err = child.communicate(timeout=10)
                assert all(value not in out + err for value in private_values if value), "Private invitation, ownership or password reached bridge logs"
                assert child.returncode == 0, out + err

        pair("normal", "host", "guest")
        pair("directory-public", "host-directory-public", "guest-directory-public", code_length=32)
        pair("directory-private", "host-directory-private", "guest-directory-private", code_length=32)
        pair("mismatch", "host-mismatch", "guest-mismatch", ("EAWR-NET-1503", "EAWR-NET-1503"))
        pair("bad-setup", "host-bad-setup", "guest-bad-setup", ("EAWR-NET-1535", "EAWR-NET-1528"))
        pair("silent", "host-silent", "guest-silent", ("silent", "EAWR-NET-1536"))
        no_alias_server = subprocess.Popen([relay, "--certificate-file", str(cert), "--key-file", str(key), "--port", "0", "--room-aliases", "0"],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        children.append(no_alias_server)
        no_alias_lines = queue.Queue()
        threading.Thread(target=lambda: [no_alias_lines.put(line) for line in no_alias_server.stdout], daemon=True).start()
        no_alias_ready = no_alias_lines.get(timeout=30)
        assert no_alias_ready.startswith("relay ready port="), no_alias_ready
        no_alias_port = no_alias_ready.strip().split("=")[1]
        pair("directory-no-alias", "host-directory-no-alias", "guest-directory-no-alias", code_length=32, selected_port=no_alias_port)
        no_alias_server.terminate()
        no_alias_server.wait(timeout=10)
        print("TLS publication/cancel/retry -> GNS staging -> setup/tick-zero -> equal tick-10 hashes, build refusal and bounded snapshot wait passed")
        return 0
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait()
        server.terminate()
        server.wait(timeout=10)
        for path in directory.iterdir():
            path.unlink()
        directory.rmdir()


if __name__ == "__main__":
    sys.exit(main())
