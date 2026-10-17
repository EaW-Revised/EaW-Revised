"""Independent two- and nine-seat viewer GNS processes through a real TLS room service."""
import pathlib
import queue
import socket
import subprocess
import sys
import threading
import time
import uuid


def published(path, length):
    try:
        value = path.read_text()
    except (FileNotFoundError, PermissionError):
        return None
    return value if len(value) == length else None


def main():
    bridge, relay, root = sys.argv[1:]
    directory = pathlib.Path(root) / uuid.uuid4().hex
    directory.mkdir(parents=True)
    cert, key = directory / "certificate.pem", directory / "key.pem"
    subprocess.run([relay, "--generate-certificate", "--certificate-file", str(cert), "--key-file", str(key)], check=True, capture_output=True)
    server = subprocess.Popen([relay, "--certificate-file", str(cert), "--key-file", str(key), "--port", "0", "--room-aliases", "1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children = []
    peer_logs = {}

    def child_output(child):
        out, err = child.communicate()
        if child in peer_logs:
            out, err = (path.read_text(encoding="utf-8", errors="replace") for path in peer_logs[child])
        return out, err

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

        def start_server(aliases):
            # Every host publishes a directory room, and the directory creates at most four principals per address and minute:
            # a few scenarios per server keep each inside that budget.
            extra = subprocess.Popen([relay, "--certificate-file", str(cert), "--key-file", str(key), "--port", "0", "--room-aliases", aliases],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            children.append(extra)
            extra_lines = queue.Queue()
            threading.Thread(target=lambda: [extra_lines.put(row) for row in extra.stdout], daemon=True).start()
            ready = extra_lines.get(timeout=30)
            assert ready.startswith("relay ready port="), ready
            return extra, ready.strip().split("=")[1]

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
            deadline = time.monotonic() + 60
            while published(invitation, code_length) is None:
                assert host.poll() is None, host.communicate()
                assert time.monotonic() < deadline, "publication timeout"
                time.sleep(0.01)
            guest = subprocess.Popen([bridge, guest_role, selected_port, str(cert), str(invitation)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            children.append(guest)
            proof_paths = [pathlib.Path(str(invitation) + "." + role) for role in (host_role, guest_role)]
            lengths = [len(value) for value in expected] if expected else [64, 64]
            while not all(published(path, length) is not None for path, length in zip(proof_paths, lengths)):
                for child in (host, guest):
                    assert child.poll() is None, child.communicate()
                assert time.monotonic() < deadline, "setup/tick-zero timeout"
                time.sleep(0.01)
            proof = tuple(path.read_text() for path in proof_paths)
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

        # cancel, normal, mismatch and the invalid host use four principals on the first server; silent, public and refill four more.
        pair("normal", "host", "guest")
        pair("mismatch", "host-mismatch", "guest-mismatch", ("staging", "EAWR-NET-1503"))
        # An invalid host fails before a guest can attach: its typed refusal is the whole proof (the room-closed code a member
        # sees is the relay room tests' subject).
        bad_setup = directory / "bad-setup.txt"
        run("host-bad-setup", bad_setup)
        assert pathlib.Path(str(bad_setup) + ".host-bad-setup").read_text() == "EAWR-NET-1535", "invalid staging refuses with 1535"
        second, second_port = start_server("1")
        pair("silent", "host-silent", "guest-silent", ("silent", "EAWR-NET-1536"), selected_port=second_port)
        pair("directory-public", "host-directory-public", "guest-directory-public", code_length=32, selected_port=second_port)
        pair("refill", "host-refill", "guest-refill", ("refilled", "rejoined"), selected_port=second_port)
        second.terminate()
        second.wait(timeout=10)
        private_server, private_port = start_server("1")
        pair("directory-private", "host-directory-private", "guest-directory-private", code_length=32, selected_port=private_port)
        private_server.terminate()
        private_server.wait(timeout=10)

        def leave_refill(seats):
            leave_server, leave_port = start_server("1")
            invitation = directory / f"leave{seats}.txt"
            live = {}
            deadline = time.monotonic() + 90

            def launch(role):
                paths = [pathlib.Path(str(invitation) + "." + role + suffix) for suffix in (".stdout", ".stderr")]
                with paths[0].open("w", encoding="utf-8") as out, paths[1].open("w", encoding="utf-8") as err:
                    child = subprocess.Popen([bridge, role, leave_port, str(cert), str(invitation)], stdout=out, stderr=err, text=True)
                children.append(child)
                peer_logs[child] = paths
                live[role] = child
                return child

            def wait_for(suffix):
                path = pathlib.Path(str(invitation) + suffix)
                while not path.exists():
                    for role, child in live.items():
                        assert child.poll() is None, (role, child_output(child))
                    assert time.monotonic() < deadline, f"{seats}-seat leave/refill timeout: {suffix}"
                    time.sleep(0.01)

            host_role = f"host-leave{seats}"
            launch(host_role)
            while published(invitation, 4) is None:
                assert live[host_role].poll() is None, child_output(live[host_role])
                assert time.monotonic() < deadline, "leave room publication timeout"
                time.sleep(0.01)
            for seat in range(1, seats):
                role = f"guest-leave{seats}-{seat}"
                launch(role)
                wait_for(".joined-" + role)
            wait_for(".leave-go")
            for seat in range(seats - 2, seats):
                role = f"guest-leave{seats}-{seat}"
                child = live.pop(role)
                child.wait(timeout=20)
                assert child.returncode == 0, child_output(child)
                assert pathlib.Path(str(invitation) + ".departed-" + role).read_text() == "left"
            pathlib.Path(str(invitation) + ".resume").touch()
            wait_for(".vacant")
            for seat in range(seats - 2, seats):
                role = f"guest-leave{seats}-new-{seat}"
                launch(role)
                wait_for(".joined-" + role)
            proofs = [pathlib.Path(str(invitation) + "." + role) for role in live]
            while not all(published(proof, 64) is not None for proof in proofs):
                for role, child in live.items():
                    assert child.poll() is None, (role, child_output(child))
                assert time.monotonic() < deadline, "refilled room must Start and reach tick 10"
                time.sleep(0.01)
            assert len({proof.read_text() for proof in proofs}) == 1, "refilled online peers agree at tick 10"
            pathlib.Path(str(invitation) + ".done").touch()
            for role, child in live.items():
                child.wait(timeout=10)
                assert child.returncode == 0, (role, child_output(child))
            leave_server.terminate()
            leave_server.wait(timeout=10)
            print(f"{seats}-seat online room: two guests left together, seats reopened, replacements joined and Start reached equal tick-10 hashes")

        leave_refill(3)
        leave_refill(9)
        no_alias_server, no_alias_port = start_server("0")
        pair("directory-no-alias", "host-directory-no-alias", "guest-directory-no-alias", code_length=32, selected_port=no_alias_port)
        # A host that can only be joined by code fails at once, typed, on a service without room aliases.
        no_code = directory / "no-code.txt"
        run("host-no-code", no_code, no_alias_port)
        assert pathlib.Path(str(no_code) + ".host-no-code").read_text() == "EAWR-NET-1787", "no room codes: typed failure"
        no_alias_server.terminate()
        no_alias_server.wait(timeout=10)
        nine_server, nine_port = start_server("1")
        invitation = directory / "nine.txt"
        roles = ["host-nine", *(f"guest-nine-{seat}" for seat in range(1, 8)), "guest-nine-directory-8"]
        peers = []
        deadline = time.monotonic() + 90
        for role in roles:
            if peers:
                while published(invitation, 4) is None:
                    assert peers[0].poll() is None, peers[0].communicate()
                    assert time.monotonic() < deadline, "nine-seat publication timeout"
                    time.sleep(0.01)
            output_paths = [pathlib.Path(str(invitation) + "." + role + suffix) for suffix in (".stdout", ".stderr")]
            # File-backed streams cannot block a peer on a full diagnostic pipe.
            with output_paths[0].open("w", encoding="utf-8") as out, output_paths[1].open("w", encoding="utf-8") as err:
                peer = subprocess.Popen([bridge, role, nine_port, str(cert), str(invitation)], stdout=out, stderr=err, text=True)
            peer_logs[peer] = output_paths
            peers.append(peer)
            children.append(peer)
            if role != "host-nine":
                attached = pathlib.Path(str(invitation) + ".joined-" + role)
                while not attached.exists():
                    for member in peers:
                        assert member.poll() is None, (member.args[1], child_output(member))
                    assert time.monotonic() < deadline, "nine-seat member attachment timeout: " + role
                    time.sleep(0.01)
        proofs = [pathlib.Path(str(invitation) + "." + role) for role in roles]
        while not all(published(proof, 64) is not None for proof in proofs):
            for peer in peers:
                assert peer.poll() is None, (peer.args[1], child_output(peer))
            assert time.monotonic() < deadline, "nine-seat setup/tick-zero timeout"
            time.sleep(0.01)
        assert len({proof.read_text() for proof in proofs}) == 1, "nine online peers with 1/2/4/8 workers have equal tick-10 hashes"
        assert published(pathlib.Path(str(invitation) + ".nine-repair"), 8) == "restored", "the directory guest proves native repair and viewer pause/resume before tick 10"
        pathlib.Path(str(invitation) + ".done").touch()
        for peer in peers:
            peer.wait(timeout=10)
            out, err = child_output(peer)
            assert peer.returncode == 0, out + err
        nine_server.terminate()
        nine_server.wait(timeout=10)
        print("TLS publication/cancel/retry -> GNS staging -> setup/tick-zero -> equal tick-10 hashes, build refusal and bounded snapshot wait passed")
        print("Nine-seat online 3v3v3 code and room-list admission plus native pair repair passed with equal battle hashes across 1/2/4/8 simulation workers")
        return 0
    finally:
        failed = sys.exc_info()[0] is not None
        diagnostics = []
        if failed:
            # Failure is already decided. Let the originating peer finish reporting
            # before killing the room's remaining processes and losing its cause.
            reporting_deadline = time.monotonic() + 3
            while any(child.args[0] == bridge and child.poll() is None for child in children):
                if time.monotonic() >= reporting_deadline:
                    break
                time.sleep(0.01)
        for child in children:
            killed = child.poll() is None
            if child.poll() is None:
                child.kill()
            child.wait()
            if failed:
                if child.args[0] == bridge:
                    out, err = child_output(child)
                    diagnostics.append(f"{child.args[1]} exit={child.returncode} killed_by_harness={killed}\n{out}{err}")
                else:
                    diagnostics.append("relay stderr:\n" + child.stderr.read())
        server.terminate()
        server.wait(timeout=10)
        if failed:
            diagnostics.append("initial relay stderr:\n" + server.stderr.read())
            for status in directory.glob("*.host-status"):
                diagnostics.append("host status/pump history:\n" + status.read_text(encoding="utf-8", errors="replace"))
            report = directory.with_suffix(".failure.txt")
            report.write_text("\n".join(diagnostics), encoding="utf-8")
            print(f"Online process failure diagnostics: {report}\n" + "\n".join(diagnostics), file=sys.stderr)
        for path in directory.iterdir():
            path.unlink()
        directory.rmdir()


if __name__ == "__main__":
    sys.exit(main())
