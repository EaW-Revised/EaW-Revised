#!/usr/bin/env python3
"""Contract tests of eawr_mods_service against docs/mods-service-protocol.md.

Drives the built service through its stdio pipes, the way the launcher's Godot manager does
(OS.execute_with_pipe): every implemented method and error code, the 1 MiB line limit, the framing rules, the exit
on a closed stdin and after shutdown, and the GUI subsystem of the Windows executable (no console window). It also
re-records committed transcripts and compares them byte for byte after normalising build-specific hello
values, service run time and native-root-dependent chain keys (protocol doc, section 9).

Usage: mods_service_contract.py <eawr_mods_service> [--update-transcripts]
  --update-transcripts  rewrite transcripts/*.jsonl from this build instead of comparing
"""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import queue
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import contextlib
import ctypes


def _service_minor() -> int:
    """kProtocolMinor from the service header: the one place the minor is written."""
    header = pathlib.Path(__file__).resolve().parents[2] / "apps" / "mods_service" / "src" / "service.hpp"
    found = re.search(r"kProtocolMinor\s*=\s*(\d+)", header.read_text(encoding="utf-8"))
    if found is None:
        raise ValueError(f"no 'kProtocolMinor = <number>' in {header}: update _service_minor in this test with the header")
    return int(found.group(1))


SERVICE_MINOR = _service_minor()

SERVICE: pathlib.Path | None = None
UPDATE = False
TRANSCRIPTS = pathlib.Path(__file__).resolve().parent / "transcripts"
TIMEOUT = 60
LIMIT = 1024 * 1024
MAX_ID = 2**53 - 1


def wire(message: object) -> str:
    return json.dumps(message, separators=(",", ":"), ensure_ascii=False)


def hello(request_id: int = 1, **protocol: int) -> dict:
    return {"id": request_id, "method": "hello",
            "params": {"client": "eawr-launcher", "client_version": "0.1.0",
                       "protocol": {"major": protocol.get("major", 1), "min_minor": protocol.get("min_minor", 0)}}}


def request(request_id: int, method: str, **params: object) -> dict:
    return {"id": request_id, "method": method, "params": params}


# The committed transcripts: each is one session, stdin closed after the last line.
TRANSCRIPT_SESSIONS: dict[str, list[str]] = {
    "hello": [wire(hello()), wire(request(2, "shutdown"))],
    "loadnotes_empty": [wire(hello()), wire(request(2, "loadnotes.read", chain_fingerprint="0" * 64)), wire(request(3, "shutdown"))],
    "unknown_method": [wire(hello()), wire(request(2, "records.query")), wire(request(3, "no.such.method"))],
    # Needs a live lock (see prepare_transcript_session): profile p is held by instance 1.
    "profile_locks": [wire(hello()), wire(request(2, "profile.locks")),
                      wire(request(3, "profile.save", profile={"id": "p", "name": "P2", "layers": []})),
                      wire(request(4, "profile.delete", id="p")), wire(request(5, "shutdown"))],
    "malformed": ["not json", "[1,2]", wire({"method": "hello"}), wire(request(1, "shutdown")), wire(hello(2))],
}

def write_state(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def prepare_transcript_session(name: str, user_data: pathlib.Path) -> None:
    """profile_locks: instance 1 runs profile p; this very test process is the live game process."""
    if name != "profile_locks":
        return
    write_state(user_data / "mods" / "profiles" / "p.json", json.dumps(
        {"format": "eawr-mod-profile", "version": 1, "id": "p", "name": "P", "layers": []}))
    write_state(user_data / "launcher" / "game-running.json", json.dumps({"profile": "p", "pid": os.getpid()}))


VOLATILE = re.compile(rb'"(service_version|engine_build|game_release|kit_build)":"[^"\\]*"')
NORMALISED = {b"service_version": b"0.0.0", b"engine_build": b"0.0.0+transcript", b"game_release": b"0.0.0", b"kit_build": b"0.0.0 (transcript)"}


def normalise(data: bytes) -> bytes:
    data = VOLATILE.sub(lambda match: b'"' + match.group(1) + b'":"' + NORMALISED[match.group(1)] + b'"', data)
    # Only service-owned timing and native-root-dependent cache keys are volatile.
    # Raw types, duration and key transitions are asserted in loadcheck_contract.py.
    data = re.sub(rb'"elapsed_ms":\d+', b'"elapsed_ms":0', data)
    return re.sub(rb'"chain_fingerprint":"[0-9a-f]{64}"', b'"chain_fingerprint":"' + b'0' * 64 + b'"', data)


def to_bytes(lines: list[str | bytes]) -> bytes:
    return b"".join((line if isinstance(line, bytes) else line.encode("utf-8")) + b"\n" for line in lines)


def run(payload: bytes, *args: str) -> tuple[list[dict], int, str]:
    """Writes the payload, closes stdin, returns (stdout messages, exit code, stderr)."""
    completed = subprocess.run([str(SERVICE), *args], input=payload, capture_output=True, timeout=TIMEOUT)
    messages = []
    assert completed.stdout == b"" or completed.stdout.endswith(b"\n"), "stdout must end with LF"
    for raw in completed.stdout.split(b"\n")[:-1] if completed.stdout else []:
        assert b"\r" not in raw, "the service writes LF line ends only"
        message = json.loads(raw.decode("utf-8"))
        assert isinstance(message, dict), f"stdout line is not an object: {raw[:200]!r}"
        messages.append(message)
    return messages, completed.returncode, completed.stderr.decode("utf-8", "replace")


def session(lines: list[str | bytes]) -> list[dict]:
    messages, code, stderr = run(to_bytes(lines))
    assert code == 0, f"exit code {code}; stderr:\n{stderr}"
    return messages


class Interactive:
    """A service with open pipes, read line by line (stdin stays open until close())."""

    def __init__(self, drain_stderr: bool = True, args: list[str] | None = None) -> None:
        self.process = subprocess.Popen([str(SERVICE), *(args or [])], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.lines: queue.Queue[bytes] = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        if drain_stderr:
            threading.Thread(target=self.process.stderr.read, daemon=True).start()

    def _pump(self) -> None:
        for raw in self.process.stdout:
            self.lines.put(raw)
        self.lines.put(b"")

    def send(self, message: dict) -> None:
        self.process.stdin.write((wire(message) + "\n").encode("utf-8"))
        self.process.stdin.flush()

    def receive(self) -> dict:
        raw = self.lines.get(timeout=TIMEOUT)
        assert raw, "stdout closed"
        return json.loads(raw)

    def close(self) -> None:
        if self.process.stdin and not self.process.stdin.closed:
            try:
                self.process.stdin.close()
            except OSError:
                pass

    def wait(self) -> int:
        return self.process.wait(timeout=TIMEOUT)

    def kill(self) -> None:
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=TIMEOUT)


def pe_subsystem(path: pathlib.Path) -> int:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20B, "expected PE32+"
    return struct.unpack_from("<H", data, optional + 68)[0]


class ServiceContract(unittest.TestCase):
    def assert_error(self, message: dict, request_id: int | None, code: str) -> None:
        self.assertEqual(message.get("id", "missing"), request_id, message)
        self.assertNotIn("result", message)
        self.assertEqual(message["error"]["code"], code, message)
        self.assertIsInstance(message["error"]["message"], str)
        self.assertTrue(message["error"]["message"])

    def assert_hello_result(self, result: dict) -> None:
        self.assertEqual(result["protocol"], {"major": 1, "minor": SERVICE_MINOR})
        self.assertEqual(result["capabilities"], ["chain.check", "chain.resolve", "files.page", "game.find", "hello", "loadcheck.cancel", "loadcheck.result", "loadcheck.start", "loadnotes.read", "mods.changes", "mods.discover", "profile.delete", "profile.list", "profile.load", "profile.locks", "profile.save", "records.cancel", "records.get", "records.index", "settings.set_builtin_layer", "shutdown"])
        self.assertEqual(result["capabilities"], sorted(result["capabilities"]))
        self.assertRegex(result["service_version"], r"^\d+\.\d+\.\d+$")
        self.assertEqual(result["game_release"], result["service_version"])
        self.assertRegex(result["engine_build"], r"^[A-Za-z0-9_.+-]+$")
        # The player-facing form of engine_build: "<label> (<short sha>)" plus " dirty" for a modified tree.
        self.assertRegex(result["kit_build"], r"^[A-Za-z0-9_.-]+ \([0-9a-f]{7}\)( dirty)?$|^[A-Za-z0-9_.+-]+$")
        if " (" in result["kit_build"]:  # an unrecognised identity (a test override) is shown unchanged
            self.assertEqual(result["kit_build"].split(" ")[0], result["engine_build"].split("+")[0])

    @unittest.skipUnless(sys.platform == "win32", "PE check of the Windows executable")
    def test_windows_executable_is_gui_subsystem(self):
        # 2 = IMAGE_SUBSYSTEM_WINDOWS_GUI: Windows never gives it a console window. Every other test here talks to it
        # over inherited pipes, as Godot's OS.execute_with_pipe does.
        self.assertEqual(pe_subsystem(SERVICE), 2)

    def test_running_instances_lock_their_profiles(self):
        with tempfile.TemporaryDirectory() as temporary:
            user = pathlib.Path(temporary).resolve() / "user"
            user.mkdir()
            gone = 0x3FFFFFF0   # no such process
            client = Interactive(args=["--user-data", str(user)])
            try:
                def call(ident, method, **params):
                    client.send(request(ident, method, **params))
                    return client.receive()

                def locked(message, ident, profile, instances):
                    self.assert_error(message, ident, "EAWR-MODS-SVC-0016")
                    self.assertEqual(message["error"]["data"], {"profile": profile, "instances": instances})
                    self.assertNotIn("could not be read", message["error"]["message"])

                client.send(hello()); self.assertIn("result", client.receive())
                for name in ("p", "q", "r"):
                    self.assertIn("result", call(3, "profile.save", profile={"id": name, "name": name, "layers": []}))
                self.assertEqual(call(4, "profile.locks")["result"], {"locked": [], "unreadable": []})
                me = os.getpid()
                write_state(user / "launcher" / "game-running.json", json.dumps({"profile": "p", "pid": me}))
                write_state(user / "instances" / "2" / "launcher" / "game-running.json",
                            json.dumps({"profile": "p", "pid": me, "instance": 2, "host_port": 47626}))
                write_state(user / "instances" / "3" / "launcher" / "game-running.json", json.dumps({"profile": "q", "pid": gone}))
                write_state(user / "instances" / "4" / "launcher" / "game-running.json", json.dumps({"profile": "", "pid": me}))
                write_state(user / "instances" / "5" / "launcher" / "game-running.json", "{not json")
                write_state(user / "instances" / "6" / "launcher" / "game-running.json",
                            json.dumps({"profile": "r", "pid": me, "process_start_time": "1"}))   # PID reused: another creation time
                write_state(user / "instances" / "9" / "launcher" / "game-running.json", json.dumps({"profile": "r", "pid": me}))  # no such id
                # The corrupt state of instance 5 is not "free": which profile it holds is unknown, so changes fail closed.
                self.assertEqual(call(5, "profile.locks")["result"],
                                 {"locked": [{"profile": "p", "instances": [1, 2]}], "unreadable": [5]})
                refused = call(40, "profile.save", profile={"id": "q", "name": "Q1", "layers": []})
                self.assert_error(refused, 40, "EAWR-MODS-SVC-0016")
                self.assertIn("could not be read", refused["error"]["message"])
                self.assertEqual(refused["error"]["data"], {"profile": "q", "instances": [], "unreadable": [5]})
                self.assert_error(call(41, "profile.delete", id="q"), 41, "EAWR-MODS-SVC-0016")
                (user / "instances" / "5" / "launcher" / "game-running.json").unlink()
                self.assertEqual(call(42, "profile.locks")["result"],
                                 {"locked": [{"profile": "p", "instances": [1, 2]}], "unreadable": []})
                before = (user / "mods" / "profiles" / "p.json").read_bytes()
                locked(call(6, "profile.save", profile={"id": "p", "name": "Renamed", "layers": []}), 6, "p", [1, 2])
                locked(call(7, "profile.delete", id="p"), 7, "p", [1, 2])
                self.assertEqual((user / "mods" / "profiles" / "p.json").read_bytes(), before)
                self.assertEqual(call(8, "profile.load", id="p")["result"]["profile"]["name"], "p")   # reading is always allowed
                # Dead, reused-PID, vanilla and corrupt states hold nothing.
                self.assertIn("result", call(9, "profile.save", profile={"id": "q", "name": "Q2", "layers": []}))
                self.assertIn("result", call(10, "profile.save", profile={"id": "r", "name": "R2", "layers": []}))
                self.assertEqual(call(11, "profile.delete", id="q")["result"], {"deleted": True})
                # The lock is the state files' business, not the caller's: a request cannot clear it.
                locked(call(12, "profile.save", profile={"id": "p", "name": "p", "layers": [], "launch": {"render_thread": "safe"}}), 12, "p", [1, 2])
                # Once the instances are gone the profile is free again.
                (user / "launcher" / "game-running.json").unlink()
                locked(call(13, "profile.delete", id="p"), 13, "p", [2])
                (user / "instances" / "2" / "launcher" / "game-running.json").unlink()
                self.assertEqual(call(14, "profile.locks")["result"], {"locked": [], "unreadable": []})
                self.assertIn("result", call(15, "profile.save", profile={"id": "p", "name": "Renamed", "layers": []}))
                self.assertEqual(call(16, "profile.delete", id="p")["result"], {"deleted": True})
                call(17, "shutdown")
            finally:
                client.close(); client.kill()

    def test_revised_layer_is_derived_and_configurable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            user = root / "user"
            mod = root / "mod"
            (mod / "Data").mkdir(parents=True)
            user.mkdir()
            settings = user / "eawr-settings.cfg"
            revised = root / "Revised"
            client = Interactive(args=["--user-data", str(user)])
            try:
                def call(ident, method, **params):
                    client.send(request(ident, method, **params))
                    return client.receive()
                client.send(hello()); self.assertIn("result", client.receive())
                call(2, "mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[str(mod)], walk_trees=False)
                profile = {"id":"old", "name":"Old", "layers":[]}
                call(3, "profile.save", profile=profile)
                row = call(4, "profile.load", id="old")["result"]["rows"][-1]
                self.assertEqual((row["key"], row["kind"], row["enabled"], row["state"]), ("builtin:revised", "builtin", True, "missing"))
                (revised / "Data").mkdir(parents=True)
                settings.write_text("eawr-settings 1\nrevised_layer_path=" + revised.as_posix() + "\naudio.master=24\nfuture.option=keep\n")
                row = call(5, "profile.load", id="old")["result"]["rows"][-1]
                self.assertEqual((pathlib.Path(row["root"]), row["state"]), (revised, "installed"))
                self.assertEqual(call(6, "settings.set_builtin_layer", key="builtin:revised", enabled=False)["result"], {"key":"builtin:revised", "enabled":False})
                loaded = call(7, "profile.load", id="old")["result"]
                self.assertFalse(loaded["rows"][-1]["enabled"])
                self.assertEqual(loaded["profile"]["layers"], [])
                self.assertIn("audio.master=24", settings.read_text())
                self.assertIn("future.option=keep", settings.read_text())
                self.assert_error(call(10, "settings.set_builtin_layer", key="builtin:revised", enabled="false"), 10, "EAWR-MODS-SVC-0005")
                invalid = {"id":"bad", "name":"Bad", "layers":[{"key":"builtin:revised", "root":str(revised)}]}
                self.assert_error(call(11, "profile.save", profile=invalid), 11, "EAWR-MODS-SVC-0005")
                old = user / "mods" / "profiles" / "old.json"
                old.write_text(json.dumps({"format":"eawr-mod-profile", "version":1, "id":"old", "name":"Old", "layers":[{"key":"builtin:revised", "root":None}], "separators":[{"before":1, "label":"after"}]}))
                legacy = call(12, "profile.load", id="old")["result"]
                self.assertEqual(legacy["profile"]["layers"], [])
                self.assertEqual(legacy["profile"]["separators"][0]["before"], 0)
                self.assertTrue(legacy["notes"])
                settings.write_text("eawr-settings 99\nfuture.option=keep\n")
                before = settings.read_bytes()
                self.assert_error(call(13, "settings.set_builtin_layer", key="builtin:revised", enabled=True), 13, "EAWR-MODS-SVC-0015")
                self.assertEqual(settings.read_bytes(), before)
                settings.write_text("eawr-settings 1\nrevised_layer_path=relative/path\n")
                row = call(14, "profile.load", id="old")["result"]["rows"][-1]
                self.assertIn("Relative", row["note"])
                self.assert_error(call(8, "settings.set_builtin_layer", key="bad", enabled=True), 8, "EAWR-MODS-SVC-0005")
                call(9, "shutdown")
            finally:
                client.close(); client.kill()

    def test_hello(self):
        messages = session([wire(hello(7))])
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0]["id"], 7)
        self.assertNotIn("error", messages[0])
        self.assert_hello_result(messages[0]["result"])

    def test_hello_accepts_unknown_members_and_omitted_params_elsewhere(self):
        greeting = hello()
        greeting["params"]["future_option"] = {"x": 1}
        greeting["trace"] = "ignored"
        messages = session([wire(greeting), wire({"id": 2, "method": "shutdown"})])
        self.assertIn("result", messages[0])
        self.assertEqual(messages[1], {"id": 2, "result": {}})

    def test_hello_params_are_checked(self):
        cases = [
            {"client_version": "1", "protocol": {"major": 1, "min_minor": 0}},
            {"client": 5, "client_version": "1", "protocol": {"major": 1, "min_minor": 0}},
            {"client": "c", "client_version": "1"},
            {"client": "c", "client_version": "1", "protocol": [1, 0]},
            {"client": "c", "client_version": "1", "protocol": {"major": 1}},
            {"client": "c", "client_version": "1", "protocol": {"major": -1, "min_minor": 0}},
            {"client": "c", "client_version": "1", "protocol": {"major": 1, "min_minor": 0.5}},
        ]
        for params in cases:
            with self.subTest(params=params):
                messages = session([wire({"id": 3, "method": "hello", "params": params})])
                self.assert_error(messages[0], 3, "EAWR-MODS-SVC-0005")

    def test_protocol_mismatch_reports_the_service_version(self):
        for protocol in ({"major": 2}, {"major": 0}, {"major": 1, "min_minor": SERVICE_MINOR + 1}):
            with self.subTest(protocol=protocol):
                messages = session([wire(hello(4, **protocol)), wire(request(5, "shutdown"))])
                self.assert_error(messages[0], 4, "EAWR-MODS-SVC-0006")
                self.assert_hello_result(messages[0]["error"]["data"])
                # The session did not open.
                self.assert_error(messages[1], 5, "EAWR-MODS-SVC-0007")

    def test_hello_required_and_repeated(self):
        messages = session([wire(request(1, "shutdown")), wire(hello(2)), wire(hello(3)), wire(request(4, "shutdown"))])
        self.assert_error(messages[0], 1, "EAWR-MODS-SVC-0007")
        self.assertIn("result", messages[1])
        self.assert_error(messages[2], 3, "EAWR-MODS-SVC-0008")
        self.assertEqual(messages[3], {"id": 4, "result": {}})

    def test_unknown_method(self):
        long_name = "x" * 5000
        messages = session([wire(request(9, "no.such.method")), wire(hello(10)), wire(request(11, "records.query")),
                            wire(request(12, long_name))])
        self.assert_error(messages[0], 9, "EAWR-MODS-SVC-0004")
        self.assert_error(messages[2], 11, "EAWR-MODS-SVC-0004")
        self.assert_error(messages[3], 12, "EAWR-MODS-SVC-0004")
        self.assertLess(len(messages[3]["error"]["message"]), 200)

    def test_invalid_requests(self):
        lines = [
            wire({"id": -1, "method": "hello"}),
            wire({"id": 1.5, "method": "hello"}),
            wire({"id": "1", "method": "hello"}),
            wire({"id": MAX_ID + 1, "method": "hello"}),
            wire({"id": None, "method": "hello"}),
            wire({"method": "hello"}),
            wire({"id": 3}),
            wire({"id": 4, "method": ""}),
            wire({"id": 5, "method": 7}),
            wire({"id": 6, "method": "hello", "params": []}),
            wire({"id": MAX_ID, "method": "no.such.method"}),
        ]
        messages = session(lines)
        self.assertEqual(len(messages), len(lines))
        for message in messages[:6]:
            self.assert_error(message, None, "EAWR-MODS-SVC-0002")
        for message, request_id in zip(messages[6:10], (3, 4, 5, 6)):
            self.assert_error(message, request_id, "EAWR-MODS-SVC-0002")
        self.assert_error(messages[10], MAX_ID, "EAWR-MODS-SVC-0004")

    def test_malformed_lines(self):
        lines: list[str | bytes] = [
            "not json",
            *['{"id":' + number + ',"method":"hello"}' for number in ("01", "+1", ".5", "1.", "1.e5")],
            "[1,2]",
            '"text"',
            "42",
            '{"id":1,"method":"hello"',
            b"\xef\xbb\xbf" + wire(hello()).encode(),
            b'{"id":1,"method":"\xff"}',
            b'{"id":1,"method":"\xed\xa0\x80"}',
            '{"a":' * 70 + "1" + "}" * 70,  # nested deeper than 64
        ]
        messages = session(lines + [wire(hello(2))])
        self.assertEqual(len(messages), len(lines) + 1)
        for message in messages[:-1]:
            self.assert_error(message, None, "EAWR-MODS-SVC-0001")
        self.assertEqual(messages[-1]["id"], 2)
        self.assertIn("result", messages[-1])

    def test_nesting_boundary(self):
        for container in ("object", "array"):
            for depth in (64, 65):
                with self.subTest(container=container, depth=depth):
                    # Count the request object as level 1, including empty innermost containers.
                    value = "{}" if container == "object" else "[]"
                    for _ in range(depth - 2):
                        value = ('{"x":' + value + '}') if container == "object" else '[' + value + ']'
                    line = '{"id":1,"method":"no.such.method","extra":' + value + '}'
                    messages = session([line])
                    self.assert_error(messages[0], 1 if depth == 64 else None,
                                      "EAWR-MODS-SVC-0004" if depth == 64 else "EAWR-MODS-SVC-0001")

    def test_undrained_stderr_does_not_stall_replies(self):
        service = Interactive(drain_stderr=False)
        try:
            for _ in range(200):
                service.process.stdin.write(b"not json\n")
                service.process.stdin.flush()
                self.assert_error(service.receive(), None, "EAWR-MODS-SVC-0001")
            service.close()
            self.assertEqual(service.wait(), 0)
            stderr = service.process.stderr.read()
            self.assertLessEqual(len(stderr), 2048)
            self.assertEqual(stderr.count(b"log suppressed\n"), 1)
            self.assertTrue(stderr.endswith(b"log suppressed\n"))
        finally:
            service.close()
            service.kill()

    def test_hello_log_cuts_at_utf8_boundary(self):
        greeting = hello()
        greeting["params"]["client"] = "x" * 63 + "é" + "tail"
        greeting["params"]["client_version"] = "\x7f\n" + "é" * 40
        messages, code, stderr = run(to_bytes([wire(greeting)]))
        self.assertEqual(code, 0)
        self.assertIn("result", messages[0])
        self.assertNotIn("\ufffd", stderr)
        self.assertNotIn("\x7f", stderr)
        self.assertIn("x" * 63 + " ??", stderr)
        self.assertEqual(len(stderr.splitlines()), 1)

    @unittest.skipUnless(sys.platform == "win32", "PE imports of the Windows executable")
    def test_windows_executable_uses_only_system_dlls(self):
        # Reuse the launcher's PE import and delay-import reader.
        helper = TRANSCRIPTS.parents[2] / "apps" / "launcher" / "tests" / "launcher_smoke.py"
        spec = importlib.util.spec_from_file_location("launcher_smoke", helper)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        imports, _, _ = module.read_imports(SERVICE)
        self.assertTrue(imports)
        self.assertFalse([name for name in imports if name.startswith(module.FORBIDDEN_PREFIXES)], imports)
        self.assertFalse(imports - module.ALLOWED_IMPORTS, imports)

    def test_framing(self):
        payload = (b"\n   \t\n" + wire(hello(1)).encode() + b"\r\n\r\n"
                   + wire(request(2, "no.such.method")).encode() + b"\n"
                   + wire(request(3, "shutdown")).encode())  # last line without LF, answered at end of input
        messages, code, _ = run(payload)
        self.assertEqual(code, 0)
        self.assertEqual([message["id"] for message in messages], [1, 2, 3])
        self.assertEqual(messages[2], {"id": 3, "result": {}})

    def test_line_limit(self):
        def padded(request_id: int, size: int) -> bytes:
            head = '{"id":%d,"method":"pad","params":{"p":"' % request_id
            tail = '"}}'
            return (head + "x" * (size - len(head) - len(tail)) + tail).encode()

        exact, over = padded(1, LIMIT), padded(2, LIMIT + 1)
        self.assertEqual((len(exact), len(over)), (LIMIT, LIMIT + 1))
        with tempfile.TemporaryDirectory() as temporary:
            transcript = pathlib.Path(temporary) / "limit.jsonl"
            payload = (exact + b"\n" + exact.replace(b'"id":1', b'"id":3') + b"\r\n" + over + b"\n"
                       + over + b"\r\n" + b"y" * (3 * LIMIT) + b"\n" + wire(request(4, "pad")).encode() + b"\n")
            messages, code, stderr = run(payload, "--transcript", str(transcript))
            self.assertEqual(code, 0)
            self.assertEqual(len(messages), 6)
            self.assert_error(messages[0], 1, "EAWR-MODS-SVC-0004")
            self.assert_error(messages[1], 3, "EAWR-MODS-SVC-0004")
            for message in messages[2:5]:
                self.assert_error(message, None, "EAWR-MODS-SVC-0003")
            self.assert_error(messages[5], 4, "EAWR-MODS-SVC-0004")
            self.assertIn("1048577 bytes", stderr)
            oversize = [json.loads(line).get("oversize_bytes") for line in transcript.read_text("utf-8").splitlines()]
            self.assertEqual([size for size in oversize if size is not None], [LIMIT + 1, LIMIT + 1, 3 * LIMIT])

    def test_exits_when_stdin_closes(self):
        for opened in (False, True):
            with self.subTest(after_hello=opened):
                service = Interactive()
                try:
                    if opened:
                        service.send(hello())
                        self.assertIn("result", service.receive())
                    service.close()
                    self.assertEqual(service.wait(), 0)
                finally:
                    service.kill()

    def test_shutdown_exits_with_stdin_open(self):
        service = Interactive()
        try:
            service.send(hello())
            self.assertIn("result", service.receive())
            service.send(request(2, "shutdown"))
            self.assertEqual(service.receive(), {"id": 2, "result": {}})
            self.assertEqual(service.wait(), 0)  # stdin is still open here
        finally:
            service.close()
            service.kill()

    def test_workshop_changes_and_states(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            library = base / "library"
            content = library / "steamapps/workshop/content/32470"
            item = content / "77"
            (item / "Data/XML").mkdir(parents=True)
            asset = item / "Data/XML/synthetic.xml"
            asset.write_text("synthetic asset", encoding="utf-8")
            acf = library / "steamapps/workshop/appworkshop_32470.acf"
            def manifest(installed="old", latest="old", downloading=0, update=0):
                acf.write_text('"AppWorkshop" { "NeedsDownload" "' + str(downloading)
                               + '" "NeedsUpdate" "' + str(update) + '" "WorkshopItemsInstalled" { "77" {'
                               + '"manifest" "' + installed + '" "timeupdated" "1000" } } '
                               + '"WorkshopItemDetails" { "77" { "manifest" "' + latest
                               + '" "timeupdated" "1000" } } }', encoding="utf-8")
            manifest()
            service = Interactive(args=["--user-data", str(base / "user")])
            try:
                service.send(hello())
                self.assertIn("mods.changes", service.receive()["result"]["capabilities"])
                counter = 1
                def call(method, **params):
                    nonlocal counter
                    counter += 1
                    service.send(request(counter, method, **params))
                    return service.receive()
                def discover():
                    return call("mods.discover", walk_trees=False)["result"]["records"][0]
                call("mods.discover", steam_roots=[], steam_libraries=[str(library)], walk_trees=False)
                initial = call("mods.changes")["result"]
                self.assertEqual(initial["complete"], True)
                self.assertEqual(initial["changed"], False)
                revision = initial["revision"]
                self.assertRegex(revision, r"^[0-9a-f]{64}$")
                self.assertEqual(call("mods.changes", previous=revision)["result"], initial)
                # Asset edits leave the immediate directory timestamp unchanged: no recursive scan.
                asset.write_text("changed asset content", encoding="utf-8")
                self.assertFalse(call("mods.changes", previous=revision)["result"]["changed"])
                manifest(latest="new-longer", update=1)
                updated = call("mods.changes", previous=revision)["result"]
                self.assertTrue(updated["changed"])
                self.assertTrue(updated["complete"])
                self.assertEqual(discover()["state"], "needs-update")
                manifest(latest="new-longer", downloading=1, update=1)
                self.assertEqual(discover()["state"], "downloading")
                blocked = call("chain.check", layers=[{"key": "workshop:77"}])["result"]
                self.assertFalse(blocked["launchable"])
                self.assertTrue(any(d["code"] == "EAWR-MODS-0010" for d in blocked["diagnostics"]))
                manifest(installed="new-longer", latest="new-longer")
                self.assertEqual(discover()["state"], "installed")
                revision = call("mods.changes")["result"]["revision"]
                (content / "78").mkdir()
                self.assertTrue(call("mods.changes", previous=revision)["result"]["changed"])
                revision = call("mods.changes")["result"]["revision"]
                (content / "78").rmdir()
                self.assertTrue(call("mods.changes", previous=revision)["result"]["changed"])
                # A wrong-type ACF is a deterministic unreadable-source case on every platform.
                acf.unlink()
                acf.mkdir()
                self.assertEqual(call("mods.changes", previous=revision)["result"],
                                 {"revision": "", "changed": False, "complete": False})
                acf.rmdir()
                manifest()
                self.assertTrue(call("mods.changes", previous=revision)["result"]["complete"])
                for previous in ("", "x" * 64, "0" * 63, "A" * 64, "stale-after-restart"):
                    self.assertTrue(call("mods.changes", previous=previous)["result"]["changed"])
                for previous in (None, 1, {}, []):
                    self.assert_error(call("mods.changes", previous=previous), counter, "EAWR-MODS-SVC-0005")
                call("shutdown")
                self.assertEqual(service.wait(), 0)
            finally:
                service.close()
                service.kill()

    def test_pages_and_profile_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            service = Interactive(args=["--user-data", str(base / "user")])
            try:
                counter = 0
                def call(method, **params):
                    nonlocal counter
                    counter += 1
                    service.send(request(counter, method, **params))
                    return service.receive()
                service.send(hello())
                service.receive()
                folders = [base / name for name in ("A", "B", "C")]
                for folder in folders: folder.mkdir()
                first = call("mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[str(p) for p in folders], walk_trees=False, limit=1)["result"]
                token = first["next_cursor"]
                second = call("mods.discover", cursor=token, limit=1, walk_trees=False)["result"]
                self.assertNotEqual(first["records"][0]["key"], second["records"][0]["key"])
                third = call("mods.discover", cursor=second["next_cursor"], limit=1, walk_trees=False)["result"]
                self.assertIsNone(third["next_cursor"])
                default_walk = call("mods.discover")["result"]
                self.assertTrue(all(r["fingerprint"]["tree"] for r in default_walk["records"]))
                self.assert_error(call("mods.discover", cursor=token), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("profile.list", cursor=token), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("mods.discover", cursor=token, manual_folders=[], walk_trees=False), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("mods.discover", limit=0), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("game.find", hint=42), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("game.find", hint="relative"), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("mods.discover", manual_folders=[1]), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("chain.resolve", leaf="missing", known_chains={}), counter, "EAWR-MODS-SVC-0005")
                for bad_id in ("../escape", "con", "", "a/b"):
                    for method in ("profile.load", "profile.delete"):
                        self.assert_error(call(method, id=bad_id), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("profile.load", id="absent"), counter, "EAWR-MODS-SVC-0005")
                self.assert_error(call("profile.save", profile={"id":"x", "name":"x", "layers":[], "launch":{"render_thread":"invalid"}}), counter, "EAWR-MODS-SVC-0005")
                for name in ("a", "b"):
                    self.assertIn("result", call("profile.save", profile={"id":name,"name":name,"layers":[]}))
                (base / "user" / "mods" / "profiles" / "broken.json").write_text("{not json", encoding="utf-8")
                invalid_list = call("profile.list")["result"]["profiles"]
                self.assertEqual(next(p for p in invalid_list if p["id"] == "broken")["status"], "invalid")
                listed = call("profile.list", limit=1)["result"]
                self.assertEqual(listed["profiles"][0]["id"], "a")
                self.assertEqual(call("profile.list", cursor=listed["next_cursor"])["result"]["profiles"][0]["id"], "b")
                call("profile.delete", id="a")
                self.assert_error(call("profile.list", cursor=listed["next_cursor"]), counter, "EAWR-MODS-SVC-0005")
                manual = {"id":"manual", "name":"Manual", "layers":[{"key":first["records"][0]["key"], "root":str(folders[0])}]}
                call("mods.discover", manual_folders=[str(folders[0])])
                self.assertIn("result", call("profile.save", profile=manual))
                self.assertEqual(call("profile.load", id="manual")["result"]["status"], "ok")
                folders[0].rmdir()
                self.assertEqual(call("profile.load", id="manual")["result"]["status"], "missing")
                listed = call("profile.list")["result"]["profiles"]
                self.assertEqual(next(p for p in listed if p["id"] == "manual")["status"], "missing")
                call("shutdown")
                self.assertEqual(service.wait(), 0)
            finally:
                service.close()
                service.kill()

    def test_numeric_params_keep_precision(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            profile = {"id":"precision", "name":"Precision", "layers":[]}
            messages, code, stderr = run(to_bytes([wire(hello()),
                wire(request(2, "mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[], walk_trees=False)),
                wire(request(3, "profile.save", profile={**profile, "version":1.0000001})),
                wire(request(4, "profile.save", profile={**profile, "launch":{"frame_rate_limit":1.0000001}})),
                wire(request(5, "chain.resolve", leaf="missing", known_chains={"format":"eawr-known-chains", "version":1.0000001, "chains":[]})),
                wire(request(6, "shutdown"))]), "--user-data", str(base / "user"))
            self.assertEqual(code, 0, stderr)
            for message in messages[2:5]:
                self.assert_error(message, message["id"], "EAWR-MODS-SVC-0005")

    def test_steam_choices(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            game = base / "steamapps" / "common" / "Fixture"
            (game / "GameData" / "Data").mkdir(parents=True)
            (game / "corruption" / "Data").mkdir(parents=True)
            (game / "corruption" / "StarWarsG.exe").write_bytes(b"")
            user = base / "user"
            (user / "launcher").mkdir(parents=True)
            (user / "launcher" / "launcher.json").write_text(wire({"schema":"eawr-launcher/1", "game_root":str(game)}), encoding="utf-8")
            messages, code, stderr = run(to_bytes([wire(hello()), wire(request(2, "game.find", hint=str(game))),
                wire(request(3, "game.find")), wire(request(4, "shutdown"))]), "--user-data", str(user))
            self.assertEqual(code, 0, stderr)
            for message in messages[1:3]:
                self.assertEqual(message["result"]["kind"], "steam")
                self.assertEqual(pathlib.Path(message["result"]["root"]), game)

    def test_discovery_byte_budget(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            folders = [base / str(i) for i in range(3)]
            for i, folder in enumerate(folders):
                folder.mkdir()
                (folder / "modinfo.json").write_text(wire({"name":str(i), "padding":"x" * 400000}), encoding="utf-8")
            service = Interactive(args=["--user-data", str(base / "user")])
            try:
                service.send(hello()); service.receive()
                service.send(request(2, "mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[str(p) for p in folders], walk_trees=False, limit=500))
                first = service.receive()
                self.assertLessEqual(len(wire(first).encode("utf-8")), LIMIT)
                self.assertEqual(len(first["result"]["records"]), 2)
                service.send(request(3, "mods.discover", cursor=first["result"]["next_cursor"], walk_trees=False, limit=500))
                second = service.receive()
                self.assertLessEqual(len(wire(second).encode("utf-8")), LIMIT)
                self.assertEqual(len(second["result"]["records"]), 1)
                self.assertIsNone(second["result"]["next_cursor"])
                (folders[0] / "modinfo.json").write_text(wire({"name":"large", "padding":"x" * (LIMIT - 80)}), encoding="utf-8")
                service.send(request(4, "mods.discover", walk_trees=False, manual_folders=[str(folders[0])]))
                self.assert_error(service.receive(), 4, "EAWR-MODS-SVC-0011")
                # The stream remains usable after the oversized-record error.
                service.send(request(5, "mods.discover", walk_trees=False, manual_folders=[str(folders[1])]))
                self.assertEqual(len(service.receive()["result"]["records"]), 1)
                service.send(request(6, "shutdown")); service.receive()
                self.assertEqual(service.wait(), 0)
            finally:
                service.close(); service.kill()

    def test_model_methods(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            game = base / "game"
            user = base / "user"
            (game / "GameData" / "Data").mkdir(parents=True)
            (game / "corruption" / "Data").mkdir(parents=True)
            (game / "corruption" / "StarWarsG.exe").write_bytes(b"")
            for name in ("Leaf", "Parent"):
                (game / "corruption" / "Mods" / name / "Data").mkdir(parents=True)
            recorded = base / "models.jsonl"
            # Roots are synthetic. Discovery injection prevents consulting any host Steam library.
            profile = {"id": "fixture", "name": "Fixture", "layers": [
                {"key": "mods:Leaf", "root": str(game / "corruption" / "Mods" / "Leaf")} ]}
            lines = [hello(), request(2, "game.find", hint=str(game)),
                     request(3, "mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[], walk_trees=False, limit=1),
                     request(4, "chain.resolve", leaf="mods:Leaf", edges=[{"child": "mods:Leaf", "parent": "mods:Parent"}],
                             known_chains={"format": "eawr-known-chains", "version": 1, "chains": []}),
                     request(5, "profile.save", profile=profile), request(6, "profile.list"),
                     request(7, "profile.load", id="fixture"),
                     request(16, "settings.set_builtin_layer", key="builtin:revised", enabled=False),
                     request(17, "profile.load", id="fixture"), request(8, "profile.delete", id="fixture"),
                     request(9, "profile.list"), request(10, "profile.load", id="../escape"),
                     request(11, "chain.resolve", leaf="mods:unknown"),
                     request(12, "chain.resolve", leaf="mods:Leaf", edges=[
                         {"child": "mods:Leaf", "parent": "mods:Parent"}, {"child": "mods:Parent", "parent": "mods:Leaf"}]),
                     request(13, "game.find", hint=str(base / "missing")), request(14, "shutdown")]
            service = Interactive(args=["--user-data", str(user), "--transcript", str(recorded)])
            messages = []
            try:
                for line in lines:
                    service.send(line)
                    message = service.receive()
                    messages.append(message)
                    if line["id"] == 3:
                        service.send(request(15, "mods.discover", cursor=message["result"]["next_cursor"], limit=1, walk_trees=False))
                        messages.append(service.receive())
                self.assertEqual(service.wait(), 0)
            finally:
                service.close()
                service.kill()
            results = {msg["id"]: msg for msg in messages}
            self.assertEqual(pathlib.Path(results[2]["result"]["root"]), game)
            self.assertEqual(len(results[3]["result"]["records"]), 1)
            self.assertIsInstance(results[3]["result"]["next_cursor"], str)
            self.assertIsNone(results[15]["result"]["next_cursor"])
            self.assertNotEqual(results[3]["result"]["records"][0]["key"], results[15]["result"]["records"][0]["key"])
            self.assertEqual([r["key"] for r in results[4]["result"]["layers"]], ["mods:Leaf", "mods:Parent"])
            self.assertEqual([r["key"] for r in results[4]["result"]["rows"]], ["mods:Leaf", "mods:Parent", "builtin:revised"])
            full = results[5]["result"]["profile"]
            self.assertEqual(full["format"], "eawr-mod-profile")
            self.assertIsNone(full["launch"]["frame_rate_limit"])
            self.assertEqual(results[6]["result"]["profiles"], [{"id": "fixture", "name": "Fixture", "status": "ok"}])
            self.assertEqual(results[7]["result"]["status"], "ok")
            self.assertTrue(results[8]["result"]["deleted"])
            self.assertEqual(results[9]["result"]["profiles"], [])
            self.assert_error(results[10], 10, "EAWR-MODS-SVC-0005")
            self.assertFalse(results[11]["result"]["launchable"])
            self.assertFalse(results[12]["result"]["launchable"])
            self.assertIn("EAWR-MODS-0013", [d["code"] for d in results[12]["result"]["diagnostics"]])
            self.assertIsNone(results[13]["result"]["root"])
            self.assertTrue(results[13]["result"]["diagnostics"])
            entries = [json.loads(line) for line in normalise(recorded.read_bytes()).decode().splitlines()]
            def clean(value):
                if isinstance(value, str):
                    return value.replace(str(base), "<fixture>").replace(base.as_posix(), "<fixture>").replace("\\", "/")
                if isinstance(value, list): return [clean(item) for item in value]
                if isinstance(value, dict):
                    return {key: "<cursor>" if key in ("cursor", "next_cursor") and item is not None else clean(item) for key, item in value.items()}
                return value
            data = "".join(wire(clean(entry)) + "\n" for entry in entries)
            committed = TRANSCRIPTS / "model_methods.jsonl"
            if UPDATE: committed.write_text(data, encoding="utf-8", newline="\n")
            else: self.assertEqual(data, committed.read_text(encoding="utf-8"))

    def test_player_order_transcripts(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = pathlib.Path(temporary).resolve()
            game = base / "game"
            (game / "GameData" / "Data").mkdir(parents=True)
            (game / "corruption" / "Data").mkdir(parents=True)
            (game / "corruption" / "StarWarsG.exe").write_bytes(b"")
            for name in ("Leaf", "Parent"):
                (game / "corruption" / "Mods" / name / "Data").mkdir(parents=True)
            (game / "corruption" / "Mods" / "Leaf" / "modinfo.json").write_text(
                wire({"name":"Leaf", "dependencies":[{"modtype":0,"identifier":"Parent"}]}), encoding="utf-8")
            cases = {
                "chain_check_clean": (["mods:Leaf", "mods:Parent"], [], True, False, False),
                "chain_check_warning": (["mods:Parent", "mods:Leaf"], [], False, True, False),
                "chain_check_accepted": (["mods:Parent", "mods:Leaf"], ["EAWR-MODS-0019"], True, True, False),
                "chain_check_missing": (["mods:Leaf"], ["EAWR-MODS-0003"], False, False, True),
                "chain_check_unknown": (["mods:Unknown"], ["EAWR-MODS-0003"], False, False, True),
            }
            for name, (keys, accepted, launchable, warnings, diagnostics) in cases.items():
                with self.subTest(name=name):
                    recorded = base / (name + ".jsonl")
                    messages, code, stderr = run(to_bytes([
                        wire(hello(min_minor=1)), wire(request(2, "game.find", hint=str(game))),
                        wire(request(3, "mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[], walk_trees=False)),
                        wire(request(4, "chain.check", layers=[{"key":key} for key in keys], accepted_warnings=accepted)),
                        wire(request(5, "shutdown"))]), "--user-data", str(base / "user"), "--transcript", str(recorded))
                    self.assertEqual(code, 0, stderr)
                    result = messages[3]["result"]
                    self.assertEqual(result["launchable"], launchable)
                    self.assertEqual(bool(result["warnings"]), warnings)
                    self.assertEqual(bool(result["diagnostics"]), diagnostics)
                    self.assertEqual([d["code"] for d in result["warnings"]], ["EAWR-MODS-0019"] if warnings else [])
                    self.assertEqual([d["code"] for d in result["diagnostics"]], ["EAWR-MODS-0003"] if diagnostics else [])
                    # Service-recorded rows, with only host-local fixture paths/build identity normalised.
                    data = normalise(recorded.read_bytes()).decode("utf-8")
                    entries = [json.loads(line) for line in data.splitlines()]
                    def clean(value):
                        if isinstance(value, str):
                            return value.replace(str(base), "<fixture>").replace(base.as_posix(), "<fixture>").replace("\\", "/")
                        if isinstance(value, list): return [clean(item) for item in value]
                        if isinstance(value, dict): return {key: clean(item) for key, item in value.items()}
                        return value
                    data = "".join(wire(clean(entry)) + "\n" for entry in entries)
                    committed = TRANSCRIPTS / (name + ".jsonl")
                    if UPDATE: committed.write_text(data, encoding="utf-8", newline="\n")
                    else: self.assertEqual(data, committed.read_text(encoding="utf-8"))

    def test_chain_check_params(self):
        for params in ({}, {"layers":None}, {"layers":["key"]}, {"layers":[{}]}, {"layers":[{"key":""}]},
                       {"layers":[], "accepted_warnings":"code"}, {"layers":[], "accepted_warnings":[1]},
                       {"layers":[], "known_chains":{}}, {"layers":[], "known_chains":[]}):
            with self.subTest(params=params):
                messages = session([wire(hello()), wire(request(2, "chain.check", **params))])
                self.assert_error(messages[1], 2, "EAWR-MODS-SVC-0005")

    def test_bad_command_line(self):
        for args in (["--nope"], ["--transcript"]):
            with self.subTest(args=args):
                messages, code, stderr = run(b"", *args)
                self.assertEqual((messages, code), ([], 2))
                self.assertIn("usage", stderr)

    def test_transcripts(self):
        for name, lines in TRANSCRIPT_SESSIONS.items():
            with self.subTest(transcript=name), tempfile.TemporaryDirectory() as temporary:
                recorded = pathlib.Path(temporary) / f"{name}.jsonl"
                prepare_transcript_session(name, pathlib.Path(temporary))
                _, code, stderr = run(to_bytes(lines), "--user-data", temporary, "--transcript", str(recorded))
                self.assertEqual(code, 0, stderr)
                data = normalise(recorded.read_bytes())
                committed = TRANSCRIPTS / f"{name}.jsonl"
                if UPDATE:
                    TRANSCRIPTS.mkdir(parents=True, exist_ok=True)
                    committed.write_bytes(data)
                    continue
                self.assertEqual(data.decode("utf-8"), committed.read_bytes().decode("utf-8"),
                                 f"{committed.name} differs from this build; rerun with --update-transcripts")
                entries = [json.loads(line) for line in data.decode("utf-8").splitlines()]
                self.assertEqual(entries[0], {"dir": "meta",
                                              "msg": {"protocol": f"1.{SERVICE_MINOR}", "recorded_by": "eawr_mods_service"}})
                self.assertTrue(all(entry["dir"] in ("c2s", "s2c") for entry in entries[1:]))


class RecordsContract(unittest.TestCase):
    assert_error = ServiceContract.assert_error
    # Reuse only the assertion helpers, not a second copy of the protocol suite.
    @contextlib.contextmanager
    def fixture(self, mode="real", name="records", transcript=None, extra_objects=""):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            game, user = root / "game", root / "user"
            for folder in (game / "corruption/Data/XML", game / "GameData/Data", root / "game/corruption/Mods/leaf/Data/XML", root / "game/corruption/Mods/parent/Data/XML"):
                folder.mkdir(parents=True)
            (game / "corruption/StarWarsG.exe").write_bytes(b"")
            user.mkdir()
            for folder in (game / "corruption/Data", game / "GameData/Data"):
                (folder / "MegaFiles.xml").write_text("<Mega_Files><File>fixture.meg</File></Mega_Files>")
                entry_name, payload = b"Data/XML/fixture.xml", b"<Fixture/>"
                archive = struct.pack("<IIH", 1, 1, len(entry_name)) + entry_name
                archive += struct.pack("<IIIII", 0x12345678, 0, len(payload), 8 + 2 + len(entry_name) + 20, 0) + payload
                (folder / "fixture.meg").write_bytes(archive)
            for file, tag in {"HardPointDataFiles":"Hard_Point_Files", "FactionFiles":"Faction_Files",
                              "CampaignFiles":"Campaign_Files", "SFXEventFiles":"SFXEvent_Files"}.items():
                (game / "corruption/Data/XML" / (file + ".xml")).write_text(f"<{tag}/>")
            (game / "corruption/Data/XML/GameObjectFiles.xml").write_text(
                "<Game_Object_Files><File>objects.xml</File><File>removed.xml</File></Game_Object_Files>")
            (game / "corruption/Data/XML/objects.xml").write_text(
                '<Objects><SpaceUnit Name="A"><Old>old</Old><Speed>1</Speed><Same>same</Same></SpaceUnit>'
                '<SpaceUnit Name="Base"><Inherited>base</Inherited></SpaceUnit></Objects>')
            (game / "corruption/Data/XML/removed.xml").write_text('<Objects><SpaceUnit Name="Removed"><Old>removed</Old></SpaceUnit></Objects>')
            (root / "game/corruption/Mods/leaf/Data/XML/GameObjectFiles.xml").write_text("<Game_Object_Files><File>objects.xml</File></Game_Object_Files>")
            (root / "game/corruption/Mods/leaf/Data/XML/objects.xml").write_text(
                '<Objects><SpaceUnit Name="Base"><Inherited>base</Inherited></SpaceUnit>'
                '<SpaceUnit Name="A"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type><Speed>3</Speed><Same>same</Same>'
                '<Empty/><Nested><Child Flag="yes">value</Child></Nested></SpaceUnit>'
                '<SpaceUnit Name="A"><Speed>2</Speed></SpaceUnit>' + extra_objects + '</Objects>')
            (root / "game/corruption/Mods/parent/Data/XML/objects.xml").write_text('<Objects><SpaceUnit Name="A"><Speed>2</Speed></SpaceUnit></Objects>')
            (root / "game/corruption/Mods/leaf/modinfo.json").write_text('{"name":"Leaf","dependencies":[{"modtype":0,"identifier":"parent"}]}')
            (root / "game/corruption/Mods/parent/modinfo.json").write_text('{"name":"Parent"}')
            args = ["--user-data", str(user)]
            if mode == "fake":
                # Sibling built by the same CMake job; the helper's PID/starts files are observable process seams.
                candidates = [SERVICE.with_name("fake_loadcheck.exe"),
                              SERVICE.parents[2] / "tests/mods_service/fake_loadcheck.exe",
                              SERVICE.parents[3] / "tests/mods_service/Release/fake_loadcheck.exe"]
                fake = next((p for p in candidates if p.is_file()), None)
                self.assertIsNotNone(fake, "fake_loadcheck must be built for records contracts")
                args += ["--records-helper", str(fake)]
            if transcript:
                args += ["--transcript", str(transcript)]
            client = Interactive(args=args)
            self.ident = 0
            self.events = []
            self.client = client
            try:
                self.result("hello")
                self.result("game.find", hint=str(game))
                self.result("mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[str(root / "game/corruption/Mods/leaf"), str(root / "game/corruption/Mods/parent")], walk_trees=False)
                self.result("profile.save", profile={"id": name, "name": name, "layers": [
                    {"key":"mods:leaf", "root":str(root / "game/corruption/Mods/leaf")}, {"key":"mods:parent", "root":str(root / "game/corruption/Mods/parent")}]})
                yield root, name
            finally:
                client.close()
                self.assertEqual(client.wait(), 0)

    def call(self, method, **params):
        self.ident += 1
        self.client.send(hello(self.ident) if method == "hello" else request(self.ident, method, **params))
        while True:
            value = self.client.receive()
            if "event" in value:
                self.assertEqual(value["event"], "records.progress")
                self.assertEqual(value["id"], self.ident)
                self.events.append(value)
                continue
            self.assertEqual(value.get("id"), self.ident, value)
            return value

    def result(self, method, **params):
        response = self.call(method, **params)
        self.assertIn("result", response, response)
        return response["result"]

    def test_records_index_get_filters_and_recorded_transcript(self):
        with tempfile.TemporaryDirectory() as temporary:
            transcript = pathlib.Path(temporary) / "records.jsonl"
            with self.fixture(transcript=transcript) as (root, profile):
                first = self.result("records.index", profile=profile, limit=1)
                self.assertEqual(first["total"], 3)
                self.assertEqual([r["id"] for r in first["rows"]], ["A"])
                self.assertIsInstance(first["next_cursor"], str)
                self.assertEqual([r["key"] for r in first["layers"]], ["mods:leaf", "mods:parent", "expansion", "base"])
                self.assertTrue(self.events)
                # The service may start only its read-only records helper, never a game or shell.
                from ctypes import wintypes
                class ProcessEntry(ctypes.Structure):
                    _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                                ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                                ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                                ("exe", wintypes.WCHAR * 260)]
                kernel = ctypes.windll.kernel32
                kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
                kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
                kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
                kernel.CloseHandle.argtypes = [wintypes.HANDLE]
                snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
                processes = []
                entry = ProcessEntry(); entry.size = ctypes.sizeof(entry)
                try:
                    more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
                    while more:
                        processes.append((entry.pid, entry.parent, entry.exe))
                        more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
                finally:
                    kernel.CloseHandle(snapshot)
                children = [(pid, exe) for pid, parent, exe in processes if parent == self.client.process.pid]
                self.assertEqual([exe.lower() for _, exe in children], ["eawr_loadcheck.exe"])
                self.assertFalse([pid for pid, parent, _ in processes if parent == children[0][0]])
                event_count = len(self.events)
                second = self.result("records.index", profile=profile, limit=1, cursor=first["next_cursor"])
                self.assertEqual(second["rows"][0]["id"], "Base")
                self.assertEqual(second["counts"], first["counts"])
                last = self.result("records.index", profile=profile, limit=1, cursor=second["next_cursor"])
                self.assertEqual(last["rows"][0]["id"], "Removed")
                self.assertIsNone(last["next_cursor"])
                for filters, expected in [({"object":"a"}, ["A", "Base"]), ({"tag":"sPeEd"}, ["A"]),
                    ({"tag":"Old", "dropped":True}, ["A", "Removed"]),
                    ({"several_mods_same_tag":True}, ["A"]),
                    ({"changed_by_layer":"mods:parent", "tag":"Speed"}, ["A"]),
                    ({"changed_by_layer":"mods:leaf", "tag":"Same"}, []),
                    ({"object":"Removed", "dropped":True}, ["Removed"]),
                    ({"object":"Removed", "several_mods_same_tag":True}, [])]:
                    with self.subTest(filters=filters):
                        page = self.result("records.index", profile=profile, filters=filters)
                        self.assertEqual([r["id"] for r in page["rows"]], expected)
                stale = self.call("records.index", profile=profile, filters={"tag":"Speed"}, cursor=first["next_cursor"])
                self.assert_error(stale, self.ident, "EAWR-MODS-SVC-0005")
                detail = self.result("records.get", profile=profile, id="a", category="game_objects")
                self.assertEqual(detail["variant_chain"], ["A", "Base"])
                rows = {r["tag"]:r for r in detail["tags"]}
                self.assertEqual(rows["Old"]["how"], "dropped")
                self.assertEqual(rows["Old"]["effective"], [])
                self.assertEqual(rows["Speed"]["vanilla"][0]["node"]["raw_text"], "1")
                self.assertEqual(rows["Speed"]["effective"][0]["node"]["raw_text"], "3")
                self.assertEqual(len([c for c in rows["Speed"]["authored"] if c["source"]["layer_id"] == "mods:leaf"]), 2)
                self.assertEqual(rows["Empty"]["authored"][0]["values"][0]["raw_text"], "")
                self.assertEqual(rows["Nested"]["effective"][0]["node"]["children"][0]["attributes"][0]["value"], "yes")
                self.assertEqual(rows["Inherited"]["how"], "inherited")
                self.assertEqual(rows["Inherited"]["authored"][0]["object_id"], "Base")
                self.assertEqual(rows["Inherited"]["effective"][0]["source_object_id"], "Base")
                removed = self.result("records.get", profile=profile, id="Removed", category="game_objects")
                self.assertTrue(all(r["how"] == "dropped" for r in removed["tags"]))
                self.assert_error(self.call("records.get", profile=profile, id="missing", category="game_objects"), self.ident, "EAWR-MODS-SVC-0005")
                self.assert_error(self.call("records.get", profile=profile, id="A", category="abilities"), self.ident, "EAWR-MODS-SVC-0005")
                self.assertEqual(len(self.events), event_count, "same-chain pages and detail must reuse one build")
                self.result("shutdown")
            data = normalise(transcript.read_bytes()).replace(str(root).replace("\\", "\\\\").encode(), b"<fixture>")
            data = data.replace(str(root).replace("\\", "/").encode(), b"<fixture>")
            data = data.replace(b"\\\\", b"/")
            data = re.sub(rb'"(next_cursor|cursor)":"[^"\\]*"', rb'"\1":"<cursor>"', data)
            # Manual discovery keys hash absolute temporary roots. Preserve identity
            # relationships while giving each observed root a stable transcript label.
            path_keys = {}
            def path_key(match):
                value = match.group(1)
                label = path_keys.setdefault(value, f"<root-{len(path_keys) + 1}>".encode())
                return b'"path:' + label + b'"'
            data = re.sub(rb'"path:([0-9a-f]{64})"', path_key, data)
            fixture = TRANSCRIPTS / "records.jsonl"
            if UPDATE:
                fixture.write_bytes(data)
            else:
                self.assertEqual(data, fixture.read_bytes())

    def test_variant_cycle_flags_one_object_and_get_is_capped(self):
        loops = ('<SpaceUnit Name="Loop"><Variant_Of_Existing_Type>Loop</Variant_Of_Existing_Type><Speed>9</Speed></SpaceUnit>'
                 '<SpaceUnit Name="P"><Variant_Of_Existing_Type>Q</Variant_Of_Existing_Type></SpaceUnit>'
                 '<SpaceUnit Name="Q"><Variant_Of_Existing_Type>P</Variant_Of_Existing_Type></SpaceUnit>'
                 '<SpaceUnit Name="Kid"><Variant_Of_Existing_Type>Loop</Variant_Of_Existing_Type></SpaceUnit>'
                 '<SpaceUnit Name="Orphan"><Variant_Of_Existing_Type>Nowhere</Variant_Of_Existing_Type></SpaceUnit>')
        # 40 tags of ~40 KB each (1.6 MB) cannot fit one answer; one tag of 700 KB cannot fit even a slice.
        big = '<SpaceUnit Name="Big">' + "".join(f"<T{i}>{'x' * 40000}</T{i}>" for i in range(40)) + "</SpaceUnit>"
        huge = '<SpaceUnit Name="Huge"><Giant>' + "y" * 700000 + "</Giant><After>ok</After></SpaceUnit>"
        with self.fixture(extra_objects=loops + big + huge) as (root, profile):
            page = self.result("records.index", profile=profile, limit=500)
            rows = {r["id"]: r for r in page["rows"]}
            self.assertEqual(sorted(rows), ["A", "Base", "Big", "Huge", "Kid", "Loop", "Orphan", "P", "Q", "Removed"])
            for name in ("Loop", "P", "Q", "Kid"):
                self.assertEqual(rows[name]["diagnostic"]["code"], "EAWR-XML-0009", name)
                self.assertIn("variant cycle", rows[name]["diagnostic"]["message"])
            self.assertIn("Loop -> Loop", rows["Loop"]["diagnostic"]["message"])
            self.assertEqual(rows["Orphan"]["diagnostic"]["code"], "EAWR-XML-0008")
            for name in ("A", "Base", "Big", "Removed"):
                self.assertNotIn("diagnostic", rows[name], name)
            loop = self.result("records.get", profile=profile, id="loop", category="game_objects")
            self.assertEqual(loop["tags"], [])
            self.assertEqual(loop["diagnostic"]["code"], "EAWR-XML-0009")
            self.assertNotIn("truncated", loop)
            ok = self.result("records.get", profile=profile, id="A", category="game_objects")
            self.assertNotIn("diagnostic", ok)
            self.assertNotIn("truncated", ok)
            self.assertNotIn("tag_count", ok)
            self.assertEqual(ok["variant_chain"], ["A", "Base"])
            slices, offset, seen = [], 0, []
            while True:
                params = {"profile": profile, "id": "Big", "category": "game_objects"}
                if offset: params["tag_offset"] = offset
                part = self.result("records.get", **params)
                slices.append(part)
                seen += [t["tag"] for t in part["tags"]]
                self.assertEqual(part["tag_count"], 40)
                self.assertEqual(part["tag_offset"], offset)
                if not part.get("truncated"):
                    break
                self.assertGreater(part["next_tag_offset"], offset)
                offset = part["next_tag_offset"]
            self.assertGreater(len(slices), 1)
            self.assertEqual(sorted(seen), sorted(f"T{i}" for i in range(40)))
            huge = self.result("records.get", profile=profile, id="Huge", category="game_objects")
            giant = {t["tag"]: t for t in huge["tags"]}["Giant"]
            self.assertTrue(giant["too_large"])
            self.assertEqual(giant["effective"], [])
            self.assertEqual(giant["authored"], [])
            self.assertNotIn("too_large", {t["tag"]: t for t in huge["tags"]}["After"])
            self.assert_error(self.call("records.get", profile=profile, id="Big", category="game_objects", tag_offset=99), self.ident, "EAWR-MODS-SVC-0005")
            self.assert_error(self.call("records.get", profile=profile, id="Big", category="game_objects", tag_offset=-1), self.ident, "EAWR-MODS-SVC-0005")
            filtered = self.result("records.index", profile=profile, filters={"tag": "Speed"})
            self.assertEqual([r["id"] for r in filtered["rows"]], ["A"])
            self.result("shutdown")

    def test_records_fingerprint_reuse_window_and_invalidation(self):
        with self.fixture() as (root, profile):
            leaf = root / "game/corruption/Mods/leaf"
            first = self.result("records.index", profile=profile, limit=1)
            key = first["chain_fingerprint"]
            def touch(text):
                (leaf / "Data/XML/extra.txt").write_text(text)
            # Inside the window a changed file is not noticed: the loaded helper answers with the same fingerprint.
            touch("one")
            self.assertEqual(self.result("records.index", profile=profile, limit=1)["chain_fingerprint"], key)
            self.assertEqual(self.result("records.get", profile=profile, id="A", category="game_objects")["chain_fingerprint"], key)
            # fresh:true always re-fingerprints.
            fresh = self.result("records.index", profile=profile, limit=1, fresh=True)
            self.assertNotEqual(fresh["chain_fingerprint"], key)
            key = fresh["chain_fingerprint"]
            # A read-only method keeps the window; a mutating one (mods.discover) ends it.
            touch("two plus")
            self.result("profile.list")
            self.assertEqual(self.result("records.index", profile=profile, limit=1)["chain_fingerprint"], key)
            self.result("mods.discover", steam_roots=[], steam_libraries=[], manual_folders=[str(leaf), str(root / "game/corruption/Mods/parent")], walk_trees=False)
            after = self.result("records.index", profile=profile, limit=1)
            self.assertNotEqual(after["chain_fingerprint"], key)
            key = after["chain_fingerprint"]
            # profile.save also ends it.
            touch("three plus plus")
            self.result("profile.save", profile={"id": profile, "name": profile, "layers": [
                {"key":"mods:leaf", "root":str(leaf)}, {"key":"mods:parent", "root":str(root / "game/corruption/Mods/parent")}]})
            self.assertNotEqual(self.result("records.index", profile=profile, limit=1)["chain_fingerprint"], key)
            # After the window passes the next request notices a change by itself.
            key = self.result("records.index", profile=profile, limit=1)["chain_fingerprint"]
            touch("four plus plus plus")
            time.sleep(2.2)
            self.assertNotEqual(self.result("records.index", profile=profile, limit=1)["chain_fingerprint"], key)
            for bad in ("yes", 1, None):
                self.assert_error(self.call("records.index", profile=profile, fresh=bad), self.ident, "EAWR-MODS-SVC-0005")

    def test_records_refusals_do_not_spawn_helpers(self):
        with self.fixture(mode="fake") as (root, profile):
            for method, params in [("records.index", {"profile":"missing"}), ("records.index", {"profile":profile, "limit":0}),
                ("records.index", {"profile":profile, "filters":{"dropped":"yes"}}),
                ("records.get", {"profile":profile}), ("records.cancel", {"request":"bad"})]:
                self.assert_error(self.call(method, **params), self.ident, "EAWR-MODS-SVC-0005")
            saved = root / "user/mods/profiles/records.json"
            original = saved.read_text()
            for layers in [[{"key":"mods:leaf", "root":str(root / "game/corruption/Mods/leaf")}],
                           [{"key":"mods:unknown", "root":str(root / "missing-unknown-root")}],
                           [{"key":"mods:leaf", "root":str(root / "absent")}]]:
                doc = json.loads(original); doc["layers"] = layers; saved.write_text(json.dumps(doc))
                self.assert_error(self.call("records.index", profile=profile), self.ident, "EAWR-MODS-SVC-0005")
            saved.write_text(original)
            metadata = root / "game/corruption/Mods/leaf/modinfo.json"
            metadata.write_text('{"name":"Leaf","dependencies":[{"modtype":0,"identifier":"missing"}]}')
            self.assert_error(self.call("records.index", profile=profile), self.ident, "EAWR-MODS-SVC-0005")
            self.assertFalse((root / "user/records-helper.pid").exists())
            self.assertFalse(list((root / "user").rglob("running-*.json")))

    def test_records_cancel_busy_shutdown_and_eof_kill_only_owned_helper(self):
        for ending in ("cancel", "shutdown", "eof", "delete", "save"):
            with self.subTest(ending=ending), self.fixture(mode="fake", name="slow") as (root, profile):
                self.ident += 1; active = self.ident
                self.client.send(request(active, "records.index", profile=profile))
                self.assertEqual(self.client.receive()["event"], "records.progress")
                pid = int((root / "user/records-helper.pid").read_text())
                kernel = ctypes.windll.kernel32
                kernel.OpenProcess.restype = ctypes.c_void_p
                kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
                kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                process = kernel.OpenProcess(0x00100000, False, pid)
                self.assertTrue(process)
                try:
                    self.ident += 1
                    if ending == "cancel":
                        self.client.send(request(self.ident, "records.index", profile=profile))
                        self.assert_error(self.client.receive(), self.ident, "EAWR-MODS-SVC-0010")
                        self.ident += 1
                        self.client.send(request(self.ident, "records.cancel", request=active))
                    elif ending == "shutdown":
                        self.client.send(request(self.ident, "shutdown"))
                    elif ending == "delete":
                        self.client.send(request(self.ident, "profile.delete", id=profile))
                    elif ending == "save":
                        self.client.send(request(self.ident, "profile.save", profile={"id":profile, "name":profile, "layers":[]}))
                    else:
                        self.client.close()
                    values = [self.client.receive() for _ in range(1 if ending == "eof" else 2)]
                    responses = {v["id"]:v for v in values}
                    self.assert_error(responses[active], active, "EAWR-MODS-SVC-0009")
                    if ending == "cancel": self.assertEqual(responses[self.ident]["result"], {"cancelled":True})
                    self.assertEqual(kernel.WaitForSingleObject(process, 10000), 0, "owned helper must exit")
                    self.assertFalse(list((root / "user").rglob("running-*.json")))
                    if ending not in ("eof", "shutdown"):
                        self.assertEqual(self.result("records.cancel", request=active), {"cancelled":False})
                finally:
                    kernel.CloseHandle(process)

    def test_records_helper_cache_errors_and_chain_change(self):
        for mode in ("cached", "crash", "nonjson", "oversize", "changed"):
            with self.subTest(mode=mode), self.fixture(mode="fake", name=mode) as (root, profile):
                self.ident += 1; active = self.ident
                self.client.send(request(active, "records.index", profile=profile))
                self.assertEqual(self.client.receive()["event"], "records.progress")
                if mode == "changed":
                    (root / "game/corruption/Mods/leaf/Data/XML/objects.xml").write_text("changed while indexing")
                    (root / "user/continue").write_text("continue")
                while True:
                    response = self.client.receive()
                    if "event" not in response: break
                if mode == "cached":
                    self.assertIn("result", response)
                    before = len(self.events)
                    self.result("records.index", profile=profile)
                    self.assertEqual(len(self.events), before)
                    self.assertEqual((root / "user/records-helper.starts").read_text().splitlines(), ["start"])
                    (root / "game/corruption/Mods/leaf/Data/XML/objects.xml").write_text("new fingerprint")
                    # Inside the reuse window an on-disk edit is noticed only with fresh:true.
                    self.result("records.index", profile=profile, fresh=True)
                    self.assertEqual(len((root / "user/records-helper.starts").read_text().splitlines()), 2)
                    # Godot uses forward slashes; discovery can return native separators.
                    saved = json.loads((root / "user/mods/profiles/cached.json").read_text())
                    saved["id"] = "slashes"
                    saved["layers"][0]["key"] = "workshop:111"
                    for layer in saved["layers"]:
                        layer["root"] = pathlib.Path(layer["root"]).as_posix()
                    self.result("profile.save", profile=saved)
                    self.result("records.index", profile="slashes")
                    self.assertEqual(len((root / "user/records-helper.starts").read_text().splitlines()), 3)
                else:
                    self.assert_error(response, active, "EAWR-MODS-SVC-0011")


def main() -> int:
    global SERVICE, UPDATE
    args = sys.argv[1:]
    if "--update-transcripts" in args:
        UPDATE = True
        args.remove("--update-transcripts")
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    SERVICE = pathlib.Path(args[0]).resolve()
    if not SERVICE.is_file():
        print(f"mods_service_contract: {SERVICE} does not exist", file=sys.stderr)
        return 2
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
