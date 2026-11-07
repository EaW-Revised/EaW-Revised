"""Synthetic helper/service contracts; no installed game, network or mod assets."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import os
import re
import shutil
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


def _clean_env(extra=None):
    """The helper's environment without a profile path inherited from whoever runs the tests."""
    env = {key: value for key, value in os.environ.items() if key != "EAWR_LOAD_PROFILE"}
    env.update(extra or {})
    return env


FIXTURE_SOURCE = Path(__file__).resolve().parent.parent / "units" / "unit_tables_fixture.cpp"


def _fixture_text(name):
    """A raw-string constant of the unit-table test fixture, so the XML is not copied here."""
    match = re.search(r"constexpr std::string_view " + name + r' = R"xml\((.*?)\)xml";', FIXTURE_SOURCE.read_text(), re.S)
    assert match, name
    return match.group(1)


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

    def test_persisted_status_duration_corruption_and_profile_deletion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, user = root / "game", root / "user"
            for name in ("corruption/Data", "GameData/Data"):
                (game / name).mkdir(parents=True)
                (game / name / "marker").write_text("synthetic")
            (game / "corruption/StarWarsG.exe").write_bytes(b"")
            counter = 0
            client = None

            def call(method, **params):
                nonlocal counter
                counter += 1
                client.send(contract.hello(counter) if method == "hello" else contract.request(counter, method, **params))
                response = client.receive()
                self.assertEqual(response.get("id"), counter, response)
                self.assertIn("result", response)
                return response["result"]

            def restart():
                nonlocal counter, client
                if client:
                    call("shutdown")
                    self.assertEqual(client.process.wait(timeout=10), 0)
                    client.close()
                client = contract.Interactive(args=["--user-data", str(user), "--loadcheck-helper", str(FAKE)])
                counter = 0
                call("hello")
                call("game.find", hint=str(game))

            try:
                restart()
                call("profile.save", profile={"id":"timed", "name":"timed", "layers":[]})
                initial = call("loadcheck.result", profile="timed")
                self.assertEqual(initial["status"], "not_run")
                self.assertIsNone(initial["report"])
                self.assertRegex(initial["chain_fingerprint"], r"^[0-9a-f]{64}$")
                counter += 1
                client.send(contract.request(counter, "loadcheck.start", profile="timed"))
                self.assertEqual(client.receive()["event"], "loadcheck.progress")
                self.assertEqual(client.receive()["event"], "loadcheck.progress")
                report = client.receive()["result"]["report"]
                self.assertIs(type(report["elapsed_ms"]), int)
                self.assertGreaterEqual(report["elapsed_ms"], 100, "sleeping helper measures wall time, not CPU time")
                markers = list((user / "mods/cache/loadcheck").glob("last-*.json"))
                self.assertEqual(len(markers), 1)
                marker = markers[0]
                self.assertLessEqual(marker.stat().st_size, 256)
                saved = marker.read_text()
                self.assertEqual(json.loads(saved)["chain_fingerprint"], initial["chain_fingerprint"])
                self.assertEqual(json.loads(saved)["elapsed_ms"], report["elapsed_ms"])
                restart()
                current = call("loadcheck.result", profile="timed")
                self.assertEqual(current["status"], "current")
                self.assertEqual(current["report"], report)
                self.assertEqual(current["chain_fingerprint"], initial["chain_fingerprint"])
                call("profile.save", profile={"id":"other", "name":"other", "layers":[]})
                self.assertEqual(call("loadcheck.result", profile="other")["status"], "not_run", "another profile sharing the chain has no run marker")
                (game / "corruption/Data/marker").write_text("updated synthetic metadata")
                restart()
                stale = call("loadcheck.result", profile="timed")
                self.assertEqual(stale["status"], "out_of_date")
                self.assertIsNone(stale["report"])
                self.assertNotEqual(stale["chain_fingerprint"], initial["chain_fingerprint"])
                invalid_markers = ["{", "x" * 257, "{}", saved.replace('"version":1', '"version":2'),
                                   saved.replace(initial["chain_fingerprint"], "bad"),
                                   json.dumps(dict(json.loads(saved), elapsed_ms=-1))]
                for invalid in invalid_markers:
                    marker.write_text(invalid)
                    result = call("loadcheck.result", profile="timed")
                    self.assertEqual(result["status"], "not_run")
                    self.assertIsNone(result["report"])
                marker.write_text(saved)
                self.assertEqual(call("profile.delete", id="timed"), {"deleted":True})
                self.assertFalse(marker.exists())
                call("profile.save", profile={"id":"timed", "name":"timed", "layers":[]})
                self.assertEqual(call("loadcheck.result", profile="timed")["status"], "not_run")
            finally:
                if client:
                    client.close()

    def test_corrupt_cached_report_returns_designed_error_and_recovers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, user = root / "game", root / "user"
            for name in ("corruption/Data", "GameData/Data"):
                (game / name).mkdir(parents=True)
                (game / name / "marker").write_text("synthetic")
            (game / "corruption/StarWarsG.exe").write_bytes(b"")
            client = contract.Interactive(args=["--user-data", str(user), "--loadcheck-helper", str(FAKE)])
            try:
                for message in (contract.hello(), contract.request(2, "game.find", hint=str(game)),
                                contract.request(3, "profile.save", profile={"id":"clean", "name":"clean", "layers":[]})):
                    client.send(message)
                    self.assertIn("result", client.receive())
                client.send(contract.request(4, "loadcheck.start", profile="clean"))
                self.assertEqual(client.receive()["event"], "loadcheck.progress")
                self.assertEqual(client.receive()["event"], "loadcheck.progress")
                report = client.receive()["result"]["report"]
                client.send(contract.request(5, "loadcheck.result", profile="clean"))
                current = client.receive()["result"]
                cache = user / "mods/cache/loadcheck" / (current["chain_fingerprint"] + ".json")
                saved = cache.read_bytes()
                invalid = ["{", "", "null", "[]", '"text"', "{}",
                           json.dumps(dict(report, diagnostics="invalid")),
                           json.dumps(dict(report, error_count=1))]
                for request_id, body in enumerate(invalid, 6):
                    with self.subTest(body=body):
                        cache.write_text(body, encoding="utf-8")
                        client.send(contract.request(request_id, "loadcheck.result", profile="clean"))
                        self.assertEqual(client.receive(), {"id":request_id, "error":{
                            "code":"EAWR-MODS-SVC-0011", "message":"invalid cached report"}})
                cache.write_bytes(saved)
                client.send(contract.request(20, "loadcheck.result", profile="clean"))
                self.assertEqual(client.receive()["result"]["report"], report)
                client.send(contract.request(21, "shutdown"))
                self.assertEqual(client.receive(), {"id":21, "result":{}})
            finally:
                client.close()

    def test_profile_delete_cancels_run_without_recreating_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, user = root / "game", root / "user"
            for name in ("corruption/Data", "GameData/Data"):
                (game / name).mkdir(parents=True)
                (game / name / "marker").write_text("synthetic")
            (game / "corruption/StarWarsG.exe").write_bytes(b"")
            client = contract.Interactive(args=["--user-data", str(user), "--loadcheck-helper", str(FAKE)])
            try:
                for message in (contract.hello(), contract.request(2, "game.find", hint=str(game)),
                                contract.request(3, "profile.save", profile={"id":"slow-delete", "name":"slow-delete", "layers":[]})):
                    client.send(message)
                    self.assertIn("result", client.receive())
                client.send(contract.request(4, "loadcheck.start", profile="slow-delete"))
                self.assertEqual(client.receive()["event"], "loadcheck.progress")
                client.send(contract.request(5, "profile.delete", id="slow-delete"))
                answers = {value["id"]: value for value in (client.receive(), client.receive())}
                self.assertEqual(answers[5]["result"], {"deleted":True})
                self.assertEqual(answers[4]["error"]["code"], "EAWR-MODS-SVC-0009")
                self.assertFalse(list((user / "mods/cache/loadcheck").glob("last-*.json")))
            finally:
                client.close()

    def _large_report_case(self, mode, succeeds):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, user = root / "game", root / "user"
            for name in ("corruption/Data", "GameData/Data"):
                (game / name).mkdir(parents=True)
                (game / name / "marker").write_text("synthetic")
            (game / "corruption/StarWarsG.exe").write_bytes(b"")
            client = contract.Interactive(args=["--user-data", str(user), "--loadcheck-helper", str(FAKE)])
            try:
                for message in (contract.hello(), contract.request(2, "game.find", hint=str(game)),
                                contract.request(3, "profile.save", profile={"id":mode, "name":mode, "layers":[]})):
                    client.send(message)
                    self.assertIn("result", client.receive())
                client.send(contract.request(4, "loadcheck.start", profile=mode))
                self.assertEqual(client.receive()["event"], "loadcheck.progress")
                answer = client.receive()
                self.assertEqual(answer["id"], 4)
                self.assertLessEqual(len(contract.wire(answer).encode()), contract.LIMIT)
                if succeeds:
                    report = answer["result"]["report"]
                    self.assertGreater(len(contract.wire(report).encode()), contract.LIMIT - 1024)
                    client.send(contract.request(5, "loadcheck.result", profile=mode))
                    result = client.receive()
                    self.assertLessEqual(len(contract.wire(result).encode()), contract.LIMIT)
                    self.assertEqual(result["result"]["status"], "current")
                    self.assertEqual(result["result"]["report"], report)
                else:
                    self.assertEqual(answer["error"]["code"], "EAWR-MODS-SVC-0011")
                    self.assertIn("report exceeded the line limit", answer["error"]["message"])
                    self.assertFalse(list((user / "mods/cache/loadcheck").glob("*.json")), "oversize report installs no report or marker")
                client.send(contract.request(6, "shutdown"))
                self.assertEqual(client.receive(), {"id":6, "result":{}})
            finally:
                client.close()

    def test_near_limit_report_keeps_room_for_result_envelope(self):
        self._large_report_case("large-valid", True)

    def test_too_large_report_is_rejected_before_cache_install(self):
        self._large_report_case("near-limit", False)

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
                        self.assertIs(type(report["elapsed_ms"]), int)
                        self.assertGreaterEqual(report["elapsed_ms"], 0)
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
                # Volatile hello/root/key/timing fields are normalized; raw timing/status/key semantics are asserted above and below.
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
                                     str(root / "game")], capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout.splitlines()[-1])["report"]
            self.assertFalse(report["ok"])
            self.assertTrue(report["diagnostics"])
            self.assertGreater(report["error_count"], 0)
            for severity in ("error", "warning"):
                self.assertEqual(report[severity + "_count"], sum(row["count"] for row in report["diagnostics"] if row["severity"] == severity))

    def _synthetic_helper_run(self, root, extra, environment=None):
        for folder in ("game/corruption/Data", "game/GameData/Data", "user"):
            (root / folder).mkdir(parents=True, exist_ok=True)
        profile = root / "profile.json"
        profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1,
                                       "id": "synthetic", "name": "synthetic", "layers": []}))
        return subprocess.run([str(HELPER), str(profile), str(root / "user"), str(root / "user/report.json"),
                               str(root / "game")] + extra, capture_output=True, timeout=60, env=_clean_env(environment))

    def test_real_helper_type_limit_requires_a_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = self._synthetic_helper_run(root, [], None)
            self.assertEqual(result.returncode, 0, result.stderr)
            # The flags come first: the helper takes them out of the argument list wherever they appear.
            profile = root / "profile.json"
            result = subprocess.run([str(HELPER), "--load-profile-type-limit", "3", str(profile), str(root / "user"),
                                     str(root / "user/report2.json"), str(root / "game")], capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn(b"--load-profile-type-limit needs", result.stderr)
            self.assertFalse((root / "user/report2.json").exists())

    def test_real_helper_load_profile_flag_and_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            flag_out = root / "flag-profile.json"
            result = self._synthetic_helper_run(root, [], None)
            self.assertEqual(result.returncode, 0, result.stderr)
            profile = root / "profile.json"
            result = subprocess.run([str(HELPER), "--load-profile", str(flag_out), str(profile), str(root / "user"),
                                     str(root / "user/report.json"), str(root / "game")], capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode, 0, result.stderr)
            flagged = json.loads(flag_out.read_text())
            self.assertEqual(flagged["schema"], "eawr.load_profile/1")
            self.assertEqual(flagged["recorder_scope"], "calling-thread-only")
            self.assertEqual(flagged["type_limit"], 0)
            env_out = root / "env-profile.json"
            result = self._synthetic_helper_run(root, [], {"EAWR_LOAD_PROFILE": str(env_out)})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(env_out.read_text())["schema"], "eawr.load_profile/1")
            # With a profile the cap is accepted and stamped in the profile report.
            result = subprocess.run([str(HELPER), "--load-profile", str(flag_out), "--load-profile-type-limit", "7", str(profile),
                                     str(root / "user"), str(root / "user/report.json"), str(root / "game")],
                                    capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(flag_out.read_text())["type_limit"], 7)
            # An invalid cap is refused.
            result = subprocess.run([str(HELPER), "--load-profile", str(flag_out), "--load-profile-type-limit", "0", str(profile),
                                     str(root / "user"), str(root / "user/report.json"), str(root / "game")],
                                    capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode, 2)

    def test_real_helper_long_environment_profile_path(self):
        # The path is far longer than any fixed buffer (the size query has to retry), yet every directory name is
        # short enough for NTFS and the whole path uses the extended-length form, so it is a valid place to write.
        root = Path(tempfile.mkdtemp())
        try:
            deep = root
            for _ in range(6):
                deep = deep / ("d" * 200)
            extended = "\\\\?\\" + str(deep)
            os.makedirs(extended)
            env_out = extended + "\\env-profile.json"
            self.assertGreater(len(env_out), 1200)
            result = self._synthetic_helper_run(root, [], {"EAWR_LOAD_PROFILE": env_out})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(Path(env_out).read_text())["schema"], "eawr.load_profile/1")
        finally:
            shutil.rmtree("\\\\?\\" + str(root), ignore_errors=True)  # the plain form cannot delete a path this long

    def test_real_helper_partial_load_warning_end_to_end(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "game/corruption/Data"
            (root / "game/GameData/Data").mkdir(parents=True)
            xml = data / "XML"
            (xml / "Enum").mkdir(parents=True)
            (root / "user").mkdir()

            def put(name, text):
                (xml / name).write_text(text, encoding="utf-8")

            (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            (root / "game/GameData/Data/MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            put("GameObjectFiles.xml", "<Game_Object_Files><File>lc_units.xml</File></Game_Object_Files>")
            put("lc_units.xml", "<Game_Objects>" + "".join(
                "<SpaceUnit Name=\"LC_Unit_%d\"><Space_Model_Name>lc_%d.alo</Space_Model_Name></SpaceUnit>" % (i, i)
                for i in range(4)) + "</Game_Objects>")
            put("HardpointDataFiles.xml", "<Hard_Point_Files></Hard_Point_Files>")
            put("FactionFiles.xml", "<Faction_Files></Faction_Files>")
            put("CampaignFiles.xml", "<Campaign_Files></Campaign_Files>")
            put("SFXEventFiles.xml", "<SFXEvent_Files></SFXEvent_Files>")
            put("TargetingPrioritySetFiles.xml",
                "<Targeting_Priority_Set_Files><File>EawrPriorities.xml</File></Targeting_Priority_Set_Files>")
            put("EawrPriorities.xml", _fixture_text("priorities_xml"))
            put("Enum/GameObjectCategoryType.xml", _fixture_text("categories_xml"))
            put("Enum/GameObjectPropertiesType.xml", _fixture_text("properties_xml"))
            put("GameConstants.xml", _fixture_text("constants_xml"))
            profile = root / "profile.json"
            profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1,
                                           "id": "partial", "name": "partial", "layers": []}))
            profile_out = root / "profile-report.json"

            def run(extra):
                result = subprocess.run([str(HELPER)] + extra + [str(profile), str(root / "user"), str(root / "user/report.json"),
                                         str(root / "game")], capture_output=True, timeout=120, env=_clean_env())
                self.assertEqual(result.returncode, 0, result.stderr)
                return json.loads(result.stdout.splitlines()[-1])["report"]

            def partial_rows(report):
                return [row for row in report["diagnostics"] if row["code"] == "EAWR-MODS-LC-0001"]

            capped = run(["--load-profile", str(profile_out), "--load-profile-type-limit", "2"])
            rows = partial_rows(capped)
            self.assertEqual(len(rows), 1, capped["diagnostics"])
            self.assertEqual(rows[0]["severity"], "warning")
            self.assertIn("2 of 4 game object types", json.dumps(rows[0]))
            # By design the warning only labels the run as partial: it adds a warning and no error, and leaves
            # ok to the loader's own verdict. A full load carries no such row.
            self.assertFalse([row for row in capped["diagnostics"] if row["code"] == "EAWR-MODS-LC-0001" and row["severity"] == "error"])
            self.assertEqual(json.loads(profile_out.read_text())["type_limit"], 2)
            self.assertEqual(partial_rows(run([])), [])

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
            result = subprocess.run([str(HELPER),str(profile),str(root / "user"),str(root / "user/report.json"),str(root / "game")], capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode,0,result.stderr)
            requirements = json.loads(result.stdout.splitlines()[-1])["report"]["requirements"]
            parents = [r["key"] for r in requirements if r["kind"] == "parent"]
            self.assertIn("workshop:333",parents)
            self.assertNotIn("workshop:222",parents)

    def test_real_helper_launch_script_hint_is_not_a_requirement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for folder in ("game/corruption/Data", "game/GameData/Data", "user", "mod/Data"):
                (root / folder).mkdir(parents=True)
            # No modinfo: the only metadata is a launch script whose MODPATH names some folder.
            (root / "mod/launch.bat").write_text("StarWarsG.exe MODPATH=Mods/Some-Folder\n")
            profile = root / "profile.json"
            profile.write_text(json.dumps({"format":"eawr-mod-profile", "version":1, "id":"hint", "name":"hint",
                "layers":[{"key":"workshop:111", "root":str(root / "mod")}]}))
            result = subprocess.run([str(HELPER),str(profile),str(root / "user"),str(root / "user/report.json"),str(root / "game")], capture_output=True, timeout=60, env=_clean_env())
            self.assertEqual(result.returncode,0,result.stderr)
            report = json.loads(result.stdout.splitlines()[-1])["report"]
            self.assertEqual([r for r in report["requirements"] if r["kind"] == "parent"], [])
            self.assertNotIn("EAWR-MODS-0003", [d["code"] for d in report["diagnostics"]])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
