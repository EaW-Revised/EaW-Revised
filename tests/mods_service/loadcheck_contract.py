"""Synthetic helper/service contracts; no installed game, network or mod assets."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import mods_service_contract as contract

SERVICE, FAKE, HELPER = map(Path, sys.argv[1:4])
UPDATE = "--update-transcripts" in sys.argv
contract.SERVICE = SERVICE
ctypes.windll.kernel32.OpenProcess.restype = wintypes.HANDLE
ctypes.windll.kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
ctypes.windll.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


class LoadCheckContracts(unittest.TestCase):
    def test_shutdown_and_eof_cancel(self):
        for close_stdin in (False, True):
            with self.subTest(eof=close_stdin), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                game = root / "game"
                for name in ("corruption/Data", "GameData/Data"):
                    (game / name).mkdir(parents=True)
                    (game / name / "marker").write_text("synthetic")
                (game / "corruption/StarWarsG.exe").write_bytes(b"")
                user = root / "user"
                client = contract.Interactive(args=["--user-data", str(user), "--loadcheck-helper", str(FAKE)])
                try:
                    for message in (contract.hello(), contract.request(2, "game.find", hint=str(game)),
                                    contract.request(3, "profile.save", profile={"id":"slow", "name":"slow", "layers":[]})):
                        client.send(message)
                        self.assertIn("result", client.receive())
                    client.send(contract.request(4, "loadcheck.start", profile="slow"))
                    self.assertEqual(client.receive()["event"], "loadcheck.progress")
                    if close_stdin:
                        client.process.stdin.close()
                    else:
                        client.send(contract.request(5, "shutdown"))
                    cancelled = client.receive()
                    self.assertEqual(cancelled["error"]["code"], "EAWR-MODS-SVC-0009")
                    if not close_stdin:
                        self.assertEqual(client.receive(), {"id":5, "result":{}})
                    self.assertEqual(client.process.wait(timeout=10), 0)
                finally:
                    client.close()

    def test_process_protocol(self):
        for mode in ("clean", "diagnostic", "slow", "crash", "nonjson", "invalid", "oversize", "wrapped", "changed", "badseverity", "badtotals"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                game = root / "game"
                for name in ("corruption/Data", "GameData/Data"):
                    (game / name).mkdir(parents=True)
                    (game / name / "marker").write_text("synthetic")
                (game / "corruption/StarWarsG.exe").write_bytes(b"")
                user = root / "user"
                transcript = root / "transcript.jsonl"
                client = contract.Interactive(args=["--user-data", str(user), "--loadcheck-helper", str(FAKE),
                                                    "--transcript", str(transcript)])
                try:
                    counter = 0

                    def call(method, **params):
                        nonlocal counter
                        counter += 1
                        client.send(contract.hello(counter) if method == "hello" else contract.request(counter, method, **params))
                        response = client.receive()
                        self.assertEqual(response.get("id"), counter)
                        return response

                    call("hello")
                    call("game.find", hint=str(game))
                    self.assertIn("result", call("profile.save", profile={"id": mode, "name": mode, "layers": [], "launch":{"frame_rate_limit":-1}}))
                    self.assertIsNone(call("loadcheck.result", profile=mode)["result"]["report"])
                    counter += 1
                    start = counter
                    client.send(contract.request(start, "loadcheck.start", profile=mode))
                    self.assertEqual(client.receive(), {"event": "loadcheck.progress", "id": start,
                                     "data": {"stage": "mount", "done": 0, "total": 5}})
                    if mode == "slow":
                        self.assertEqual(call("loadcheck.start", profile=mode)["error"]["code"], "EAWR-MODS-SVC-0010")
                        counter += 1
                        cancel_id = counter
                        client.send(contract.request(cancel_id, "loadcheck.cancel", request=start))
                        answers = [client.receive(), client.receive()]
                        by_id = {a["id"]: a for a in answers}
                        self.assertEqual(by_id[cancel_id]["result"], {"cancelled": True})
                        self.assertEqual(by_id[start]["error"]["code"], "EAWR-MODS-SVC-0009")
                        pid = int((user / "helper.pid").read_text())
                        process = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
                        if process:
                            code = ctypes.c_ulong()
                            ctypes.windll.kernel32.GetExitCodeProcess(process, ctypes.byref(code))
                            ctypes.windll.kernel32.CloseHandle(process)
                            self.assertNotEqual(code.value, 259, "cancel must kill the helper")
                        self.assertEqual(call("loadcheck.cancel", request=start)["result"], {"cancelled": False})
                    elif mode in ("crash", "nonjson", "invalid", "oversize", "wrapped", "changed", "badseverity", "badtotals"):
                        if mode == "changed":
                            (game / "corruption/Data/marker").write_text("changed while loading")
                            (user / "continue").write_text("continue")
                            self.assertEqual(client.receive()["data"]["stage"], "complete")
                        answer = client.receive()
                        self.assertEqual(answer["error"]["code"], "EAWR-MODS-SVC-0011")
                        if mode == "changed": self.assertIn("chain changed during load check", answer["error"]["message"])
                        if mode == "wrapped": self.assertIn("report exceeded the line limit", answer["error"]["message"])
                        self.assertLessEqual(len(contract.wire(answer).encode()), contract.LIMIT)
                        self.assertTrue(answer["error"]["data"]["diagnostics"])
                        self.assertEqual(answer["error"]["data"]["diagnostics"][0]["severity"], "error")
                        self.assertEqual(answer["error"]["data"]["error_count"], 1)
                        self.assertEqual(answer["error"]["data"]["warning_count"], 0)
                        self.assertFalse(list((user / "mods/cache/loadcheck").glob("*.json")))
                        self.assertIsNone(call("loadcheck.result", profile=mode)["result"]["report"])
                    else:
                        self.assertEqual(client.receive()["data"]["stage"], "complete")
                        report = client.receive()["result"]["report"]
                        self.assertEqual(report["content_code"], "ABCDEFGHIJ")
                        self.assertTrue(report["ok"])
                        self.assertEqual(report["error_count"], 0)
                        self.assertEqual(report["warning_count"], 5 if mode == "diagnostic" else 0)
                        if mode == "diagnostic":
                            self.assertTrue(all(row["severity"] == "warning" for row in report["diagnostics"]))
                            page = call("loadcheck.result", profile=mode, limit=1)["result"]
                            self.assertEqual(page["report"]["diagnostics"], report["diagnostics"][:1])
                            self.assertEqual(page["report"]["warning_count"], 5)
                            page2 = call("loadcheck.result", profile=mode, limit=1, cursor=page["next_cursor"])["result"]
                            self.assertEqual(page2["report"]["diagnostics"], report["diagnostics"][1:])
                            self.assertEqual(page2["report"]["warning_count"], 5)
                            self.assertIsNone(page2["next_cursor"])
                        else:
                            self.assertEqual(call("loadcheck.result", profile=mode)["result"]["report"], report)
                        # Cache persists, and a changed base layer invalidates it.
                        self.assertTrue(list((user / "mods/cache/loadcheck").glob("*.json")))
                        (game / "corruption/Data/marker").write_text("updated synthetic bytes")
                        self.assertIsNone(call("loadcheck.result", profile=mode)["result"]["report"])
                    call("shutdown")
                finally:
                    client.close()
                normal = contract.normalise(transcript.read_bytes()).decode()
                # Only volatile root/hello data are normalised; every wire message comes from the service.
                normal = normal.replace(str(game).replace("\\", "\\\\"), "<game-root>")
                destination = Path(__file__).with_name("transcripts") / f"loadcheck_{mode}.jsonl"
                if UPDATE:
                    destination.write_text(normal, encoding="utf-8", newline="\n")
                else:
                    actual = [json.loads(s) for s in normal.splitlines()]
                    expected = [json.loads(s) for s in destination.read_text().splitlines()]
                    if mode == "slow":
                        actual.sort(key=lambda s: json.dumps(s, sort_keys=True))
                        expected.sort(key=lambda s: json.dumps(s, sort_keys=True))
                    self.assertEqual(actual, expected)

    def test_real_helper_refuses_bad_catalog(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for folder in ("game/corruption/Data", "game/GameData/Data", "user"):
                (root / folder).mkdir(parents=True)
            profile = root / "profile.json"
            profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1,
                                           "id": "synthetic", "name": "synthetic", "layers": []}))
            result = subprocess.run([str(HELPER), str(profile), str(root / "user"), str(root / "user/report.json"),
                                     str(root / "game")], capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout.splitlines()[-1])["report"]
            self.assertFalse(report["ok"])
            self.assertTrue(report["diagnostics"])
            self.assertGreater(report["error_count"], 0)
            for severity in ("error", "warning"):
                self.assertEqual(report[severity + "_count"], sum(row["count"] for row in report["diagnostics"] if row["severity"] == severity))

    def test_real_helper_selected_variant_requirements(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for folder in ("game/corruption/Data", "game/GameData/Data", "user", "mod/Data"):
                (root / folder).mkdir(parents=True)
            (root / "mod/modinfo.json").write_text(json.dumps({"name":"main", "dependencies":[{"modtype":1,"identifier":"222"}]}))
            (root / "mod/other-modinfo.json").write_text(json.dumps({"name":"variant", "dependencies":[{"modtype":1,"identifier":"333"}]}))
            profile = root / "profile.json"
            profile.write_text(json.dumps({"format":"eawr-mod-profile", "version":1, "id":"variant", "name":"variant",
                "layers":[{"key":"workshop:111#variant:other-modinfo.json", "root":str(root / "mod")}]}))
            result = subprocess.run([str(HELPER),str(profile),str(root / "user"),str(root / "user/report.json"),str(root / "game")], capture_output=True, timeout=60)
            self.assertEqual(result.returncode,0,result.stderr)
            requirements = json.loads(result.stdout.splitlines()[-1])["report"]["requirements"]
            parents = [r["key"] for r in requirements if r["kind"] == "parent"]
            self.assertIn("workshop:333",parents)
            self.assertNotIn("workshop:222",parents)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
