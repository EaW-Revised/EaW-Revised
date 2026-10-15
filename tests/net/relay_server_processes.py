"""Independent TLS wire client against the standalone executable, with no GNS/game data."""
from __future__ import annotations

import base64
import hashlib
import hmac
import os
from pathlib import Path
import queue
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import warnings


def text(value: str) -> bytes:
    raw = value.encode("ascii")
    return struct.pack("!H", len(raw)) + raw


def field(raw: bytes) -> tuple[str, bytes]:
    size = struct.unpack("!H", raw[:2])[0]
    return raw[2:2 + size].decode("ascii"), raw[2 + size:]


def receive(stream: ssl.SSLSocket) -> bytes:
    def exact(size: int) -> bytes:
        result = b""
        while len(result) < size:
            chunk = stream.recv(size - len(result))
            if not chunk:
                raise EOFError("TLS stream ended")
            result += chunk
        return result
    size = struct.unpack("!I", exact(4))[0]
    assert 2 <= size <= 66560
    return exact(size)


def send(stream: ssl.SSLSocket, kind: int, payload: bytes = b"") -> None:
    body = bytes((2, kind)) + payload
    stream.sendall(struct.pack("!I", len(body)) + body)


def directory_guest() -> None:
    import json
    credentials = json.loads(sys.stdin.read())
    context = ssl.create_default_context(cafile=sys.argv[3])
    native = socket.create_connection(("127.0.0.1", int(sys.argv[2])), timeout=5)
    with context.wrap_socket(native, server_hostname="localhost") as stream:
        send(stream, 20, text(""))
        opened = receive(stream); assert opened[:2] == b"\x02\x1e"
        principal, raw = field(opened[2:]); assert len(principal) == 32 and raw == b""
        send(stream, 25, text(credentials["room"]) + text(credentials["password"]) + text("Guest\n"))
        admitted = receive(stream); assert admitted[:2] == b"\x02\x21"
        room, raw = field(admitted[2:]); assert room == credentials["room"] and struct.unpack("!I", raw[:4])[0] == 2 and raw[4:] == text("")  # seat, no invitation
        send(stream, 27, text("hello from another process\n")); assert receive(stream) == b"\x02\x1e"
        send(stream, 26); assert receive(stream) == b"\x02\x1e"
    print("directory guest passed")


def provider_contracts(exe: str, directory: Path, cert: Path, key: Path, mixed_config: Path) -> None:
    """Config-level contracts of the TURN provider keys (#3627): typed startup errors, env override, no secrets on argv."""
    key_id, api_token = "keyid-process_1", "tok_PROCESS_SECRET_0123456789abcdef"
    plain = directory / "provider.conf"
    plain.write_text(chr(10).join((f"certificate_file={cert}", f"key_file={key}", "port=0", "")), encoding="utf-8")
    environment = dict(os.environ)
    for name in [name for name in environment if name.startswith("EAWR_RELAY_")]:
        del environment[name]

    def run(arguments: list[str], extra: dict[str, str] | None = None, config: Path = plain) -> subprocess.CompletedProcess:
        return subprocess.run([exe, "--config", str(config)] + arguments, env=dict(environment, **(extra or {})),
                              capture_output=True, text=True, timeout=10)

    keys = {"EAWR_RELAY_CLOUDFLARE_TURN_KEY_ID": key_id, "EAWR_RELAY_CLOUDFLARE_TURN_API_TOKEN": api_token}
    unknown = run(["--turn-provider", "bogus"])
    assert unknown.returncode != 0 and "EAWR-NET-1555" in unknown.stderr and "bogus" not in unknown.stderr
    missing = run(["--turn-provider", "cloudflare"])
    assert missing.returncode != 0 and "EAWR-NET-1555" in missing.stderr
    on_argv = run(["--turn-provider", "cloudflare", "--cloudflare-turn-api-token", api_token], {"EAWR_RELAY_CLOUDFLARE_TURN_KEY_ID": key_id})
    assert on_argv.returncode != 0 and "EAWR-NET-1555" in on_argv.stderr and "cloudflare_turn_api_token" in on_argv.stderr
    assert api_token not in on_argv.stdout + on_argv.stderr
    id_on_argv = run(["--cloudflare_turn_key_id", key_id], {"EAWR_RELAY_CLOUDFLARE_TURN_API_TOKEN": api_token})
    assert id_on_argv.returncode != 0 and "EAWR-NET-1555" in id_on_argv.stderr and key_id not in id_on_argv.stdout + id_on_argv.stderr
    mixed = run([], dict(keys, EAWR_RELAY_TURN_PROVIDER="cloudflare"), mixed_config)
    assert mixed.returncode != 0 and "EAWR-NET-1555" in mixed.stderr, "Cloudflare provider plus coturn keys fails startup"
    wrong_side = run([], keys)
    assert wrong_side.returncode != 0 and "EAWR-NET-1555" in wrong_side.stderr, "Cloudflare secrets without the provider fail startup"
    assert api_token not in mixed.stdout + mixed.stderr + wrong_side.stdout + wrong_side.stderr
    process = subprocess.Popen([exe, "--config", str(plain)], env=dict(environment, EAWR_RELAY_TURN_PROVIDER="cloudflare", **keys),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        ready = process.stdout.readline()
        assert ready.startswith("relay ready port="), "Environment keys start the Cloudflare provider: " + ready
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
    leftovers = process.stdout.read() + process.stderr.read()
    assert api_token not in ready + leftovers and key_id not in ready + leftovers
    print("TURN provider config contracts passed")


def main() -> None:
    exe = str(Path(sys.argv[1]).resolve())
    artifacts = Path(sys.argv[2]); artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=artifacts) as temporary:
        directory = Path(temporary)
        cert, key = directory / "cert.pem", directory / "key.pem"
        pin = subprocess.check_output([exe, "--generate-certificate", "--certificate-file", str(cert),
                                       "--key-file", str(key)], text=True).strip()
        assert len(pin) == 64 and int(pin, 16) >= 0
        assert subprocess.check_output([exe, "--fingerprint", "--certificate-file", str(cert)], text=True).strip() == pin
        token = "a" * 64
        secret = "b" * 64
        config = directory / "relay.local.conf"
        config.write_text(f"certificate_file={cert}\nkey_file={key}\nrequire_access_token=1\naccess_token=bad-file-value\n"
                          f"port=12345\nturn_uri=turn:127.0.0.1:3478\nturn_secret={secret}\n", encoding="utf-8")
        env = dict(os.environ, EAWR_RELAY_ACCESS_TOKEN=token, EAWR_RELAY_PORT="12346")
        process = subprocess.Popen([exe, "--config", str(config), "--port", "0"], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=lambda: [lines.put(line) for line in process.stdout], daemon=True).start()
        streams: list[ssl.SSLSocket] = []
        try:
            ready = lines.get(timeout=10)
            assert ready.startswith("relay ready port="), ready
            port = int(ready.split("=")[1]); assert port not in (0, 12345, 12346)
            context = ssl.create_default_context(cafile=str(cert))

            def connect() -> ssl.SSLSocket:
                native = socket.create_connection(("127.0.0.1", port), timeout=3)
                try:
                    stream = context.wrap_socket(native, server_hostname="localhost")
                except BaseException:
                    native.close(); raise
                streams.append(stream); return stream

            health = subprocess.check_output([exe, "--health", "--address", "127.0.0.1", "--port", str(port),
                                               "--certificate-sha256", pin], text=True).strip()
            assert health == "healthy"
            ca_health = subprocess.check_output([exe, "--health", "--address", "127.0.0.1", "--port", str(port),
                                                "--certificate-name", "localhost", "--ca-file", str(cert)], text=True).strip()
            assert ca_health == "healthy"
            # An explicit name selects system trust, even if a local server certificate is supplied.
            system_health = subprocess.run([exe, "--health", "--address", "127.0.0.1", "--port", str(port),
                                            "--certificate-name", "localhost", "--certificate-file", str(cert)],
                                           text=True, capture_output=True, timeout=15)
            assert system_health.returncode != 0 and "EAWR-NET-1740" in system_health.stderr, system_health
            assert str(cert) not in system_health.stderr and "127.0.0.1" not in system_health.stderr
            host = connect()
            send(host, 1, text(token) + text("host") + struct.pack("!II", 1000, 0))  # lifetime, alias flag
            published = receive(host); assert published[:2] == b"\x02\x05"
            code, raw = field(published[2:]); identity, raw = field(raw)
            alias, raw = field(raw)
            assert alias == ""
            assert struct.unpack("!I", raw[:4])[0] == 1000
            owner, raw = field(raw[4:]); assert len(owner) == 32 and raw == b""  # publisher ownership capability
            assert len(code) == 32 and identity == "host"
            peer = connect(); send(peer, 2, text(code) + text("peer"))
            for stream, local, remote in ((host, "host", "peer"), (peer, "peer", "host")):
                paired = receive(stream); assert paired[:2] == b"\x02\x06"
                values: list[str] = []; raw = paired[2:]
                for _ in range(6):
                    value, raw = field(raw); values.append(value)
                assert values[:4] == [code, local, remote, "turn:127.0.0.1:3478"]
                username, password = values[4:]
                assert password == base64.b64encode(hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()).decode()
                assert len(raw) == 4 and struct.unpack("!I", raw)[0] == 86460
                assert 86400 < int(username.split(":")[0]) - time.time() <= 86460
            signal = bytes(range(256)) * 256
            send(host, 3, signal)
            forwarded = receive(peer); assert forwarded[:2] == b"\x02\x07"
            sender, raw = field(forwarded[2:]); assert sender == "host" and raw == signal
            send(peer, 3, b"reverse")
            sender, raw = field(receive(host)[2:]); assert sender == "peer" and raw == b"reverse"
            unauthorized = connect(); send(unauthorized, 1, text("wrong") + text("intruder") + struct.pack("!II", 1000, 0))
            assert receive(unauthorized) == b"\x02\x09" + struct.pack("!I", 1520)
            malformed = connect(); send(malformed, 4, b"trailing")
            assert receive(malformed) == b"\x02\x09" + struct.pack("!I", 1521)
            oversized = connect(); oversized.sendall(struct.pack("!I", 66561))
            try:
                assert not oversized.recv(1), "Oversize prefix cannot allocate a body or receive admission"
            except (ssl.SSLError, ConnectionResetError):
                pass
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", DeprecationWarning)
                obsolete = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                obsolete.check_hostname = False; obsolete.verify_mode = ssl.CERT_NONE
                obsolete.minimum_version = ssl.TLSVersion.TLSv1
                obsolete.maximum_version = ssl.TLSVersion.TLSv1_1
                obsolete.set_ciphers("ALL:@SECLEVEL=0")
            native = socket.create_connection(("127.0.0.1", port), timeout=3)
            old_tls = obsolete.wrap_socket(native, server_hostname="localhost", do_handshake_on_connect=False)
            try:
                old_tls.do_handshake()
            except ssl.SSLError:
                application_data = b""
                try:
                    application_data = old_tls.recv(1)
                except (ssl.SSLError, OSError):
                    pass
                assert application_data == b"", "Rejected TLS version cannot receive application data"
            else:
                old_tls.close(); raise AssertionError("Server accepted TLS below 1.2")
            finally:
                old_tls.close()

            # Parent is the directory host; a separate Python process admits and
            # chats as the guest. Private admission input travels only on stdin/TLS.
            for stream in streams:
                stream.close()
            time.sleep(0.3)
            directory_host = connect()
            send(directory_host, 20, text(""))
            opened = receive(directory_host); assert opened[:2] == b"\x02\x1e"
            principal, raw = field(opened[2:]); assert len(principal) == 32 and raw == b""
            metadata = b"".join(text(value) for value in
                                ("Process room", "Host", "build", "protocol", "content", "mod", "mode", "map"))
            metadata += struct.pack("!IIII", 1, 2, 0, 0)
            room_password = "process-private"
            send(directory_host, 22, text(token) + metadata + text(room_password) + text("") + text(""))  # no bridge invitation or owner
            response = receive(directory_host); assert response[:2] == b"\x02\x20"
            room, raw = field(response[2:]); ownership, raw = field(raw)
            assert len(room) == len(ownership) == 32 and room != ownership and struct.unpack("!I", raw)[0] == 30000
            import json
            guest_process = subprocess.run([sys.executable, __file__, "--directory-guest", str(port), str(cert)],
                                           input=json.dumps({"room": room, "password": room_password}),
                                           text=True, capture_output=True, timeout=10)
            assert guest_process.returncode == 0, guest_process.stderr
            assert guest_process.stdout.strip() == "directory guest passed"
            assert room_password not in guest_process.stdout + guest_process.stderr
            send(directory_host, 28, struct.pack("!I", 0))
            response = receive(directory_host); assert response[:2] == b"\x02\x1f"
            through, count, sequence, seat = struct.unpack("!IIII", response[2:18])
            name, raw = field(response[18:]); line, raw = field(raw)
            members = struct.unpack("!I", raw[:4])[0]; host_seat = struct.unpack("!I", raw[4:8])[0]
            host_name, raw = field(raw[8:])
            assert (through, count, sequence, seat, name, line) == (1, 1, 1, 2, "Guest", "hello from another process")
            assert members == host_seat == 1 and host_name == "Host" and raw == b""
            assert all(value.encode() not in response for value in (room_password, ownership, principal, token, secret))
            send(directory_host, 24, text(room) + text(ownership)); assert receive(directory_host) == b"\x02\x1e"
            directory_host.close()
            configured_health = subprocess.check_output([exe, "--health", "--config", str(config), "--port", str(port)],
                                                       env=env, text=True).strip()
            assert configured_health == "healthy", "Container health trust derives from its mounted certificate"
            invalid = subprocess.run([exe, "--config", str(config), "--queue-frames", "0"], env=env,
                                     capture_output=True, text=True, timeout=5)
            assert invalid.returncode != 0 and "EAWR-NET-1519" in invalid.stderr
            assert token not in invalid.stderr and secret not in invalid.stderr
            provider_contracts(exe, directory, cert, key, config)
        finally:
            for stream in streams:
                stream.close()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
            error = process.stderr.read()
            assert token not in error and secret not in error
            output = ready + "".join(list(lines.queue))
            assert token not in output and secret not in output
    print("Standalone TLS process, wire, TURN credentials, config precedence and malformed-input contracts passed")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--directory-guest":
        directory_guest()
    else:
        main()
