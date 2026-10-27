#!/usr/bin/env python3
"""Contract tests of eawr_mods_service against docs/mods-service-protocol.md.

Drives the built service through its stdio pipes, the way the launcher's Godot manager does
(OS.execute_with_pipe): every implemented method and error code, the 1 MiB line limit, the framing rules, the exit
on a closed stdin and after shutdown, and the GUI subsystem of the Windows executable (no console window). It also
re-records the committed transcripts in transcripts/ and compares them byte for byte, after normalising the three
build-specific hello values (protocol doc, section 9).

Usage: mods_service_contract.py <eawr_mods_service> [--update-transcripts]
  --update-transcripts  rewrite transcripts/*.jsonl from this build instead of comparing
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import queue
import re
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

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
    "unknown_method": [wire(hello()), wire(request(2, "files.page")), wire(request(3, "no.such.method"))],
    "malformed": ["not json", "[1,2]", wire({"method": "hello"}), wire(request(1, "shutdown")), wire(hello(2))],
}

VOLATILE = re.compile(rb'"(service_version|engine_build|game_release)":"[^"\\]*"')
NORMALISED = {b"service_version": b"0.0.0", b"engine_build": b"0.0.0+transcript", b"game_release": b"0.0.0"}


def normalise(data: bytes) -> bytes:
    return VOLATILE.sub(lambda match: b'"' + match.group(1) + b'":"' + NORMALISED[match.group(1)] + b'"', data)


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
        self.assertEqual(result["protocol"], {"major": 1, "minor": 3})
        self.assertEqual(result["capabilities"], ["chain.check", "chain.resolve", "game.find", "hello", "loadcheck.cancel", "loadcheck.result", "loadcheck.start", "mods.discover", "profile.delete", "profile.list", "profile.load", "profile.save", "shutdown"])
        self.assertEqual(result["capabilities"], sorted(result["capabilities"]))
        self.assertRegex(result["service_version"], r"^\d+\.\d+\.\d+$")
        self.assertEqual(result["game_release"], result["service_version"])
        self.assertRegex(result["engine_build"], r"^[A-Za-z0-9_.+-]+$")

    @unittest.skipUnless(sys.platform == "win32", "PE check of the Windows executable")
    def test_windows_executable_is_gui_subsystem(self):
        # 2 = IMAGE_SUBSYSTEM_WINDOWS_GUI: Windows never gives it a console window. Every other test here talks to it
        # over inherited pipes, as Godot's OS.execute_with_pipe does.
        self.assertEqual(pe_subsystem(SERVICE), 2)

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
        for protocol in ({"major": 2}, {"major": 0}, {"major": 1, "min_minor": 4}):
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
        messages = session([wire(request(9, "no.such.method")), wire(hello(10)), wire(request(11, "files.page")),
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
                     request(7, "profile.load", id="fixture"), request(8, "profile.delete", id="fixture"),
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
                _, code, stderr = run(to_bytes(lines), "--transcript", str(recorded))
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
                                              "msg": {"protocol": "1.3", "recorded_by": "eawr_mods_service"}})
                self.assertTrue(all(entry["dir"] in ("c2s", "s2c") for entry in entries[1:]))


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
